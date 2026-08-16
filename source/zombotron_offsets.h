/* zombotron_offsets.h -- Unity 6000.2.6f2 engine internals for Zombotron 1.4.8.
 *
 * PROVENANCE. Two binaries were used and they are not interchangeable:
 *
 *   - A SYMBOLIZED 6000.2.6f2 reference pair supplied the names, the struct field
 *     offsets, and the semantics (what each function does, what its return values
 *     mean). Its code addresses are recorded as REF_RVA_* for traceability ONLY.
 *
 *   - Zombotron's own libunity.so (22,528,096 bytes, BuildID fdb85ace7f26fe1f)
 *     supplied every value the port actually uses: the GAME_RVA_*, the guard
 *     words, and the fingerprints below. All three were disassembled and read by
 *     eye to confirm they are the functions the reference named.
 *
 * WHY FINGERPRINTS COME FROM THE GAME, NOT THE REFERENCE.
 * The reference is a different build configuration of the same engine version
 * (21.2 MB of .text vs the game's 16.8 MB -- Zombotron ships with engine code
 * stripping on). Same version does NOT mean same codegen: frame sizes differ from
 * inlining, and ChoreographerBase::Get calls a helper in the game that the
 * reference inlined outright, with different register allocation on top. A
 * reference-derived fingerprint matched TimeManager for 9 of 10 words and missed
 * Choreographer entirely. Deriving from the game fixes that, and still tolerates
 * a future Zombotron patch rebuilt with the same toolchain.
 *
 * Struct field offsets are the exception: those really are version-invariant, and
 * the game's TimeManager::Update prologue was checked to confirm it.
 *
 * Masked operand classes: ADRP/ADR/B/BL/B.cond/CBZ/TBZ/LDR-literal, the ADD that
 * completes an ADRP address pair, and SP frame adjustments. Everything else,
 * including struct field offsets in ldr/str, is matched exactly.
 */
#ifndef ZOMBOTRON_OFFSETS_H
#define ZOMBOTRON_OFFSETS_H

/* ============================ ZOMBOTRON NOTES ============================
 * This file is inherited from zombotron_nx (Unity 6000.2.6f2). Zombotron is the
 * SAME engine version, so the FINGERPRINTS below are reused unchanged -- tier 1
 * (masked-opcode scan) is what actually locates each function at runtime, and it
 * was verified against Zombotron's own libunity.so:
 *
 *     TimeManager::Update          fp x1  rva 0x5cf5dc   (guard 0xf940b008)  OK
 *     AndroidAudio::GetOutputType  fp x1  rva 0x78ce68   (guard 0xd10203ff)  OK
 *     FMOD OpenSL buffer geometry  fp x1  rva 0x1019090  (run byte-identical) OK
 *     ChoreographerBase::Get       fp x1  rva 0x77d2f4   (guard 0xd10303ff)  OK*
 *
 *     *zombotron's Choreographer fp had one build-specific ldr immediate hard-
 *      coded (word 11). Cross-referencing clayjamclassic_nx (Unity 6000.3.0f1)
 *      showed that same word is the only thing that drifts between builds; masking
 *      its imm12 makes the fp match zombotron, clayjam AND Zombotron. 4/4 now.
 *
 * All GAME_RVA_* below are Zombotron's own verified addresses. WaitVSync and the
 * region-granularity table (zombotron_region_patch.h) were likewise re-derived
 * against Zombotron's libunity.so (see those blocks). The allocator/region code
 * is byte-identical to Zombotron (same 6000.2.6f2 build), so the 21-site region
 * patch transferred at full fidelity -- 21/21 disassembler-verified.
 * ======================================================================= */

#include <stdint.h>

typedef struct { uint32_t pattern, mask; } fp_word;

typedef struct {
    const char    *name;
    const fp_word *fp;
    int            n;
    uint32_t       game_rva;   /* verified for Zombotron 1.4.8 */
    uint32_t       guard;      /* first word at game_rva, exact */
    uint32_t       ref_rva;    /* provenance only -- never a runtime address */
} fp_target;


