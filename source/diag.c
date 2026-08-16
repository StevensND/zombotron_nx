/* diag.c -- see diag.h. Frame-1 black-hang instrumentation.
 *
 * This software may be modified and distributed under the terms of the MIT
 * license. See the LICENSE file for details.
 */
#define _GNU_SOURCE
#include <switch.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "diag.h"
#include "util.h"   /* stallPrintf -- see below, NOT debugPrintf */

/* EVERY line in this file must use stallPrintf, never debugPrintf.
 * debugPrintf takes a global mutex; the thread this file exists to report
 * on is typically blocked inside the filesystem while holding it, so a
 * single debugPrintf here deadlocks the watchdog against its own subject.
 * That is exactly what happened: an earlier pass converted the lines
 * starting "[wd]" but missed two that start "\n[wd]", and the watchdog went
 * silent for four runs -- stall.log contained only the arm line. */
#include "so_util.h" /* so_find_module_by_addr for backtrace symbolication */

/* Base of the loaded libil2cpp (set in main.c). Used by gc_set_stack_ptr below
 * to reach Boehm's GC_threads table -- the same base the stop-the-world bridge
 * in libc_shim.c dereferences, guarded there by eight instruction-word checks. */
extern uintptr_t g_il2cpp_base;

/* ------------------------------------------------------------------ tunables */
#define DIAG_MAX_THREADS   96
/* GC snapshot-suspension. Kept as a toggle but OFF: letting threads run during
 * the mark is not managed-safe (a thread can allocate during the mark and have
 * the object swept -- observed as a NULL free-list deref at libunity+0x4fa7e0,
 * frame ~186). Real suspension (freeze through the mark) is managed-correct; the
 * load-time perturbation it caused is now handled a different way -- automatic
 * collection is disabled and the collector is driven only at frame boundaries
 * (see zombotron_boot.c controlled_gc_*), so a mark never lands mid-frame while
 * Unity is deserialising. Set to 1 only to re-test concurrent marking. */
#define GC_SNAPSHOT_SUSPEND 0
#define DIAG_POLL_NS       (1000ull * 1000ull * 1000ull)   /* watchdog tick: 1s */
/* 3s, not 6s. The engine's slowest legitimate frame measured 3.9s during the
 * initial scene load, so 6s was chosen to avoid false positives -- but the load
 * frames are now identifiable by their own [loop] SLOW markers, and waiting 6s
 * into a 100% CPU spin risks the console being power-cycled before the dump
 * lands. False positives during load are cheap; a missed dump is not. */
#define DIAG_STALL_NS      (3000ull * 1000ull * 1000ull)   /* declare stall: 3s  */
/* Invasive thread-context dump. The watchdog pauses every thread
 * (svcSetThreadActivity) to read its registers/backtrace. That is fine when the
 * "stall" is a thread blocked in a syscall (slow SD read), but it DEADLOCKS a
 * thread that is mid-compute inside Unity's scene integration -- pausing it while
 * it holds an internal lock and then having the loader wait on that lock hangs
 * the load. That is the frame-21 "Intro" freeze: the load legitimately takes
 * >3s, the watchdog fires, and the dump freezes it. Default OFF -> the watchdog
 * still detects and logs stalls (and the per-thread registry state, which is
 * lock-free) but never pauses a thread. Set to 1 only to re-enable backtraces
 * for debugging a genuine hang. */
#define DIAG_WD_INVASIVE   0
#define DIAG_REDUMP_NS     (6000ull * 1000ull * 1000ull)   /* re-dump cadence    */

typedef struct {
  volatile int          in_use;
  uint64_t              tid;            /* svcGetThreadId — matches crash reports */
  Handle                handle;         /* real thread handle for svcGetThreadContext3 */
  pthread_t             pth;            /* host handle, for setname target match  */
  char                  name[32];
  const void           *entry;
  int                   is_main_engine;
  /* live wait beacon */
  volatile int          wait_kind;
  volatile const void  *wait_obj;
  volatile uint64_t     wait_since;     /* tick (raw) when current wait began     */
  /* liveness counters */
  volatile uint64_t     waits_total;
  volatile uint64_t     wakes_total;
  volatile uint64_t     futex_spins;
  volatile uint64_t     last_active;    /* tick of last beacon activity           */
} DiagThread;

static DiagThread       g_threads[DIAG_MAX_THREADS];
static Mutex            g_reg_lock;     /* zero-init libnx Mutex == unlocked       */
static __thread DiagThread *self;       /* this thread's slot (host TLS)           */

