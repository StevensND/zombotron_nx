/* zombotron_video.h -- splash video playback for Zombotron's two intro clips.
 *
 * The engine cannot play them: libunity reaches UnityEngine.Video through the
 * Android NDK media API and zombotron_imports.c answers those 40 entry points
 * with AMEDIA_ERROR_UNSUPPORTED, so every clip reports as unplayable. Rather
 * than build an AMediaExtractor/AMediaCodec surface on top of ffmpeg and hope
 * libunity takes its memory-output path rather than its AHardwareBuffer one,
 * this bypasses VideoPlayer entirely:
 *
 *   - zombotron_il2cpp.c REPLACES VideoPlayer::Play and ::Stop;
 *   - zb_video_play() reads the staged clip, decodes it with ffmpeg on its own
 *     thread and publishes finished frames as packed YUV420 planes;
 *   - zb_video_draw(), called from the eglSwapBuffers wrapper in imports.c,
 *     uploads the newest published frame into three GL_LUMINANCE textures and
 *     draws one letterboxed quad, converting YUV to RGB in the shader.
 *
 * Nothing here touches the engine's own frame loop. IntroScript's coroutine is
 * gated on a wall-clock timer, not on VideoPlayer state, so the intro takes the
 * same amount of time whether this succeeds, fails or is compiled out. That is
 * also why the player never blocks: a missing file, an unbuildable decoder or a
 * codec switch-ffmpeg lacks all degrade to "black screen for N seconds", which
 * is exactly the current behaviour.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef ZOMBOTRON_VIDEO_H
#define ZOMBOTRON_VIDEO_H

/* Load <root>/videos/manifest.txt. Cheap: reads a few hundred bytes and starts
 * no threads, opens no devices and touches no GL. Safe to call before the
 * engine exists. Idempotent. */
void zb_video_init(const char *game_root);

/* Start playback of the clip whose duration best matches `length_seconds`
 * (<= 0 means "next in play order"). Returns immediately -- decoding happens on
 * a worker thread. A second call while playing stops the first clip. */
void zb_video_play(double length_seconds);

/* Stop playback and release the decoder. GL objects are freed by the next
 * zb_video_draw() on the render thread, since only that thread has a context. */
void zb_video_stop(void);

/* Draw the newest decoded frame, if any, over the current framebuffer. MUST be
 * called on the thread that owns the GL context, immediately before
 * eglSwapBuffers. Saves and restores every piece of GL state it touches. Does
 * nothing at all when no clip is playing. */
void zb_video_draw(void);

/* Mix pending movie audio into an S16 interleaved buffer already holding the
 * engine's own PCM. Called from the FMOD pump in jni_fake.c. Returns the number
 * of frames mixed. MUST NOT LOG -- it runs on the audio pump thread. */
int zb_video_mix_audio(short *dst, int frames, int channels);

/* Whether a clip is currently decoding. Diagnostic only. */
int zb_video_is_playing(void);

#endif /* ZOMBOTRON_VIDEO_H */
