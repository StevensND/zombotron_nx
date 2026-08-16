/* ==========================================================================
 * zombotron_extrace.c  --  managed-exception throw-site tracer (diagnostic)
 *
 * The engine boots fully (render loop at 30fps, GC bridge live) but two one-time
 * NullReferenceExceptions fire at frame 3 with EMPTY managed traces (IL2CPP
 * stripping is on). The previous build hooked il2cpp::vm::Exception::Raise
 * (0x178273c) and caught nothing -- so those NREs do NOT flow through Raise
 * (hardware null-check / rethrow path). Every catchable C++ throw, however it
 * is constructed, must pass through __cxa_throw, so we hook BOTH:
 *
 *   [extrace-raise]  il2cpp::vm::Exception::Raise   @ 0x178273c
 *   [extrace-cxa]    __cxa_throw (bundled libc++abi) @ 0x182531c
 *
 * Each detour lands in a naked stub that lives in the loader's OWN r-x .text
 * (no dynamically-allocated executable memory -- awkward on Switch): it saves
 * the argument/return registers, calls a C logger with (obj, caller-LR,
 * caller-FP), restores them, re-runs the target's first four (relocation-free)
 * prologue instructions, and branches to target+16 so the throw proceeds
 * untouched. The logger prints the exception class (best-effort) plus a short
 * frame-pointer backtrace as il2cpp+0xRVA; map each with dump.cs
 * (grep "RVA: 0x<rva>"). Adding/popping our own frame BEFORE the copied prologue
 * keeps the C++ unwinder's view of the target frame intact, so try/catch and
 * stack unwinding are unaffected.
 *
 * Fail-safe: each hook verifies the target's first prologue word + image bounds
 * before arming; on mismatch it is skipped and boot is unchanged. Define
 * ZB_NO_EXCEPTION_TRACER to compile the whole thing out.
 * ======================================================================== */

#include <stdint.h>
#include <stddef.h>

#include "so_util.h"     /* so_module, hook_arm64, so_rva_in_image, load_virtbase/size */
#include "util.h"        /* debugPrintf */

#ifndef ZB_NO_EXCEPTION_TRACER

extern uintptr_t g_il2cpp_base;   /* set by libc_shim.c once libil2cpp is finalised */
extern size_t    g_il2cpp_size;

typedef void *(*il2cpp_obj_get_class_t)(void *obj);
typedef const char *(*il2cpp_class_get_name_t)(void *klass);
static il2cpp_obj_get_class_t   p_obj_get_class  = 0;
static il2cpp_class_get_name_t  p_class_get_name = 0;

/* Absolute runtime addresses of (target+16), loaded by the asm stubs to resume. */
uint64_t g_raise_continue = 0;
uint64_t g_cxa_continue   = 0;

/* SceneLoader.LoadScene(string) detour (log-and-continue). Its prologue has an
 * internal `bl 0x1a690a0` at instr 3, so the stub re-emits that call absolutely
 * via g_ls_bltgt, then resumes at method+16 via g_ls_continue. */
uint64_t g_ls_bltgt    = 0;   /* = base + 0x1a690a0 (the internal callee)      */
uint64_t g_ls_continue = 0;   /* = base + 0x1a69088 (method + 16)              */
void    *g_sceneloader_instance = 0;   /* captured `this` -> poll _progress@0x78 */

/* AntPoolLoaderSystem detour (ctor capture). Its prologue's 4th instr is
 * `adrp x22,#0x3972000`, re-emitted absolutely via g_pl_x22; resume at ctor+16
 * (0x1a6ce18) via g_pl_cont. Poll _isStarted/_current/_count to see if the pool
 * warmup (whose OnComplete advances Loading->Lobby) is stuck. */
void    *g_poolloader_instance = 0;
uint64_t g_pl_x22  = 0;   /* = base + 0x3972000 (adrp x22 target)   */
uint64_t g_pl_cont = 0;   /* = base + 0x1a6ce18 (ctor + 16)         */

/* GameStateService.Set(GameState) detour @0x19acfb4 -- log every state change and
 * its caller. Prologue instr3 is `adrp x21,#0x3972000`, re-emitted via g_gss_x21;
 * resume at Set+16 (0x19acfc4) via g_gss_cont. State enum: 0 Loading,1 Lobby,
 * 2 Gameplay,3 Pause,4 Debriefing. */
uint64_t g_gss_x21  = 0;   /* = base + 0x3972000 */
uint64_t g_gss_cont = 0;   /* = base + 0x19acfc4 */
void *g_gss_instance   = 0;   /* captured GameStateService `this` (for forcing) */
int   g_current_state  = -1;

/* LobbyController.Initialize detour @0x19d64c8 -- capture `this` so we can call
 * LobbyController.Show() (0x19d6ce0) ourselves to reveal the menu. Its prologue's
 * instrs 3 & 4 are adrp x21,#0x3972000 / adrp x20,#0x373a000; both re-emitted
 * absolutely; resume at Initialize+16 (0x19d64d8). */