static volatile int      g_frame = -1;
static volatile uint64_t g_last_progress;   /* tick of last diag_frame()           */
static volatile int      g_wd_started;
static Thread            g_wd_thread;

/* ------------------------------------------------------------------ helpers */
static inline uint64_t now_tick(void) { return armGetSystemTick(); }
static inline uint64_t tick_to_ns(uint64_t t) { return armTicksToNs(t); }

static const char *wait_kind_name(int k) {
  switch (k) {
    case DIAG_W_COND:   return "cond_wait";
    case DIAG_W_JOIN:   return "join";
    case DIAG_W_SEM:    return "sem_wait";
    case DIAG_W_MUTEX:  return "mutex_lock";
    case DIAG_W_RWLOCK: return "rwlock";
    case DIAG_W_FUTEX:  return "futex_spin";
    default:            return "running";
  }
}

static DiagThread *slot_alloc(void) {
  mutexLock(&g_reg_lock);
  DiagThread *t = NULL;
  for (int i = 0; i < DIAG_MAX_THREADS; i++) {
    if (!g_threads[i].in_use) { t = &g_threads[i]; break; }
  }
  if (t) {
    memset(t, 0, sizeof(*t));
    t->in_use = 1;
    t->pth = pthread_self();
    t->handle = threadGetCurHandle();   /* real handle, usable from the watchdog */
    uint64_t tid = 0;
    if (R_SUCCEEDED(svcGetThreadId(&tid, CUR_THREAD_HANDLE))) t->tid = tid;
    t->last_active = now_tick();
  }
  mutexUnlock(&g_reg_lock);
  return t;
}

/* Return this thread's slot, lazily allocating one if it ran code we didn't
 * trampoline (e.g. the process main thread). Never returns NULL unless the
 * registry is full (then beacons silently no-op). */
static DiagThread *diag_self(void) {
  if (self) return self;
  DiagThread *t = slot_alloc();
  if (t && t->name[0] == 0) {
    /* default label until a real name arrives */
    snprintf(t->name, sizeof(t->name), "T%llu", (unsigned long long)t->tid);
  }
  self = t;
  return t;
}

/* ------------------------------------------------------------------ public */
void diag_thread_register(const void *entry, int is_main_engine) {
  DiagThread *t = diag_self();
  if (!t) return;
  t->entry = entry;
  t->is_main_engine = is_main_engine;
  if (is_main_engine && t->name[0] == 'T')   /* keep until Unity renames it */
    snprintf(t->name, sizeof(t->name), "engine_main");
}

void diag_thread_unregister(void) {
  if (!self) return;
  mutexLock(&g_reg_lock);
  self->in_use = 0;
  mutexUnlock(&g_reg_lock);
  self = NULL;
}

void diag_set_name(void *target_pthread, const char *name) {
  if (!name) return;
  DiagThread *t = NULL;
  if (target_pthread) {
    pthread_t want = (pthread_t)target_pthread;
    mutexLock(&g_reg_lock);
    for (int i = 0; i < DIAG_MAX_THREADS; i++) {
      if (g_threads[i].in_use && pthread_equal(g_threads[i].pth, want)) { t = &g_threads[i]; break; }
    }
    mutexUnlock(&g_reg_lock);
  }
  if (!t) t = diag_self();   /* PR_SET_NAME / self-naming case */
  if (!t) return;
  strncpy(t->name, name, sizeof(t->name) - 1);
  t->name[sizeof(t->name) - 1] = 0;
}

/* ------------------------------------------------ GC register-root capture
 * When the bridge really suspends a thread (svcSetThreadActivity), a live
 * object whose ONLY reference sits in that frozen thread's CPU register is
 * invisible to Boehm's conservative scan: the real signal handler would have
 * pushed the register context onto the stack for scanning, ours cannot. That
 * shows up as an intermittent use-after-free during the heavy Intro scene load
 * (GC runs constantly there and threads freeze at arbitrary points).
 *
 * Fix: hand the collector a fixed (uncollectable) AND pointer-scanned buffer,
 * obtained via il2cpp_gc_alloc_fixed, and drop each paused thread's GP
 * registers into its slot during suspend. Boehm then scans those words as
 * potential roots (conservatively -- non-pointers are bounds-checked away), so
 * a register-only reference keeps its object marked. Cleared on resume so a
 * running thread's stale registers don't over-retain across the next mark.
 * If il2cpp_gc_alloc_fixed is absent the capture is simply off (bridge behaves
 * as before -- exposed to the rare miss, but never worse). */
