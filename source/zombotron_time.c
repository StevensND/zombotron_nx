/* zombotron_time.c -- drive UnityEngine.Time from our own clock.
 *
 * WHY THIS IS NEEDED
 * The engine's native TimeManager::Update is NEVER CALLED on this port. The
 * heartbeat proved it directly:
 *
 *     [loop] frame=2 fps=0.5 | engine: TimeManager NOT captured
 *                              (TimeManager::Update never ran)
 *
 * nativeRender is being called and returning -- frames advance -- but Unity's
 * player loop never ticks the time subsystem, so every managed Time.* value
 * stays frozen at its initial value. A loading spinner driven by
 * `transform.Rotate(0,0,speed * Time.deltaTime)` therefore renders one frame of
 * motion and then sits still forever, which is exactly the reported symptom.
 *
 * WHY NOT THE NATIVE HOOK ALONE
 * zombotron_patches.c hooks TimeManager::Update and can re-enter its body with a
 * wall-clock newTime, which is what badpiggies/PvZ do. That only works if
 * something hands us the TimeManager instance. PvZ gets it from an inlineable
 * GetTimeManager() accessor; Unity 6 has no such symbol (TimeManager::GetRealtime
 * takes `this`), and our entry hook never fires, so the instance is never
 * captured and the native path has nothing to drive.
 *
 * So this works one level up, at the managed boundary: UnityEngine.Time's
 * getters are icalls into libil2cpp, and hooking them makes the C# side see time
 * advancing regardless of what the native subsystem is doing. It is the same
 * technique as the input hooks, which already work on hardware.
 *
 * SEMANTICS
 * All values derive from one monotonic base captured at install time:
 *   time / unscaledTime / realtimeSinceStartup  seconds since install
 *   deltaTime / unscaledDeltaTime / smoothDeltaTime  measured per frame,
 *       clamped to [1ms, 100ms] so a long synchronous scene load cannot emit a
 *       single enormous delta that makes physics and animation explode
 *   fixedDeltaTime / fixedUnscaledDeltaTime  a constant 1/60
 *   timeScale  1.0
 *   frameCount / renderedFrameCount  our own loop counter
 *
 * zb_time_tick() must be called once per frame from the render loop; that is
 * what samples deltaTime.
 *
 * MIT.
 */
#include <stdint.h>
#include <time.h>

#include "so_util.h"
#include "util.h"
#include "zombotron_time.h"

static uint64_t g_base_ns;
static uint64_t g_prev_ns;
static volatile float  g_delta = 1.0f / 60.0f;
static volatile int32_t g_frames;
static int g_installed;

/* Time.time SAMPLED ONCE PER FRAME, not per read.
 *
 * Two reasons, and the second one bit hard on hardware.
 *
 * CORRECTNESS: in Unity, Time.time is constant for the duration of a frame.
 * Returning a continuously-advancing value breaks anything that compares two
 * timestamps taken within one frame -- they never compare equal, and code like
 * `if (Time.time - _lastX >= interval)` drifts.
 *
 * COST: every Time.* getter is read many times per frame, by the engine and by
 * every MonoBehaviour.Update, animation and particle system. The first version
 * called clock_gettime(CLOCK_MONOTONIC) on EACH read -- a syscall. On hardware
 * this ran at 0.4 fps with dt pinned at the 100ms clamp ceiling. */
static volatile float g_now_s = 0.0f;

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Cached; refreshed once per frame by zb_time_tick(). */
static float elapsed_s(void) { return g_now_s; }

void zb_time_tick(void) {
  uint64_t n = now_ns();
  if (!g_base_ns) { g_base_ns = n; g_prev_ns = n; }
  double d = (double)(n - g_prev_ns) / 1e9;
  g_prev_ns = n;
  /* Clamp. A synchronous scene load can park the loop for seconds; handing that
   * to the game as one deltaTime would teleport animations and integrators. */
  if (d < 0.001) d = 0.001;
  if (d > 0.100) d = 0.100;
  g_delta = (float)d;
  g_now_s = (float)((double)(n - g_base_ns) / 1e9);
  g_frames++;
}

