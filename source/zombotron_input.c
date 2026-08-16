/* zombotron_input.c -- Switch HID -> Unity input.
 *
 * STATUS: the sampling half is real; the injection half is NOT implemented. The
 * game will boot and render with this file as-is and will not respond to input.
 * See PORT_PLAN milestone [I].
 *
 * WHY THIS DOES NOT SAMPLE HID ITSELF.
 * An earlier draft of this file called padUpdate/hidGetTouchScreenStates
 * directly. That was wrong twice over: it duplicated work, and it was strictly
 * worse than what the substrate already does. android_native_feed_hid()
 * (android_native_unity.c) drives nx_pointer.c, which merges the touchscreen,
 * the analogue-stick cursor, gyro aiming and a USB mouse into ONE pointer
 * stream, maps it into render space, flips to Unity's bottom-left origin, and
 * hands the result to the slot tracker in zombotron_touch.c. That path was
 * orphaned when main.c was rewritten -- nothing called it. This file calls it,
 * and reads the tracked state back out.
 *
 * WHY NOT THE badpiggies_nx APPROACH.
 * Bad Piggies hooked UnityEngine.Input.get_touchCount / GetMouseButton / GetTouch
 * at known il2cpp RVAs. Zombotron ships Rewired (Rewired_Android.dll and
 * Rewired_Core.dll are in its assembly list), which reads through its own device
 * abstraction, so hooking UnityEngine.Input can miss the path the game actually
 * consults. The engine-level route is nativeInjectEvent, which is what the Java
 * UnityPlayer calls for every real touch and key event.
 *
 * WHAT IS LEFT TO DO.
 * nativeInjectEvent(env, thiz, InputEvent, deviceId) takes a Java jobject that
 * Unity then queries back through JNI -- getActionMasked, getPointerCount,
 * getPointerId, getX, getY, getSource, getKeyCode, getEventTime... So the
 * remaining work is a stateful fake MotionEvent/KeyEvent in jni_fake.c whose
 * getters answer from the state below, the same trick unity_jni.c already uses
 * for InputStream. Source bits for the pads should be
 * SOURCE_GAMEPAD | SOURCE_JOYSTICK so Unity enumerates a controller through its
 * normal device path and Rewired picks it up; Zombotron is gamepad-native on
 * desktop, so that may be all it needs.
 *
 * MIT.
 */
#include <stdint.h>
#include <string.h>
#include <switch.h>

#include "util.h"
#include "config.h"
#include "android_native_unity.h"
#include "zombotron_touch.h"
#include "zombotron_entrypoints.h"
#include "zombotron_il2cpp.h"
#include "zombotron_touchhook.h"
#include "opensles.h"   /* zb_audio_stats: audio pipeline counters */

/* android.view.InputDevice source bits, for when the events get built. */
#define ASOURCE_KEYBOARD     0x00000101
#define ASOURCE_GAMEPAD      0x00000401
#define ASOURCE_TOUCHSCREEN  0x00001002
#define ASOURCE_JOYSTICK     0x01000010

/* Device ids handed to nativeInjectEvent. Any stable nonzero pair works; Unity
 * uses them to bucket events per physical device. */
#define ZB_DEVICE_PAD        1
#define ZB_DEVICE_TOUCH      2

static uint64_t g_buttons_prev;
static int      g_warned;

/* Snapshot of the pad, kept so the injection code can diff edges when it lands. */
uint64_t zb_input_buttons(void)      { return g_buttons_prev; }

void zb_pump_input(void *env, void *thiz, fn_inject inject) {
#if ZB_ENABLE_POINTER_INPUT
  /* Drives nx_pointer -> zombotron_touch (touchscreen + stick cursor + gyro +
   * USB mouse, merged into one pointer stream). */
  android_native_feed_hid();

  zb_touch touches[NX_MAX_TOUCH];
  int ntouch = zb_touch_poll(touches, NX_MAX_TOUCH);
#else
  int ntouch = 0;
#endif

  /* Pad -> il2cpp hooks. android_native_feed_hid already called padUpdate on the
   * substrate's own PadState, so re-reading here would double-update; use a
   * separate handle that only samples, never advances. */
  static PadState pad;
  static int pad_ready;
  if (!pad_ready) { padInitializeDefault(&pad); pad_ready = 1; }
  padUpdate(&pad);
  uint64_t buttons = padGetButtons(&pad);
  HidAnalogStickState l = padGetStickPos(&pad, 0);
  HidAnalogStickState r = padGetStickPos(&pad, 1);

  /* Hand the pad to the UnityEngine.Input hooks, which is what Rewired reads.
   * Sticks normalise to -1..1 from libnx's +/-32767 range. */
  zb_pad_set(buttons,
             l.x / 32767.0f, l.y / 32767.0f,
             r.x / 32767.0f, r.y / 32767.0f);

  /* Audio pipeline counters, from the MAIN thread. The counters themselves are
   * bumped on the SDL audio and FMOD mixer threads, which must never touch the
   * log -- doing so is what stopped the console booting two builds ago. Printed
   * every 300 frames (~5 s) so a single run separates duplication from underrun
   * without another speculative change. */
  {
    static unsigned tick;
    if ((tick++ % 300u) == 0u) {
      char st[96];
      zb_audio_stats(st, sizeof st);
      debugPrintf("[fmod] %s\n", st);
    }
  }

  uint64_t changed = buttons ^ g_buttons_prev;
  g_buttons_prev = buttons;

  if (!inject)
    return;

  /* ------------------------------------------------------------------
   * TODO [I]: build Java InputEvent objects and inject them.
   *
   *   for each finger in touches[0..ntouch):
   *       jobject ev = jni_make_motion_event(touches[i].phase, touches, ntouch,
   *                                          ASOURCE_TOUCHSCREEN);
   *       inject(env, thiz, ev, ZB_DEVICE_TOUCH);
   *
   *   for each changed pad button:
   *       jobject ev = jni_make_key_event(keycode, down ? 0 : 1,
   *                                       ASOURCE_GAMEPAD | ASOURCE_JOYSTICK);
   *       inject(env, thiz, ev, ZB_DEVICE_PAD);
   *
   * jni_make_motion_event / jni_make_key_event do not exist yet. Until they do,
   * this deliberately injects nothing: handing the engine a malformed jobject
   * faults inside libunity, which is a far worse failure than no input.
   * ------------------------------------------------------------------ */
  (void)env; (void)thiz; (void)changed; (void)ntouch;

#if ZB_ENABLE_POINTER_INPUT
  zb_touchhook_tick();   /* pointer -> UnityEngine.Input mouse API */
#endif
  zb_il2cpp_frame_end();   /* latch prev-buttons for GetKeyDown/GetKeyUp edges */

  if (!g_warned) {
    g_warned = 1;
#if ZB_ENABLE_POINTER_INPUT
    debugPrintf("[input] pad + pointer wired via UnityEngine.Input hooks "
                "(pointer presented as mouse; see zombotron_touchhook.c)\n");
#else
    debugPrintf("[input] GAMEPAD ONLY -- touch, stick-cursor, gyro and mouse "
                "disabled (ZB_ENABLE_POINTER_INPUT=0); full pad goes to the game\n");
#endif
  }
}