#define GC_ROOT_REGS_PER_SLOT 32     /* x0-x28 + fp + lr = 31, padded to 32 */
static volatile uint64_t *g_gc_root_regs = NULL;  /* scanned fixed buffer      */
static int                g_gc_root_slots = 0;    /* == DIAG_MAX_THREADS if on */

/* ------------------------------------------- GC per-thread stack-pointer set
 * Boehm marks each *other* thread's stack from thread->stop_info.stack_ptr up to
 * the stack base; on a real target the SIGPWR handler stores the thread's SP
 * there at every stop. Our bridge never runs that handler, so for a thread that
 * is actively running managed code (not parked in GC_do_blocking) that field is
 * stale or zero -- and GC_push_all_stacks ABORTs on a zero there (verified in
 * libil2cpp at il2cpp+0x17b2a84). Since we already read the frozen thread's
 * context for the register capture, we also write its real SP into that field,
 * exactly as the handler would. That both feeds the conservative scan the right
 * stack range and removes the abort.
 *
 * GC_thread layout (from libil2cpp GC_new_thread/GC_push_all_stacks):
 *   +0x00 next (hash chain)   +0x08 id (pthread_t)   +0x18 stop_info.stack_ptr
 * GC_threads is a 256-bucket open hash at il2cpp+0x3b936a8; the bucket index is
 * ((id>>8) ^ id) folded with >>16, low byte. We reproduce that hash, walk the
 * chain, and only write when a node's id matches -- so a wrong offset (different
 * libil2cpp) finds nothing and writes nothing rather than corrupting memory. */
#define GC_THREADS_OFF   0x3b936a8   /* GC_threads[256] open-hash table */
#define GC_TH_NEXT       0x00
#define GC_TH_ID         0x08
#define GC_TH_STACKPTR   0x18
#define GC_INCREMENTAL_OFF 0x3b92ce8 /* bdwgc GC_incremental flag (nonzero = on) */
/* GC bridge counters -- read by the watchdog dump and the crash handler
 * (nx_crash_handler.c externs these) to make the next crash log conclusive. */
volatile uint64_t g_gc_sp_set     = 0;   /* stack_ptr writes (found in GC_threads) */
volatile uint64_t g_gc_susp_calls = 0;   /* diag_gc_suspend_thread invocations     */
volatile uint64_t g_gc_ctx_fail   = 0;   /* svcGetThreadContext3 gave up -> no roots*/
volatile int32_t  g_gc_paused_now = 0;   /* threads paused by the bridge right now  */

static void gc_set_stack_ptr(pthread_t id, uintptr_t sp) {
  uintptr_t b = g_il2cpp_base;
  if (!b || !sp) return;
  uint64_t id64 = (uint64_t)(uintptr_t)id;
  uint32_t h = (uint32_t)(id64 >> 8) ^ (uint32_t)id64;
  h ^= (h >> 16);
  h &= 0xff;
  uintptr_t node = *(volatile uintptr_t *)(b + GC_THREADS_OFF + (uintptr_t)h * 8);
  for (int guard = 0; node && guard < 4096; guard++) {
    if (*(volatile uintptr_t *)(node + GC_TH_ID) == (uintptr_t)id) {
      *(volatile uintptr_t *)(node + GC_TH_STACKPTR) = sp;   /* == what the handler stores */
      g_gc_sp_set++;
      return;
    }
    node = *(volatile uintptr_t *)(node + GC_TH_NEXT);
  }
  /* not found: thread not (yet) in GC_threads, or offsets stale -> write nothing */
}

void diag_gc_init_root_capture(void *(*alloc_fixed)(unsigned long)) {
  if (!alloc_fixed) {
    debugPrintf("[gc] register-root capture OFF: il2cpp_gc_alloc_fixed absent -- "
                "real-suspend can miss register-only roots\n");
    return;
  }
  unsigned long bytes =
      (unsigned long)DIAG_MAX_THREADS * GC_ROOT_REGS_PER_SLOT * sizeof(uint64_t);
  void *buf = alloc_fixed(bytes);
  if (!buf) {
    debugPrintf("[gc] register-root capture OFF: il2cpp_gc_alloc_fixed(%lu) failed\n", bytes);
    return;
  }
  memset(buf, 0, bytes);
  g_gc_root_regs  = (volatile uint64_t *)buf;
  g_gc_root_slots = DIAG_MAX_THREADS;
  debugPrintf("[gc] register-root capture ON: %lu-byte scanned buffer (%d slots)\n",
              bytes, g_gc_root_slots);
}

