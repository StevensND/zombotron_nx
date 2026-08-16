/* zombotron_jni.h -- Java classes Rewired reaches through AndroidJavaObject.
 *
 * Slots into jni_fake.c's dispatch chain ahead of unity_jni.c, on the same
 * contract: claim a class by name, then answer its calls by return kind.
 *
 * Covers android.hardware.input.InputManager, android.view.InputDevice,
 * android.os.Vibrator and android.os.VibrationEffect -- none of which the
 * inherited substrate handled, so they were falling through to generic no-op
 * stubs that return 0/null. For Rewired that reads as "no controller attached",
 * which no amount of UnityEngine.Input hooking can fix on its own.
 *
 * See zombotron_jni.c for why the vendor/product id matters more than the name.
 */
#ifndef ZOMBOTRON_JNI_H
#define ZOMBOTRON_JNI_H

#include <stdarg.h>
#include <stdint.h>

int  zombotron_owns_class(const char *cls);

void     *zombotron_dispatch_object(void *recv, const void *id, va_list va);
uint64_t  zombotron_dispatch_int   (void *recv, const void *id, va_list va);
void      zombotron_dispatch_void  (void *recv, const void *id, va_list va);

/* Stop rumble once its requested duration expires. Android's vibrate(ms) is
 * fire-and-forget with an implicit stop; the Switch's is level-triggered and
 * runs until told otherwise, so this must be called once per frame or a single
 * buzz never ends. Called from the render loop. */
void zb_jni_rumble_tick(void);

/* provided by jni_fake.c */
extern void *jni_make_intarray(const int32_t *vals, int n);

#endif /* ZOMBOTRON_JNI_H */
