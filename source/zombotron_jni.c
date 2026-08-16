/* zombotron_jni.c -- Java classes Rewired reaches through AndroidJavaObject.
 *
 * WHAT THIS IS FOR
 * zombotron_il2cpp.c hooks UnityEngine.Input so a joystick EXISTS and reports
 * state. That is only half of it. Rewired's AndroidUnityInputHelper separately
 * queries the Android framework for device METADATA, and uses it to pick which
 * controller template to apply -- which is what decides whether Zombotron draws
 * Nintendo glyphs or Xbox ones, and which physical button lands on which action.
 *
 * The DEX confirms this cannot come from the app: classes.dex contains no
 * Rewired Java at all (4348 classes, all androidx / Unity / pairip). So Rewired
 * reaches the framework directly by name through AndroidJavaObject, and
 * global-metadata.dat names exactly what it asks for:
 *
 *     android.hardware.input.InputManager
 *     android.hardware.input.InputManager$InputDeviceListener
 *     android.view.InputDevice
 *     android.os.VibrationEffect
 *     getVendorId / getProductId / getDescriptor / getName
 *     registerInputDeviceListener / onInputDeviceAdded
 *
 * None of that existed anywhere in the substrate -- these classes fell through
 * to jni_fake.c's generic no-op stubs, which return 0/null. A null device list
 * is why Rewired would find nothing even with the Input hooks in place.
 *
 * THE VID/PID IS THE IMPORTANT PART
 * Rewired matches controllers against its hardware maps by vendor and product
 * id, not primarily by name. 0x057E/0x2009 is Nintendo / Switch Pro Controller,
 * which is what selects Rewired's Nintendo template -- and Zombotron already
 * ships the JOYSTICK_ELEMENT_NAME_NINTENDO_* prompt strings that template
 * refers to. Getting these two numbers right is what makes the game draw
 * A/B/X/Y/ZL/ZR correctly with no art work at all.
 *
 * MIT.
 */
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "util.h"
#include "config.h"
#include "zombotron_jni.h"

/* Must match jni_fake.c / unity_jni.c exactly. */
struct FakeID { uint32_t tag; char cls[96]; char name[64]; char sig[160]; };

extern void *jni_make_string(const char *utf);
extern void *jni_make_object(const char *label);

static int has(const char *h, const char *n) { return h && n && strstr(h, n) != NULL; }

/* ---------------------------------------------------------------- identity */

/* Nintendo Co., Ltd. / Switch Pro Controller, as reported over Bluetooth HID.
 * Joy-Cons are 0x2006 (L) and 0x2007 (R); the Pro Controller is the right thing
 * to claim because it covers every button Zombotron binds and maps cleanly in
 * both handheld and docked mode. */
#define ZB_VENDOR_ID   0x057E
#define ZB_PRODUCT_ID  0x2009
#define ZB_DEVICE_ID   1
#define ZB_DEVICE_NAME "Nintendo Switch Pro Controller"

/* android.view.InputDevice source bits. */
#define ASOURCE_KEYBOARD    0x00000101
#define ASOURCE_DPAD        0x00000201
#define ASOURCE_GAMEPAD     0x00000401
#define ASOURCE_JOYSTICK    0x01000010

#define ZB_SOURCES (ASOURCE_GAMEPAD | ASOURCE_JOYSTICK | ASOURCE_DPAD | ASOURCE_KEYBOARD)

/* ------------------------------------------------------------ handle types */

enum { CPJ_TAG = 0x43504a31 /* 'CPJ1' */ };
enum { CPJ_INPUTDEVICE, CPJ_VIBRATOR, CPJ_VIBEFFECT, CPJ_INTARRAY };

typedef struct {
  uint32_t tag;
  int      kind;
  int32_t  ints[4];      /* CPJ_INTARRAY payload / VibrationEffect ms+amplitude */
  int32_t  n;
} CpHandle;

static CpHandle *cpj_new(int kind) {
  CpHandle *h = calloc(1, sizeof *h);
  if (!h)
    return NULL;
  h->tag = CPJ_TAG;
  h->kind = kind;
  return h;
}

static int is_cpj(void *p, int kind) {
  CpHandle *h = p;
  return h && h->tag == CPJ_TAG && h->kind == kind;
}

/* ----------------------------------------------------------------- rumble */

static HidVibrationDeviceHandle g_vib[2];
static int g_vib_ready;

