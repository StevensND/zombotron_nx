/* zombotron_touchhook.h -- Switch pointer -> UnityEngine.Input mouse API.
 * See zombotron_touchhook.c for why the mouse path rather than the touch path. */
#ifndef ZOMBOTRON_TOUCHHOOK_H
#define ZOMBOTRON_TOUCHHOOK_H

#include <stdint.h>
#include "so_util.h"

/* UnityEngine.Input mouse/touch getters -- same class as GetKey (0x32d2xxx),
 * verified against dump.cs (line 632020+) and the first instruction of each in
 * libil2cpp.so. These back the touch hooks; with pointer input disabled they
 * install the DENY-mode stubs that report no mouse and no touch to the game. */
#define RVA_Input_GetMouseButton      0x32d2218u
#define RVA_Input_GetMouseButtonDown  0x32d2254u
#define RVA_Input_GetMouseButtonUp    0x32d2290u
#define RVA_Input_get_mousePosition   0x32d26d8u
#define RVA_Input_get_mousePresent    0x32d2ad8u
#define RVA_Input_get_touchSupported  0x32d2b44u
#define RVA_Input_get_touchCount      0x32d2bb0u

#define GUARD_Input_str_prologue      0xf81e0ffeu  /* str x30,[sp,#-0x20]!      */
#define GUARD_Input_stp_prologue      0xa9bf4ffeu  /* stp x30,x19,[sp,#-0x10]!  */
#define GUARD_Input_get_mousePosition 0xd10083ffu  /* sub sp, sp, #0x20         */

int  zb_touchhook_install(so_module *il2cpp);

/* Sample the pointer. Call once per frame, AFTER zb_pump_input. */
void zb_touchhook_tick(void);

#endif /* ZOMBOTRON_TOUCHHOOK_H */
