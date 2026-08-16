/* zombotron_patches.c -- Unity 6000.2.6f2 engine patches for Zombotron on Switch.
 *
 * Three patches, all fail-safe:
 *
 *   [G'] ChoreographerBase::Get() -> NULL
 *        Unity 6 blocks the frame loop on a Choreographer frame callback that no
 *        Switch will ever deliver. Returning NULL makes the engine take its
 *        no-choreographer path and free-run on our clock. This function does not
 *        exist in 2020.3, which is why badpiggies_nx has no equivalent patch.
 *
 *   [A]  AndroidAudio::GetAndroidAudioOutputType() -> 2  (OpenSL ES)
 *        Left alone it returns 1 (AudioTrack) because the "OpenSL available"
 *        decision is gated on real AudioManager capability flags our faked JNI
 *        never sets -- and AudioTrack needs a Java layer we do not have, so the
 *        game would run silent. Forcing 2 makes GetPlatformOutputOverride select
 *        FMOD_OUTPUTTYPE 22, which opensles.c already implements over SDL2.
 *
 *   [G]  TimeManager::Update(double) -> detour
 *        Substitutes a monotonic wall clock for the newTime the Java side would
 *        normally supply, so deltaTime is real, animations run, and async loads
 *        complete. A background thread keeps stepping the clock while the main
 *        thread is stuck inside a synchronous scene load.
 *
 * EVERY installer verifies the located address still starts with the opcode the
 * fingerprint promised before it writes anything. Guard mismatch -> skip + log.
 * Unresolved address (0) -> skip + log. There is no path that half-patches.
 *
 * MIT.
 */
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <switch.h>

#include "so_util.h"
#include "util.h"
#include "zombotron_offsets.h"
#include "zombotron_locate.h"
#include "zombotron_region_patch.h"
#include "config.h"

/* Tripwire -- see the note in zombotron_il2cpp.c. A feature flag whose header is
 * not included evaluates to 0 and silently removes the feature, with no warning
 * and no log line. That has now happened twice in this port; it does not get to
 * happen a third time. */
#ifndef ZB_PATCH_FMOD_BUFFER_GEOMETRY
#error "config.h not included -- feature flags in this file would compile out silently"
#endif

#define A64_RET        0xd65f03c0u

/* ------------------------------------------------------------------ clock */

static uint64_t nx_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Direct entry into TimeManager::Update's BODY.
 *
 * In Unity 6 the entry (10 instructions) only bumps two counters and checks the
 * pause flag; the body at +0x24 opens with `sub sp, sp, #0xe0` and builds its own
 * register-save frame. So it is directly callable as a normal function.
 *
 * badpiggies_nx needs an assembly trampoline here ONLY because Unity 2020.3 built
 * that frame in the entry prologue instead, leaving the body frame-incorrect to
 * call. Do not port bp_tm_trampoline.s over -- it would build a second frame on
 * top of the one this body builds for itself. */
typedef void (*fn_tm_body)(void *tm, double newTime);
static fn_tm_body g_tm_body = NULL;

static Mutex   g_clock_lock;
static void   *g_tm = NULL;            /* captured TimeManager instance */
static uint64_t g_clk_base_ns = 0;
static double   g_last_engine_time = 0.0;  /* last time fed to the engine */
static volatile uint64_t g_last_main_tick_ns = 0;
static volatile int g_clock_thread_run = 0;
static Thread g_clock_thread;
static uint8_t g_clock_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));

/* Android vsync counter: engine waits on it, nothing on Switch advances it. */
static volatile uint64_t *g_vsync_counter = NULL;
static uint64_t g_vsync_last_ns = 0;

/* Verify the WaitVSync loop really is where the derivation said before handing
 * a background thread a pointer it will write to 60 times a second. Two words
 * checked, not one: the adrp that forms the mutex and the ldr that forms the
 * counter -- the ldr is the instruction the counter offset was decoded from, so
 * if it matches, the offset is right by construction. */
static int zb_bind_vsync_counter(so_module *unity) {
  uintptr_t base = (uintptr_t)unity->load_virtbase;
  uint32_t w0 = *(volatile uint32_t *)(base + GAME_RVA_WaitVSync);
  uint32_t w1 = *(volatile uint32_t *)(base + GAME_RVA_WaitVSync_ldr);
  if (w0 != GUARD_WaitVSync || w1 != GUARD_WaitVSync_ldr) {
    debugPrintf("[vsync] ABORT: WaitVSync guards %08x/%08x (want %08x/%08x) -- "
                "counter NOT bound, engine will wait forever\n",
                w0, w1, GUARD_WaitVSync, GUARD_WaitVSync_ldr);
    return 0;
  }
  g_vsync_counter = (volatile uint64_t *)(base + GAME_RVA_VSYNC_COUNTER);
  debugPrintf("[vsync] counter bound @ %p (libunity+0x%x), bumping at ~60Hz\n",
              (void *)g_vsync_counter, (unsigned)GAME_RVA_VSYNC_COUNTER);
  return 1;
}