/* TimeManager::Update(double)
 * game 0x649e4c  (reference 0xccaca4)  16 words, 13 exact */
static const fp_word FP_TM_UPDATE[] = {
    { 0xf940b008, 0xffffffff },
    { 0xb9416809, 0xffffffff },
    { 0x3946a00a, 0xffffffff },
    { 0x91000508, 0xffffffff },
    { 0x11000529, 0xffffffff },
    { 0xf900b008, 0xffffffff },
    { 0xb9016809, 0xffffffff },
    { 0x3400000a, 0xff00001f },
    { 0xd65f03c0, 0xffffffff },
    { 0xd10003ff, 0xffc003ff },
    { 0x6d0723e9, 0xffffffff },
    { 0xa9084ffe, 0xffffffff },
    { 0x1e604008, 0xffffffff },
    { 0xaa0003f3, 0xffffffff },
    { 0x94000000, 0xfc000000 },
    { 0x1e604100, 0xffffffff },
};
#define FP_TM_UPDATE_N        16
#define GAME_RVA_TM_UPDATE    0x5cf5dcu   /* Zombotron: fp-verified, guard 0xf940b008 */
#define GUARD_TM_UPDATE       0xf940b008u
#define REF_RVA_TM_UPDATE     0xccaca4u

/* ChoreographerBase::Get()
 * game 0x7ff308  (reference 0xeb02f4)  16 words, 8 exact */
static const fp_word FP_CHOREO_GET[] = {
    { 0xd10003ff, 0xffc003ff },
    { 0xa90a57fe, 0xffffffff },
    { 0xa90b4ff4, 0xffffffff },
    { 0xd53bd054, 0xffffffff },
    { 0xf9401688, 0xffffffff },
    { 0xf9004fe8, 0xffffffff },
    { 0x94000000, 0xfc000000 },
    { 0x7100041f, 0xffffffff },
    { 0x54000000, 0xff00000f },
    { 0x35000000, 0xff00001f },
    { 0x90000013, 0x9f00001f },
    { 0xf9400268, 0xffc003ff },   /* ZOMBOTRON: was {0xf9470268,ffffffff}; imm12 masked
                                     (ldr x8,[x19,#imm]) -- .bss offset is build-specific:
                                     zombotron #0xe00, clayjam #0x280, Zombotron #0x600.
                                     Relaxing imm12 matches all three; verified fp x1. */
    { 0xb5000008, 0xff00001f },
    { 0x90000000, 0x9f00001f },
    { 0x91000000, 0xffc003ff },
    { 0xd503201f, 0xffffffff },
};
#define FP_CHOREO_GET_N        16
#define GAME_RVA_CHOREO_GET    0x77d2f4u   /* Zombotron: fp-verified (relaxed), guard 0xd10303ff */
#define GUARD_CHOREO_GET       0xd10303ffu
#define REF_RVA_CHOREO_GET     0xeb02f4u

/* AndroidAudio::GetAndroidAudioOutputType(int)
 * game 0x80f880  (reference 0xebfc9c)  16 words, 8 exact */
static const fp_word FP_AUDIO_OUTTYPE[] = {
    { 0xd10003ff, 0xffc003ff },
    { 0xa90467fe, 0xffffffff },
    { 0xa9055ff8, 0xffffffff },
    { 0xa90657f6, 0xffffffff },
    { 0xa9074ff4, 0xffffffff },
    { 0x2a0003f3, 0xffffffff },
    { 0x90000000, 0x9f00001f },
    { 0x91000000, 0xffc003ff },
    { 0x2a1f03e1, 0xffffffff },
    { 0x94000000, 0xfc000000 },
    { 0x36000000, 0xfff8001f },
    { 0x52800080, 0xffffffff },
    { 0x14000000, 0xfc000000 },
    { 0x94000000, 0xfc000000 },
    { 0x90000017, 0x9f00001f },
    { 0x12000008, 0xffffffff },
};
#define FP_AUDIO_OUTTYPE_N        16
#define GAME_RVA_AUDIO_OUTTYPE    0x78ce68u   /* Zombotron: fp-verified, guard 0xd10203ff */
#define GUARD_AUDIO_OUTTYPE       0xd10203ffu
#define REF_RVA_AUDIO_OUTTYPE     0xebfc9cu


