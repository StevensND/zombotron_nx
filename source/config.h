/* config.h -- Zombotron Nintendo Switch wrapper configuration.
 *
 * Forked from badpiggies_nx, itself from the colorsheep_nx / laytonbmr_nx /
 * vln_nx SoLoader lineage (MIT). The loader-tuning constants below are
 * engine-generation properties; the game-identity constants are Zombotron's.
 *
 * Target: Zombotron 1.4.8 (com.PanikArcade.Zombotron), Unity 6000.2.6f2 /
 * IL2CPP / arm64-v8a. No PAIRIP VM (licence-check classes only, Java-side),
 * Unity native audio via the FMOD output path, single data.unity3d layout.
 *
 * MIT license -- see LICENSE.
 */
#ifndef __CONFIG_H__
#define __CONFIG_H__

/* ============================ MEMORY LAYOUT ==============================
 * Retuned to the pvz_fusion_nx shape after the first on-hardware OOM, and paired
 * with the 256MB->64MB region-granularity patch (zombotron_region_patch.h).
 *
 * WHAT THE OOM LOG SHOWED
 * Unity issued eight PROT_NONE reservations of 0x1FFFF000 (512 MB - one page)
 * each: 4088 MB on a console reporting 3189 MB total. The old layout gave a
 * 1536 MB OC window and a 1792 MB heap-backed arena, so four reservations landed
 * in the window, two in the arena, two fell through to newlib, and the ninth
 * failed. Re-slicing alone could never have fixed it -- 8 x 511 MB does not fit
 * in 3189 MB. The granularity patch is what makes those reservations 64 MB.
 *
 * THE THREE POOLS, AND WHY THEIR SIZES GO THIS WAY
 *   OC WINDOW  virtual only (PROT_NONE in a stack-region hole). Costs address
 *              space, not RAM, so it should be LARGE -- it is where big
 *              reservations belong. The finder clamps to the largest hole it can
 *              find (1621-1889 MB observed), so raising the cap simply lets a
 *              lucky run use more of its hole.
 *   OC POOL    real memory backing the pages actually touched inside that
 *              window. This is the one that must not be starved: 384 MB was too
 *              small once reservations stopped spilling to the arena.
 *   ARENA      heap-backed, real memory, used when the window is full. It should
 *              be SMALL: spending real RAM on reservations that are mostly
 *              untouched is exactly the failure mode above.
 *
 * The old layout had the last two backwards (small pool, huge arena).
 * ========================================================================= */

/* Newlib heap for the engine / libc++ / il2cpp managed heaps; the remainder goes
 * to the .so loader. */
#define MEMORY_MB 768

/* Graphics-driver headroom, held back from the newlib heap.
 *
 * __libnx_initheap took everything except 0x200000 (2 MB). switch-mesa and the
 * nouveau/nvidia layer allocate GPU memory from the SAME process pool, and a
 * single 1920x1080 RGBA swapchain buffer is ~8 MB -- so a 2 MB margin means the
 * driver gets whatever happened to be left over and nothing more.
 *
 * That matches the observed failure precisely: boot logs report
 * "phys: total=3189 MB used=3185 MB free=3 MB", the first ~25 frames render
 * fine off the buffers allocated during surface creation, and then the console
 * hard-freezes at a FIXED frame count -- independent of threading mode, which
 * is why forcing single-threaded rendering moved the freeze by one frame and no
 * further. A driver allocation failing inside the compositor path takes the
 * system down rather than returning an error to us.
 *
 * 192 MB covers a triple-buffered 1080p swapchain (~24 MB), the game's render
 * textures, GPU command buffers and texture staging, with margin. If the game
 * later OOMs on the managed side instead, this is the knob to trade back. */
#define GFX_RESERVE_MB 192u

/* mmap arena granularity. MUST equal ZB_REGION_GRANULARITY_MB in
 * zombotron_region_patch.h: the arena hands back region-aligned reservations and
 * a mismatch means Unity trims pages the arena still believes are live.
 * pvz_fusion records that 16MB was tried and CORRUPTED Unity's Dynamic Heap
 * allocator at init (overlapping regions from the over-map/trim pattern);
 * 64MB is the known-good floor. */