#define CLOCK_STALL_NS 100000000ull    /* 100 ms of main-thread silence == stalled */
#define CLOCK_TICK_NS   16666667ull    /* ~60 Hz */

/* Step the engine clock once. Caller must hold g_clock_lock. */
static void zb_clock_step(void *tm) {
  uint64_t now = nx_now_ns();
  if (!g_clk_base_ns)
    g_clk_base_ns = now;
  double t = (double)(now - g_clk_base_ns) / 1e9;
  g_last_engine_time = t;
  g_tm_body(tm, t);
}

/* Detour for TimeManager::Update.
 *
 * We jump past the entry, so we owe the engine the entry's own side effects --
 * both counter bumps and the pause short-circuit -- before calling the body.
 * Skipping frameCount would freeze Time.frameCount and anything keyed off it;
 * skipping the pause check would keep stepping time through a pause. */
static void zb_tm_update_hook(void *tm, double newTime) {
  (void)newTime;   /* the Java-supplied time is meaningless here */

  uint64_t *frame_count  = (uint64_t *)((uintptr_t)tm + TM_FIELD_FRAMECOUNT);
  uint32_t *render_count = (uint32_t *)((uintptr_t)tm + TM_FIELD_RENDERCOUNT);
  uint8_t  *paused       = (uint8_t  *)((uintptr_t)tm + TM_FIELD_PAUSE);

  (*frame_count)++;
  (*render_count)++;
  if (*paused)
    return;

  mutexLock(&g_clock_lock);
  g_tm = tm;                       /* let the stall thread find it */
  g_last_main_tick_ns = nx_now_ns();
  zb_clock_step(tm);
  mutexUnlock(&g_clock_lock);
}

/* Keeps time moving when nativeRender is blocked inside a synchronous load.
 * Without this the engine's own loading coroutines never observe elapsed time
 * and a scene load can wedge permanently. */
static void zb_clock_thread_fn(void *arg) {
  (void)arg;
  /* Engine prologues read their stack canary from TPIDR_EL0+0x28. Any thread
   * that re-enters engine code needs its own bionic TLS block, or the very
   * first guarded function faults. ChoreographerBase::Get() is one such
   * prologue -- see its `mrs x19, tpidr_el0` / `ldr x8, [x19, #0x28]`. */
  install_bionic_tls(g_clock_tls);

  while (g_clock_thread_run) {
    svcSleepThread(CLOCK_TICK_NS);

    /* Advance the vsync counter in wall-clock steps. A catch-up loop rather than
     * a single increment, so a late tick does not permanently lose frames; but
     * strictly time-based, never "bump once per iteration" -- running ahead of
     * the compositor floods vi and takes the system down. */
    if (g_vsync_counter) {
      uint64_t now = nx_now_ns();
      if (!g_vsync_last_ns) g_vsync_last_ns = now;
      while ((int64_t)(now - g_vsync_last_ns) >= (int64_t)VSYNC_PERIOD_NS) {
        __atomic_add_fetch(g_vsync_counter, 1, __ATOMIC_RELAXED);
        g_vsync_last_ns += VSYNC_PERIOD_NS;
      }
    }
    uint64_t now = nx_now_ns();
    if (!g_tm || now - g_last_main_tick_ns < CLOCK_STALL_NS)
      continue;
    if (mutexTryLock(&g_clock_lock)) {
      if (g_tm)
        zb_clock_step(g_tm);
      mutexUnlock(&g_clock_lock);
    }
  }
}

/* Exposed so the render loop can report engine-side clock state. */
void  *zb_clock_tm(void)        { return g_tm; }
double zb_clock_last_time(void) { return g_last_engine_time; }

/* ---------------------------------------------------------------- helpers */

/* Verify the located address still begins with the opcode the fingerprint
 * matched. Cheap, but it is the difference between "we patched the function we
 * meant to" and "we corrupted whatever happened to be there". */
static int guard_ok(const char *what, uintptr_t addr, uint32_t guard) {
  if (!addr) {
    debugPrintf("[patch] %-28s SKIP (unresolved)\n", what);
    return 0;
  }
  uint32_t got = *(volatile uint32_t *)addr;
  if (got != guard) {
    debugPrintf("[patch] %-28s SKIP (guard 0x%08x, found 0x%08x)\n",
                what, guard, got);
    return 0;
  }
  return 1;
}

