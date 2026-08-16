/* main.c -- Zombotron Nintendo Switch wrapper entry point.
 *
 * Unity 6000.2.6f2 / IL2CPP, arm64-v8a. Loads libmain + libunity + libil2cpp +
 * lib_burst_generated, resolves their imports against native Switch
 * implementations, and drives the lifecycle the Java UnityPlayer normally runs.
 * The UnityPlayer natives are recovered BY NAME from libunity's RegisterNatives
 * table (jni_fake.c); the RVAs in zombotron_entrypoints.h only cross-check that.
 *
 * Forked from badpiggies_nx / colorsheep_nx / laytonbmr_nx / vln_nx (MIT). The
 * memory / heap / overcommit / crash-handler scaffolding above main() is
 * inherited verbatim -- it is engine-generation-generic. Everything Zombotron
 * and Unity 6 specific lives below the banner comment in main(), and in the
 * zombotron_*.c units:
 *
 *   zombotron_locate.c    find the engine functions we patch, at runtime
 *   zombotron_patches.c   choreographer / audio-output / TimeManager clock
 *   zombotron_boot.c      the Unity 6 lifecycle + render loop
 *   zombotron_imports.c   the 69 media-NDK imports Unity 6 adds
 *   zombotron_input.c     HID sampling (event injection NOT yet implemented)
 *
 * Deltas vs the 2020.3 reference this was forked from -- each one fails silently
 * rather than loudly if you get it wrong:
 *   - initJni is 4-ARG (env, thiz, Context, int), not 3-arg;
 *   - nativeUnityPlayerSetRunning(bool) exists and gates the frame loop;
 *   - a fourth module, lib_burst_generated.so, is loaded (optional);
 *   - libswappywrapper / libvulkan / libaaudio are refused at dlopen;
 *   - engine patch sites are located by fingerprint, not hard-coded RVA;
 *   - the region-granularity 256MB->64MB table is NOT derived for Unity 6
 *     (see PORT_PLAN milestone [R]) -- 8GB consoles are the practical target.
 */
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <sys/stat.h>
#include <switch.h>
#include <SDL2/SDL.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "android_native_unity.h"
#include "opensles.h"
#include "zombotron_entrypoints.h"
#include "diag.h"
#include "zombotron_locate.h"
#include "zombotron_root.h"
#include "asset_pack.h"
#include "zombotron_il2cpp.h"
#include "zombotron_video.h"
#include "zombotron_time.h"
void zb_gpua_enable(void);   /* zombotron_gpuarena.c */
void zb_cbig_enable(void);   /* zombotron_gpuarena.c -- dedicated CPU big-alloc reserve */
#include "zombotron_touchhook.h"
#include "zombotron_screen.h"
#include "zombotron_offsets.h"

/* Resolved at runtime from the .nro's own location -- see zombotron_root.c.
 * Compiling the folder name in meant the SD directory had to be named exactly
 * "zombotron_nx" or check_data() reported libmain.so missing while it sat right
 * there, and debug.log went to the same nonexistent folder so there was nothing
 * to diagnose with. */
#define DATA_ROOT  zb_game_root()

/* Progress callback the asset-pack build/verify calls; log it so the (multi-minute)
 * first-boot pack build is visible in debug.log instead of looking like a hang. */
void startup_status_update(const char *message) {
  debugPrintf("[pack] %s\n", message ? message : "");
}
#define LIB_MAIN   "libmain.so"
#define LIB_UNITY  "libunity.so"
#define LIB_IL2CPP "libil2cpp.so"

/* jni_fake.c: recover a UnityPlayer native by name from the captured RegisterNatives table */
void *jni_lookup_unity_native(const char *name);
/* unity_glue.c */
void unity_environment_init(const char *data_root);

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

/* mmap arena (consumed by mmap_fake/munmap_fake in libc_shim.c). */
void  *g_mmap_arena_base = NULL;
size_t g_mmap_arena_size = 0;
int    g_overcommit      = 0;
u64    g_alias_base = 0, g_alias_size = 0;
unsigned g_oc_heap_mb = 0, g_oc_freed_mb = 0;
int      g_oc_hint_map = 0, g_oc_hint_unmap = 0;
unsigned g_oc_alias_mb = 0;
void    *g_oc_win = NULL;
int      g_oc_probe_tried = 0, g_oc_shrink_tried = 0;
extern int oc_arena_init(void *window, size_t window_bytes, void *pool, size_t pool_bytes);
extern void oc_precommit(size_t bytes);
unsigned g_oc_probe_rc = 0, g_oc_shrink_rc = 0;
unsigned long g_oc_win_addr = 0;
u64      g_oc_sysres = 0;
/* [heapfix] result of the boot-time heap-shrink, logged from main() once the log
 * file exists (see __libnx_initheap). status: 0 not attempted, 1 success,
 * 2 base-mismatch (override != kernel heap, no change), 3 svcSetHeapSize failed. */
int      g_heapfix_status = 0;
unsigned g_heapfix_freed_mb = 0;
/* Kernel-heap base (from a side-effect-free svcGetInfo probe) vs the override
 * base we were handed. Under a forwarder these DIFFER: the override is a
 * separate mapping aliased from the kernel heap, so svcSetHeapSize resizes the
 * wrong region and frees the physical pages backing our mmap arena -- the arena
 * is then reserved-but-unbacked and Unity's asset GC data-aborts on the first
 * write into it at frame 0. We only ever shrink when kbase == obase. Set once in
 * __libnx_initheap; read by BOTH shrink sites (heapfix here + memshrink in main). */
u64      g_heapfix_kbase = 0;
u64      g_heapfix_obase = 0;

so_module main_mod, unity_mod, il2cpp_mod;

/* Strong override of nx_crash_handler.c's weak stub: name addresses that fall inside
 * our loaded .so images (creport can't, since they aren't real modules). */
int crash_resolve_module(uintptr_t addr, char *name_out, size_t name_cap, uintptr_t *base_out) {
  const struct { const char *n; so_module *m; } mods[] = {
    { "libmain.so", &main_mod }, { "libunity.so", &unity_mod }, { "libil2cpp.so", &il2cpp_mod },
  };
  for (unsigned i = 0; i < sizeof(mods)/sizeof(*mods); i++) {
    uintptr_t b = (uintptr_t)mods[i].m->load_virtbase;
    if (b && addr >= b && addr < b + mods[i].m->load_size) {
      snprintf(name_out, name_cap, "%s", mods[i].n);
      *base_out = b;
      return 1;
    }
  }
  return 0;
}