#define MMAP_ARENA_ALIGN    ((size_t)64 * 1024 * 1024)

/* Heap-backed spill cap. Was 1792 MB (clamped to 832 MB at runtime), which spent
 * real RAM on untouched reservations. Small on purpose now. */
#define MMAP_ARENA_RESERVE  ((size_t)192 * 1024 * 1024)

/* Stack-region overcommit arena (libc_shim.c). */
#define OC_WINDOW_BYTES     ((size_t)2048 * 1024 * 1024)  /* was 1536; virtual only */
#define OC_POOL_BYTES       ((size_t) 896 * 1024 * 1024)  /* was 384; real, was the starved one */
#define MMAP_VIRT_RESERVE   ((size_t)6144 * 1024 * 1024)
#define OVERCOMMIT_HEAP_MB  608u

/* (SO_NAME / SO_CPP_NAME / MAIN_MVGL removed: they named Chaos Rings 3's
 * libcrx.so and its .mvgl archive, were referenced by nothing in this tree, and
 * only invited someone to wire them up.) */

/* --- Zombotron identity (JNI Context shim: getPackageName / versionCode) ---
 * Read from the APK: AndroidManifest.xml (package, versionCode 38, versionName
 * 1.4.8) and assets/bin/Data/unity_app_guid. The CS_* names are what the
 * inherited jni_fake.c / unity_jni.c expect. */
/* TODO(identity): read the real applicationId from YOUR APK before release --
 *   aapt dump badging your.apk | grep package
 * It is NOT stored in assets/; it lives in the APK's AndroidManifest.xml. The
 * fake JNI Context.getPackageName() returns this, and GameAnalytics reads it.
 * A wrong value only mis-tags analytics (which we neutralise anyway), so this
 * placeholder is safe for bring-up. */
#define CS_PACKAGE       "com.qubestudios.zombotron"   /* <-- VERIFY from your APK */
#define CS_VERSION_NAME  "1.0.0"        /* <-- set to your APK versionName */
#define CS_VERSION_CODE  1             /* <-- set to your APK versionCode */
#define CS_APP_GUID      "e16910fd-46b9-4428-be0c-5c9f0e326fb5"  /* assets/bin/Data/unity_app_guid */

#define LOG_NAME    "sdmc:/switch/zombotron_nx/debug.log"

/* Game data root == the .nro's own folder (SoLoader SD convention). */
#define GAME_HOME   "sdmc:/switch/zombotron_nx"

/* on-hardware file logging (debug.log). Set to 0 for release builds. */
/* Disable il2cpp's GC at boot?
 *
 * The reference ports set this because Boehm's stop-the-world uses POSIX
 * signals that Switch never delivers, so a collection mid-frame hangs. But
 * libc_shim.c already carries a bridge for exactly that (pthread_kill_gc posts
 * the ack the undelivered handler would have), so disabling the GC is belt AND
 * braces -- and it has a cost this game will not tolerate:
 *
 *   Resources.UnloadUnusedAssets() needs the GC to decide what is unreferenced.
 *   With the GC off the operation never completes and the main thread parks in
 *   UnloadUnusedAssetsOperation::IntegrateMainThread forever. That is the
 *   observed stall at ~frame 400, and the absence of any [gc] line in the log
 *   proves the bridge was never even reached.
 *
 * So: leave the GC ON and let the bridge do its job.
 *
 * Set to 1 to restore the old belt-and-braces behaviour. */
/* Was 1 while the bridge carried badpiggies_nx's offsets: enabling the GC then
 * hung at frame 0, because the first stop-the-world read four unrelated
 * addresses, never matched a signal number and never posted an ack -- worse
 * than the ~frame 400 stall it was meant to fix.
 *
 * The four globals are now derived from Zombotron's own libil2cpp (see the
 * comment block above GC_SUSPEND_SIG_OFF in libc_shim.c; reproducible with
 * tools/re/gc_globals.py), and six instruction-word guards fail the bridge
 * closed if they are ever stale again. So the GC stays enabled and the bridge
 * handles stop-the-world, which is what UnloadUnusedAssets needs. */