static int patch_return_const(const char *what, int target_id,
                              const uint32_t *stub, size_t stub_words) {
  uintptr_t addr = zb_addr(target_id);
  const fp_target *tg = &ZOMBOTRON_TARGETS[target_id];
  if (!guard_ok(what, addr, tg->guard))
    return 0;
  if (so_patch_code((void *)addr, stub, stub_words * 4) < 0) {
    debugPrintf("[patch] %-28s FAILED (so_patch_code)\n", what);
    return 0;
  }
  debugPrintf("[patch] %-28s applied @ %p\n", what, (void *)addr);
  return 1;
}

/* ------------------------------------------------------------------ install */

/* 256MB -> 64MB region granularity. ALL-OR-NOTHING and VERIFY-FIRST: two of the
 * ten sites (GetMemoryBlockFromPointer / MarkMemoryBlocks) index the same block
 * table via >>28, so a partial application corrupts the allocator's bookkeeping
 * rather than merely failing to help. Returns 1 if applied. */
int zb_install_region_patch(so_module *unity) {
  uintptr_t base = (uintptr_t)unity->load_virtbase;

  for (size_t i = 0; i < ZB_REGION_WORDS_N; i++) {
    const NxPatchWord *p = &ZB_REGION_WORDS[i];
    uint32_t got = *(volatile uint32_t *)(base + p->off);
    if (got != p->from) {
      debugPrintf("[region] ABORT: site %u @+0x%06x expected %08x, found %08x "
                  "-- NOTHING patched, engine runs stock\n",
                  (unsigned)i, p->off, p->from, got);
      return 0;
    }
  }
  for (size_t i = 0; i < ZB_REGION_WORDS_N; i++) {
    const NxPatchWord *p = &ZB_REGION_WORDS[i];
    uint32_t w = p->to;
    if (so_patch_code((void *)(base + p->off), &w, 4) < 0) {
      debugPrintf("[region] FAILED writing site %u @+0x%06x\n",
                  (unsigned)i, p->off);
      return 0;
    }
  }
  debugPrintf("[region] %u/%u sites patched: region granularity 256MB -> %dMB\n",
              (unsigned)ZB_REGION_WORDS_N, (unsigned)ZB_REGION_WORDS_N,
              ZB_REGION_GRANULARITY_MB);
  return 1;
}