extern uintptr_t g_il2cpp_base;       /* libc_shim.c: GC stop-the-world bridge */
extern size_t    g_il2cpp_size;       /* libc_shim.c: bounds guard for the same */
extern void nx_sd_flush(void);        /* libc_shim.c: periodic SD commit       */

/* audio warmup gate for opensles.c (frames since boot) */
static volatile uint32_t g_frame_count = 0;
uint32_t port_frame_count(void) { return g_frame_count; }
/* The render loop lives in zombotron_boot.c, so it cannot touch the static
 * above directly; the crash handler reads it to report which frame died. */
void port_frame_tick(void) { g_frame_count++; }

/* Main-thread bionic TLS, shared with zombotron_boot.c so the engine entry can
 * re-assert it without reaching into main()'s locals. */
static void *g_main_tls;
void zb_set_main_tls(void *buf) { g_main_tls = buf; }
void zb_reassert_main_tls(void) { if (g_main_tls) install_bionic_tls(g_main_tls); }

/* Zombotron: libunity ~20.8M + libil2cpp ~60M + lib_burst + relocation headroom.
 * Zombotron sized this at 240M for a 48M libil2cpp; Zombotron's is 60M, so this
 * is bumped to 288M. If so_load(libil2cpp) still fails with OOM in the first
 * debug.log, this is the first knob -- it is carved from the newlib heap, so do
 * not raise it further than needed. */
#define SO_REGION_BYTES (288u * 1024 * 1024)

/* ==========================================================================
 * Inherited memory/heap/overcommit scaffolding (verbatim from the vln/cr3_nx
 * base, MIT). Engine-generation-generic; not Color-Sheep-specific.
 * ========================================================================== */
static void *oc_find_stack_window(size_t want, size_t *out_size) {
  *out_size = 0;
  u64 sbase = 0, ssize = 0;
  svcGetInfo(&sbase, InfoType_StackRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&ssize, InfoType_StackRegionSize,    CUR_PROCESS_HANDLE, 0);
  if (!sbase || !ssize) return NULL;
  u64 end = sbase + ssize, a = sbase, best_a = 0, best_l = 0;
  int holes = 0, mapped = 0;
  while (a < end) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) break;
    u64 ms = mi.addr, me = mi.addr + mi.size;
    if (me <= a) break;
    if (mi.type == MemType_Unmapped) {
      u64 hs = ms < sbase ? sbase : ms, he = me > end ? end : me;
      if (he > hs) {
        if (he - hs > best_l) { best_l = he - hs; best_a = hs; }
        if (holes < 8)
          debugPrintf("[oc] stack hole %d: %p .. %p (%u MB)\n",
                      holes++, (void *)hs, (void *)he, (unsigned)((he - hs) >> 20));
      }
    } else mapped++;
    a = me;
  }
  debugPrintf("[oc] stack scan: base=%p size=%u MB, %d holes, %d mapped spans, largest=%u MB\n",
              (void *)sbase, (unsigned)(ssize >> 20), holes, mapped, (unsigned)(best_l >> 20));
  if (!best_a) return NULL;
  u64 aligned = (best_a + (MMAP_ARENA_ALIGN - 1)) & ~(MMAP_ARENA_ALIGN - 1);
  if (aligned >= best_a + best_l) return NULL;
  u64 avail = ((best_a + best_l) - aligned) & ~(MMAP_ARENA_ALIGN - 1);
  if (!avail) return NULL;
  if (avail > want) avail = want;
  *out_size = avail;
  return (void *)aligned;
}

static int overcommit_setup(void *addr, size_t size, size_t so_zone,
                            void **out_addr, size_t *out_fake) {
  (void)addr; (void)size; (void)so_zone; (void)out_addr; (void)out_fake;
  g_oc_hint_map   = envIsSyscallHinted(0x2c);
  g_oc_hint_unmap = envIsSyscallHinted(0x2d);
  svcGetInfo(&g_alias_base, InfoType_AliasRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&g_alias_size, InfoType_AliasRegionSize,    CUR_PROCESS_HANDLE, 0);
  g_oc_alias_mb = (unsigned)(g_alias_size >> 20);
  svcGetInfo(&g_oc_sysres, InfoType_SystemResourceSizeTotal, CUR_PROCESS_HANDLE, 0);
  return 0;   /* no system resource -> svcMapPhysicalMemory unusable; heap-backed */
}

/* Captured in __libnx_initheap and logged from main(): the log file is not open
 * yet at heap-setup time, and the previous reserve attempt failed SILENTLY --
 * it edited a branch that never executes under hbmenu. Reporting the actual
 * outcome is the difference between "reserved" and "believed to have
 * reserved". */