/* ---------------------------------------------------- GC stop-the-world pause
 * Actually suspend/resume a mutator thread for the Boehm-GC stop-the-world, by
 * its host pthread_t (passed as void* to keep pthread.h out of diag.h). The GC
 * bridge in libc_shim.c posts the suspend/restart ack the never-delivered
 * signal handler would have -- but that left the target RUNNING through the
 * collector's mark window, so on a long frame-1 load 20+ job/loader threads
 * raced the mark and intermittently wedged the collector in its resend spin.
 * Pausing for real makes the mark window race-free, like the signal suspension
 * the game gets on Android; the register capture above closes the root gap that
 * freezing a thread would otherwise open.
 *
 * The registry read is LOCK-FREE -- exactly what the watchdog's dump_threads
 * already does -- so it never blocks on g_reg_lock, which the target might hold
 * mid-registration (taking it here would self-deadlock the collector). We never
 * pause `self`/the collector. Return 1 if the thread was matched (whether or not
 * it needed the activity call), 0 if unknown -- the caller then falls back to
 * ack-only, which is exactly the pre-existing behaviour, so an unregistered
 * thread is never worse off than before. */
int diag_gc_suspend_thread(void *target_pthread) {
  pthread_t want = (pthread_t)(uintptr_t)target_pthread;
  if (pthread_equal(want, pthread_self())) return 1;   /* never pause the collector */
  g_gc_susp_calls++;
  Handle cur = threadGetCurHandle();
  for (int i = 0; i < DIAG_MAX_THREADS; i++) {
    if (g_threads[i].in_use && pthread_equal(g_threads[i].pth, want)) {
      Handle h = g_threads[i].handle;
      if (h && h != cur) {
        svcSetThreadActivity(h, ThreadActivity_Paused);
        /* svcSetThreadActivity is asynchronous: a thread busy in userland (very
         * likely at frame-0/1 load) may not have descheduled yet, so the first
         * context read can fail. Retry briefly so we ALWAYS capture this thread's
         * roots -- a thread we fail to read contributes neither registers nor a
         * stack range, and Boehm would then free objects it still holds. */
        ThreadContext ctx;
        Result rc = svcGetThreadContext3(&ctx, h);
        for (int tries = 0; R_FAILED(rc) && tries < 64; tries++) {
          svcSleepThread(30000);   /* 30us; lets the async pause settle */
          rc = svcGetThreadContext3(&ctx, h);
        }
        if (R_SUCCEEDED(rc)) {
          /* stack-range root: hand Boehm this thread's real SP (what the SIGPWR
           * handler would have stored), so it scans the live stack and doesn't
           * hit the zero-stack_ptr abort. */
          gc_set_stack_ptr(want, (uintptr_t)ctx.sp);
          /* register-only roots: stash GP regs in the scanned fixed buffer */
          if (g_gc_root_regs && i < g_gc_root_slots) {
            volatile uint64_t *s = g_gc_root_regs + (size_t)i * GC_ROOT_REGS_PER_SLOT;
            for (int r = 0; r < 29; r++) s[r] = ctx.cpu_gprs[r].x;
            s[29] = ctx.fp;
            s[30] = ctx.lr;
            s[31] = 0;
          }
        } else {
          g_gc_ctx_fail++;   /* roots for this thread NOT captured this cycle */
        }
#if GC_SNAPSHOT_SUSPEND
        /* Roots are snapshotted; let the thread run again immediately instead of
         * freezing it through the whole mark. The register buffer keeps this
         * snapshot (overwritten on this thread's next suspend), so the mark still
         * sees these roots. */
        svcSetThreadActivity(h, ThreadActivity_Runnable);
#else
        g_gc_paused_now++;   /* keep it frozen until the restart signal */
#endif
      }
      return 1;
    }
  }
  return 0;
}