static void rumble_init(void) {
  if (g_vib_ready)
    return;
  /* Handheld and detached Joy-Cons use different device handles; try handheld
   * first and fall back, matching how the pad itself is configured. */
  Result rc = hidInitializeVibrationDevices(g_vib, 2, HidNpadIdType_Handheld,
                                            HidNpadStyleTag_NpadHandheld);
  if (R_FAILED(rc))
    rc = hidInitializeVibrationDevices(g_vib, 2, HidNpadIdType_No1,
                                       HidNpadStyleTag_NpadFullKey);
  g_vib_ready = R_SUCCEEDED(rc);
  debugPrintf("[jni] rumble %s\n", g_vib_ready ? "ready" : "unavailable");
}

/* amplitude is Android's 1..255, or -1 for "default". Switch wants 0.0..1.0
 * amplitudes at two frequency bands. */
static void rumble_set(int amplitude) {
  rumble_init();
  if (!g_vib_ready)
    return;
  float amp = (amplitude < 0) ? 0.5f : (float)amplitude / 255.0f;
  if (amp > 1.0f) amp = 1.0f;
  if (amp < 0.0f) amp = 0.0f;

  HidVibrationValue v[2];
  memset(v, 0, sizeof v);
  for (int i = 0; i < 2; i++) {
    v[i].amp_low   = amp;
    v[i].freq_low  = 160.0f;
    v[i].amp_high  = amp;
    v[i].freq_high = 320.0f;
  }
  hidSendVibrationValues(g_vib, v, 2);
}

static void rumble_stop(void) {
  rumble_init();
  if (!g_vib_ready)
    return;
  HidVibrationValue v[2];
  memset(v, 0, sizeof v);
  for (int i = 0; i < 2; i++) {
    v[i].freq_low = 160.0f;
    v[i].freq_high = 320.0f;
  }
  hidSendVibrationValues(g_vib, v, 2);
}

/* Stop rumble once its requested duration has elapsed. Android's vibrate(ms) is
 * fire-and-forget with an implicit stop; Switch's is level-triggered and runs
 * until told otherwise, so without this a single buzz never ends. */
static uint64_t g_rumble_until_ns;

static uint64_t now_ns(void) {
  return armTicksToNs(armGetSystemTick());
}

void zb_jni_rumble_tick(void) {
  if (g_rumble_until_ns && now_ns() >= g_rumble_until_ns) {
    g_rumble_until_ns = 0;
    rumble_stop();
  }
}

static void rumble_for(int ms, int amplitude) {
  if (ms <= 0) {
    g_rumble_until_ns = 0;
    rumble_stop();
    return;
  }
  rumble_set(amplitude);
  g_rumble_until_ns = now_ns() + (uint64_t)ms * 1000000ull;
}

int zb_jni_input_device_ids(int32_t *out, int max);

/* ------------------------------------------------------- class ownership */

int zombotron_owns_class(const char *cls) {
  if (!cls)
    return 0;
  return has(cls, "hardware/input/InputManager") ||
         has(cls, "view/InputDevice") ||
         has(cls, "os/Vibrator") ||
         has(cls, "os/VibrationEffect");
}

/* ------------------------------------------------------ object dispatch */

void *zombotron_dispatch_object(void *recv, const void *id_, va_list va) {
  const struct FakeID *id = id_;
  const char *cls = id->cls, *m = id->name;
  (void)recv;

  /* InputManager.getInputDevice(id) -> InputDevice
   * Rewired calls this for each id from getInputDeviceIds(). */
  if (has(cls, "hardware/input/InputManager")) {
    /* Order matters: "getInputDeviceIds" also contains "getInputDevice". */
    if (has(m, "getInputDeviceIds")) {
      int32_t ids[4];
      int n = zb_jni_input_device_ids(ids, 4);
      debugPrintf("[jni] InputManager.getInputDeviceIds -> %d device(s)\n", n);
      return jni_make_intarray(ids, n);
    }
    if (has(m, "getInputDevice")) {
      debugPrintf("[jni] InputManager.getInputDevice -> %s (%04x:%04x)\n",
                  ZB_DEVICE_NAME, ZB_VENDOR_ID, ZB_PRODUCT_ID);
      return cpj_new(CPJ_INPUTDEVICE);
    }
    return NULL;
  }

  if (has(cls, "view/InputDevice")) {
    /* Static InputDevice.getDevice(id) reaches here too. */
    if (has(m, "getDevice") && !has(m, "getDeviceId"))
      return cpj_new(CPJ_INPUTDEVICE);
    if (has(m, "getName"))
      return jni_make_string(ZB_DEVICE_NAME);
    /* Rewired keys its per-device persistence on this; it must be stable across
     * runs or remaps are lost every boot. */
    if (has(m, "getDescriptor"))
      return jni_make_string("zombotron_nx:switch:pro:0");
    if (has(m, "getVibrator"))
      return cpj_new(CPJ_VIBRATOR);
    /* getMotionRange(...) -> null. Rewired treats a missing range as "use the
     * template default", which is what we want; fabricating ranges risks
     * disagreeing with the template's own axis calibration. */
    if (has(m, "getMotionRange"))
      return NULL;
    return NULL;
  }

  if (has(cls, "os/VibrationEffect")) {
    /* createOneShot(long ms, int amplitude) / createWaveform(...) */
    if (has(m, "createOneShot")) {
      CpHandle *h = cpj_new(CPJ_VIBEFFECT);
      if (h) {
        h->ints[0] = (int32_t)va_arg(va, long long);
        h->ints[1] = va_arg(va, int);
        h->n = 2;
      }
      return h;
    }
    if (has(m, "createPredefined") || has(m, "createWaveform")) {
      CpHandle *h = cpj_new(CPJ_VIBEFFECT);
      if (h) { h->ints[0] = 40; h->ints[1] = -1; h->n = 2; }
      return h;
    }
    return NULL;
  }

  if (has(cls, "os/Vibrator"))
    return NULL;

  return NULL;
}

