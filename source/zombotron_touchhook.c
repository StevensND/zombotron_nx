/* zombotron_touchhook.c -- expose the Switch touchscreen to the game.
 *
 * WHY NOT nativeInjectEvent
 * The original plan was to synthesise Android MotionEvent jobjects and feed
 * nativeInjectEvent. That needs a stateful fake MotionEvent in jni_fake.c with
 * every getter Unity calls back into (getActionMasked, getPointerCount,
 * getPointerId, getX, getY, getEventTime, getSource...), and a malformed one
 * faults inside libunity. Meanwhile the il2cpp hook technique is already proven
 * on this exact game: the joystick hooks made Rewired report
 * "Input kind changed to : Joystick" on hardware. So touch uses the same route.
 *
 * WHAT IS HOOKED, AND WHY THIS SHAPE
 * The pointer is presented as a MOUSE rather than as a touchscreen:
 *
 *   get_mousePresent    -> true
 *   get_mousePosition   -> current pointer, in Unity screen space
 *   GetMouseButton/Down/Up(0) -> pointer down / pressed this frame / released
 *   get_touchSupported  -> false
 *   get_touchCount      -> 0
 *
 * Unity's EventSystem falls back to the mouse path when touch is unsupported,
 * and every uGUI button, slider and drag handler works off that path. Reporting
 * touchSupported=true instead would require GetTouch(int) to return a `Touch`
 * STRUCT BY VALUE -- an sret call whose ABI must match exactly, and whose Touch
 * layout is version-specific. Getting that wrong corrupts the caller's stack.
 * The mouse path needs none of that and covers the same UI.
 *
 * The source data is zombotron_touch.c, fed by android_native_feed_hid ->
 * nx_pointer.c, which already merges touchscreen, stick cursor, gyro and USB
 * mouse into one pointer stream in render space with a bottom-left origin --
 * exactly what get_mousePosition wants.
 *
 * MIT.
 */
#include <stdint.h>
#include <string.h>

#include "so_util.h"
#include "util.h"
#include "config.h"
#include "zombotron_touch.h"
#include "zombotron_touchhook.h"

/* UnityEngine.Vector3 is returned in the first three FP registers. */
typedef struct { float x, y, z; } V3;

static float g_x, g_y;
static int   g_down, g_down_prev;

void zb_touchhook_tick(void) {
#if !ZB_ENABLE_POINTER_INPUT
  return;   /* not called in gamepad-only mode; kept so the file still links */
#else
  zb_touch t[NX_MAX_TOUCH];
  int n = zb_touch_poll(t, NX_MAX_TOUCH);

  g_down_prev = g_down;
  g_down = 0;
  for (int i = 0; i < n; i++) {
    /* An UP entry is reported for exactly one frame after release, so it must
     * not count as "still down" -- otherwise a tap never produces a ButtonUp. */
    if (t[i].phase == ZB_TOUCH_UP)
      continue;
    g_x = t[i].x;
    g_y = t[i].y;
    g_down = 1;
    break;                    /* primary pointer only */
  }
#endif
}

/* With ZB_ENABLE_POINTER_INPUT=0 these do not merely go quiet -- they actively
 * report "no mouse, no touch, nothing pressed". That is the point of still
 * installing them: an unhooked get_touchSupported would return the engine's
 * native Android answer (true), and the game's UI would then wait for touches
 * that can never arrive. Denying is not the same as not answering. */
#if ZB_ENABLE_POINTER_INPUT
static V3      hk_mousePosition(void)    { V3 v; v.x = g_x; v.y = g_y; v.z = 0.0f; return v; }
static uint8_t hk_mousePresent(void)     { return 1; }
static uint8_t hk_GetMouseButton(int b)     { return (b == 0 && g_down) ? 1 : 0; }
static uint8_t hk_GetMouseButtonDown(int b) { return (b == 0 && g_down && !g_down_prev) ? 1 : 0; }
static uint8_t hk_GetMouseButtonUp(int b)   { return (b == 0 && !g_down && g_down_prev) ? 1 : 0; }
#else
static V3      hk_mousePosition(void)    { V3 v; v.x = 0.0f; v.y = 0.0f; v.z = 0.0f; return v; }
static uint8_t hk_mousePresent(void)     { return 0; }
static uint8_t hk_GetMouseButton(int b)     { (void)b; return 0; }
static uint8_t hk_GetMouseButtonDown(int b) { (void)b; return 0; }
static uint8_t hk_GetMouseButtonUp(int b)   { (void)b; return 0; }
#endif
static uint8_t hk_touchSupported(void)   { return 0; }
static int32_t hk_touchCount(void)       { return 0; }

typedef struct { const char *name; uint32_t rva, guard; void *fn; } TouchHook;

int zb_touchhook_install(so_module *il2cpp) {
  const TouchHook H[] = {
    { "get_mousePosition",   RVA_Input_get_mousePosition,  GUARD_Input_get_mousePosition,  (void *)hk_mousePosition },
    { "get_mousePresent",    RVA_Input_get_mousePresent,   GUARD_Input_stp_prologue,       (void *)hk_mousePresent },
    { "get_touchSupported",  RVA_Input_get_touchSupported, GUARD_Input_stp_prologue,       (void *)hk_touchSupported },
    { "get_touchCount",      RVA_Input_get_touchCount,     GUARD_Input_stp_prologue,       (void *)hk_touchCount },
    { "GetMouseButton",      RVA_Input_GetMouseButton,     GUARD_Input_str_prologue,       (void *)hk_GetMouseButton },
    { "GetMouseButtonDown",  RVA_Input_GetMouseButtonDown, GUARD_Input_str_prologue,       (void *)hk_GetMouseButtonDown },
    { "GetMouseButtonUp",    RVA_Input_GetMouseButtonUp,   GUARD_Input_str_prologue,       (void *)hk_GetMouseButtonUp },
  };
  const int n = (int)(sizeof(H) / sizeof(H[0]));
  uintptr_t base = (uintptr_t)il2cpp->load_virtbase;
  int applied = 0;
  for (int i = 0; i < n; i++) {
    if (!so_rva_in_image(il2cpp, H[i].rva, 4)) {
      debugPrintf("[touch] %-20s SKIP (rva 0x%06x past image 0x%zx)\n",
                  H[i].name, H[i].rva, il2cpp->load_size);
      continue;
    }
    uint32_t got = *(volatile uint32_t *)(base + H[i].rva);
    if (got != H[i].guard) {
      debugPrintf("[touch] %-20s SKIP (guard %08x, found %08x)\n",
                  H[i].name, H[i].guard, got);
      continue;
    }
    hook_arm64(base + H[i].rva, (uintptr_t)H[i].fn);
    applied++;
  }
#if ZB_ENABLE_POINTER_INPUT
  debugPrintf("[touch] %d/%d pointer hooks applied (touchscreen presented as mouse)\n",
              applied, n);
#else
  debugPrintf("[touch] %d/%d hooks applied -- DENY mode: mousePresent=0, "
              "touchSupported=0, touchCount=0, no buttons (gamepad only)\n",
              applied, n);
#endif
  return applied;
}