int diag_gc_resume_thread(void *target_pthread) {
  pthread_t want = (pthread_t)(uintptr_t)target_pthread;
  if (pthread_equal(want, pthread_self())) return 1;
  Handle cur = threadGetCurHandle();
  for (int i = 0; i < DIAG_MAX_THREADS; i++) {
    if (g_threads[i].in_use && pthread_equal(g_threads[i].pth, want)) {
      Handle h = g_threads[i].handle;
      if (h && h != cur) {
#if GC_SNAPSHOT_SUSPEND
        /* Snapshot mode: the thread was already resumed at suspend time and is
         * running. Keep its register snapshot in the buffer (it is overwritten on
         * the next suspend); clearing it here would race the running thread. */
        (void)h;
#else
        /* drop this thread's register roots before it runs again */
        if (g_gc_root_regs && i < g_gc_root_slots) {
          volatile uint64_t *s = g_gc_root_regs + (size_t)i * GC_ROOT_REGS_PER_SLOT;
          for (int r = 0; r < GC_ROOT_REGS_PER_SLOT; r++) s[r] = 0;
        }
        svcSetThreadActivity(h, ThreadActivity_Runnable);
        if (g_gc_paused_now > 0) g_gc_paused_now--;
#endif
      }
      return 1;
    }
  }
  return 0;
}

void diag_wait_enter(int kind, const void *obj) {
  DiagThread *t = diag_self();
  if (!t) return;
  t->wait_kind  = kind;
  t->wait_obj   = obj;
  t->wait_since = now_tick();
  t->waits_total++;
  t->last_active = t->wait_since;
}

void diag_wait_exit(void) {
  DiagThread *t = self;          /* exit without a prior enter is harmless */
  if (!t) return;
  t->wait_kind = DIAG_W_NONE;
  t->wait_obj  = NULL;
  t->wakes_total++;
  t->last_active = now_tick();
}

void diag_futex_spin(const void *obj) {
  DiagThread *t = diag_self();
  if (!t) return;
  /* publish as a futex wait but keep counting spins so the watchdog can tell
   * "alive but never satisfied" from "hard-parked". Reset wait_since only on
   * the transition *into* a futex wait (or onto a different uaddr), so the
   * dumped "parked secs" measures the current spin episode. */
  int was_futex = (t->wait_kind == DIAG_W_FUTEX && t->wait_obj == obj);
  t->wait_kind  = DIAG_W_FUTEX;
  t->wait_obj   = obj;
  uint64_t now = now_tick();
  if (!was_futex) t->wait_since = now;
  t->futex_spins++;
  t->last_active = now;
}

void diag_frame(int frame) {
  g_frame = frame;
  g_last_progress = now_tick();
}

/* ------------------------------------------------------------------ watchdog */
static uint64_t prev_waits[DIAG_MAX_THREADS];
static uint64_t prev_wakes[DIAG_MAX_THREADS];
static uint64_t prev_spins[DIAG_MAX_THREADS];

/* ---- CPU-context snapshot: see *where in libunity/il2cpp* a thread is wedged.
 * The shim beacons only show which sync primitive a thread sits in; when the
 * hang is inside native engine code (our case: main thread parked inside
 * Unity_nativeRender), this backtrace is what actually pinpoints it. */

/* Our own NRO code region, resolved once via svcQueryMemory on a local fn. */
static uint64_t g_nro_base, g_nro_size;
/* Set per-thread by snapshot_thread: enable the stack scan only for the main /
 * loader threads so the dump stays readable. */
static int g_scan_stack;
static void nro_range_init(void) {
  MemoryInfo mi; u32 pi;
  if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)(uintptr_t)&nro_range_init)) && mi.size) {
    g_nro_base = mi.addr; g_nro_size = mi.size;
  }
}
/* Write a symbolicated label for `addr` into buf: "libX+0xoff" / "NRO+0xoff"
 * / raw absolute. */
