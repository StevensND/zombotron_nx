/* zombotron_screen.c -- make UnityEngine.Screen report the real render size.
 *
 * WHY: the 100% CPU hang is an infinite loop in the game's RenderingMaster.
 *
 * The watchdog caught UnityMain spinning at libil2cpp+0x29ca0d4 with LR only
 * 0x14 bytes earlier -- a ~20-byte loop:
 *
 *     scale = step;                       // step = 0.005f
 *     do {
 *         scale += step;
 *         if (scale * baseW >= Screen.get_width())  break;
 *         if (scale * baseH >= Screen.get_height()) break;
 *     } while (true);
 *
 * (0x55a6c64 and 0x55a6c8c resolve to Screen::get_width / get_height, and the
 * game logs "RenderingMaster: While resolution cannot be changed on this
 * platform, we are scaling the render texture and its raw image up to the max
 * resolution available!" immediately before hanging.)
 *
 * If baseW or baseH is 0 then scale*0 == 0 is always below the limit and the
 * loop never terminates -- burning one core at 100% forever, which is exactly
 * what was observed. And the engine already told us the base was zero:
 * "RenderTexture.Create failed: Texture must have width greater than 0."
 *
 * So the loop is a symptom. The disease is a screen dimension arriving as 0.
 * These hooks make the three Screen accessors report the size the port actually
 * renders at, which both fixes the source and bounds the loop.
 *
 * Screen.width/height come from the engine's surface, which IS correct
 * (nwindow reports 1280x720). currentResolution is the suspect: it is served by
 * get_currentResolution_Injected(out Resolution), which fills a struct from
 * Android display metrics -- a path the fake JNI answers, and one that has
 * demonstrably been returning zeros elsewhere ("API Level 0", "Device Model ' '").
 *
 * MIT.
 */
#include <stdint.h>
#include <string.h>

#include "so_util.h"
#include "util.h"
#include "config.h"
#include "zombotron_screen.h"

/* UnityEngine.Resolution: { int m_Width; int m_Height; RefreshRate m_RefreshRate; }
 * and RefreshRate is { uint m_Numerator; uint m_Denominator; } in Unity 6. */
typedef struct { int32_t w, h; uint32_t num, den; } UnityResolution;

static int32_t sw(void) { return screen_width  > 0 ? screen_width  : ZB_FORCE_SCREEN_W; }
static int32_t sh(void) { return screen_height > 0 ? screen_height : ZB_FORCE_SCREEN_H; }

static int32_t hk_get_width(void)  { return sw(); }
static int32_t hk_get_height(void) { return sh(); }

/* UnityEngine.Display.main.systemWidth/systemHeight. These are what
 * SettingsData.ResolutionDesired{Width,Height}Get() fall back on in the "auto"
 * resolution mode the game selects at boot, and they are the source of the zero
 * that made RenderTexture.Create fail and the scale loop spin. Instance methods
 * returning int, so ignoring `this` and returning a constant is ABI-safe. */
static int32_t hk_get_systemWidth(void)  { return sw(); }
static int32_t hk_get_systemHeight(void) { return sh(); }

static void hk_get_currentResolution_Injected(UnityResolution *out) {
  if (!out) return;
  out->w = sw();
  out->h = sh();
  out->num = 60;      /* 60/1 Hz -- a zero refresh rate is its own hazard */
  out->den = 1;
}

/* Neutralise the loop itself.
 *
 * The Screen hooks fix the loop's LIMITS but not its BASE: the watchdog dump
 * after installing them shows UnityMain still spinning, now with
 * PC inside our own get_height hook and LR at the loop's call site -- i.e. the
 * hooks run correctly and the loop still cannot exit, because
 * scale * 0 never reaches any positive limit.
 *
 * The base comes from a render texture the engine already refused to create
 * ("RenderTexture.Create failed: Texture must have width greater than 0"), so
 * there is no value to repair at this level. Panik.RenderingMaster's whole
 * purpose here is to scale a render target "up to the max resolution available"
 * on a platform whose resolution it believes it cannot set -- which on this port
 * is exactly what the forced 1280x720 surface already does.
 *
 * So the method is made a no-op: `private void _RenderingRefresh(bool)`, so an
 * immediate return is ABI-safe (no return value, caller restores its own state).
 * The game keeps whatever render setup it already had.
 *
 * If this produces a black screen rather than a running game, the next step is
 * to find where its base dimensions are read and fix THAT -- but a black screen
 * is diagnosable and an infinite loop is not. */