/* ---- TimeManager layout ------------------------------------------------
 * Read directly off the GAME's TimeManager::Update prologue at 0x649e4c:
 *     ldr  x8,  [x0, #0x160]     frameCount   (u64)
 *     ldr  w9,  [x0, #0x168]     renderCount  (u32)
 *     ldrb w10, [x0, #0x1a8]     pause        (u8)
 *     add  x8, x8, #1 ; add w9, w9, #1 ; str both back
 *     cbz  w10, +0x24            not paused -> body
 *     ret                        paused -> return without stepping time
 *
 * NOT the 2020.3/2022.3 values (0xc8 / 0xd0 / 0xf8) used by badpiggies_nx and
 * ZookeeperDX_NX. Unity 6 moved all three. */
#define TM_FIELD_FRAMECOUNT   0x160   /* u64 */
#define TM_FIELD_RENDERCOUNT  0x168   /* u32 */
#define TM_FIELD_PAUSE        0x1a8   /* u8  */

/* Entry -> body. The body opens `sub sp, sp, #0x90` in the game (#0xe0 in the
 * reference -- inlining differs, structure does not) and builds its own frame,
 * so it is directly callable.
 *
 * badpiggies_nx needs bp_tm_trampoline.s only because Unity 2020.3 built that
 * frame in the ENTRY prologue. Do not port it here: it would build a second
 * frame over the one this body already builds. */
#define TM_BODY_DELTA         0x24

/* ---- Audio output selection -------------------------------------------
 * AndroidAudio::GetAndroidAudioOutputType(int) returns a selector that
 * GetPlatformOutputOverride() maps onto FMOD_OUTPUTTYPE (decoded from the
 * reference at 0x10cea7c):
 *
 *     1 -> 21   AudioTrack (Java)   silent here: no Java layer
 *     2 -> 22   OpenSL ES           what opensles.c implements
 *     4 -> 23   AAudio + flag       no libaaudio.so on Switch
 *     3 -> 24   AAudio variant      same
 *
 * The "return 2" branch is gated on AudioManager capability flags the faked JNI
 * never sets, so unpatched this returns 1 and the game runs silent. */
#define AUDIO_SELECT_OPENSL   2

/* movz w0, #2 ; ret */
#define PATCH_RET_OPENSL      { 0x52800040u, 0xd65f03c0u }
/* mov x0, #0 ; ret */
#define PATCH_RET_NULL        { 0xd2800000u, 0xd65f03c0u }