#define ZB_DISABLE_IL2CPP_GC 0

/* Panik.RenderingMaster::_RenderingRefresh contains a scale loop that cannot
 * terminate when its base render-texture dimensions are 0:
 *
 *     scale = 0.005f;
 *     do { scale += 0.005f;
 *          if (scale * baseW >= Screen.width)  break;
 *          if (scale * baseH >= Screen.height) break; } while (true);
 *
 * It was stubbed to a bare `ret` to break that spin, which worked -- and cost
 * the whole render path, because the same method creates renderTextureCurrent,
 * binds it to renderingRawImage and calls UpdateRenderingTexture() on the three
 * cameras. Result: a game that runs at full speed and draws nothing.
 *
 * The zero is now fixed at its source -- Display.main.systemHeight, hooked in
 * zombotron_screen.c -- so the method is left live and does its job.
 *
 * Set to 1 to restore the stub. That trades the black screen back for a 100%
 * CPU spin, so only do it if the loop demonstrably still hangs. */
#define ZB_STUB_RENDERING_REFRESH 0

/* Joystick button index order.
 *
 * 1 = Unity's collapsed Android order, which is what Unity's Input system
 *     actually exposes: 0=A 1=B 2=X 3=Y 4=L1 5=R1 6=Select 7=Start 8=L3 9=R3.
 * 0 = the raw KEYCODE_BUTTON_A+N order the table originally shipped, which
 *     leaves holes at 2 (BUTTON_C) and 5 (BUTTON_Z) and therefore puts L1/R1 at
 *     6/7 instead of 4/5.
 *
 * L and R reading as unbound in game is exactly what the raw-keycode order
 * predicts: Unity collapses the two absent buttons, so everything from index 2
 * up sits one slot high and the shoulder buttons land on Select/Start. A and B
 * are index 0/1 in both orders, which is why they always worked and hid this.
 *
 * Set to 0 if the face buttons regress -- that is the whole revert. */
#define ZB_BUTTON_MAP_UNITY_ORDER 1

/* Analogue stick vertical sign.
 *
 * 0 = pass libnx's sign through unchanged (+Y = stick up), which is what this
 * port did originally. 1 = negate, matching Android's AXIS_Y convention where
 * -1 is up.
 *
 * Negating was a guess and it read as inverted on hardware, so it is off. If
 * up/down is inverted again after the mouse-axis fix, flip this -- that is the
 * whole change, and it applies to both sticks.
 *
 * Note the earlier report of inversion was taken while the cursor was still
 * being driven through the mouse channel, so it may have been describing that
 * path rather than this one. This flag exists so settling it costs one line
 * instead of a round trip. */
/* Confirmed on hardware: with the mouse-axis feed removed, stick input is
 * correct apart from vertical, on both sticks. Negation on. */
#define ZB_STICK_Y_INVERT 1

/* OpenSL period, in frames, forced into FMOD's output description.
 *
 * This is the real fix for the clicking and the repeated effects. FMOD was being
 * asked for periods bigger than its entire DSP pool, so every buffer it handed
 * over was part fresh audio and part stale ring contents. See
 * FMOD_PERIOD_LDR_OFF in zombotron_offsets.h for the derivation.
 *
 * 256 frames is ~10.7 ms at 24000 Hz and comfortably below any plausible Unity
 * DSP pool (the smallest default is 256 x 2). Smaller means more callbacks and
 * more slack; larger risks the original mismatch returning. Must divide into the
 * pool at least once or N goes back to 0 and the output goes silent. */
#define ZB_AUDIO_PERIOD_FRAMES 256