void    *g_lobby_instance = 0;
uint64_t g_li_x21  = 0;   /* = base + 0x3972000 */
uint64_t g_li_x20  = 0;   /* = base + 0x373a000 */
uint64_t g_li_cont = 0;   /* = base + 0x19d64d8 */

/* TransitionController detour @0x19ec994 (ShowImmediately, called in Game.Start to
 * raise the loading curtain) -- capture `this`. Prologue instr4 is cbz x0,#0x19ec9d0
 * (PC-relative), so the stub recreates the branch: resume at +16 (0x19ec9a4) or the
 * cbz target (0x19ec9d0). Reveal via HideImmediately/ShowTip(false)/ShowPreloaderImmediately(false). */
void    *g_transition_instance = 0;
uint64_t g_tc_cont   = 0;  /* = base + 0x19ec9a4 (ShowImmediately + 16) */
uint64_t g_tc_cbztgt = 0;  /* = base + 0x19ec9d0 (cbz-taken target)     */

/* GamepadController detour @0x19beee0 (Execute, per-frame) -- capture `this` so we
 * can call its Show() (0x19b8bbc): this is the menu's gamepad cursor/navigation,
 * which the real handler <Start>b__7 shows but our LobbyController.Show force did
 * not. Prologue instrs 1-4 (str d12 / stp d11,d10 / stp d9,d8 / stp x30,x21) are
 * relocation-free; resume at Execute+16 (0x19beef0). */
void    *g_gamepad_instance = 0;
uint64_t g_gp_cont = 0;   /* = base + 0x19beef0 */

/* ---- shared helpers -------------------------------------------------- */