/* FMOD OpenSL buffer-geometry validation (the terminal bound check)
 *
 * WHY THIS EXISTS. `GetAndroidAudioOutputType` above already forces selector 2
 * -> FMOD_OUTPUTTYPE 22 (OpenSL), and that half works: `slCreateEngine()` is
 * reached on hardware. FMOD then rejects the device anyway --
 *     Unity: FMOD failed to initialize the output device. (60)
 * -- and `[fmod] OpenSL CreateAudioPlayer:` never prints, so it dies between
 * engine creation and player creation. pvz_fusion hit exactly this and named it
 * (PORTING.md sec 3a): FMOD validates the output period against the DSP mixer
 * buffer and fails if framesPerBuffer > (dspNumBuffers-1)*dspBufferLength even
 * after one halving. A title whose baked dspNumBuffers == 1 makes that bound
 * (1-1)*len == 0, which NO positive period can satisfy -- so reporting a smaller
 * period cannot help and the bound check itself has to be forced.
 *
 * Confirmed in this libunity at 0x11a3494 (~0x900 bytes past OFF_fmodProcess,
 * i.e. inside the FMOD audio-device region):
 *
 *   0x11a3490  cbz  w9, fail          ; framesPerBuffer == 0   (guard, left intact)
 *   0x11a3494  sub  w10, w20, #1      ; dspNumBuffers - 1
 *   0x11a3498  mul  w10, w10, w21     ; * dspBufferLength
 *   0x11a349c  cmp  w9, w10
 *   0x11a34a0  b.ls +0xc              ; already fits -> skip the halving
 *   0x11a34a4  lsr  w9, w9, #1        ; halve framesPerBuffer
 *   0x11a34a8  str  w9, [x19, #0x3f8]
 *   0x11a34ac  cmp  w9, w10
 *   0x11a34b0  b.ls +0x10             ; <-- PATCH to unconditional b
 *   0x11a34b4  mov  x0, x19 ; bl ...  ; FAILURE path -> error 60
 *   0x11a34c0  ...                    ; SUCCESS path, builds the player
 *
 * The success path derives its buffer count as N = (w20*w21)/period at
 * 0x11a34e4, which stays >= 1 because jni_fake reports a small (64-frame)
 * period, so forcing the branch does not produce a degenerate N. Both zero
 * guards above are left intact and pass (48000 / 64 are non-zero).
 *
 * Registers are masked so re-allocation across a game update does not break the
 * match; the +0x3f8 displacement and the shape are what pin it. Matches exactly
 * once in libunity's .text. */
static const fp_word FP_FMOD_BUFGEOM[] = {
    { 0x51000400u, 0xfffffc00u },  /* sub  wA, wB, #1              */
    { 0x1b007c00u, 0xffe0fc00u },  /* mul  wA, wA, wC              */
    { 0x6b00001fu, 0xffe0fc1fu },  /* cmp  wD, wA                  */
    { 0x54000009u, 0xff00001fu },  /* b.ls +N                      */
    { 0x53017c00u, 0xfffffc00u },  /* lsr  wD, wD, #1              */
    { 0xb903fa60u, 0xffffffe0u },  /* str  wD, [x19, #0x3f8]       */
    { 0x6b00001fu, 0xffe0fc1fu },  /* cmp  wD, wA                  */
    { 0x54000009u, 0xff00001fu },  /* b.ls +N   <- patched         */
};
#define FP_FMOD_BUFGEOM_N        8
#define GAME_RVA_FMOD_BUFGEOM    0x1019090u   /* Zombotron: fp-verified; +0x1c/-8/-4/+0x78 all byte-identical to Zombotron */
#define GUARD_FMOD_BUFGEOM       0x5100068au  /* sub w10, w20, #1            */
#define REF_RVA_FMOD_BUFGEOM     0xe113e8u    /* pvz_fusion, same run        */

/* Offset from the run's first word to the branch we rewrite, and the exact
 * words involved. Guarded separately: matching the run is not on its own
 * permission to write to it. */
#define FMOD_BUFGEOM_BRANCH_OFF  0x1cu

/* Up-front buffer count, same run, +0x78.
 *
 * The bound check above was only half the problem. Past it, FMOD sizes its
 * buffer queue by INTEGER DIVISION and enqueues that many buffers before the
 * first callback drains any:
 *
 *     0x11a34e4  mul   w10, w20, w21       ; dspNumBuffers * dspBufferLength
 *     0x11a34d8  ldr   w9,  [x19, #0x3f8]  ; framesPerBuffer
 *     0x11a350c  udiv  w9,  w10, w9        ; N   <- PATCHED
 *     0x11a3520  stur  w9,  [x29, #-0x34]  ; numBuffers in the SL queue locator
 *
 * dspNumBuffers is 1 here (that is why the bound was 0), so N = bufferLength /
 * framesPerBuffer, and it truncates to 0. Zero buffers means FMOD enqueues
 * nothing, the queue is permanently dry, the buffer-queue callback never fires
 * and the device plays silence -- with every init step reporting success. The
 * observed log stopped at SetPlayState(PLAYING) forever, three builds running.
 *
 * The natural fix would be to report a smaller framesPerBuffer, and that is what
 * two builds tried. It cannot work: the log contains ZERO [jni] lines, so
 * AudioManager.getProperty is NEVER CALLED by this title. The values arrive
 * through the `cbz x9` else-branch at 0x11a3484 from the FMOD output description
 * itself, which the fake JNI has no way to touch. The divisor is unreachable, so
 * the division is what has to go.
 *
 * Replacing the udiv with `mov w9, #N` makes the queue depth a constant. Safe
 * against the shim: BQ_SLOTS is 256, far above any value used here. */