static const uint32_t RET_STUB[2] = { 0xd65f03c0u, 0xd503201fu };  /* ret ; nop */

typedef struct { const char *name; uint32_t rva, guard; void *fn; } ScreenHook;

int zb_screen_install(so_module *il2cpp) {
  const ScreenHook H[] = {
    { "get_width",  RVA_Screen_get_width,  GUARD_Screen_stp, (void *)hk_get_width  },
    { "get_height", RVA_Screen_get_height, GUARD_Screen_stp, (void *)hk_get_height },
    { "get_currentResolution_Injected", RVA_Screen_get_currentResolution_Injected,
      GUARD_Screen_str, (void *)hk_get_currentResolution_Injected },
    { "Display::get_systemWidth",  RVA_Display_get_systemWidth,
      GUARD_Display_sub, (void *)hk_get_systemWidth  },
    { "Display::get_systemHeight", RVA_Display_get_systemHeight,
      GUARD_Display_sub, (void *)hk_get_systemHeight },
  };
  const int n = (int)(sizeof(H) / sizeof(H[0]));
  uintptr_t base = (uintptr_t)il2cpp->load_virtbase;
  int applied = 0;
  for (int i = 0; i < n; i++) {
    if (!so_rva_in_image(il2cpp, H[i].rva, 4)) {
      debugPrintf("[screen] %-34s SKIP (rva 0x%06x past image 0x%zx)\n",
                  H[i].name, H[i].rva, il2cpp->load_size);
      continue;
    }
    uint32_t got = *(volatile uint32_t *)(base + H[i].rva);
    if (got != H[i].guard) {
      debugPrintf("[screen] %-34s SKIP (guard %08x, found %08x)\n",
                  H[i].name, H[i].guard, got);
      continue;
    }
    hook_arm64(base + H[i].rva, (uintptr_t)H[i].fn);
    applied++;
  }
  debugPrintf("[screen] %d/%d hooks applied -- Screen reports %dx%d @60Hz\n",
              applied, n, sw(), sh());

  /* And stub the scale loop's owner -- ONLY if the base dimensions are still
   * broken. With the Display hooks above in place they are not: the loop's base
   * is the render texture built from ResolutionDesired{Width,Height}Get(), which
   * now resolve to the real render size instead of 0, so the loop terminates on
   * its first iteration and the method does its actual job.
   *
   * That job is not optional. _RenderingRefresh is what creates
   * renderTextureCurrent, assigns it to renderingRawImage, and calls
   * UpdateRenderingTexture() on CameraGame, CameraUi and CameraUiGlobal. With it
   * stubbed the game runs perfectly and draws nothing -- "UI Camera
   * RenderTexture not found", then a black screen, which is exactly what the
   * stub's own comment predicted would happen. */
#if ZB_STUB_RENDERING_REFRESH
  {
    uintptr_t a = base + RVA_RenderingMaster__RenderingRefresh;
    uint32_t got = so_rva_in_image(il2cpp, RVA_RenderingMaster__RenderingRefresh, 8)
                     ? *(volatile uint32_t *)a : 0;
    if (got != GUARD_RenderingRefresh) {
      debugPrintf("[screen] _RenderingRefresh SKIP (guard %08x, found %08x) -- "
                  "the scale loop is LIVE and will hang\n",
                  GUARD_RenderingRefresh, got);
    } else if (so_patch_code((void *)a, RET_STUB, sizeof RET_STUB) < 0) {
      debugPrintf("[screen] _RenderingRefresh patch FAILED\n");
    } else {
      debugPrintf("[screen] Panik.RenderingMaster::_RenderingRefresh stubbed "
                  "(non-terminating scale loop removed) -- EXPECT A BLACK SCREEN: "
                  "this also skips render-texture creation and camera binding\n");
      applied++;
    }
  }
#else
  (void)RET_STUB;
  debugPrintf("[screen] _RenderingRefresh LEFT LIVE -- its scale loop is bounded "
              "by the Display hooks above; set ZB_STUB_RENDERING_REFRESH to 1 to "
              "restore the stub if it spins again\n");
#endif
  return applied;
}