/* Buffers FMOD enqueues up front, forced as a constant into libunity.
 *
 * See FMOD_BUFCOUNT_OFF in zombotron_offsets.h. FMOD derives this by integer
 * division and gets 0 for this title's baked DSP geometry, which is why every
 * init step succeeded and nothing was ever enqueued.
 *
 * WHY 4 AGAIN. Dropping this to 1 changed nothing -- the glitching was identical
 * -- which is what proved the buffer count was never the cause. bq_Enqueue was
 * storing FMOD's pointer and the mixer read through it asynchronously, so the
 * shim raced FMOD's ring at every depth. That is fixed properly in opensles.c:
 * the queue now copies each block on enqueue and owns its own memory.
 *
 * With the aliasing gone, depth is pure slack against underrun and costs only
 * latency. 4 blocks at this title's period is a few tens of milliseconds.
 *
 * Raise it if audio drops out under load; it must never be 0, which is the
 * silent-but-healthy-looking state this port spent three cycles in. */
/* 0 = leave FMOD's own division alone (correct once the period is sane).
 * Nonzero = force the queue depth to that constant, the old workaround. */
#define ZB_AUDIO_UPFRONT_BUFFERS 0

/* PROPERTY_OUTPUT_FRAMES_PER_BUFFER reported to FMOD.
 *
 * This is not just a latency knob -- it is the divisor that decides whether FMOD
 * enqueues anything at all. Disassembling the OpenSL init success path in this
 * libunity (0x11a34c0 onward):
 *
 *     0x11a34e4  mul   w10, w20, w21       ; dspNumBuffers * dspBufferLength
 *     0x11a34d8  ldr   w9,  [x19, #0x3f8]  ; framesPerBuffer  <- this value
 *     0x11a350c  udiv  w9,  w10, w9        ; N = product / framesPerBuffer
 *     0x11a3520  stur  w9,  [x29, #-0x34]  ; N = up-front buffer count
 *
 * FMOD enqueues N buffers before the first callback drains any. INTEGER
 * DIVISION: if framesPerBuffer exceeds the product, N is 0, FMOD enqueues
 * nothing, the queue is permanently dry, the buffer-queue callback never fires
 * and the output is silent -- with every init step still reporting success.
 * That is exactly the observed log: slCreateEngine -> fmt -> CreateAudioPlayer
 * -> SetPlayState(PLAYING), and then nothing, forever.
 *
 * And dspNumBuffers is known to be 1 here: that is precisely why the geometry
 * bound (dspNumBuffers-1)*dspBufferLength was 0 and the check had to be forced.
 * So N = dspBufferLength / framesPerBuffer, and 256 truncates to 0 for any baked
 * dspBufferLength below 256.
 *
 * 64 gives four times the count for the same buffer length. If audio is still
 * silent AND [fmod] OpenSL buffer-queue callback firing still never appears, go
 * lower (32, then 16) -- N is what has to become nonzero. Must never be 0 or
 * "": FMOD parses that as framesPerBuffer == 0 and fails init with error 60. */
/* NOTE: this value is never actually read by Zombotron -- the boot log contains
 * no [jni] getProperty lines at all, so FMOD never queries the AudioManager.
 * Kept correct-and-small for other titles and because reporting 0 would fail
 * init outright, but changing it does nothing here. The real lever is
 * ZB_AUDIO_UPFRONT_BUFFERS above. */
#define ZB_AUDIO_FRAMES_PER_BUFFER 64

/* SDL audio callback size, in frames.
 *
 * At 24000 Hz, 1024 frames is a 42 ms callback -- long enough that the mixer can
 * drain FMOD's entire queue part-way through and spend the remainder emitting
 * silence, which is precisely the shape of a click. If the counters come back
 * with a high `short` or `dry`, this is the lever: 256 frames is ~10 ms, so each
 * callback consumes far less and FMOD gets several more chances to refill.
 *
 * Left at the historical 1024 for now so this build's counters describe the
 * behaviour actually being complained about, not a changed one. */
#define ZB_AUDIO_CALLBACK_FRAMES 1024