static void resolve_addr(char *buf, size_t n, uint64_t addr) {
  so_module *m = so_find_module_by_addr((const void *)(uintptr_t)addr);
  if (m) {
    /* m->name is the full sdmc path; the leading dirs are identical for every
     * loaded .so, so a fixed-width truncation makes libunity and libil2cpp
     * indistinguishable. Print the basename instead. */
    const char *base = m->name, *p;
    for (p = m->name; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    snprintf(buf, n, "%s+0x%llx", base,
             (unsigned long long)(addr - (uint64_t)(uintptr_t)m->load_virtbase));
  } else if (g_nro_size && addr >= g_nro_base && addr < g_nro_base + g_nro_size) {
    snprintf(buf, n, "NRO+0x%llx", (unsigned long long)(addr - g_nro_base));
  } else {
    snprintf(buf, n, "0x%llx", (unsigned long long)addr);
  }
}

__attribute__((unused))
static void dump_thread_context(const char *name, const ThreadContext *ctx) {
  char a[40], b[40];
  resolve_addr(a, sizeof a, ctx->pc.x);
  resolve_addr(b, sizeof b, ctx->lr);
  stallPrintf("[wd]   %s  PC=%s  LR=%s\n", name, a, b);
  stallPrintf("[wd]     SP=0x%llx FP=0x%llx X0=0x%llx X1=0x%llx X2=0x%llx\n",
              (unsigned long long)ctx->sp, (unsigned long long)ctx->fp,
              (unsigned long long)ctx->cpu_gprs[0].x,
              (unsigned long long)ctx->cpu_gprs[1].x,
              (unsigned long long)ctx->cpu_gprs[2].x);
  /* For a thread parked in svcArbitrateLock, X1 is typically the mutex address
   * and X0 the owner's thread-handle tag -> identifies who holds the lock. */
  /* Clean backtrace via the frame-pointer (x29) chain: [fp]=caller fp, [fp+8]=lr.
   * Bound every dereference to the thread's mapped stack so a wild fp can't fault
   * the watchdog itself. */
  uint64_t slo = 0, shi = 0;
  { MemoryInfo mi; u32 pi;
    if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, ctx->sp)) && mi.size) { slo = mi.addr; shi = mi.addr + mi.size; } }
  uint64_t fp = ctx->fp;
  for (int depth = 0; depth < 32 && (fp & 7) == 0; depth++) {
    if (slo) { if (fp < slo || fp + 16 > shi) break; }     /* stay in mapped stack */
    else if (fp < 0x1000) break;                            /* query failed: loose guard */
    const uint64_t nextfp = ((const uint64_t *)(uintptr_t)fp)[0];
    const uint64_t lr     = ((const uint64_t *)(uintptr_t)fp)[1];
    if (!lr) break;
    char s[40]; resolve_addr(s, sizeof s, lr);
    stallPrintf("[wd]     bt[%d] %s\n", depth, s);
    if (nextfp <= fp) break;   /* fp must climb up the stack */
    fp = nextfp;
  }
  /* Unity's hand-written wait stubs clobber the FP chain, so the bt[] above
   * often dead-ends in our glue. Raw-scan the top of the stack for any slot that
   * points into libunity / libil2cpp code -- those are return addresses the FP
   * walk missed, and they reveal what the thread is actually wedged inside.
   * Caller gates this (main/loader threads only) to keep the log readable. */
  if (g_scan_stack && slo) {
    uint64_t sp = ctx->sp & ~7ull;
    if (sp < slo) sp = slo;
    uint64_t top = sp + 0x2000;            /* ~1024 slots is plenty for the active frames */
    if (top > shi) top = shi;
    int printed = 0;
    for (uint64_t addr = sp; addr + 8 <= top && printed < 24; addr += 8) {
      uint64_t v = ((const uint64_t *)(uintptr_t)addr)[0];
      so_module *m = so_find_module_by_addr((const void *)(uintptr_t)v);
      if (!m) continue;
      if (!strstr(m->name, "unity") && !strstr(m->name, "il2cpp")) continue;  /* skip glue/main */
      /* A real return address points to the instruction *after* a call, so the
       * 4 bytes at v-4 must be BL (0b100101 imm26) or BLR (0xD63F0000 mask).
       * Without this check the scan reports jump-table targets, vtable pointers
       * and stale frames -- all of which look like code addresses but are NOT on
       * the live call chain. This filter is what makes the backtrace trustworthy. */
      uint32_t prev = ((const uint32_t *)(uintptr_t)(v - 4))[0];
      int is_bl  = (prev & 0xFC000000u) == 0x94000000u;
      int is_blr = (prev & 0xFFFFFC1Fu) == 0xD63F0000u;
      if (!is_bl && !is_blr) continue;
      char s[48]; resolve_addr(s, sizeof s, v);
      stallPrintf("[wd]     ret@0x%-4llx %s%s\n", (unsigned long long)(addr - sp), s,
                  is_blr ? " (via blr)" : "");
      printed++;
    }
  }
}

/* Pause just long enough to snapshot, RESUME before printing (so the watchdog
 * can't deadlock on a stdio/heap lock the paused thread was holding). */