static int is_null_ref(const char *cls) {
    /* substring match for "NullReference" without pulling in <string.h> */
    for (const char *p = cls; *p; p++) {
        const char *a = p; const char *b = "NullReference";
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

static const char *class_name_of(void *obj) {
    if (!obj || ((uintptr_t)obj & 7) || !p_obj_get_class || !p_class_get_name)
        return 0;
    void *k = p_obj_get_class(obj);
    if (!k || ((uintptr_t)k & 7)) return 0;
    return p_class_get_name(k);
}

/* Conservative stack scan: from the throw's stack pointer, print every 8-byte
 * slot whose value lands in the MANAGED code range (dump.cs methods span
 * 0x182b914..0x3557a9c; the C++ runtime and libc++abi sit below that). These are
 * the return addresses of the managed call chain -- i.e. the game methods on the
 * stack when the throw happened. Reading the live stack is safe. */
#define MANAGED_LO 0x182b914ull
#define MANAGED_HI 0x3557a9cull
static void print_site(uint64_t lr, uint64_t sp) {
    uintptr_t base = g_il2cpp_base;
    size_t    size = g_il2cpp_size;
    if (base && lr > base && (size == 0 || lr - base < size))
        debugPrintf("[extrace]     raised-at il2cpp+0x%llx\n", (unsigned long long)(lr - base));

    if (!base || !sp || (sp & 7)) return;
    int hits = 0; uint64_t last = 0;
    for (uint64_t a = sp; a < sp + 0x1000 && hits < 16; a += 8) {
        uint64_t v = *(volatile uint64_t *)a;
        if (v <= (uint64_t)base) continue;
        uint64_t rva = v - (uint64_t)base;
        if (rva >= MANAGED_LO && rva <= MANAGED_HI && v != last) {
            debugPrintf("[extrace]     game    il2cpp+0x%llx\n", (unsigned long long)rva);
            last = v; hits++;
        }
    }
    if (!hits) debugPrintf("[extrace]     (no managed frames on stack)\n");
}

/* ---- loggers (called from the asm stubs) ----------------------------- */

void raise_trace_log(void *ex, uint64_t lr, uint64_t sp);
void raise_trace_log(void *ex, uint64_t lr, uint64_t sp) {
    static int budget = 40;
    if (budget <= 0) return;
    budget--;
    const char *cls = class_name_of(ex);
    debugPrintf("[extrace-raise] #%d throw %s\n", 40 - budget, cls ? cls : "<unknown>");
    if (cls && is_null_ref(cls)) print_site(lr, sp);   /* scan only the NREs we want */
}

void cxa_trace_log(void *thrown, uint64_t lr, uint64_t sp);
void cxa_trace_log(void *thrown, uint64_t lr, uint64_t sp) {
    static int budget = 60;
    if (budget <= 0) return;
    budget--;
    const char *cls = 0;
    if (thrown && ((uintptr_t)thrown & 7) == 0) {
        void *inner = *(void **)thrown;         /* il2cpp wrapper -> ex */
        cls = class_name_of(inner);
    }
    if (!cls) cls = class_name_of(thrown);
    debugPrintf("[extrace-cxa] #%d throw %s\n", 60 - budget, cls ? cls : "<unknown>");
    if (cls && is_null_ref(cls)) print_site(lr, sp);   /* scan only the NREs we want */
}

/* ---- naked detour stubs (live in the loader's r-x .text) ------------- */

void il2cpp_raise_trace_entry(void);
__asm__(
    ".text\n.align 2\n"
    ".global il2cpp_raise_trace_entry\n"
    ".type   il2cpp_raise_trace_entry, %function\n"
    "il2cpp_raise_trace_entry:\n"
    "   stp x0, x1, [sp, #-0x50]!\n"
    "   stp x2, x3, [sp, #0x10]\n"
    "   stp x4, x5, [sp, #0x20]\n"
    "   stp x6, x7, [sp, #0x30]\n"
    "   stp x8, x30, [sp, #0x40]\n"
    "   mov x1, x30\n"
    "   add x2, sp, #0x50\n"       /* x2 = original SP (before our push) */
    "   bl  raise_trace_log\n"
    "   ldp x8, x30, [sp, #0x40]\n"
    "   ldp x6, x7, [sp, #0x30]\n"
    "   ldp x4, x5, [sp, #0x20]\n"
    "   ldp x2, x3, [sp, #0x10]\n"
    "   ldp x0, x1, [sp], #0x50\n"
    /* --- original Raise prologue (0x178273c, first 16 bytes) --- */
    "   sub sp, sp, #0x60\n"
    "   stp x30, x21, [sp, #0x40]\n"
    "   stp x20, x19, [sp, #0x50]\n"
    "   mov x19, x2\n"
    "   adrp x16, g_raise_continue\n"
    "   add  x16, x16, #:lo12:g_raise_continue\n"
    "   ldr  x16, [x16]\n"
    "   br   x16\n"
);

void cxa_throw_trace_entry(void);
__asm__(
    ".text\n.align 2\n"
    ".global cxa_throw_trace_entry\n"
    ".type   cxa_throw_trace_entry, %function\n"
    "cxa_throw_trace_entry:\n"
    "   stp x0, x1, [sp, #-0x50]!\n"
    "   stp x2, x3, [sp, #0x10]\n"
    "   stp x4, x5, [sp, #0x20]\n"
    "   stp x6, x7, [sp, #0x30]\n"
    "   stp x8, x30, [sp, #0x40]\n"
    "   mov x1, x30\n"
    "   add x2, sp, #0x50\n"       /* x2 = original SP (before our push) */
    "   bl  cxa_trace_log\n"
    "   ldp x8, x30, [sp, #0x40]\n"
    "   ldp x6, x7, [sp, #0x30]\n"
    "   ldp x4, x5, [sp, #0x20]\n"
    "   ldp x2, x3, [sp, #0x10]\n"
    "   ldp x0, x1, [sp], #0x50\n"
    /* --- original __cxa_throw prologue (0x182531c, first 16 bytes) --- */
    "   paciasp\n"
    "   stp x29, x30, [sp, #-0x30]!\n"
    "   stp x22, x21, [sp, #0x10]\n"
    "   stp x20, x19, [sp, #0x20]\n"
    "   adrp x16, g_cxa_continue\n"
    "   add  x16, x16, #:lo12:g_cxa_continue\n"
    "   ldr  x16, [x16]\n"
    "   br   x16\n"
);

/* ---- SceneLoader.LoadScene(string) log-and-continue ------------------- */

void scene_load_log(void *self, void *scene_name);
void scene_load_log(void *self, void *scene_name) {
    g_sceneloader_instance = self;   /* poll _progress@0x78 from the render loop */
    static int n = 0;
    if (n >= 24) return;
    char buf[128]; int o = 0;
    if (scene_name) {
        int len = *(int *)((char *)scene_name + 0x10);
        const uint16_t *s = (const uint16_t *)((char *)scene_name + 0x14);
        for (int i = 0; i < len && o < 126; i++) {
            uint16_t c = s[i]; buf[o++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
        }
    }
    buf[o] = 0;
    debugPrintf("[sceneload] SceneLoader.LoadScene(\"%s\")\n", buf);
    n++;
}

/* Polled from the render loop: report the live scene-load progress so we can tell
 * a stuck-at-0 (scene data not streaming) from a stuck-at-~0.9 (activation gate)
 * from slow progress. SceneLoader._progress is the float field at this+0x78. */
void zb_sceneloader_poll(void);
void zb_sceneloader_poll(void) {
    static int calls = 0;
    int tick = (calls++ % 120 == 0);
    if (!tick) return;                      /* ~ every 120 frames */
    void *sl = g_sceneloader_instance;
    if (sl) {
        static int last = -1;
        float p = *(float *)((char *)sl + 0x78);
        void *op = *(void **)((char *)sl + 0x60);   /* _operation (AsyncOperation) */
        int pm = (int)(p * 1000.0f);
        if (pm != last) {
            debugPrintf("[sceneload] _progress=%d.%03d op=%p\n", pm / 1000, pm % 1000, op);
            last = pm;
        }
    }
    void *pl = g_poolloader_instance;
    if (pl) {
        static int last_cur = -2;
        int started = *(unsigned char *)((char *)pl + 0x50);   /* _isStarted */
        int cur     = *(int *)((char *)pl + 0x48);             /* _current   */
        int cnt     = *(int *)((char *)pl + 0x4c);             /* _count     */
        int pidx    = *(int *)((char *)pl + 0x40);             /* _currentPoolIndex */
        void *pools = *(void **)((char *)pl + 0x28);           /* List<AntPoolPreset> */
        int npools  = pools ? *(int *)((char *)pools + 0x18) : -1;   /* List._size */
        if (cur != last_cur) {
            debugPrintf("[poolload] started=%d pool=%d/%d item=%d/%d\n",
                        started, pidx, npools, cur, cnt);
            last_cur = cur;
        }
        /* One-time: resolve the OnComplete handler that should drive Loading->Lobby.
         * OnComplete(delegate) stored it at instance+0x60; Il2CppDelegate.method_ptr
         * (the target method's native entry) is at delegate+0x10. Log its RVA so we
         * can name and disassemble the handler. Also log the two neighbouring
         * callback slots in case OnComplete used a different field. */
        static int logged_h = 0;
        if (!logged_h) {
            for (int off = 0x58; off <= 0x60; off += 8) {
                void *dlg = *(void **)((char *)pl + off);
                if (dlg) {
                    void *mp = *(void **)((char *)dlg + 0x10);
                    debugPrintf("[poolload] callback@+0x%x delegate=%p handler=il2cpp+0x%llx\n",
                                off, dlg,
                                (unsigned long long)((uint64_t)mp - (uint64_t)g_il2cpp_base));
                }
            }
            logged_h = 1;
        }

        /* FORCE the menu to appear. The real "loading done" handler
         * Game.<>c__DisplayClass28_0.<Start>b__7 plays the theme then calls
         * LobbyController.Show() (-> SetupLobby + LobbyView.Show) -- the menu is a
         * UI element, NOT a scene. That handler isn't firing, so once the warm-up
         * is done and we hold the LobbyController (captured at its Initialize), we
         * call Show() ourselves. Once only. (Set(Lobby) only TEARS DOWN the loading
         * scenario -- it does not build the menu -- so we do not call it.) */
        static int forced_menu = 0;
        if (!forced_menu && g_lobby_instance &&
            started == 0 && cnt > 0 && cur == cnt) {
            typedef void (*show_fn)(void *, void *);
            show_fn showfn = (show_fn)((char *)g_il2cpp_base + 0x19d6ce0);
            debugPrintf("[forcemenu] warm-up done -> LobbyController.Show()\n");
            showfn(g_lobby_instance, (void *)0);
            debugPrintf("[forcemenu] LobbyController.Show() returned OK\n");
            /* Lift the loading curtain + hide the tip + dots so the menu underneath
             * is actually visible. Instant variants (no animation dependency). */
            if (g_transition_instance) {
                typedef void *(*tc0_fn)(void *, void *);
                typedef void *(*tc1_fn)(void *, int, void *);
                tc1_fn preimm = (tc1_fn)((char *)g_il2cpp_base + 0x19ec87c); /* ShowPreloaderImmediately(bool) */
                tc1_fn tip    = (tc1_fn)((char *)g_il2cpp_base + 0x19ec62c); /* ShowTip(bool) */
                tc0_fn hideimm= (tc0_fn)((char *)g_il2cpp_base + 0x19eca0c); /* HideImmediately() */
                debugPrintf("[forcemenu] hide curtain: ShowPreloaderImmediately(false)+ShowTip(false)+HideImmediately()\n");
                preimm(g_transition_instance, 0, (void *)0);
                tip(g_transition_instance, 0, (void *)0);
                hideimm(g_transition_instance, (void *)0);
                debugPrintf("[forcemenu] curtain lifted OK\n");
            } else {
                debugPrintf("[forcemenu] (no TransitionController captured -> menu may be under the curtain)\n");
            }
            forced_menu = 1;
        }

        /* Activate the menu's gamepad cursor/navigation. The real handler
         * <Start>b__7 calls GamepadController.Show() right after LobbyController.Show();
         * our force skipped it, so the pad reached Rewired but nothing drove the menu.
         * Show it once we hold the instance (captured at its per-frame Execute). */
        static int forced_gamepad = 0;
        if (forced_menu && !forced_gamepad && g_gamepad_instance) {
            typedef void (*gshow_fn)(void *, void *);
            gshow_fn gshow = (gshow_fn)((char *)g_il2cpp_base + 0x19b8bbc);
            debugPrintf("[forcemenu] GamepadController.Show() (activate pad cursor/nav)\n");
            gshow(g_gamepad_instance, (void *)0);
            forced_gamepad = 1;
            debugPrintf("[forcemenu] GamepadController.Show() returned OK\n");
        }
    }
}

/* At entry x0=this, x1=aSceneName. Log x1, then re-run the 4 clobbered prologue
 * instrs (stp x30,x19 / mov x19,x0 / bl 0x1a690a0 / mov x1,x0) with the bl issued
 * absolutely, and resume at method+16. */
void scene_load_trace_entry(void);
__asm__(
    ".text\n.align 2\n"
    ".global scene_load_trace_entry\n"
    ".type   scene_load_trace_entry, %function\n"
    "scene_load_trace_entry:\n"
    "   stp x0, x1, [sp, #-0x50]!\n"
    "   stp x2, x3, [sp, #0x10]\n"
    "   stp x4, x5, [sp, #0x20]\n"
    "   stp x6, x7, [sp, #0x30]\n"
    "   stp x8, x30, [sp, #0x40]\n"
    "   bl  scene_load_log\n"            /* args already in x0=this, x1=aSceneName */
    "   ldp x8, x30, [sp, #0x40]\n"
    "   ldp x6, x7, [sp, #0x30]\n"
    "   ldp x4, x5, [sp, #0x20]\n"
    "   ldp x2, x3, [sp, #0x10]\n"
    "   ldp x0, x1, [sp], #0x50\n"
    /* --- re-exec LoadScene prologue (0x1a69078) with the bl fixed up --- */
    "   stp x30, x19, [sp, #-0x10]!\n"   /* instr1 */
    "   mov x19, x0\n"                   /* instr2 */
    "   adrp x17, g_ls_bltgt\n"          /* instr3: bl 0x1a690a0 -> absolute */
    "   add  x17, x17, #:lo12:g_ls_bltgt\n"
    "   ldr  x17, [x17]\n"
    "   blr  x17\n"
    "   mov  x1, x0\n"                   /* instr4 */
    "   adrp x16, g_ls_continue\n"       /* resume at method+16 */
    "   add  x16, x16, #:lo12:g_ls_continue\n"
    "   ldr  x16, [x16]\n"
    "   br   x16\n"
);

/* ---- AntPoolLoaderSystem ctor capture -------------------------------- */

void pool_ctor_log(void *self);
void pool_ctor_log(void *self) {
    if (g_poolloader_instance) return;
    g_poolloader_instance = self;
    debugPrintf("[poolload] AntPoolLoaderSystem created @%p\n", self);
}

/* ctor prologue: str x30,[sp,#-0x30]! / stp x22,x21 / stp x20,x19 / adrp x22,#0x3972000.
 * Re-run instrs 1-3, re-emit the adrp absolutely into x22 via g_pl_x22, resume at
 * ctor+16 (0x1a6ce18) via g_pl_cont. */
void pool_ctor_trace_entry(void);
__asm__(
    ".text\n.align 2\n"
    ".global pool_ctor_trace_entry\n"
    ".type   pool_ctor_trace_entry, %function\n"
    "pool_ctor_trace_entry:\n"
    "   stp x0, x1, [sp, #-0x50]!\n"
    "   stp x2, x3, [sp, #0x10]\n"
    "   stp x4, x5, [sp, #0x20]\n"
    "   stp x6, x7, [sp, #0x30]\n"
    "   stp x8, x30, [sp, #0x40]\n"
    "   bl  pool_ctor_log\n"             /* x0 = this */
    "   ldp x8, x30, [sp, #0x40]\n"
    "   ldp x6, x7, [sp, #0x30]\n"
    "   ldp x4, x5, [sp, #0x20]\n"
    "   ldp x2, x3, [sp, #0x10]\n"
    "   ldp x0, x1, [sp], #0x50\n"
    "   str x30, [sp, #-0x30]!\n"        /* instr1 */
    "   stp x22, x21, [sp, #0x10]\n"     /* instr2 */
    "   stp x20, x19, [sp, #0x20]\n"     /* instr3 */
    "   adrp x16, g_pl_x22\n"            /* instr4: adrp x22,#0x3972000 -> absolute */
    "   add  x16, x16, #:lo12:g_pl_x22\n"
    "   ldr  x22, [x16]\n"
    "   adrp x16, g_pl_cont\n"
    "   add  x16, x16, #:lo12:g_pl_cont\n"
    "   ldr  x16, [x16]\n"
    "   br   x16\n"
);

/* ---- GameStateService.Set(GameState) log-and-continue ---------------- */

void gss_set_log(void *self, int state, uint64_t lr);
void gss_set_log(void *self, int state, uint64_t lr) {
    g_gss_instance  = self;
    g_current_state = state;
    static const char *nm[] = { "Loading", "Lobby", "Gameplay", "Pause", "Debriefing" };
    uint64_t rva = lr - (uint64_t)g_il2cpp_base;
    debugPrintf("[gamestate] Set(%d %s) <- il2cpp+0x%llx\n",
                state, (state >= 0 && state < 5) ? nm[state] : "?",
                (unsigned long long)rva);
}

/* Set prologue: stp x30,x21 / stp x20,x19 / adrp x21,#0x3972000 / mov w20,w1.
 * Log w1 (new state) + caller LR, re-run instrs 1-2, re-emit adrp x21 via
 * g_gss_x21, re-run mov w20,w1, resume at Set+16 (0x19acfc4) via g_gss_cont. */
void gss_set_trace_entry(void);
__asm__(
    ".text\n.align 2\n"
    ".global gss_set_trace_entry\n"
    ".type   gss_set_trace_entry, %function\n"
    "gss_set_trace_entry:\n"
    "   stp x0, x1, [sp, #-0x50]!\n"
    "   stp x2, x3, [sp, #0x10]\n"
    "   stp x4, x5, [sp, #0x20]\n"
    "   stp x6, x7, [sp, #0x30]\n"
    "   stp x8, x30, [sp, #0x40]\n"
    "   mov x2, x30\n"                 /* arg2 = caller LR (x0=this, x1=state kept) */
    "   bl  gss_set_log\n"
    "   ldp x8, x30, [sp, #0x40]\n"
    "   ldp x6, x7, [sp, #0x30]\n"
    "   ldp x4, x5, [sp, #0x20]\n"
    "   ldp x2, x3, [sp, #0x10]\n"
    "   ldp x0, x1, [sp], #0x50\n"
    "   stp x30, x21, [sp, #-0x20]!\n"  /* instr1 */
    "   stp x20, x19, [sp, #0x10]\n"    /* instr2 */
    "   adrp x16, g_gss_x21\n"          /* instr3: adrp x21,#0x3972000 -> absolute */
    "   add  x16, x16, #:lo12:g_gss_x21\n"
    "   ldr  x21, [x16]\n"
    "   mov  w20, w1\n"                 /* instr4 */
    "   adrp x16, g_gss_cont\n"
    "   add  x16, x16, #:lo12:g_gss_cont\n"
    "   ldr  x16, [x16]\n"
    "   br   x16\n"
);

/* ---- LobbyController.Initialize capture ------------------------------ */

void lobby_init_log(void *self);
void lobby_init_log(void *self) {
    if (g_lobby_instance) return;
    g_lobby_instance = self;
    debugPrintf("[forcemenu] LobbyController.Initialize ran -> controller @%p (menu can be shown)\n", self);
}

void lobby_init_trace_entry(void);
__asm__(
    ".text\n.align 2\n"
    ".global lobby_init_trace_entry\n"
    ".type   lobby_init_trace_entry, %function\n"
    "lobby_init_trace_entry:\n"
    "   stp x0, x1, [sp, #-0x50]!\n"
    "   stp x2, x3, [sp, #0x10]\n"
    "   stp x4, x5, [sp, #0x20]\n"
    "   stp x6, x7, [sp, #0x30]\n"
    "   stp x8, x30, [sp, #0x40]\n"
    "   bl  lobby_init_log\n"           /* x0 = this */
    "   ldp x8, x30, [sp, #0x40]\n"
    "   ldp x6, x7, [sp, #0x30]\n"
    "   ldp x4, x5, [sp, #0x20]\n"
    "   ldp x2, x3, [sp, #0x10]\n"
    "   ldp x0, x1, [sp], #0x50\n"
    "   stp x30, x21, [sp, #-0x20]!\n"   /* instr1 */
    "   stp x20, x19, [sp, #0x10]\n"     /* instr2 */
    "   adrp x16, g_li_x21\n"            /* instr3: adrp x21,#0x3972000 */
    "   add  x16, x16, #:lo12:g_li_x21\n"
    "   ldr  x21, [x16]\n"
    "   adrp x16, g_li_x20\n"            /* instr4: adrp x20,#0x373a000 */
    "   add  x16, x16, #:lo12:g_li_x20\n"
    "   ldr  x20, [x16]\n"
    "   adrp x16, g_li_cont\n"
    "   add  x16, x16, #:lo12:g_li_cont\n"
    "   ldr  x16, [x16]\n"
    "   br   x16\n"
);

/* ---- TransitionController capture (to lift the loading curtain) ------- */

void tc_capture_log(void *self);
void tc_capture_log(void *self) {
    if (g_transition_instance) return;
    g_transition_instance = self;
    debugPrintf("[forcemenu] TransitionController captured @%p\n", self);
}

/* ShowPreloader prologue: stp x30,x23 / stp x22,x21 / stp x20,x19 / adrp x21,#0x3972000.
 * Re-run 1-3, re-emit adrp x21 via g_tc_x21, resume at +16 (0x19ec774). */
void tc_capture_trace_entry(void);
__asm__(
    ".text\n.align 2\n"
    ".global tc_capture_trace_entry\n"
    ".type   tc_capture_trace_entry, %function\n"
    "tc_capture_trace_entry:\n"
    "   stp x0, x1, [sp, #-0x50]!\n"
    "   stp x2, x3, [sp, #0x10]\n"
    "   stp x4, x5, [sp, #0x20]\n"
    "   stp x6, x7, [sp, #0x30]\n"
    "   stp x8, x30, [sp, #0x40]\n"
    "   bl  tc_capture_log\n"           /* x0 = this */
    "   ldp x8, x30, [sp, #0x40]\n"
    "   ldp x6, x7, [sp, #0x30]\n"
    "   ldp x4, x5, [sp, #0x20]\n"
    "   ldp x2, x3, [sp, #0x10]\n"
    "   ldp x0, x1, [sp], #0x50\n"
    "   stp x30, x19, [sp, #-0x10]!\n"  /* instr1: stp x30,x19,[sp,#-0x10]! */
    "   mov x19, x0\n"                  /* instr2: mov x19,x0 */
    "   ldr x0, [x0, #0x20]\n"          /* instr3: ldr x0,[x0,#0x20] */
    "   cbz x0, 1f\n"                   /* instr4: cbz x0,#0x19ec9d0 (recreated) */
    "   adrp x16, g_tc_cont\n"          /* x0 != 0 -> resume at +16 (0x19ec9a4) */
    "   add  x16, x16, #:lo12:g_tc_cont\n"
    "   ldr  x16, [x16]\n"
    "   br   x16\n"
    "1: adrp x16, g_tc_cbztgt\n"        /* x0 == 0 -> cbz target (0x19ec9d0) */
    "   add  x16, x16, #:lo12:g_tc_cbztgt\n"
    "   ldr  x16, [x16]\n"
    "   br   x16\n"
);

/* ---- GamepadController capture (menu's gamepad cursor) --------------- */

void gamepad_exec_log(void *self);
void gamepad_exec_log(void *self) {
    if (g_gamepad_instance) return;
    g_gamepad_instance = self;
    debugPrintf("[forcemenu] GamepadController.Execute ran -> cursor controller @%p\n", self);
}

/* Execute prologue: str d12,[sp,#-0x50]! / stp d11,d10 / stp d9,d8 / stp x30,x21.
 * All relocation-free; re-run verbatim then resume at Execute+16. Our stub only
 * touches integer regs, so d8-d12 are preserved for the re-run. */
void gamepad_exec_trace_entry(void);
__asm__(
    ".text\n.align 2\n"
    ".global gamepad_exec_trace_entry\n"
    ".type   gamepad_exec_trace_entry, %function\n"
    "gamepad_exec_trace_entry:\n"
    "   stp x0, x1, [sp, #-0x50]!\n"
    "   stp x2, x3, [sp, #0x10]\n"
    "   stp x4, x5, [sp, #0x20]\n"
    "   stp x6, x7, [sp, #0x30]\n"
    "   stp x8, x30, [sp, #0x40]\n"
    "   bl  gamepad_exec_log\n"         /* x0 = this */
    "   ldp x8, x30, [sp, #0x40]\n"
    "   ldp x6, x7, [sp, #0x30]\n"
    "   ldp x4, x5, [sp, #0x20]\n"
    "   ldp x2, x3, [sp, #0x10]\n"
    "   ldp x0, x1, [sp], #0x50\n"
    "   str d12, [sp, #-0x50]!\n"        /* instr1 */
    "   stp d11, d10, [sp, #0x10]\n"     /* instr2 */
    "   stp d9, d8, [sp, #0x20]\n"       /* instr3 */
    "   stp x30, x21, [sp, #0x30]\n"     /* instr4 */
    "   adrp x16, g_gp_cont\n"
    "   add  x16, x16, #:lo12:g_gp_cont\n"
    "   ldr  x16, [x16]\n"
    "   br   x16\n"
);

/* ---- install --------------------------------------------------------- */
static int arm_one(so_module *il2cpp, uint32_t rva, uint32_t guard,
                   uintptr_t stub, uint64_t *cont, const char *name) {
    uintptr_t base = (uintptr_t)il2cpp->load_virtbase;
    if (!so_rva_in_image(il2cpp, rva, 16)) {
        debugPrintf("[extrace] %s NOT armed: rva 0x%x past image 0x%zx\n",
                    name, rva, il2cpp->load_size);
        return 0;
    }
    uint32_t w0 = *(volatile uint32_t *)(base + rva);
    if (w0 != guard) {
        debugPrintf("[extrace] %s NOT armed: prologue guard %08x, found %08x\n",
                    name, guard, w0);
        return 0;
    }
    *cont = (uint64_t)(base + rva + 16);
    hook_arm64(base + rva, stub);
    debugPrintf("[extrace] %s tracer armed @ il2cpp+0x%x\n", name, rva);
    return 1;
}

void zb_il2cpp_install_exception_tracer(so_module *il2cpp) {
    uintptr_t base = (uintptr_t)il2cpp->load_virtbase;
    p_obj_get_class  = (il2cpp_obj_get_class_t)(base + 0x174216cu);   /* il2cpp_object_get_class */
    p_class_get_name = (il2cpp_class_get_name_t)(base + 0x1741904u);  /* il2cpp_class_get_name   */

    arm_one(il2cpp, 0x178273cu, 0xd10183ffu,
            (uintptr_t)&il2cpp_raise_trace_entry, &g_raise_continue, "Exception::Raise");
    arm_one(il2cpp, 0x182531cu, 0xd503233fu,
            (uintptr_t)&cxa_throw_trace_entry, &g_cxa_continue, "__cxa_throw");

    /* SceneLoader.LoadScene(string) @0x1a69078 -- log the requested scene so we
     * know whether the loading scenario ever reaches the menu load. Guard on its
     * first prologue word `stp x30,x19,[sp,#-0x10]!` = 0xa9bf4ffe. Fail-safe. */
    if (so_rva_in_image(il2cpp, 0x1a69078u, 16) &&
        *(volatile uint32_t *)(base + 0x1a69078u) == 0xa9bf4ffeu) {
        g_ls_bltgt    = (uint64_t)(base + 0x1a690a0u);
        g_ls_continue = (uint64_t)(base + 0x1a69088u);
        hook_arm64(base + 0x1a69078u, (uintptr_t)&scene_load_trace_entry);
        debugPrintf("[sceneload] SceneLoader.LoadScene trace armed @ il2cpp+0x1a69078\n");
    } else {
        debugPrintf("[sceneload] SceneLoader.LoadScene NOT armed (guard/bounds)\n");
    }

    /* AntPoolLoaderSystem.ctor @0x1a6ce08 -- capture the instance to poll its
     * warmup counters. Guard on first word `str x30,[sp,#-0x30]!` = 0xf81d0ffe. */
    if (so_rva_in_image(il2cpp, 0x1a6ce08u, 16) &&
        *(volatile uint32_t *)(base + 0x1a6ce08u) == 0xf81d0ffeu) {
        g_pl_x22  = (uint64_t)(base + 0x3972000u);
        g_pl_cont = (uint64_t)(base + 0x1a6ce18u);
        hook_arm64(base + 0x1a6ce08u, (uintptr_t)&pool_ctor_trace_entry);
        debugPrintf("[poolload] AntPoolLoaderSystem.ctor trace armed @ il2cpp+0x1a6ce08\n");
    } else {
        debugPrintf("[poolload] AntPoolLoaderSystem.ctor NOT armed (guard/bounds)\n");
    }

    /* GameStateService.Set(GameState) @0x19acfb4 -- log state transitions + caller.
     * Guard on first word `stp x30,x21,[sp,#-0x20]!` = 0xa9be57fe. */
    if (so_rva_in_image(il2cpp, 0x19acfb4u, 16) &&
        *(volatile uint32_t *)(base + 0x19acfb4u) == 0xa9be57feu) {
        g_gss_x21  = (uint64_t)(base + 0x3972000u);
        g_gss_cont = (uint64_t)(base + 0x19acfc4u);
        hook_arm64(base + 0x19acfb4u, (uintptr_t)&gss_set_trace_entry);
        debugPrintf("[gamestate] GameStateService.Set trace armed @ il2cpp+0x19acfb4\n");
    } else {
        debugPrintf("[gamestate] GameStateService.Set NOT armed (guard/bounds)\n");
    }

    /* LobbyController.Initialize @0x19d64c8 -- capture the controller instance so
     * we can force its Show(). Guard first word `stp x30,x21,[sp,#-0x20]!`. */
    if (so_rva_in_image(il2cpp, 0x19d64c8u, 16) &&
        *(volatile uint32_t *)(base + 0x19d64c8u) == 0xa9be57feu) {
        g_li_x21  = (uint64_t)(base + 0x3972000u);
        g_li_x20  = (uint64_t)(base + 0x373a000u);
        g_li_cont = (uint64_t)(base + 0x19d64d8u);
        hook_arm64(base + 0x19d64c8u, (uintptr_t)&lobby_init_trace_entry);
        debugPrintf("[forcemenu] LobbyController.Initialize trace armed @ il2cpp+0x19d64c8\n");
    } else {
        debugPrintf("[forcemenu] LobbyController.Initialize NOT armed (guard/bounds)\n");
    }

    /* TransitionController.ShowImmediately @0x19ec994 -- called in Game.Start to
     * raise the loading curtain; capture `this` to lift it later. Guard first word
     * `stp x30,x19,[sp,#-0x10]!` = 0xa9bf4ffe. */
    if (so_rva_in_image(il2cpp, 0x19ec994u, 16) &&
        *(volatile uint32_t *)(base + 0x19ec994u) == 0xa9bf4ffeu) {
        g_tc_cont   = (uint64_t)(base + 0x19ec9a4u);
        g_tc_cbztgt = (uint64_t)(base + 0x19ec9d0u);
        hook_arm64(base + 0x19ec994u, (uintptr_t)&tc_capture_trace_entry);
        debugPrintf("[forcemenu] TransitionController.ShowImmediately trace armed @ il2cpp+0x19ec994\n");
    } else {
        debugPrintf("[forcemenu] TransitionController.ShowImmediately NOT armed (guard/bounds)\n");
    }

    /* GamepadController.Execute @0x19beee0 -- capture the menu's gamepad cursor
     * controller so we can Show() it. Guard `str d12,[sp,#-0x50]!` = 0xfc1b0fec. */
    if (so_rva_in_image(il2cpp, 0x19beee0u, 16) &&
        *(volatile uint32_t *)(base + 0x19beee0u) == 0xfc1b0fecu) {
        g_gp_cont = (uint64_t)(base + 0x19beef0u);
        hook_arm64(base + 0x19beee0u, (uintptr_t)&gamepad_exec_trace_entry);
        debugPrintf("[forcemenu] GamepadController.Execute trace armed @ il2cpp+0x19beee0\n");
    } else {
        debugPrintf("[forcemenu] GamepadController.Execute NOT armed (guard/bounds)\n");
    }
}

#else  /* ZB_NO_EXCEPTION_TRACER */
void zb_il2cpp_install_exception_tracer(so_module *il2cpp) { (void)il2cpp; }
void zb_sceneloader_poll(void) { }
#endif