/* ---- the hooks. All are static, zero-arg, and return by value. ---- */
static float   hk_time(void)                   { return elapsed_s(); }
static float   hk_deltaTime(void)              { return g_delta; }
static float   hk_unscaledTime(void)           { return elapsed_s(); }
static float   hk_unscaledDeltaTime(void)      { return g_delta; }
static float   hk_fixedUnscaledDeltaTime(void) { return 1.0f / 60.0f; }
static float   hk_fixedDeltaTime(void)         { return 1.0f / 60.0f; }
static float   hk_smoothDeltaTime(void)        { return g_delta; }
static float   hk_timeScale(void)              { return 1.0f; }
static int32_t hk_frameCount(void)             { return g_frames; }
static int32_t hk_renderedFrameCount(void)     { return g_frames; }
static float   hk_realtimeSinceStartup(void)   { return elapsed_s(); }

typedef struct {
  const char *name;
  uint32_t    rva;
  uint32_t    guard;
  void       *fn;
} TimeHook;

int zb_time_install(so_module *il2cpp) {
  static const uint32_t G = 0xa9bf4ffeu;   /* stp x30, x19, [sp, #-0x10]! */
  const TimeHook H[] = {
    { "get_time",                   RVA_Time_get_time,                   G, (void *)hk_time },
    { "get_deltaTime",              RVA_Time_get_deltaTime,              G, (void *)hk_deltaTime },
    { "get_unscaledTime",           RVA_Time_get_unscaledTime,           G, (void *)hk_unscaledTime },
    { "get_unscaledDeltaTime",      RVA_Time_get_unscaledDeltaTime,      G, (void *)hk_unscaledDeltaTime },
    { "get_fixedUnscaledDeltaTime", RVA_Time_get_fixedUnscaledDeltaTime, G, (void *)hk_fixedUnscaledDeltaTime },
    { "get_fixedDeltaTime",         RVA_Time_get_fixedDeltaTime,         G, (void *)hk_fixedDeltaTime },
    { "get_smoothDeltaTime",        RVA_Time_get_smoothDeltaTime,        G, (void *)hk_smoothDeltaTime },
    { "get_timeScale",              RVA_Time_get_timeScale,              G, (void *)hk_timeScale },
    { "get_frameCount",             RVA_Time_get_frameCount,             G, (void *)hk_frameCount },
    { "get_renderedFrameCount",     RVA_Time_get_renderedFrameCount,     G, (void *)hk_renderedFrameCount },
    { "get_realtimeSinceStartup",   RVA_Time_get_realtimeSinceStartup,   G, (void *)hk_realtimeSinceStartup },
  };
  const int n = (int)(sizeof(H) / sizeof(H[0]));
  uintptr_t base = (uintptr_t)il2cpp->load_virtbase;

  g_base_ns = now_ns();
  g_prev_ns = g_base_ns;
  g_now_s   = 0.0f;

  int applied = 0;
  for (int i = 0; i < n; i++) {
    if (!so_rva_in_image(il2cpp, H[i].rva, 4)) {
      debugPrintf("[time] %-26s SKIP (rva 0x%06x past image 0x%zx)\n",
                  H[i].name, H[i].rva, il2cpp->load_size);
      continue;
    }
    uint32_t got = *(volatile uint32_t *)(base + H[i].rva);
    if (got != H[i].guard) {
      debugPrintf("[time] %-26s SKIP (guard %08x, found %08x)\n",
                  H[i].name, H[i].guard, got);
      continue;
    }
    hook_arm64(base + H[i].rva, (uintptr_t)H[i].fn);
    applied++;
  }
  g_installed = applied > 0;
  debugPrintf("[time] %d/%d UnityEngine.Time getters hooked "
              "(engine TimeManager is not ticking; driving Time from wall clock)\n",
              applied, n);
  return applied;
}

int   zb_time_installed(void) { return g_installed; }
float zb_time_now(void)       { return elapsed_s(); }
float zb_time_delta(void)     { return g_delta; }
int32_t zb_time_frames(void)  { return g_frames; }