__attribute__((unused))
static void snapshot_thread(DiagThread *t) {
  if (!t->handle || t->handle == threadGetCurHandle()) return;
  ThreadContext ctx;
  Result pr = svcSetThreadActivity(t->handle, ThreadActivity_Paused);
  Result gr = R_SUCCEEDED(pr) ? svcGetThreadContext3(&ctx, t->handle) : pr;
  if (R_SUCCEEDED(pr)) svcSetThreadActivity(t->handle, ThreadActivity_Runnable);
  if (R_FAILED(gr)) { stallPrintf("[wd]   %-16s (snapshot failed rc=0x%x)\n", t->name, gr); return; }
  /* Scan the stack only for the threads whose wait we actually need to diagnose:
   * the main render/UI thread and the async loaders. */
  g_scan_stack = (strstr(t->name, "Main") || strstr(t->name, "Preload") ||
                  strstr(t->name, "AsyncRead") || t->is_main_engine) ? 1 : 0;
  dump_thread_context(t->name[0] ? t->name : "?", &ctx);
  g_scan_stack = 0;
}

static void dump_threads(int episode, uint64_t now) {
  /* Announce entry before touching anything. dump_threads walks other threads'
   * contexts and stacks; a bad pointer there kills this thread outright, and
   * that is what happened -- stall.log held one beacon and then nothing, with no
   * stall header, so the crash was inside the dump rather than the detector. */
  stallPrintf("[wd] dump_threads: entered\n");
  uint64_t stalled_ns = tick_to_ns(now - g_last_progress);
  stallPrintf("\n[wd] ===== STALL #%d : no frame progress for %llu.%llus (last frame=%d) =====\n",
              episode, (unsigned long long)(stalled_ns / 1000000000ull),
              (unsigned long long)((stalled_ns % 1000000000ull) / 100000000ull), g_frame);
  stallPrintf("[wd] %-16s %-10s %-11s %-18s %7s  d_wait d_wake d_spin\n",
              "name", "tid", "state", "wait_obj", "secs");
  for (int i = 0; i < DIAG_MAX_THREADS; i++) {
    DiagThread *t = &g_threads[i];
    if (!t->in_use) continue;
    int kind = t->wait_kind;
    uint64_t since = t->wait_since;
    uint64_t parked_ns = (kind != DIAG_W_NONE && since) ? tick_to_ns(now - since) : 0;
    uint64_t dwait = t->waits_total - prev_waits[i];
    uint64_t dwake = t->wakes_total - prev_wakes[i];
    uint64_t dspin = t->futex_spins - prev_spins[i];
    prev_waits[i] = t->waits_total;
    prev_wakes[i] = t->wakes_total;
    prev_spins[i] = t->futex_spins;
    stallPrintf("[wd] %-16s %-10llu %-11s 0x%-16llx %3llu.%llu  %6llu %6llu %6llu%s\n",
                t->name[0] ? t->name : "?",
                (unsigned long long)t->tid,
                wait_kind_name(kind),
                (unsigned long long)(uintptr_t)t->wait_obj,
                (unsigned long long)(parked_ns / 1000000000ull),
                (unsigned long long)((parked_ns % 1000000000ull) / 100000000ull),
                (unsigned long long)dwait, (unsigned long long)dwake,
                (unsigned long long)dspin,
                t->is_main_engine ? "  <engine_main>" : "");
  }
  stallPrintf("[wd] legend: d_* = delta since previous dump (0/0/0 == hard-parked; "
              "d_spin>0 == alive on futex; d_wait>d_wake == entered a wait it hasn't left)\n");
  {
    int inc = -1;
    if (g_il2cpp_base) inc = (int)*(volatile uint32_t *)(g_il2cpp_base + GC_INCREMENTAL_OFF);
    stallPrintf("[wd] gc: incremental=%d susp_calls=%llu sp_set=%llu ctx_fail=%llu paused_now=%d\n",
                inc, (unsigned long long)g_gc_susp_calls, (unsigned long long)g_gc_sp_set,
                (unsigned long long)g_gc_ctx_fail, (int)g_gc_paused_now);
  }
#if DIAG_WD_INVASIVE
  /* native backtrace: where each thread is wedged inside libunity/il2cpp/NRO.
   * PAUSES each thread -- can deadlock a live scene load, so off by default. */
  stallPrintf("[wd] --- thread CPU contexts (frame-pointer backtrace) ---\n");
  for (int i = 0; i < DIAG_MAX_THREADS; i++) {
    if (g_threads[i].in_use) snapshot_thread(&g_threads[i]);
  }
#else
  stallPrintf("[wd] (CPU-context backtraces suppressed: non-invasive watchdog -- "
              "pausing threads mid-load deadlocked scene integration)\n");
#endif
  stallPrintf("\n");
}