/* SDL output device rate for the OpenSL mixer.
 *
 * The chain currently splits: the fake AudioManager reports 24000, FMOD creates
 * its output player at 24000, and ensure_device() ignored that and opened the
 * device at 48000 -- so mix_player resampled 24000 -> 48000 on every block. The
 * split was deliberate (players historically arrived at mixed 22050/44100 rates
 * and pinning the device to the first one was worse), but with FMOD as the only
 * producer there is exactly one rate, and matching it removes the resampler from
 * the path entirely.
 *
 * Requested with SDL_AUDIO_ALLOW_FREQUENCY_CHANGE, so if the backend cannot do
 * 24000 it substitutes its own rate, g_dev_rate follows it and mix_player
 * resamples as before -- i.e. this degrades to the old behaviour rather than
 * failing to open. The boot log now prints what was actually granted.
 *
 * Set to 48000 to restore the previous split. */
#define ZB_AUDIO_DEVICE_RATE 24000

/* FMOD OpenSL buffer-geometry bypass (libunity+0x11a34b0, b.ls -> b).
 *
 * This is the patch that got audio from "error 60" to a PLAYING output. It was
 * already present in the build BEFORE the boot hang, and that build booted --
 * so it is not the prime suspect. Gated anyway so it can be ruled out in one
 * line rather than a round trip. */
#define ZB_PATCH_FMOD_BUFFER_GEOMETRY 1

/* Pointer input: touchscreen, analogue-stick cursor, gyro aiming and USB mouse,
 * merged by nx_pointer.c and presented to the game as a mouse.
 *
 * Set to 0 for a gamepad-only port. That is not just cosmetic: the pointer layer
 * CONSUMES pad inputs that the game would otherwise see --
 *   - the left stick drives the on-screen cursor,
 *   - D-pad up/down live-tune gyro sensitivity,
 *   - Minus toggles gyro pointing,
 * so with it enabled those inputs are ambiguous at best. Disabling it hands the
 * whole pad back to the game and removes the cursor overlay from the swap path.
 *
 * The button/stick path is independent of this: zb_pump_input samples its own
 * PadState and feeds zb_pad_set() regardless. */
#define ZB_ENABLE_POINTER_INPUT 0

/* Unholy Fusion DLC entitlement.
 *
 * THIS IS AN ASSERTION OF OWNERSHIP, NOT AN UNLOCK. Set it to 1 only if you have
 * actually bought the DLC on the account whose copy of the game you dumped.
 *
 * Why it is needed. Zombotron resolves DLC entitlement through
 *   PlatformAPI_Android::UpdateOwnsDlcs()
 *     -> ZombotronMobileInAppPurchases.UnholyFusionDlc.IsUnlocked
 *     -> NonConsumableInAppPurchase.OrderState, filled in by Unity IAP from
 *        Google Play Billing.
 * There is no Play Billing here -- jni_fake has no Java layer and no store
 * connection -- so OrderState is never populated, IsUnlocked stays false, and
 * the DLC reads as unowned no matter who owns it. Same shape as
 * Display.main.systemHeight returning 0: a legitimate value the port simply has
 * no way to fetch.
 *
 * Set to 0 if you do not own it. */
/* Confirmed working on hardware: with the config.h include fixed the hook
 * installs, the entitlement reads as owned and the DLC is active in game. */
#define ZB_OWN_UNHOLY_FUSION_DLC 0

/* ---- Splash video (Panik Arcade / publisher intros) ---------------------
 *
 * libunity reaches UnityEngine.Video through 57 undefined AMedia / AImage /
 * ANativeWindow NDK symbols; zombotron_imports.c answers them with AMEDIA_ERROR_UNSUPPORTED,
 * so every clip reports as unplayable and the two intro videos never appear.
 *
 * Rather than implement the NDK media surface, zombotron_video.c replaces
 * UnityEngine.Video.VideoPlayer::Play/Stop outright, decodes the staged clip
 * with ffmpeg on its own thread and draws it as a full-screen overlay from the
 * eglSwapBuffers wrapper -- the same place the nx_pointer cursor already draws.
 *
 * This is SAFE TO TURN OFF. With ZB_VIDEO 0 the hooks are never installed and
 * the game behaves exactly as it does today: the intro is a black screen for
 * its duration and then the game continues. Nothing else depends on it.
 *
 * The clips must be staged first -- tools/stage_sd.py writes videos/ and
 * videos/manifest.txt out of your own copy. With no manifest the player logs
 * once and disables itself; it never blocks boot. */