/* Force the OpenSL period, two words before the run.
 *
 * THE ACTUAL DEFECT. The period FMOD is asked to fill is LARGER than its whole
 * DSP pool -- that is what the bound check was really telling us. (An earlier
 * note here claimed dspNumBuffers must be 1; that was wrong. The check fails
 * whenever framesPerBuffer > (numBuffers-1)*bufferLength after one halving,
 * which 2x256 with a 1024 period does just as well.) FMOD then hands over
 * buffers it cannot fill: partly fresh audio, partly whatever was in its DSP
 * ring. Replayed fragments and clicks, at every queue depth, copied or not --
 * which is why neither the buffer count nor copy-on-enqueue moved it.
 *
 * dspNumBuffers/dspBufferLength arrive as arguments (w7/w6) from FMOD's output
 * dispatcher and are Unity's DSP settings, unreachable from here. The period is
 * reachable:
 *
 *   0x11a348c  ldr w9, [x19, #0x3f8]  -> mov w9, #ZB_AUDIO_PERIOD_FRAMES
 *   0x11a3490  cbz w9, fail           -> str w9, [x19, #0x3f8]
 *
 * The second word writes it back into the output description, so FMOD produces
 * at the new period everywhere rather than only in this function's local view.
 * Dropping the `cbz` guard is safe by construction: w9 is now a nonzero
 * constant, which is the only thing that guard tested.
 *
 * With a period smaller than the pool, N = pool/period falls out >= 1 on its
 * own -- which is why ZB_AUDIO_UPFRONT_BUFFERS now defaults to 0 (do not force
 * it) and the udiv is left to compute an honest value. */
#define FMOD_PERIOD_LDR_OFF      (-8)
#define FMOD_PERIOD_CBZ_OFF      (-4)
#define GUARD_FMOD_PERIOD_LDR    0xb943fa69u  /* ldr w9,[x19,#0x3f8] */
#define GUARD_FMOD_PERIOD_CBZ    0x34000129u  /* cbz w9, fail        */
#define PATCH_FMOD_PERIOD_MOV    (0x52800000u | ((ZB_AUDIO_PERIOD_FRAMES & 0xffffu) << 5) | 9u)
#define PATCH_FMOD_PERIOD_STR    0xb903fa69u  /* str w9,[x19,#0x3f8] */

#define FMOD_BUFCOUNT_OFF        0x78u
#define GUARD_FMOD_BUFCOUNT      0x1ac90949u  /* udiv w9, w10, w9 */
/* movz w9, #ZB_AUDIO_UPFRONT_BUFFERS */
#define PATCH_FMOD_BUFCOUNT      (0x52800000u | ((ZB_AUDIO_UPFRONT_BUFFERS & 0xffffu) << 5) | 9u)
#define GUARD_FMOD_BUFGEOM_BLS   0x54000089u  /* b.ls +0x10 */
#define PATCH_FMOD_BUFGEOM_B     0x14000004u  /* b    +0x10 -- same target  */