void __libnx_initheap(void) {
  void *addr;
  size_t size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    /* Hold back GFX_RESERVE_MB for the graphics driver -- see config.h. The
     * stock 0x200000 (2 MB) margin starves switch-mesa, and a driver allocation
     * failing in the compositor path wedges the whole console. */
    const size_t gfx_reserve = (size_t)GFX_RESERVE_MB * 1024 * 1024;
    if (mem_available > mem_used + gfx_reserve)
      size = (mem_available - mem_used - gfx_reserve) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  const size_t MB = 1024 * 1024;
  size_t so_zone = SO_REGION_BYTES;
  if (so_zone > size / 2)
    so_zone = size / 2;

  extern char *fake_heap_start;
  extern char *fake_heap_end;

  void *oc_addr; size_t oc_fake;
  if (overcommit_setup(addr, size, so_zone, &oc_addr, &oc_fake)) {
    fake_heap_start = (char *)oc_addr;
    fake_heap_end   = (char *)oc_addr + oc_fake;
    heap_so_base    = (void *)ALIGN_MEM((uintptr_t)oc_addr + oc_fake, 0x1000);
    heap_so_limit   = so_zone;
    return;
  }

  const size_t big_align    = MMAP_ARENA_ALIGN;
  const size_t newlib_floor = 384 * MB;
  size_t arena_sz = MMAP_ARENA_RESERVE;
  size_t fake_heap_size;

  if (size > so_zone + big_align + newlib_floor + 256 * MB) {
    size_t avail = size - so_zone - big_align - newlib_floor;
    if (arena_sz > avail) arena_sz = avail & ~(big_align - 1);
    size_t usable    = size - so_zone - big_align;
    size_t arena_cap = ((usable * 30) / 100) & ~(big_align - 1);
    if (arena_sz > arena_cap) arena_sz = arena_cap;
    /* Leave GFX_RESERVE_MB unclaimed so the shrink below hands it back to the
     * system for switch-mesa rather than to newlib. */
    {
      size_t gfx = (size_t)GFX_RESERVE_MB * MB;
      size_t body = size - so_zone - arena_sz - big_align;
      fake_heap_size = (body > gfx + newlib_floor) ? body - gfx : body;
    }
  } else {
    fake_heap_size = (size > so_zone) ? size - so_zone : size / 2;
    arena_sz = 0;
  }

  /* The heap is shrunk to hand its unused tail back to the system -- but only
   * AFTER the full layout below is computed, and only fail-safe. See the shrink
   * block after the arena is placed. A blind shrink here (before the layout, and
   * adopting whatever base svcSetHeapSize returned) once produced a build that
   * died with no log at all; the block below instead keeps our exact base or
   * makes no change, and never moves fake_heap. */

  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base  = (void *)ALIGN_MEM((uintptr_t)addr + fake_heap_size, 0x1000);
  heap_so_limit = so_zone;

  if (arena_sz) {
    g_mmap_arena_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base + so_zone, big_align);
    g_mmap_arena_size = arena_sz;
  }

  /* Probe the real kernel-heap base ONCE, with a side-effect-free query. Both
   * shrink sites (this one and [memshrink] in main()) may touch the heap ONLY
   * when this base equals our override base -- otherwise svcSetHeapSize resizes
   * a different region and frees the physical pages aliased into the arena,
   * leaving it unbacked. svcSetHeapSize always maps the heap at the kernel
   * HeapRegionAddress, so if that != our override addr the shrink cannot be ours. */
  {
    u64 kb = 0;
    g_heapfix_obase = (u64)(uintptr_t)addr;
    g_heapfix_kbase = R_SUCCEEDED(svcGetInfo(&kb, InfoType_HeapRegionAddress,
                                             CUR_PROCESS_HANDLE, 0)) ? kb : 0;
  }

  /* Hand the committed-but-unused tail of the override heap back to the system.
   * Measured on hardware: newlib commits ~2399 MB but Unity's heap high-water is
   * ~1681 MB (a fresh first boot spikes to ~2099 MB) while phys-free sits at 3 MB
   * -- that is what starves switch-mesa's out-of-arena allocations (swapchain,
   * command buffers) and intermittently wedges the console to a black screen.
   *
   * WHY THE PREVIOUS ATTEMPT CRASHED, AND WHY THIS ONE DOES NOT:
   * svcSetHeapSize always resizes the KERNEL heap starting at its
   * HeapRegionAddress (g_heapfix_kbase). Under this forwarder that base sits ~48
   * MB BELOW the override base libnx handed us -- the override is a contiguous
   * SUB-RANGE of the kernel heap, not a separate alias. The size we pass must be
   * measured from kbase. The earlier build passed (arena_top - override_base),
   * ~48 MB too small, so the heap top landed ~48 MB below the arena top and cut
   * into the arena -> frame-0 asset-GC data abort. Passing (arena_top - kbase)
   * lands the heap top exactly at the arena top: only the never-allocated tail
   * above the arena is freed; newlib, the .so region and the mmap arena (all
   * below arena_top) are untouched.
   *
   * SAFETY GATE (revised for the real, FRAGMENTED heap): the committed heap is
   * several contiguous MemType_Heap regions from kbase (some no-access "borrowed"
   * guard pages, then the big RW region that holds newlib + the arena + the unused
   * tail). The override address lands in a small RW sub-region whose base is NOT
   * kbase, so we do NOT key off it. Instead we: (1) confirm kbase itself starts a
   * Heap region; (2) walk the contiguous Heap regions up from kbase to find the
   * TRUE committed top (used verbatim for the restore size); (3) require the arena
   * to sit inside that heap with a tail above it; (4) confirm the arena's first and
   * last pages are Heap AND writable, before and (belt-and-suspenders) after the
   * shrink. keep = arena_top - kbase aligns UP so the heap top never lands below
   * the arena. Anything unexpected -> restore to the true top -> no net change.
   * Runs before Unity or the arena touch memory, so a reverted attempt is safe. */
  if (envHasHeapOverride() && arena_sz && g_mmap_arena_base && g_heapfix_kbase) {
    uintptr_t kbase      = (uintptr_t)g_heapfix_kbase;
    uintptr_t arena_base = (uintptr_t)g_mmap_arena_base;
    uintptr_t arena_top  = arena_base + g_mmap_arena_size;
    MemoryInfo kb; u32 kp;
    int at_kbase = (arena_base > kbase)
                && R_SUCCEEDED(svcQueryMemory(&kb, &kp, (u64)kbase))
                && (kb.type & 0xff) == MemType_Heap
                && kb.addr == (u64)kbase;
    /* Walk contiguous Heap regions from kbase to the committed top. */
    u64 htop = kbase;
    if (at_kbase) {
      for (int i = 0; i < 8; i++) {
        MemoryInfo m; u32 mp;
        if (R_FAILED(svcQueryMemory(&m, &mp, htop)) || (m.type & 0xff) != MemType_Heap) break;
        if (m.addr + m.size <= htop) break;              /* no-progress guard */
        htop = m.addr + m.size;
      }
    }
    size_t orig_ksize = (size_t)(htop - kbase);
    size_t keep       = ((arena_top - kbase) + (2 * MB - 1)) & ~(2 * MB - 1);  /* top >= arena_top */
    /* Arena endpoints must be Heap AND writable (perm bit 0x2) both now... */
    MemoryInfo a0, a1; u32 ap0, ap1;
    int arena_rw = R_SUCCEEDED(svcQueryMemory(&a0, &ap0, (u64)arena_base))
                && (a0.type & 0xff) == MemType_Heap && (a0.perm & 0x2)
                && R_SUCCEEDED(svcQueryMemory(&a1, &ap1, (u64)(arena_top - 0x1000)))
                && (a1.type & 0xff) == MemType_Heap && (a1.perm & 0x2);
    if (!at_kbase) {
      g_heapfix_status = 2;                               /* kbase is not a Heap region start */
    } else if (!((u64)arena_top < htop && keep + 2 * MB <= orig_ksize
                 && keep >= newlib_floor + so_zone)) {
      g_heapfix_status = 6;                               /* arena not inside heap, or no tail */
    } else if (!arena_rw) {
      g_heapfix_status = 5;                               /* arena pages not Heap+writable */
    } else {
      void *nb = NULL;
      Result rc = svcSetHeapSize(&nb, keep);
      if (R_SUCCEEDED(rc) && nb == (void *)kbase) {
        /* ...and STILL after the shrink (catches a mis-modelled layout). */
        MemoryInfo q0, q1; u32 qp0, qp1;
        int still = R_SUCCEEDED(svcQueryMemory(&q0, &qp0, (u64)arena_base))
                 && (q0.type & 0xff) == MemType_Heap && (q0.perm & 0x2)
                 && R_SUCCEEDED(svcQueryMemory(&q1, &qp1, (u64)(arena_top - 0x1000)))
                 && (q1.type & 0xff) == MemType_Heap && (q1.perm & 0x2);
        if (still) {
          g_heapfix_status   = 1;
          g_heapfix_freed_mb = (unsigned)((orig_ksize - keep) / MB);
        } else {
          void *nb2 = NULL;
          svcSetHeapSize(&nb2, orig_ksize);               /* restore -> arena re-backed */
          g_heapfix_status = 4;
        }
      } else {
        void *nb2 = NULL;
        svcSetHeapSize(&nb2, orig_ksize);                 /* base moved / declined -> restore */
        g_heapfix_status = (R_SUCCEEDED(rc)) ? 4 : 3;
      }
    }
  }
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77)) fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78)) fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73)) fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE) fatal_error("Own process handle is unavailable.");
}