int zb_install_patches(so_module *unity) {
  int applied = 0;

  mutexInit(&g_clock_lock);
  zb_bind_vsync_counter(unity);

  /* [G'] ChoreographerBase::Get() -> NULL */
  {
    static const uint32_t stub[] = PATCH_RET_NULL;
    applied += patch_return_const("ChoreographerBase::Get", ZB_CHOREO_GET,
                                  stub, sizeof(stub) / 4);
  }

  /* [A] GetAndroidAudioOutputType() -> 2 (OpenSL ES) */
  {
    static const uint32_t stub[] = PATCH_RET_OPENSL;
    applied += patch_return_const("GetAndroidAudioOutputType", ZB_AUDIO_OUTTYPE,
                                  stub, sizeof(stub) / 4);
  }

  /* [L] SerializedFile's "script unknown or not yet loaded" warn call -> NOP.
   *
   * The one remaining frame-0/1 crash (pc=libunity+0x750f70, x0=0x5500000001,
   * far=0x5500000031, byte-identical on every occurrence, ~10-25% of boots) is
   * this call dereferencing a not-yet-remapped PPtr-style {fileID,pathID} pair
   * out of a 216-byte object-table entry while the initial scene is still
   * integrating. Proven GC-INDEPENDENT on rev77: it fired with susp_calls=0 --
   * no stop-the-world had ever run in that boot -- so this is a load-time race
   * in the game/loader timing, not collector behaviour; the GC only ever shifted
   * its odds.
   *
   * The callee (0x750f40) is purely diagnostic: it formats "script unknown or
   * not yet loaded" / "probably %s?" and logs. Disassembly shows no stores to
   * game state; the caller continues at 0x750edc identically after the call, and
   * x0-x3 are dead there. NOPping the call is therefore ABI-clean and costs only
   * a console warning. Only THIS site is patched (every crash dump has
   * lr=0x750edc); the sibling call at +0x750dfc reads a different, stack-held
   * pointer that has never been observed poisoned. */
  {
    const uint32_t soff = 0x750ed8u;        /* bl 0x750f40 (script-warn)        */
    const uint32_t want = 0x9400001au;      /* exact bl encoding in this build  */
    const uint32_t nopw = 0xd503201fu;
    uintptr_t base = (uintptr_t)unity->load_virtbase;
    uint32_t got = *(volatile uint32_t *)(base + soff);
    if (got == nopw) {
      debugPrintf("[patch] script-warn NOP              already applied\n");
      applied++;
    } else if (got != want) {
      debugPrintf("[patch] script-warn NOP              SKIP: @+0x%06x expected "
                  "%08x found %08x -- different libunity build?\n",
                  soff, want, got);
    } else if (so_patch_code((void *)(base + soff), &nopw, 4) < 0) {
      debugPrintf("[patch] script-warn NOP              FAILED (so_patch_code)\n");
    } else {
      debugPrintf("[patch] script-warn NOP              applied @+0x%06x "
                  "(removes the frame-0 script-resolution crash)\n", soff);
      applied++;
    }
  }

#if ZB_PATCH_FMOD_BUFFER_GEOMETRY
  /* [A] FMOD's OpenSL buffer-geometry bound check -> unconditional pass.
   *
   * Forcing the output type to OpenSL above is only half the job: FMOD then
   * validates the output period against the game's BAKED DSP buffer settings and
   * refuses the device with error 60 if the period does not fit
   * (dspNumBuffers-1)*dspBufferLength, even after halving it once. With a baked
   * dspNumBuffers of 1 that bound is 0 and NO period can satisfy it, so no value
   * we report from the fake AudioManager can rescue it -- the branch itself has
   * to go. Rewriting `b.ls +0x10` to `b +0x10` keeps the identical target and
   * simply always takes the success path, which then builds the player from the
   * sane sample rate and channel count. This is pvz_fusion's fix (PORTING.md
   * sec 3a) at this build's address.
   *
   * TWO guards, not one: the locator already matched the eight-word run, and
   * this additionally checks that the word actually being overwritten is a real
   * `b.ls +0x10`. Matching a run is not by itself permission to write into it,
   * and this is a bare branch rewrite with no ABI to fall back on. */
  {
    uintptr_t run = zb_addr(ZB_FMOD_BUFGEOM);
    if (!run) {
      debugPrintf("[fmod] SKIP buffer-geometry bypass: run UNRESOLVED -- "
                  "audio stays silent (FMOD error 60)\n");
    } else {
      uintptr_t br = run + FMOD_BUFGEOM_BRANCH_OFF;
      uint32_t got = *(volatile uint32_t *)br;
      if (got != GUARD_FMOD_BUFGEOM_BLS) {
        debugPrintf("[fmod] SKIP buffer-geometry bypass: +0x%x = %08x, not "
                    "`b.ls +0x10` (%08x) -- libunity differs, audio stays "
                    "silent\n", (unsigned)FMOD_BUFGEOM_BRANCH_OFF, got,
                    GUARD_FMOD_BUFGEOM_BLS);
      } else {
        static const uint32_t b_uncond = PATCH_FMOD_BUFGEOM_B;
        if (so_patch_code((void *)br, &b_uncond, sizeof b_uncond) < 0) {
          debugPrintf("[fmod] buffer-geometry bypass FAILED (so_patch_code)\n");
        } else {
          debugPrintf("[patch] %-28s applied @ %p (b.ls -> b)\n",
                      "FMOD OpenSL buffer geometry", (void *)br);
          applied++;
        }
        /* Force the OpenSL period FIRST -- this is the one that matters. FMOD
         * was being asked for periods larger than its whole DSP pool, so the
         * buffers it enqueued were part fresh audio and part stale ring. Two
         * words, guarded independently of the run match. */
        {
          uintptr_t ld = run + FMOD_PERIOD_LDR_OFF;
          uintptr_t cz = run + FMOD_PERIOD_CBZ_OFF;
          uint32_t lw = *(volatile uint32_t *)ld, cw = *(volatile uint32_t *)cz;
          if (lw != GUARD_FMOD_PERIOD_LDR || cw != GUARD_FMOD_PERIOD_CBZ) {
            debugPrintf("[fmod] SKIP period force: %08x/%08x, want %08x/%08x -- "
                        "period left as FMOD chose it (expect clicking)\n",
                        lw, cw, GUARD_FMOD_PERIOD_LDR, GUARD_FMOD_PERIOD_CBZ);
          } else {
            static const uint32_t mv = PATCH_FMOD_PERIOD_MOV;
            static const uint32_t st = PATCH_FMOD_PERIOD_STR;
            if (so_patch_code((void *)ld, &mv, sizeof mv) < 0 ||
                so_patch_code((void *)cz, &st, sizeof st) < 0) {
              debugPrintf("[fmod] period force FAILED (so_patch_code)\n");
            } else {
              debugPrintf("[patch] %-28s applied @ %p (period -> %d frames)\n",
                          "FMOD OpenSL period", (void *)ld,
                          ZB_AUDIO_PERIOD_FRAMES);
              applied++;
            }
          }
        }

        /* Optional: force the up-front buffer count. Getting past the bound
         * check is worthless if FMOD then sizes the queue to zero. Guarded
         * independently -- matching the run is not permission to write to a
         * second site within it. */
#if ZB_AUDIO_UPFRONT_BUFFERS
        uintptr_t dv = run + FMOD_BUFCOUNT_OFF;
        uint32_t dvw = *(volatile uint32_t *)dv;
        if (dvw != GUARD_FMOD_BUFCOUNT) {
          debugPrintf("[fmod] SKIP buffer-count force: +0x%x = %08x, not `udiv "
                      "w9,w10,w9` (%08x) -- FMOD may enqueue 0 buffers and play "
                      "silence\n", (unsigned)FMOD_BUFCOUNT_OFF, dvw,
                      GUARD_FMOD_BUFCOUNT);
        } else {
          static const uint32_t movz = PATCH_FMOD_BUFCOUNT;
          if (so_patch_code((void *)dv, &movz, sizeof movz) < 0) {
            debugPrintf("[fmod] buffer-count force FAILED (so_patch_code)\n");
          } else {
            debugPrintf("[patch] %-28s applied @ %p (udiv -> mov w9,#%d)\n",
                        "FMOD up-front buffer count", (void *)dv,
                        ZB_AUDIO_UPFRONT_BUFFERS);
            applied++;
          }
        }
#else
        debugPrintf("[fmod] buffer count left to FMOD's own division "
                    "(ZB_AUDIO_UPFRONT_BUFFERS=0) -- correct once the period "
                    "divides into the DSP pool\n");
#endif
      }
    }
  }
#else
  debugPrintf("[fmod] buffer-geometry bypass DISABLED "
              "(ZB_PATCH_FMOD_BUFFER_GEOMETRY=0) -- audio will fail with "
              "error 60 again; this is a bisect switch, not a fix\n");
#endif

  /* [G] TimeManager::Update -> detour.
   *
   * hook_arm64 writes a 16-byte detour (ldr x17 / br x17 / addr64) over the
   * entry. The body sits at +0x24, clear of those 16 bytes, so it survives
   * intact and stays callable. If TM_BODY_DELTA were ever <= 0x10 this would
   * overwrite the body -- hence the static assert. */
  {
    const fp_target *tg = &ZOMBOTRON_TARGETS[ZB_TM_UPDATE];
    uintptr_t addr = zb_addr(ZB_TM_UPDATE);
    if (guard_ok("TimeManager::Update", addr, tg->guard)) {
      _Static_assert(TM_BODY_DELTA > 16,
                     "detour would overwrite TimeManager::Update's body");
      g_tm_body = (fn_tm_body)(addr + TM_BODY_DELTA);
      hook_arm64(addr, (uintptr_t)&zb_tm_update_hook);
      debugPrintf("[patch] %-28s hooked @ %p (body %p)\n",
                  "TimeManager::Update", (void *)addr, (void *)g_tm_body);

      applied++;
    }
  }

  /* Start the clock thread UNCONDITIONALLY. It has two jobs and only one of them
   * depends on the TimeManager hook:
   *   - step the engine clock while the main thread is parked (needs g_tm, which
   *     the hook supplies -- skipped harmlessly if the hook did not apply);
   *   - bump the Android vsync counter, which the engine blocks on and which has
   *     nothing to do with TimeManager.
   * Nesting the thread start inside the TimeManager patch would have meant a
   * skipped clock patch silently disabling the vsync fix too, and the symptom of
   * that (engine waits forever at the first real frame) looks nothing like
   * "a patch was skipped". */
  g_clock_thread_run = 1;
  if (R_SUCCEEDED(threadCreate(&g_clock_thread, zb_clock_thread_fn, NULL,
                               NULL, 0x4000, 0x2C, -2)))
    threadStart(&g_clock_thread);
  else
    debugPrintf("[patch] clock thread FAILED to start "
                "(vsync counter will not advance -- engine will hang)\n");

  debugPrintf("[patch] %d/6 engine patches applied\n", applied);
  return applied;
}

void zb_shutdown_patches(void) {
  if (!g_clock_thread_run)
    return;
  g_clock_thread_run = 0;
  threadWaitForExit(&g_clock_thread);
  threadClose(&g_clock_thread);
}