static void watchdog_main(void *unused) {
  (void)unused;
  /* Own bionic TLS block. Every other thread that can reach engine-adjacent code
   * has one (main, clock, audio, the pthread shim); this thread never did. A
   * stack-protector prologue anywhere it reaches would read TPIDR_EL0+0x28 with
   * TPIDR unset and fault -- killing the watchdog exactly when it fires, which
   * matches the observed behaviour: one beacon, then silence, no stall header. */
  static uint8_t wd_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(wd_tls);
  int episode = 0;
  uint64_t last_dump = 0;
  /* prime so we don't false-trigger before the first frame */
  if (g_last_progress == 0) g_last_progress = now_tick();
  for (;;) {
    svcSleepThread(DIAG_POLL_NS);
    uint64_t now = now_tick();
    uint64_t idle = now - g_last_progress;

    /* Liveness beacon. Every diagnostic in this port has failed silently at
     * least once; a periodic "I am alive and here is the frame counter" line
     * makes the NEXT failure distinguishable from a dead watchdog thread. */
    {
      static uint64_t last_beat;
      if (tick_to_ns(now - last_beat) >= 2000000000ull) {
        last_beat = now;
        stallPrintf("[wd] alive: frame=%d idle=%llums%s\n",
                    g_frame, (unsigned long long)(tick_to_ns(idle) / 1000000ull),
                    tick_to_ns(idle) >= DIAG_STALL_NS ? "  STALLED" : "");
      }
    }

    if (tick_to_ns(idle) >= DIAG_STALL_NS) {
      if (last_dump == 0 || tick_to_ns(now - last_dump) >= DIAG_REDUMP_NS) {
        dump_threads(++episode, now);
        last_dump = now;
      }
    } else {
      /* progress resumed: reset so a later stall dumps fresh */
      last_dump = 0;
    }
  }
}

void diag_watchdog_start(void) {
  if (g_wd_started) return;
  g_wd_started = 1;
  nro_range_init();
  if (g_last_progress == 0) g_last_progress = now_tick();
  /* libnx thread: deliberately NOT via the pthread shim under test.
   * 16 KiB stack, priority 0x2C (same band as main), default core. */
  /* Try to outrank the engine, but NEVER fail closed.
   *
   * The failure being chased is a 100% CPU spin, and at equal priority (0x2C)
   * the watchdog is simply never scheduled against it -- two beacons then
   * silence, every run. But a blind jump to 0x18 was refused outright
   * (rc=0xe001, invalid priority: hbloader only permits a narrow band around
   * its own 0x2C) and the watchdog did not start AT ALL, which is strictly
   * worse than being starved.
   *
   * So: walk from the most useful priority down to the known-good one and take
   * the first that works. A modest bump still wins the scheduler against a
   * same-priority spinner. */
  static const int PRIOS[] = { 0x28, 0x2A, 0x2B, 0x2C };
  Result rc = 0xe001;
  int used = 0;
  for (unsigned i = 0; i < sizeof(PRIOS) / sizeof(PRIOS[0]); i++) {
    /* core 2 explicitly, not -2 (= "default"). The engine's main thread and its
     * job workers saturate the default core; giving the watchdog its own means
     * a spinner cannot hide from it even at equal priority. */
    rc = threadCreate(&g_wd_thread, watchdog_main, NULL, NULL, 0x4000, PRIOS[i], 2);
    if (R_FAILED(rc))   /* core 2 unavailable in this configuration */
      rc = threadCreate(&g_wd_thread, watchdog_main, NULL, NULL, 0x4000, PRIOS[i], -2);
    if (R_SUCCEEDED(rc)) { used = PRIOS[i]; break; }
  }
  if (R_SUCCEEDED(rc) && R_SUCCEEDED(threadStart(&g_wd_thread)))
    stallPrintf("[wd] watchdog armed (stall=%llus, poll=1s, prio=0x%02x)\n",
                (unsigned long long)(DIAG_STALL_NS / 1000000000ull), used);
  else
    stallPrintf("[wd] watchdog FAILED to start rc=0x%x\n", rc);
}