#define ZB_VIDEO 0

/* Upper bound on the decoded frame size, in texture terms.
 *
 * THIS IS THE GPU-MEMORY LEVER, and it is the same lever as ZB_FORCE_SCREEN_W/H
 * further down: the port holds GFX_RESERVE_MB back for switch-mesa and the
 * console reports ~3 MB free at boot. Planar YUV420 costs w*h*1.5 bytes across
 * three GL_LUMINANCE textures:
 *
 *     1920x1080 = 3.1 MB   (vs 8.3 MB if we converted to RGBA first)
 *     1280x720  = 1.4 MB
 *      960x540  = 0.8 MB
 *
 * A clip larger than this is downscaled by swscale, YUV to YUV, on the decode
 * thread. At the default the shipped 1920x1080 clips are passed through
 * untouched and swscale never runs. If the intro is where a graphics allocation
 * starts failing, halve these two numbers before touching anything else.
 *
 * CPU-side buffers are NOT the concern here: three staging slots at 1080p are
 * 9.3 MB out of a newlib heap with a 384 MB floor. */
#define ZB_VIDEO_MAX_W 1920
#define ZB_VIDEO_MAX_H 1080

/* ffmpeg frame threads for the video decoder. The console gives homebrew three
 * cores and our own decode thread already occupies one, so 2 is the honest
 * default; 1 disables threading entirely if a decoder turns out to be unstable
 * with it. Ignored if switch-ffmpeg was built without pthreads. */
#define ZB_VIDEO_DECODE_THREADS 2

/* Movie audio sink -- NO FLAG, because the runtime answers this itself.
 *
 * A previous revision had a ZB_VIDEO_OWN_AUDIO_DEVICE knob here, written on the
 * belief that a Unity title never opens opensles.c's g_dev and that movie audio
 * would therefore need a second SDL device. A hardware log disproved that:
 * Unity 6's FMOD Android backend reaches OpenSL ES through slCreateEngine, the
 * shim serves it, and g_dev is the port's ONE audio device -- with mix_movie()
 * already called from audio_callback every block. The fmodProcess pump in
 * jni_fake.c never runs in this build.
 *
 * opensles_movie_begin() now picks whichever device is actually open and marks
 * it as the ring's sole drain, so there is nothing left to configure. */

#define ASSET_PACK_ENABLE 1  /* fold loose assets into one indexed pack (built first boot); set 0 to disable */
#define DEBUG_LOG 0   /* release: no debug.log on the SD card. Set to 1 to diagnose. */

/* Per-call file/mmap tracing. Each traced line is a log write, and log writes go
 * to the SD card, so this is not free: the first loading-screen hang was the
 * engine main thread parked in fsFileWrite for 48 seconds writing our own log.
 * Leave at 0 unless chasing an IO or allocator bug. */
#define TRACE_IO   0
#define TRACE_MMAP 0

/* DOCKED render resolution. Handheld always renders at its native 1280x720
 * (main.c hardcodes that); this value is used for the docked path only. Touch
 * scaling and DPI reporting derive from screen_width/screen_height and follow
 * automatically.
 *
 * Set to 720p, NOT docked's native 1080p. Rendering 1080p while docked is 2.25x
 * the GPU fill of 720p, and this game cannot hold 60fps at that on the Switch:
 * the frame rate drops, the game clock slows (slow-motion coin/reward pickup
 * animations, and pickups that occasionally never register) and input is missed
 * during particle-heavy effects. At 720p the compositor upscales to the 1080p
 * output -- the image is slightly softer but it holds a fluid 60fps, matching
 * handheld. Raising these two lines toward 1080p (or a middle 1600x900) trades
 * that fluidity back for sharpness. */
#define ZB_FORCE_SCREEN_W 1280
#define ZB_FORCE_SCREEN_H 720

extern int screen_width;
extern int screen_height;

#endif
