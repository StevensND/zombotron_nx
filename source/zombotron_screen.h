/* zombotron_screen.h -- UnityEngine.Screen reports the real render size.
 * See zombotron_screen.c: a zero screen dimension makes the game's
 * RenderingMaster scaling loop spin forever at 100% CPU. */
#ifndef ZOMBOTRON_SCREEN_H
#define ZOMBOTRON_SCREEN_H
#include "so_util.h"

/* UnityEngine.Screen, Zombotron 1.4.8 (Il2CppDumper, verified against libil2cpp). */
#define RVA_Screen_get_width                       0x55a6c64u
#define RVA_Screen_get_height                      0x55a6c8cu
#define RVA_Screen_get_currentResolution_Injected  0x55a6e80u
#define GUARD_Screen_stp                           0xa9bf4ffeu  /* stp x30,x19,[sp,#-0x10]! */
#define GUARD_Screen_str                           0xf81e0ffeu  /* str x30,[sp,#-0x20]!     */

/* UnityEngine.Display -- the OTHER screen-size API, and the one that actually
 * fed the zero. Screen.* was hooked long ago; Display.main.systemHeight was not,
 * which is exactly why "the Screen hooks were necessary but not sufficient".
 *
 *   RenderingMaster._RenderingRefresh builds the render texture as
 *       new RenderTexture(_renderTextureDivision * SettingsData.ResolutionDesiredWidthGet(),
 *                         _renderTextureDivision * SettingsData.ResolutionDesiredHeightGet(), 24)
 *   _renderTextureDivision is initialised to 1.0f in RenderingMaster..ctor
 *   (0x29cadf0: mov w9,#0x3f800000 ; str w9,[x0,#0x50]), so it is not the zero.
 *   ResolutionDesiredHeightGet (0x29a1ff8) is a switch on the resolution mode
 *   returning either a hardcoded constant -- 360/480/720/1080/1440/2160 -- or,
 *   in the default and "auto" branches, Display.main.systemHeight. Width is
 *   derived from height by aspect ratio. By exhaustion the only value in that
 *   chain that can be 0 is Display.main.systemHeight, and the boot log says
 *   "Detected Update to 1.3.2 (and Up) Set Resolution to Auto" -- the branch
 *   that reads it. */
#define RVA_Display_get_systemWidth                0x55a6328u
#define RVA_Display_get_systemHeight               0x55a6410u
#define GUARD_Display_sub                          0xd100c3ffu  /* sub sp, sp, #0x30 */

/* Panik.RenderingMaster::_RenderingRefresh(bool) -- contains the non-terminating
 * scale loop. `private void`, so returning immediately is ABI-safe. */
#define RVA_RenderingMaster__RenderingRefresh 0x29c9bacu
#define GUARD_RenderingRefresh                0xd10383ffu  /* sub sp, sp, #0xe0 */

int zb_screen_install(so_module *il2cpp);
#endif