static const fp_target ZOMBOTRON_TARGETS[] = {

    { "TimeManager::Update", FP_TM_UPDATE, FP_TM_UPDATE_N,
      GAME_RVA_TM_UPDATE, GUARD_TM_UPDATE, REF_RVA_TM_UPDATE },
    { "ChoreographerBase::Get", FP_CHOREO_GET, FP_CHOREO_GET_N,
      GAME_RVA_CHOREO_GET, GUARD_CHOREO_GET, REF_RVA_CHOREO_GET },
    { "AndroidAudio::GetAndroidAudioOutputType", FP_AUDIO_OUTTYPE, FP_AUDIO_OUTTYPE_N,
      GAME_RVA_AUDIO_OUTTYPE, GUARD_AUDIO_OUTTYPE, REF_RVA_AUDIO_OUTTYPE },
    { "FMOD OpenSL buffer geometry", FP_FMOD_BUFGEOM, FP_FMOD_BUFGEOM_N,
      GAME_RVA_FMOD_BUFGEOM, GUARD_FMOD_BUFGEOM, REF_RVA_FMOD_BUFGEOM },
};
#define ZOMBOTRON_TARGETS_N (sizeof(ZOMBOTRON_TARGETS)/sizeof(ZOMBOTRON_TARGETS[0]))

/* ---- Unity 6 Android vsync counter -------------------------------------
 * WaitVSync(long) blocks in
 *     lock(mutex);
 *     while (counter < target) cond_wait(cv, mutex);
 * waiting for a display presentation that never happens on Switch, because
 * nothing on this platform advances the counter. That is the first-real-frame
 * hang: load frames complete, then nativeRender never returns.
 *
 * The clock thread bumps the counter once per 16.6 ms of wall time, which lets
 * the waiter through at ~60 Hz. Bumping it faster would run the engine ahead of
 * the compositor; Layton's port notes that flooding vi takes the whole system
 * down.
 *
 * HOW THESE WERE DERIVED, AND WHY THEY ARE TRUSTWORTHY
 * The symbolized 6000.2.6f2 reference gave the shape (WaitVSync @ ref 0xeb0118,
 * counter at ref +0x1ba3460), but its register allocation differs from a shipped
 * game build and no fingerprint transferred -- three separate strategies failed.
 *
 * What worked was a register-agnostic structural matcher for the whole loop
 * (adrp/add mutex -> lock -> adrp/ldr counter -> cmp -> b.ge -> add x0,mutex,#0x28
 * -> cond_wait -> branch back), VALIDATED against laytonbmr_nx's shipped
 * libunity.so (Unity 6000.0.58f2): it recovers that port's independently
 * documented OFF_ANDROID_VSYNC_COUNTER = 0x1243158 exactly, and finds exactly
 * one match. Run against Zombotron it also finds exactly one.
 *
 * The counter lands in .bss (writable), which is what a counter should be -- a
 * derived address pointing at .text or .rodata would mean the match was wrong.
 */
/* ZOMBOTRON: re-derived from its own libunity.so with the same register-agnostic
 * loop matcher (str x30 prologue -> adrp/add mutex -> lock -> adrp/ldr counter ->
 * cmp -> b.ge -> add x0,mutex,#0x28 -> cond_wait -> branch back). Disassembly of
 * WaitVSync @ 0x77d118 confirmed:  mutex .bss = 0x1471584, counter .bss = 0x14715e0,
 * cond var = mutex+0x28. Structurally identical to Zombotron/clayjam. */
#define GAME_RVA_WaitVSync        0x77d118u    /* str x30, [sp, #-0x30]!       */
#define GUARD_WaitVSync           0xf81d0ffeu
#define GAME_RVA_WaitVSync_ldr    0x77d13cu    /* ldr x21, [x22, #0x5e0]       */
#define GUARD_WaitVSync_ldr       0xf942f2d5u
#define GAME_RVA_VSYNC_COUNTER    0x14715e0u   /* .bss, uint64                 */
#define GAME_RVA_VSYNC_MUTEX      0x1471584u   /* .bss, pthread_mutex          */
#define VSYNC_PERIOD_NS           16666667ull  /* ~60 Hz                      */

#endif /* ZOMBOTRON_OFFSETS_H */