/* ==========================================================================
 * Color-Sheep-specific: data layout, module load, region no-op.
 * ========================================================================== */

/* Zombotron (Unity 6000.2.6f2) ships the CLASSIC SPLIT data layout, NOT a single
 * data.unity3d. The entry point is globalgamemanagers, alongside the level and
 * sharedassets .assets files and many .splitN chunks (Unity 1 MB splitting; the
 * engine reassembles name.split0..N transparently). IL2CPP metadata lives at
 * Managed/Metadata/global-metadata.dat. All staged flat under assets/bin/Data/. */
/* Resolve a native module to its on-disk path: prefer lib/<name> (grouped
 * layout), fall back to <name> at the data root (flat layout). Both are accepted,
 * so moving the .so into a lib/ subfolder is optional and never breaks a flat
 * install that keeps them next to the .nro. */
static const char *resolve_lib(const char *name, char *out, size_t outsz) {
  struct stat st;
  snprintf(out, outsz, "%s/lib/%s", DATA_ROOT, name);
  if (stat(out, &st) == 0) return out;
  snprintf(out, outsz, "%s/%s", DATA_ROOT, name);
  return out;
}

static void check_data(void) {
  char path[768];
  struct stat st;
  /* Native modules: accept either lib/<name> or <name> at the root. */
  const char *libs[] = { LIB_MAIN, LIB_UNITY, LIB_IL2CPP };
  for (unsigned i = 0; i < sizeof(libs)/sizeof(*libs); i++) {
    const char *p = resolve_lib(libs[i], path, sizeof path);
    if (stat(p, &st) < 0)
      fatal_error("Missing library:\n  %s\n\nSearched (lib/ and root) under:\n  %s\n\n"
                  "Put the .nro in the SAME folder as the .so files (or in a lib/\n"
                  "subfolder there) and the assets, or see README.md for the layout.",
                  libs[i], DATA_ROOT);
  }
  /* Scene/metadata under assets/ (root only). */
  const char *files[] = {
    "assets/bin/Data/globalgamemanagers",                     /* Zombotron: split layout entry point */
    "assets/bin/Data/level0",                                 /* first scene (proves scene data is staged) */
    "assets/bin/Data/Managed/Metadata/global-metadata.dat",    /* IL2CPP metadata */
    "assets/bin/Data/boot.config",
  };
  for (unsigned i = 0; i < sizeof(files)/sizeof(*files); i++) {
    snprintf(path, sizeof path, "%s/%s", DATA_ROOT, files[i]);
    if (stat(path, &st) < 0)
      fatal_error("Missing data file:\n  %s\n\nSearched in:\n  %s\n\n"
                  "Put the .nro in the SAME folder as the assets,\n"
                  "or see README.md for the correct layout.",
                  files[i], DATA_ROOT);
  }
}

static int load_module(so_module *mod, const char *name) {
  char path[768];
  resolve_lib(name, path, sizeof path);
  if (so_load(mod, path, heap_so_base, heap_so_limit) < 0)
    return -1;
  size_t used = ALIGN_MEM(mod->load_size, 0x1000);
  heap_so_base = (char *)heap_so_base + used;
  heap_so_limit -= used;
  crx_resolve_imports(mod);
  return 0;
}

/* Save persistence: commit the SD periodically. (Color Sheep saves via PlayerPrefs;
 * the managed PlayerPrefs flush hook is a TODO -- see below -- but committing the SD
 * still persists whatever reached the prefs file.) */
static AppletHookCookie g_applet_cookie;
static void nx_applet_hook(AppletHookType hook, void *param) {
  (void)param;
  if (hook == AppletHookType_OnFocusState || hook == AppletHookType_OnExitRequest)
    nx_sd_flush();
}

/* ===========================================================================
 * Zombotron boot.
 *
 * Everything above this line is inherited from badpiggies_nx unchanged (heap /
 * overcommit / mmap arena / crash handler / SD hygiene) -- it is engine-generic.
 * Everything below is Unity 6 and Zombotron specific.
 *
 * Differences from the 2020.3 and 2022.3 reference ports, all of which fail
 * silently rather than loudly if you get them wrong:
 *
 *   - a FOURTH module, lib_burst_generated.so, must be loaded (Burst jobs)
 *   - libswappywrapper.so / libvulkan.so / libaaudio.so must be refused at
 *     dlopen so the engine falls back to the paths this port implements
 *   - initJni takes four arguments, and nativeUnityPlayerSetRunning must be
 *     called -- see zombotron_boot.c
 *   - the engine patches are located at runtime, not by hard-coded RVA --
 *     see zombotron_locate.c
 * =========================================================================== */

#define LIB_BURST  "lib_burst_generated.so"

so_module burst_mod;

/* Implemented in zombotron_boot.c */
int zb_boot_and_run(void);
/* Implemented in zombotron_patches.c */
int  zb_install_patches(so_module *unity);
void zb_shutdown_patches(void);

