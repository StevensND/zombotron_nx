/* zombotron_entrypoints.h -- UnityPlayer native methods recovered from
 * libunity.so's RegisterNatives tables (ZOMBOTRON, Unity 6000.2.6f2, arm64).
 *
 * Derived mechanically by tools/extract_entrypoints.py: every R_AARCH64_RELATIVE
 * relocation is walked in 24-byte strides looking for the JNINativeMethod shape
 * { const char *name; const char *sig; void *fn; }. No guessing, no hand-typed
 * offsets -- if the game updates, re-run the tool.
 *
 * libunity.so links at vaddr 0, so these ARE RVAs:
 *     runtime_addr = unity_mod.load_virtbase + OFF_xxx
 *
 * PREFER resolving by NAME from the RegisterNatives capture in jni_fake.c and
 * using these only as a cross-check -- that is what the reference ports do and
 * it survives a game update. */
#ifndef ZOMBOTRON_ENTRYPOINTS_H
#define ZOMBOTRON_ENTRYPOINTS_H

#include <stdint.h>
#include "so_util.h"

#define OFF_JNI_OnLoad                        0x81440c /* exported; resolve by name */


/* ---- drive-critical lifecycle ---------------------------------- */
#define OFF_initJni                           0x00813220 /* (Landroid/content/Context;I)V */
#define OFF_nativeRecreateGfxState            0x008134ec /* (ILandroid/view/Surface;)V */
#define OFF_nativeSendSurfaceChangedEvent     0x00813554 /* ()V */
#define OFF_nativeUnityPlayerSetRunning       0x00814360 /* (Z)V */
#define OFF_nativeRender                      0x0081378c /* ()Z */
#define OFF_nativeResume                      0x008133d8 /* ()V */
#define OFF_nativePause                       0x00813374 /* ()Z */
#define OFF_nativeFocusChanged                0x00813488 /* (Z)V */
#define OFF_nativeInjectEvent                 0x008137ec /* (Landroid/view/InputEvent;I)Z */
#define OFF_nativeDone                        0x00813294 /* ()Z */
#define OFF_nativeApplicationUnload           0x00813438 /* ()V */
/* MISSING FROM THIS BUILD: nativeLowMemory */
#define OFF_nativeOrientationChanged          0x00814300 /* (II)V */
#define OFF_nativeConfigurationChanged        0x008136fc /* (Landroid/content/res/Configuration;)V */
#define OFF_nativeOnApplyWindowInsets         0x008135a8 /* (Landroid/view/WindowInsets;)V */
#define OFF_nativeViewPaddingChanged          0x00814298 /* (IIIII)V */
#define OFF_nativeMemoryUsageChanged          0x00813320 /* (I)V */

/* ---- native audio pump (FMOD output -> you supply the ByteBuffer)  */
#define OFF_fmodGetInfo                       0x011a2b58 /* (I)I */
#define OFF_fmodProcess                       0x011a2c20 /* (Ljava/nio/ByteBuffer;)I */
#define OFF_fmodProcessMicData                0x011a2cac /* (Ljava/nio/ByteBuffer;I)I */

/* ---- secondary ------------------------------------------------- */
#define OFF_nativeUnitySendMessage            0x00813dfc /* (Ljava/lang/String;Ljava/lang/String;[B)V */
#define OFF_nativeMuteMasterAudio             0x00814028 /* (Z)V */
#define OFF_nativeGetNoWindowMode             0x008143b4 /* ()Z */
#define OFF_nativeIsAutorotationOn            0x00813fc8 /* ()Z */
#define OFF_nativeSetLaunchURL                0x0081408c /* (Ljava/lang/String;)V */
#define OFF_nativeHidePreservedContent        0x00814248 /* ()V */
#define OFF_permissionResponseToNative        0x008141c4 /* (JZ)V */
#define OFF_onAudioVolumeChanged              0x0080fde8 /* (I)V */

/* ---- soft keyboard / IME --------------------------------------- */
#define OFF_nativeGetSoftInputType            0x0081e784 /* ()I */
#define OFF_nativeReportKeyboardConfigChanged 0x00813d5c /* ()V */
#define OFF_nativeSetInputArea                0x00813aac /* (IIII)V */
#define OFF_nativeSetInputSelection           0x00813c3c /* (II)V */
#define OFF_nativeSetInputString              0x00813b94 /* (Ljava/lang/String;)V */
#define OFF_nativeSetKeyboardIsVisible        0x00813b34 /* (Z)V */
#define OFF_nativeSoftInputCanceled           0x00813cac /* ()V */
#define OFF_nativeSoftInputClosed             0x00813da4 /* ()V */
#define OFF_nativeSoftInputLostFocus          0x00813d04 /* ()V */

/* ---- signatures: ret (*)(JNIEnv*, jobject thiz, args...) ---------------- */
typedef void     (*fn_initJni_u6)(void*, void*, void*, int32_t); /* 4-ARG in Unity 6 */
typedef void     (*fn_gfxstate)(void*, void*, int32_t, void*);
typedef void     (*fn_v)(void*, void*);
typedef uint8_t  (*fn_z)(void*, void*);
typedef void     (*fn_vz)(void*, void*, int32_t);
typedef uint8_t  (*fn_inject)(void*, void*, void*, int32_t);
typedef void     (*fn_orient)(void*, void*, int32_t, int32_t);
typedef void     (*fn_vobj)(void*, void*, void*);
typedef void     (*fn_pad5)(void*, void*, int32_t, int32_t, int32_t, int32_t, int32_t);
typedef int32_t  (*fn_fmodGetInfo)(void*, void*, int32_t);
typedef int32_t  (*fn_fmodProcess)(void*, void*, void* /*java.nio.ByteBuffer*/);

#define UNITY_RESOLVE(mod, off) ((void *)((uintptr_t)(mod).load_virtbase + (off)))

/* ===========================================================================
 * UNITY 6 DRIVE SEQUENCE -- differs from the 2020.3/2022.3 reference ports:
 *
 *   initJni(env, thiz, ctx, 0);              // <-- 4 ARGS. 3-arg call = crash.
 *   nativeRecreateGfxState(env, thiz, 0, surface);
 *   nativeSendSurfaceChangedEvent(env, thiz);
 *   nativeUnityPlayerSetRunning(env, thiz, 1);   // <-- U6 ONLY, before render
 *   nativeResume(env, thiz);
 *   nativeFocusChanged(env, thiz, 1);
 *   for (;;) {
 *       pump_input();                         // -> nativeInjectEvent
 *       if (!nativeRender(env, thiz)) break;
 *   }
 *   nativeApplicationUnload(env, thiz);
 *   nativeDone(env, thiz);
 *
 * AUDIO: this build has NO AndroidAudio::GetAndroidAudioOutputType force-patch
 * target the way 2020.3 did. Unity 6 registers fmodGetInfo/fmodProcess as
 * RegisterNatives callbacks for the Java AudioTrack thread. The port drives
 * them DIRECTLY: run your own SDL2 audio thread, wrap your PCM staging buffer
 * in a fake java.nio.ByteBuffer (jni_fake.c NewDirectByteBuffer), and call
 * fmodProcess(env, thiz, buf) each callback to pull mixed PCM out of the
 * engine. fmodGetInfo(env, thiz, i) returns rate/channels/format -- query it
 * once at startup rather than hardcoding 24000/256.
 * =========================================================================== */

#endif /* ZOMBOTRON_ENTRYPOINTS_H */