/* --------------------------------------------------------- int dispatch */

uint64_t zombotron_dispatch_int(void *recv, const void *id_, va_list va) {
  const struct FakeID *id = id_;
  const char *cls = id->cls, *m = id->name;
  (void)va;

  if (has(cls, "view/InputDevice")) {
    if (has(m, "getVendorId"))        return ZB_VENDOR_ID;
    if (has(m, "getProductId"))       return ZB_PRODUCT_ID;
    if (has(m, "getSources"))         return ZB_SOURCES;
    if (has(m, "getId"))              return ZB_DEVICE_ID;
    if (has(m, "getControllerNumber")) return 1;
    if (has(m, "isVirtual"))          return 0;
    if (has(m, "supportsSource"))     return 1;
    if (has(m, "getKeyboardType"))    return 0;   /* KEYBOARD_TYPE_NONE */
    return 0;
  }

  if (has(cls, "os/Vibrator")) {
    if (has(m, "hasVibrator"))            return 1;
    if (has(m, "hasAmplitudeControl"))    return 1;
    return 0;
  }

  if (has(cls, "hardware/input/InputManager"))
    return 0;

  (void)recv;
  return 0;
}

/* -------------------------------------------------------- void dispatch */

void zombotron_dispatch_void(void *recv, const void *id_, va_list va) {
  const struct FakeID *id = id_;
  const char *cls = id->cls, *m = id->name;

  if (has(cls, "os/Vibrator")) {
    if (has(m, "cancel")) {
      rumble_for(0, 0);
      return;
    }
    if (has(m, "vibrate")) {
      /* Two shapes reach here:
       *   vibrate(long milliseconds)
       *   vibrate(VibrationEffect effect)
       * The signature string tells them apart without guessing at varargs. */
      if (has(id->sig, "Landroid/os/VibrationEffect;")) {
        void *eff = va_arg(va, void *);
        if (is_cpj(eff, CPJ_VIBEFFECT)) {
          CpHandle *h = eff;
          rumble_for(h->ints[0], h->ints[1]);
        } else {
          rumble_for(40, -1);
        }
      } else {
        long long ms = va_arg(va, long long);
        rumble_for((int)ms, -1);
      }
      return;
    }
    return;
  }

  /* registerInputDeviceListener / unregisterInputDeviceListener.
   * Deliberately a no-op that does NOT store the listener: our device set never
   * changes, so there is nothing to notify, and calling back into managed code
   * from here would mean entering the runtime off a JNI stub. Rewired polls
   * GetJoystickNames anyway (once a second), which is the path that actually
   * discovers the pad. */
  if (has(cls, "hardware/input/InputManager")) {
    if (has(m, "InputDeviceListener"))
      debugPrintf("[jni] InputManager.%s ignored (device set is static)\n", m);
    return;
  }

  (void)recv;
}

/* ------------------------------------------------------------- int array */

/* InputManager.getInputDeviceIds() -> int[].
 * jni_fake.c owns array construction, so this is exposed for it to call rather
 * than being built here. Returns the count and fills `out`. */
int zb_jni_input_device_ids(int32_t *out, int max) {
  if (max < 1)
    return 0;
  out[0] = ZB_DEVICE_ID;
  return 1;
}