int main(int argc, char *argv[]) {
  /* FIRST: work out where we are. debug.log lives under the resolved root, so
   * nothing may log before this returns. */
  zb_resolve_game_root(argc, argv);

  socketInitializeDefault();
  debugPrintf("[boot] === zombotron_nx start (Unity 6000.2.6f2 / IL2CPP) ===\n");
  zb_root_report(argc, argv);

  /* CWD fix: title-override leaves cwd at the .nro folder or SD root; Unity reads
   * many files via relative paths, so chdir into DATA_ROOT. */
  {
    char cwd[256] = {0};
    getcwd(cwd, sizeof cwd);
    int rc = chdir(DATA_ROOT);
    struct stat st;
    int reach_meta = stat("assets/bin/Data/Managed/Metadata/global-metadata.dat", &st) == 0;
    int reach_data = stat("assets/bin/Data/globalgamemanagers", &st) == 0;
    debugPrintf("[boot] cwd '%s' -> chdir(%s)=%d; metadata=%d ggm=%d\n",
                cwd, DATA_ROOT, rc, reach_meta, reach_data);
    if (rc != 0)
      debugPrintf("[boot] WARNING: chdir failed -- data root may be wrong\n");
  }

  /* FastLoad clocks (CPU 1020 -> 1785 MHz) for the whole load path: module
   * relocation, 467+22 ctors, il2cpp metadata parsing and Unity's first-scene
   * asset decompression are all CPU-bound, and the hardware timing showed
   * nativeRender at 1188 ms and 2987 ms for the first two frames. FastLoad
   * trades GPU clock for CPU, which is the right way round while loading and
   * the wrong way round once rendering starts -- zombotron_boot.c drops back to
   * Normal as soon as frames are consistently fast. */
  cpu_boost(1);
  debugPrintf("[boot] CPU boost ON (FastLoad) for the load path\n");

  check_syscalls();
  debugPrintf("[boot] syscalls ok\n");
  {
    extern char *fake_heap_start, *fake_heap_end;
    debugPrintf("[boot] mem: newlib=%u MB, mmap arena=%u MB @ %p\n",
                (unsigned)((fake_heap_end - fake_heap_start) / (1024 * 1024)),
                (unsigned)(g_mmap_arena_size / (1024 * 1024)), g_mmap_arena_base);
    u64 tot = 0, used = 0;
    svcGetInfo(&tot,  InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize,  CUR_PROCESS_HANDLE, 0);
    debugPrintf("[boot] phys: total=%u MB used=%u MB free=%u MB "
                "(target >= %u MB for switch-mesa)\n",
                (unsigned)(tot >> 20), (unsigned)(used >> 20),
                (unsigned)((tot - used) >> 20), GFX_RESERVE_MB);
    if (((tot - used) >> 20) < GFX_RESERVE_MB / 2)
      debugPrintf("[boot] WARNING: less than %u MB free -- the graphics driver "
                  "may fail an allocation mid-frame and wedge the console\n",
                  GFX_RESERVE_MB / 2);
    /* [memdiag] zero-risk probe: pure svcGetInfo/env queries + arithmetic, NO
     * memory operations -- cannot change how the game runs. Tells us whether the
     * committed physical can be handed back to the driver, and how. */
    {
      u64 sysres = 0, asize = 0;
      svcGetInfo(&sysres, InfoType_SystemResourceSizeTotal, CUR_PROCESS_HANDLE, 0);
      svcGetInfo(&asize,  InfoType_AliasRegionSize,         CUR_PROCESS_HANDLE, 0);
      int hovr    = envHasHeapOverride();
      u64 ov_addr = hovr ? (u64)(uintptr_t)envGetHeapOverrideAddr() : 0;
      u64 ov_size = hovr ? (u64)envGetHeapOverrideSize() : 0;
      u64 lay_top = (u64)(uintptr_t)g_mmap_arena_base + g_mmap_arena_size;
      long long slack = hovr ? (long long)((ov_addr + ov_size) - lay_top) : -1;
      debugPrintf("[memdiag] heap_override=%d ov_size=%u MB | sysres=%u MB | alias=%u MB | "
                  "layout_top=0x%llx slack=%lld MB | newlib=%u MB\n",
                  hovr, (unsigned)(ov_size >> 20), (unsigned)(sysres >> 20),
                  (unsigned)(asize >> 20), (unsigned long long)lay_top,
                  (long long)(slack >> 20),
                  (unsigned)((fake_heap_end - fake_heap_start) >> 20));
      debugPrintf("[memdiag] interpretation: slack>0 => that many MB of override tail are "
                  "unused and safely returnable via svcSetHeapSize shrink; "
                  "if slack~0, freeing needs shrinking newlib to Unity's real peak.\n");
    }
    {
      const char *s = (g_heapfix_status == 1) ? "OK -- freed the unused tail to the graphics driver"
                    : (g_heapfix_status == 2) ? "SKIPPED (kbase is not a Heap-region start; no change)"
                    : (g_heapfix_status == 3) ? "SKIPPED (svcSetHeapSize declined; restored, no change)"
                    : (g_heapfix_status == 4) ? "REVERTED (post-shrink arena check failed; restored, no change)"
                    : (g_heapfix_status == 5) ? "SKIPPED (arena pages not Heap+writable; no change)"
                    : (g_heapfix_status == 6) ? "SKIPPED (arena not inside committed heap, or no tail; no change)"
                    :                           "not attempted";
      debugPrintf("[heapfix] tail shrink: %s; freed=%u MB "
                  "(override base=0x%llx, kernel heap base=0x%llx)\n",
                  s, g_heapfix_freed_mb,
                  (unsigned long long)g_heapfix_obase, (unsigned long long)g_heapfix_kbase);
    }
    /* [memmap] DIAGNOSTIC (no side effects): dump the real region topology around
     * the kernel-heap base, the override base and the mmap arena, so we can see
     * why the [heapfix] gate accepted/rejected and size the shrink correctly. */
    {
      u64 kbase = g_heapfix_kbase;
      u64 obase = g_heapfix_obase;
      u64 abase = (u64)(uintptr_t)g_mmap_arena_base;
      u64 atop  = abase + g_mmap_arena_size;
      struct { const char *n; u64 a; } pr[4] = {
        { "kbase",      kbase   }, { "obase",       obase   },
        { "arena_base", abase   }, { "arena_top-1", atop ? atop - 1 : 0 }
      };
      for (int i = 0; i < 4; i++) {
        MemoryInfo mi; u32 pi;
        if (pr[i].a && R_SUCCEEDED(svcQueryMemory(&mi, &pi, pr[i].a)))
          debugPrintf("[memmap] %-11s @0x%llx -> base=0x%llx size=%lluMB type=0x%x perm=0x%x attr=0x%x\n",
                      pr[i].n, (unsigned long long)pr[i].a, (unsigned long long)mi.addr,
                      (unsigned long long)(mi.size >> 20), (unsigned)(mi.type & 0xff),
                      (unsigned)mi.perm, (unsigned)mi.attr);
      }
      u64 a = kbase; int n = 0;
      while (kbase && a < atop + 0x8000000ull && n < 20) {
        MemoryInfo mi; u32 pi;
        if (R_FAILED(svcQueryMemory(&mi, &pi, a))) break;
        debugPrintf("[memmap] region[%d] base=0x%llx size=%lluMB type=0x%x perm=0x%x attr=0x%x\n",
                    n, (unsigned long long)mi.addr, (unsigned long long)(mi.size >> 20),
                    (unsigned)(mi.type & 0xff), (unsigned)mi.perm, (unsigned)mi.attr);
        u64 next = mi.addr + mi.size;
        if (next <= a) break;
        a = next; n++;
      }
    }
    /* The committed-tail return is done once, correctly, in __libnx_initheap
     * ([heapfix] above): it runs before Unity or the mmap arena touch memory and
     * is gated on a svcQueryMemory check of the real kernel-heap layout, shrinking
     * from the kernel base (not the override base). The earlier second-stage
     * shrink here repeated the ~48 MB-too-small calc that cut the arena, so it is
     * removed -- doing it twice would only risk re-introducing that crash. */
    if (envHasHeapOverride())
      debugPrintf("[memshrink] handled at heap init -- see [heapfix] above\n");
    if ((tot >> 20) < 5000)
      debugPrintf("[boot] NOTE: 4GB console. Zombotron Re-Boot's payload is ~140 MB of "
                  "libs + 250 MB of assets; the Unity 6 region-granularity patch "
                  "IS derived and applied (see [region] below).\n");
  }

  /* Stack-region overcommit arena. Any failure -> heap-backed arena. */
  {
    void *pool = NULL;
    size_t winsz = 0;
    void *win = oc_find_stack_window(OC_WINDOW_BYTES, &winsz);
    VirtmemReservation *rv = NULL;
    if (win && winsz) {
      virtmemLock();
      rv = virtmemAddReservation(win, winsz);
      virtmemUnlock();
    }
    if (win && rv && winsz) {
      pool = memalign(0x1000, OC_POOL_BYTES);
      if (pool && oc_arena_init(win, winsz, pool, OC_POOL_BYTES)) {
        debugPrintf("[oc] ARMED: window %u MB @ %p, pool %u MB @ %p\n",
                    (unsigned)(winsz >> 20), win, (unsigned)(OC_POOL_BYTES >> 20), pool);
        /* Immunise the low zone against kernel TLS placement (the intermittent
         * frame-3 [oc] crash). 384 MB covers the observed collision (~287 MB in)
         * while leaving physical headroom -- the GPU arena needs it (frame-1 OOM). */
        oc_precommit((size_t)384 * 1024 * 1024);
      } else
        debugPrintf("[oc] DISABLED: pool=%p init failed -> heap-backed only\n", pool);
    } else {
      debugPrintf("[oc] DISABLED: no usable stack hole -> heap-backed only\n");
    }
  }

  if (appletGetOperationMode() == AppletOperationMode_Console) {
    screen_width  = ZB_FORCE_SCREEN_W;   /* docked: full 1920x1080 */
    screen_height = ZB_FORCE_SCREEN_H;
  } else {
    /* Handheld: the panel is 1280x720. Rendering 1080p and letting the compositor
     * downscale wastes ~2.25x the GPU work (and GPU memory) for no visible gain on
     * a 720p screen. Render at native 720p -- the biggest single fps lever here. */
    screen_width  = 1280;
    screen_height = 720;
  }
  debugPrintf("[gfx] mode=%s render=%dx%d (forced)\n",
              appletGetOperationMode() == AppletOperationMode_Console ? "DOCKED" : "HANDHELD",
              screen_width, screen_height);

  SDL_SetMainReady();


  if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
    debugPrintf("SDL_Init failed: %s\n", SDL_GetError());

  /* FORCE SINGLE-THREADED RENDERING.
   *
   * Zombotron ships boot.config with gfx-threading-mode=4 (multithreaded). In
   * that mode Unity's RENDER THREAD submits presents independently of this
   * loop, so the frame limiter here throttles frame *generation* but not buffer
   * *submission* -- and outrunning the compositor takes the whole console down,
   * not just the game.
   *
   * That is exactly the observed progression: adding a 1 ms pacing floor moved
   * the hard freeze from frame 4 to frame 26 (a real improvement, since it slows
   * how fast the main loop feeds the render thread) but could not remove it,
   * because the submissions were never under this loop's control.
   *
   * badpiggies_nx's payload shipped with mt rendering already disabled, which is
   * why the inherited substrate never had to do this. Rewriting one line of a
   * plain-text config in the user's own staged copy is reversible and logged;
   * tools/stage_sd.py --force-st does the same thing ahead of time. */
  {
    char bc[600];
    snprintf(bc, sizeof bc, "%s/assets/bin/Data/boot.config", DATA_ROOT);
    FILE *f = fopen(bc, "r");
    if (f) {
      char buf[2048];
      size_t n = fread(buf, 1, sizeof buf - 1, f);
      fclose(f);
      buf[n] = 0;
      char *hit = strstr(buf, "gfx-threading-mode=4");
      if (hit) {
        hit[strlen("gfx-threading-mode=")] = '0';   /* 4 -> 0, same length */
        f = fopen(bc, "w");
        if (f) {
          fwrite(buf, 1, n, f);
          fclose(f);
          debugPrintf("[gfx] boot.config: gfx-threading-mode 4 -> 0 "
                      "(single-threaded rendering; MT submits presents outside "
                      "our frame pacing and wedges the compositor)\n");
        } else {
          debugPrintf("[gfx] WARNING: could not rewrite boot.config "
                      "(read-only SD?) -- MT rendering stays on\n");
        }
      } else {
        debugPrintf("[gfx] boot.config: threading already non-4\n");
      }
    } else {
      debugPrintf("[gfx] WARNING: boot.config not readable at %s\n", bc);
    }
  }

#if ASSET_PACK_ENABLE
  /* Asset pack (technique from the fruitninja_nx port): fold the loose asset tree
   * into one indexed pack file so the game does one open + seeks within a single
   * file instead of opening/stat-ing hundreds of loose files. Built on first boot
   * from the loose tree; mounted on every boot after. We KEEP the loose files as a
   * safe fallback (if the pack is ever bad, asset_pack_active() stays false and all
   * the routing hooks fall through to the loose files). Costs ~2x SD space. Set
   * ASSET_PACK_ENABLE 0 in config.h to disable. */
  if (asset_pack_open_existing(DATA_ROOT)) {
    debugPrintf("[pack] mounted existing pack (%zu entries)\n", asset_pack_entry_count());
  } else {
    debugPrintf("[pack] no pack found -- building from loose assets. FIRST BOOT ONLY, "
                "may take several minutes; do not power off.\n");
    char _adir[600];
    snprintf(_adir, sizeof _adir, "%s/assets", DATA_ROOT);
    if (!asset_pack_build(_adir, DATA_ROOT)) {
      debugPrintf("[pack] build FAILED: %s -- continuing with loose files\n", asset_pack_error());
    } else if (asset_pack_open_existing(DATA_ROOT)) {
      debugPrintf("[pack] built + mounted (%zu entries); loose files KEPT as fallback\n",
                  asset_pack_entry_count());
    } else {
      debugPrintf("[pack] built but could not mount -- continuing with loose files\n");
    }
  }
#endif

  check_data();

  /* Sweep Unity's case-probe files + migrate old loose scratch. Unity re-creates
   * CASESENSITIVETEST<guid> every boot and never cleans up. The case-test scratch and
   * the synthetic /proc,/sys,/dev backing files now live in a hidden .synth/ subfolder,
   * so also unlink any old loose .casetest / .synth_* strays from earlier builds -- one
   * boot migrates them and the data root is left with only real game files. */
  {
    DIR *dd = opendir(DATA_ROOT);
    int swept = 0;
    if (dd) {
      struct dirent *de;
      while ((de = readdir(dd))) {
        if (strncasecmp(de->d_name, "CASESENSITIVETEST", 17) == 0 ||
            strcmp(de->d_name, ".casetest") == 0 ||
            strncmp(de->d_name, ".synth_", 7) == 0) {
          char pth[320];
          snprintf(pth, sizeof pth, "%s/%s", DATA_ROOT, de->d_name);
          if (unlink(pth) == 0) swept++;
        }
      }
      closedir(dd);
    }
    if (swept) debugPrintf("[boot] swept %d loose scratch file(s)\n", swept);
    /* Hidden folder that backs the synthetic /proc,/sys,/dev files and the case
     * probe, so the data root shows only real game files. Created before the game
     * runs, so every synth open() below finds it. */
    { char sp[300]; snprintf(sp, sizeof sp, "%s/.synth", DATA_ROOT); mkdir(sp, 0777); }
    /* The case-probe scratch lives in .synth/ now, beyond the root sweep above.
     * Unity re-creates it with O_CREAT|O_EXCL every boot, which FAILS if a stale
     * copy is left behind -- so clear it here each boot, exactly as the old root
     * .casetest was swept. Without this, boot 2+ hangs before the first frame. */
    { char cp[320]; snprintf(cp, sizeof cp, "%s/.synth/casetest", DATA_ROOT); unlink(cp); }
  }

  debugPrintf("[boot] loading modules...\n");
  if (load_module(&main_mod,   LIB_MAIN)   < 0) fatal_error("Could not load %s", LIB_MAIN);
  if (load_module(&unity_mod,  LIB_UNITY)  < 0) fatal_error("Could not load %s", LIB_UNITY);
  if (load_module(&il2cpp_mod, LIB_IL2CPP) < 0) fatal_error("Could not load %s", LIB_IL2CPP);

  /* Burst is OPTIONAL. Its jobs are resolved by name at runtime; if the module is
   * absent the engine falls back to the managed implementations -- slower, but it
   * boots. Making this fatal would block bring-up for no reason. */
  if (load_module(&burst_mod, LIB_BURST) < 0) {
    debugPrintf("[boot] %s not loaded -- Burst jobs fall back to managed\n", LIB_BURST);
    memset(&burst_mod, 0, sizeof burst_mod);
  }

  g_il2cpp_base = (uintptr_t)il2cpp_mod.load_virtbase;
  g_il2cpp_size = il2cpp_mod.load_size;
  debugPrintf("[boot] libmain=%p libunity=%p libil2cpp=%p burst=%p\n",
              main_mod.load_virtbase, unity_mod.load_virtbase,
              il2cpp_mod.load_virtbase, burst_mod.load_virtbase);

  so_finalize(&main_mod);   so_flush_caches(&main_mod);
  so_finalize(&unity_mod);  so_flush_caches(&unity_mod);
  so_finalize(&il2cpp_mod); so_flush_caches(&il2cpp_mod);
  if (burst_mod.load_virtbase) { so_finalize(&burst_mod); so_flush_caches(&burst_mod); }
  debugPrintf("[boot] modules finalized + flushed\n");

  /* Locate the engine functions we patch, then patch them. Both steps are
   * fail-safe: an unresolved target is skipped, never guessed at. */
  zb_locate_all(&unity_mod);
  zb_install_patches(&unity_mod);

  /* Region granularity must be patched BEFORE any engine code runs, since the
   * memory manager reserves its first blocks during the init arrays. */
  zb_install_region_patch(&unity_mod);

  /* MAIN-THREAD BIONIC TLS -- must precede any engine code.
   *
   * Unity's stack-protector prologues do:
   *     mrs x19, tpidr_el0
   *     ldr x9, [x19, #0x28]      <- the canary
   * With TPIDR_EL0 unset that reads address 0x28 and takes a level-1
   * translation fault. It is exactly what happened at libunity+0x7042fc during
   * ctor ~100 of libunity's init_array, with x19 = 0 in the dump.
   *
   * Every thread that runs engine code needs its OWN zeroed block (a shared one
   * races: one thread's writes corrupt another's in-flight canary). The pthread
   * shim, the OpenSL audio thread and the clock thread each do this already;
   * the main thread was the one left out. */
  static uint8_t main_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(main_tls);
  zb_set_main_tls(main_tls);
  debugPrintf("[boot] main-thread bionic TLS @ %p\n", (void *)main_tls);

  so_execute_init_array(&main_mod);
  so_execute_init_array(&unity_mod);
  so_execute_init_array(&il2cpp_mod);
  if (burst_mod.load_virtbase) so_execute_init_array(&burst_mod);

  /* Release the raw ELF staging buffers now that everything is relocated and
   * constructed. This is ~123 MB for Zombotron (libil2cpp alone is 100 MB) and
   * it comes straight out of the newlib heap the engine is about to want. */
  so_free_temp(&main_mod);
  so_free_temp(&unity_mod);
  so_free_temp(&il2cpp_mod);
  if (burst_mod.load_virtbase) so_free_temp(&burst_mod);
  debugPrintf("[boot] init arrays run; staging buffers freed\n");

  jni_init();
  /* Creates fake_unityplayer_thiz / fake_context_obj / fake_surface_obj and
   * registers the asset + PlayerPrefs + Display JNI handlers against the data
   * root. Without it those three globals stay NULL (they are `= 0` in
   * unity_glue.c) and initJni is handed a null Context. */
  unity_environment_init(DATA_ROOT);

  /* Order matters and is inherited from the reference: update_mode() sizes the
   * NWindow from screen_width/height, and input_init() then binds HID against
   * that size. Calling input_init() alone leaves the window at its 720x1280
   * PORTRAIT default while Unity renders 1920x1080 landscape -- the game draws
   * into a buffer of the wrong shape and comes out cropped/rotated. */
  android_native_update_mode();
  android_native_input_init();
  appletHook(&g_applet_cookie, nx_applet_hook, NULL);

  /* Re-assert the guard: jni_init / HID / applet setup all ran in between and
   * any of them could have left tpidr in an unexpected state. The reference
   * re-asserts at exactly these two points too. */
  install_bionic_tls(main_tls);

  /* libunity's JNI_OnLoad registers the UnityPlayer natives; the capture in
   * jni_fake.c is what zombotron_boot.c later resolves by name. */
  {
    typedef int (*fn_jnionload)(void *, void *);
    fn_jnionload Unity_JNI_OnLoad =
        (fn_jnionload)so_try_find_addr_rx(&unity_mod, "JNI_OnLoad");
    if (!Unity_JNI_OnLoad) fatal_error("libunity JNI_OnLoad not found");
    debugPrintf("[boot] libunity JNI_OnLoad(fake_vm)...\n");
    int jver = Unity_JNI_OnLoad(fake_vm, NULL);
    debugPrintf("[boot] JNI_OnLoad returned 0x%x\n", jver);
  }
  {
    typedef int (*fn_jnionload)(void *, void *);
    fn_jnionload Il2cpp_JNI_OnLoad =
        (fn_jnionload)so_try_find_addr_rx(&il2cpp_mod, "JNI_OnLoad");
    if (Il2cpp_JNI_OnLoad) {
      debugPrintf("[boot] libil2cpp JNI_OnLoad(fake_vm)...\n");
      Il2cpp_JNI_OnLoad(fake_vm, NULL);
    } else {
      debugPrintf("[boot] WARNING: libil2cpp JNI_OnLoad missing (managed JNI may fail)\n");
    }
  }

  /* Managed-side input hooks. Il2CppDumper output for 1.4.8 showed that Zombotron
   * reaches input through Rewired, and that Rewired's Android path
   * (UnityInputJoystickSource + ThreadSafeUnityInput) reads UnityEngine.Input --
   * so hooking UnityEngine.Input is correct here after all. Installed AFTER
   * il2cpp's JNI_OnLoad so the runtime exists.
   *   PlayerPrefs: still no hook needed. Unity 6 reaches PlayerPrefs through Java
   *   PlayerPrefs: not needed. Unity 6 reaches PlayerPrefs through Java
   *     SharedPreferences, which unity_jni.c already implements over prefs.kv.
   *     Verified: Zombotron's libunity.so carries getSharedPreferences and the
   *     ".v2.playerprefs" store name. */
  if (zb_il2cpp_bind(&il2cpp_mod) == 0)
    zb_il2cpp_install_hooks(&il2cpp_mod);

  /* Splash video. Read the manifest first so the hook installer's log line and
   * the clip list appear together, and so a missing videos/ folder is reported
   * before anything can call Play. Neither call allocates GL, opens a device or
   * starts a thread -- the decoder only exists between a Play and a Stop.
   * Independent of zb_il2cpp_bind: these are code patches, not runtime-API
   * hooks, and the video path must not be able to take the input hooks down. */
  zb_video_init(zb_game_root());
  zb_il2cpp_install_video_hooks(&il2cpp_mod);

  /* Force Panik.PlatformAPI to the "no store platform" backend. Without this the
   * loading screen waits forever on Google Play Games authentication, which
   * cannot complete here. Independent of zb_il2cpp_bind: it is a code patch, not
   * a runtime-API hook. */
  zb_il2cpp_force_platform_none(&il2cpp_mod);

  /* Zombotron: neutralise JNI-dependent service inits (analytics / achievements /
   * GDPR) that NRE under the fake Android layer during boot. Fail-safe. */
  zb_game_patches(&il2cpp_mod);

  /* Zombotron: Unity 6 PlayerPrefs is a NATIVE libunity binding (not JNI), so the
   * loader's prefs KV store was bypassed and native prefs returned ""/0 instead of
   * the caller default -> SettingsBoxLoader.Read NRE -> Game.Start aborts -> black
   * screen. Redirect the public PlayerPrefs methods to KV-backed C hooks (returns
   * stored value else default; Set and Save persist to prefs.kv). Fail-safe. */
  zb_playerprefs_hooks(&il2cpp_mod);
  zb_gameflow_probe(&il2cpp_mod);   /* force RemoteConfig.IsInitialized true + log the loading gate */

  /* Diagnostic: name the two boot-time NullReferenceException throw sites
   * (managed traces are stripped). Prints throw-site RVAs -> map via dump.cs. */
  zb_il2cpp_install_exception_tracer(&il2cpp_mod);

  /* Drive UnityEngine.Time from our own clock. The engine's TimeManager never
   * ticks here, so without this every managed Time.* value stays frozen and the
   * game renders but never advances. */
  zb_time_install(&il2cpp_mod);

  /* Arm the GPU arena only now. Everything before this point -- the overcommit
   * pool, the mmap arena, module loading -- makes large page-aligned
   * allocations that are NOT graphics buffers, and letting those trigger the
   * arena reserved 512 MB for nothing and regressed the game to a frame-0
   * freeze. From here on the only page-aligned allocator of this size class is
   * libdrm_nouveau. */
  zb_gpua_enable();
  /* Installed in both modes. With ZB_ENABLE_POINTER_INPUT=0 these hooks are
   * what actively DENIES touch and mouse to the game; skipping them would let
   * the engine advertise its native Android values instead. */
  zb_touchhook_install(&il2cpp_mod);

  /* Screen dimensions MUST be non-zero: the game's RenderingMaster scales a
   * render texture in 0.005 steps "up to the max resolution available", and a
   * zero base makes that loop non-terminating (100% CPU, gear frozen). */
  zb_screen_install(&il2cpp_mod);

  diag_thread_register(NULL, 1);       /* mark this as the engine main thread */
  diag_set_name(NULL, "zombotron-main");
  diag_watchdog_start();               /* logs a backtrace if a frame wedges */

  int rc = zb_boot_and_run();   /* re-asserts TLS itself, right before initJni */

  zb_shutdown_patches();
  nx_sd_flush();
  debugPrintf("[boot] exit rc=%d\n", rc);
  opensles_shutdown();
  SDL_Quit();
  socketExit();

  /* The engine leaves threads and atexit handlers behind that will fault if the
   * normal C runtime teardown runs, so leave the same way the reference does. */
  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
