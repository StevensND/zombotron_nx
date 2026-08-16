/* zombotron_il2cpp.c -- present the Switch pad to Rewired as a UnityEngine.Input
 * joystick, by hooking UnityEngine.Input in the game's own libil2cpp.so.
 *
 * See zombotron_il2cpp.h for why UnityEngine.Input is the right surface here
 * (short version: Rewired's Android path reads it).
 *
 * WHAT EACH HOOK IS FOR
 *   GetJoystickNames  Rewired's UnityInputJoystickSource calls this, diffs the
 *                     result against last frame, and creates/destroys Joysticks
 *                     accordingly. Return nothing and Rewired believes no
 *                     controller exists, which is the current symptom. This is
 *                     the single most load-bearing hook.
 *   GetKey/Down/Up    Rewired polls KeyCode.Joystick1Button0..19 for the button
 *                     elements of the joystick it created above.
 *   GetAxisRaw/GetAxis  stick and trigger axes. Rewired builds the axis name at
 *                     runtime by concatenation, so the literal never appears in
 *                     global-metadata; the hook therefore PARSES whatever name
 *                     it is handed rather than matching fixed strings.
 *
 * SAFETY
 * Every installer verifies the first instruction word against the guard recorded
 * from the shipped binary before writing. Guard mismatch -> that hook is
 * skipped and logged. A missing hook means "no controller"; a wrong hook means
 * a detour into the middle of some unrelated managed method.
 *
 * The button ORDER in zb_button_map[] below is the standard Unity-on-Android
 * joystick ordering and is confirmed working on hardware: A/B/X/Y, L/R, ZL/ZR
 * and Plus/Minus land where the game expects. Jump and Fire are additionally
 * forced onto ZL/ZR by name (see g_name_map), independent of this table.
 *
 * MIT.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <switch.h>

#include "so_util.h"
#include "util.h"
#include "config.h"
#include "zombotron_il2cpp.h"
#include "zombotron_video.h"

/* Tripwire. ZB_OWN_UNHOLY_FUSION_DLC lives in config.h, and this file did not
 * include config.h -- so `#if ZB_OWN_UNHOLY_FUSION_DLC` quietly evaluated to 0
 * and the entire DLC hook, install call and its log lines vanished from the
 * build with no warning at all. The symptom was a missing [dlc] line in a log
 * that was already being read for a different bug. Any feature flag used here
 * must fail loudly if its header is missing. */
#ifndef ZB_OWN_UNHOLY_FUSION_DLC
#error "config.h not included -- feature flags in this file would compile out silently"
#endif

/* ------------------------------------------------------- il2cpp runtime API */

typedef void *(*fn_string_new)(const char *);
typedef void *(*fn_array_new)(void *klass, uintptr_t length);
typedef void *(*fn_class_from_name)(void *image, const char *ns, const char *name);
typedef void *(*fn_domain_get)(void);
typedef void *(*fn_domain_assembly_open)(void *domain, const char *name);
typedef void *(*fn_assembly_get_image)(void *assembly);
typedef void *(*fn_thread_current)(void);
typedef void *(*fn_thread_attach)(void *domain);

static fn_string_new           il2cpp_string_new;
static fn_array_new            il2cpp_array_new;
static fn_class_from_name      il2cpp_class_from_name;
static fn_domain_get           il2cpp_domain_get;
static fn_domain_assembly_open il2cpp_domain_assembly_open;
static fn_assembly_get_image   il2cpp_assembly_get_image;
static fn_thread_current       il2cpp_thread_current;
static fn_thread_attach        il2cpp_thread_attach;

static void *g_string_class;
static int   g_bound;

/* Il2CppArray layout: the element pointers begin one machine word past the
 * bounds/max_length header, i.e. at offset 0x20 on 64-bit. Il2CppDumper's
 * il2cpp.h agrees (Il2CppArray = { Il2CppObject obj; Il2CppArrayBounds *bounds;
 * il2cpp_array_size_t max_length; void *vector[]; }). */
#define IL2CPP_ARRAY_DATA(arr) ((void **)((uintptr_t)(arr) + 0x20))

int zb_il2cpp_bind(so_module *il2cpp) {
  il2cpp_string_new = (fn_string_new)so_try_find_addr_rx(il2cpp, "il2cpp_string_new");
  il2cpp_array_new = (fn_array_new)so_try_find_addr_rx(il2cpp, "il2cpp_array_new");
  il2cpp_class_from_name =
      (fn_class_from_name)so_try_find_addr_rx(il2cpp, "il2cpp_class_from_name");
  il2cpp_domain_get = (fn_domain_get)so_try_find_addr_rx(il2cpp, "il2cpp_domain_get");
  il2cpp_domain_assembly_open =
      (fn_domain_assembly_open)so_try_find_addr_rx(il2cpp, "il2cpp_domain_assembly_open");
  il2cpp_assembly_get_image =
      (fn_assembly_get_image)so_try_find_addr_rx(il2cpp, "il2cpp_assembly_get_image");
  il2cpp_thread_current = (fn_thread_current)so_try_find_addr_rx(il2cpp, "il2cpp_thread_current");
  il2cpp_thread_attach = (fn_thread_attach)so_try_find_addr_rx(il2cpp, "il2cpp_thread_attach");

  if (!il2cpp_string_new || !il2cpp_array_new || !il2cpp_class_from_name ||
      !il2cpp_domain_get || !il2cpp_domain_assembly_open || !il2cpp_assembly_get_image) {
    debugPrintf("[il2cpp] bind FAILED (string_new=%p array_new=%p class_from_name=%p)\n",
                (void *)il2cpp_string_new, (void *)il2cpp_array_new,
                (void *)il2cpp_class_from_name);
    return -1;
  }
  g_bound = 1;
  debugPrintf("[il2cpp] runtime API bound\n");
  return 0;
}

/* System.String class, needed to allocate the string[] GetJoystickNames returns.
 * Resolved lazily: the domain does not exist until the runtime has initialised,
 * which is after hooks are installed. */
static void *string_class(void) {
  if (g_string_class)
    return g_string_class;
  void *domain = il2cpp_domain_get();
  if (!domain)
    return NULL;
  void *asm_ = il2cpp_domain_assembly_open(domain, "mscorlib.dll");
  if (!asm_)
    return NULL;
  void *img = il2cpp_assembly_get_image(asm_);
  if (!img)
    return NULL;
  g_string_class = il2cpp_class_from_name(img, "System", "String");
  return g_string_class;
}

/* Managed code can only be entered from a thread the runtime knows about. Our
 * hooks run on the render thread, which il2cpp attached at startup, but the
 * check is cheap and the failure mode without it is a GC crash. */
static void ensure_attached(void) {
  if (il2cpp_thread_current && il2cpp_thread_attach && !il2cpp_thread_current())
    il2cpp_thread_attach(il2cpp_domain_get());
}

/* -------------------------------------------------------------- pad state */

static uint64_t g_buttons;
static uint64_t g_edge_prev;   /* g_buttons from the PREVIOUS zb_pad_set call. g_prev_buttons
                                * is latched too late (in frame_end, before the game reads
                                * input) so its edge is always false during the game's Update;
                                * this one is captured before overwrite -> correct rising edge. */
static float    g_lx, g_ly, g_rx, g_ry;

void zb_pad_set(uint64_t buttons, float lx, float ly, float rx, float ry) {
  g_edge_prev = g_buttons;   /* remember last frame BEFORE overwriting */
  g_buttons = buttons;
  g_lx = lx; g_ly = ly; g_rx = rx; g_ry = ry;
}

/* Joystick button index -> Switch button mask.
 *
 * The index order is Unity's Android mapping, where JoystickButtonN corresponds
 * to Android KEYCODE_BUTTON_A(96)+N: A, B, C, X, Y, Z, L1, R1, L2, R2, THUMBL,
 * THUMBR, START, SELECT, MODE. Rewired then reinterprets these through the
 * "Standardized Gamepad" template it matches from the reported joystick name.
 *
 * This map was worked out on hardware and is now fixed. The one wrinkle is the
 * two triggers: Rewired exposes ZL/ZR as axes, not buttons, so Jump and Fire are
 * handled separately -- forced by action name in the Rewired hooks further down
 * (see g_name_map) rather than resolved from this table. */
static uint64_t zb_button_map[KEYCODE_JOYSTICK_COUNT] = {
#if ZB_BUTTON_MAP_UNITY_ORDER
  /* Switch button -> Unity "Standardized Gamepad" index that Rewired reads.
   * Jump and Fire are NOT resolved here: they are the LT/RT triggers, which
   * Rewired reads as axes, so ZL/ZR are forced straight onto the "Jump"/"Fire"
   * actions by name in the Rewired hooks below (see g_name_map). Everything else
   * (A=confirm, X=reload, L/R=weapon switch, +=pause) is a plain button. */
  HidNpadButton_A,          /*  0  A  (confirm)      */
  HidNpadButton_B,          /*  1  B  (close pause)  */
  HidNpadButton_X,          /*  2  X  (reload)       */
  HidNpadButton_Y,          /*  3  Y                 */
  HidNpadButton_L,          /*  4  L  (prev weapon)  */
  HidNpadButton_R,          /*  5  R  (next weapon)  */
  0, 0,                     /*  6, 7  (Rewired maps nothing here) */
  HidNpadButton_ZL,         /*  8  ZL (jump, via name map) */
  HidNpadButton_ZR,         /*  9  ZR (fire, via name map) */
  HidNpadButton_Plus,       /* 10  +  (pause)        */
  HidNpadButton_Minus,      /* 11  -                 */
  0, 0, 0, 0, 0, 0, 0, 0    /* 12..19  unused        */
#else
  HidNpadButton_B,          /*  0  Android BUTTON_A  = bottom face */
  HidNpadButton_A,          /*  1  Android BUTTON_B  = right face  */
  0,                        /*  2  Android BUTTON_C  -- absent     */
  HidNpadButton_Y,          /*  3  Android BUTTON_X  = left face   */
  HidNpadButton_X,          /*  4  Android BUTTON_Y  = top face    */
  0,                        /*  5  Android BUTTON_Z  -- absent     */
  HidNpadButton_L,          /*  6  L1                              */
  HidNpadButton_R,          /*  7  R1                              */
  HidNpadButton_ZL,         /*  8  L2                              */
  HidNpadButton_ZR,         /*  9  R2                              */
  HidNpadButton_StickL,     /* 10  THUMBL                          */
  HidNpadButton_StickR,     /* 11  THUMBR                          */
  HidNpadButton_Plus,       /* 12  START                           */
  HidNpadButton_Minus,      /* 13  SELECT                          */
  0,                        /* 14  MODE (Home) -- do not bind      */
  0, 0, 0, 0, 0             /* 15..19 unused                       */
#endif
};

static uint64_t g_prev_buttons;

static int joystick_index_from_keycode(int keycode) {
  if (keycode >= KEYCODE_JOYSTICK1_BASE &&
      keycode < KEYCODE_JOYSTICK1_BASE + KEYCODE_JOYSTICK_COUNT)
    return keycode - KEYCODE_JOYSTICK1_BASE;
  if (keycode >= KEYCODE_JOYSTICK_ANY_BASE &&
      keycode < KEYCODE_JOYSTICK_ANY_BASE + KEYCODE_JOYSTICK_COUNT)
    return keycode - KEYCODE_JOYSTICK_ANY_BASE;
  return -1;
}

/* ------------------------------------------------------------------ hooks */

static void *(*orig_GetJoystickNames)(void);
static uint8_t (*orig_GetKey)(int);
static uint8_t (*orig_GetKeyDown)(int);
static uint8_t (*orig_GetKeyUp)(int);
static float (*orig_GetAxis)(void *);
static float (*orig_GetAxisRaw)(void *);

/* Rewired matches this against its controller templates. "Nintendo Switch Pro
 * Controller" is the name an actual Pro Controller reports over Bluetooth on
 * Android, and it is what Rewired's Nintendo template expects -- which is what
 * lights up the JOYSTICK_ELEMENT_NAME_NINTENDO_* prompts the game already ships.
 * If prompts come out as Xbox glyphs, this string is why. */
static const char *ZB_JOYSTICK_NAME = "Nintendo Switch Pro Controller";

static void *hk_GetJoystickNames(void) {
  if (!g_bound)
    return orig_GetJoystickNames ? orig_GetJoystickNames() : NULL;
  ensure_attached();
  void *sc = string_class();
  if (!sc)
    return orig_GetJoystickNames ? orig_GetJoystickNames() : NULL;

  void *arr = il2cpp_array_new(sc, 1);
  if (!arr)
    return NULL;
  IL2CPP_ARRAY_DATA(arr)[0] = il2cpp_string_new(ZB_JOYSTICK_NAME);
  return arr;
}

static uint8_t hk_GetKey(int keycode) {
  int idx = joystick_index_from_keycode(keycode);
  if (idx < 0)
    return orig_GetKey ? orig_GetKey(keycode) : 0;
  uint64_t mask = zb_button_map[idx];
  return mask && (g_buttons & mask) ? 1 : 0;
}

static uint8_t hk_GetKeyDown(int keycode) {
  int idx = joystick_index_from_keycode(keycode);
  if (idx < 0)
    return orig_GetKeyDown ? orig_GetKeyDown(keycode) : 0;
  uint64_t mask = zb_button_map[idx];
  return mask && (g_buttons & mask) && !(g_prev_buttons & mask) ? 1 : 0;
}

static uint8_t hk_GetKeyUp(int keycode) {
  int idx = joystick_index_from_keycode(keycode);
  if (idx < 0)
    return orig_GetKeyUp ? orig_GetKeyUp(keycode) : 0;
  uint64_t mask = zb_button_map[idx];
  return mask && !(g_buttons & mask) && (g_prev_buttons & mask) ? 1 : 0;
}

/* ===== Rewired action hooks =====
 * The gameplay input systems read actions through Rewired.Player directly, via
 * both the string overloads (GetButton(string) @0x2eef09c / GetButtonDown @0x2eef22c,
 * hooked further down for Jump/Fire/Menu -- the part that actually drives play) and
 * the int overloads (GetButton @0x2eef164 / GetButtonDown @0x2eef2f4, hooked here).
 * Their prologue (str x30 / stp / stp / adrp x22) is identical to AntPoolLoaderSystem.ctor,
 * so all four reuse the loader's proven re-exec-prologue trampoline (a naked stub in our
 * own r-x .text -- no dynamic executable memory). Each detour calls the trampoline
 * (= the real Rewired method) and may then force an action from a Switch button. The int
 * detours force from g_action_map[] (currently empty: the int path carries no gameplay
 * action for this game, so they run as transparent passthrough). */
extern uintptr_t g_il2cpp_base;   /* libc_shim.c: set when il2cpp maps */
extern int g_current_state;       /* zombotron_extrace.c: current GameState (3 = Pause) */

/* Runtime-filled by zb_il2cpp_install_hooks, read by the trampoline stubs below:
 * *_x22 = base + adrp-x22 target (0x3977000); *_cont = base + method + 16. */
uint64_t g_rwd_gbd_x22 = 0, g_rwd_gbd_cont = 0;
uint64_t g_rwd_gb_x22  = 0, g_rwd_gb_cont  = 0;

static const struct { int action; uint64_t hid; } g_action_map[] = {
  /* Optional Switch button -> Rewired action-ID overrides for the int overloads.
   * Empty: gameplay is mapped by NAME (g_name_map) instead. hid=0 terminates. */
  { 0, 0 }
};
#define G_ACTION_N ((int)(sizeof g_action_map / sizeof g_action_map[0]))


/* ---- trampolines: re-run the Rewired method prologue, resume at method+16 ----
 * Same shape as the loader's pool_ctor trampoline: str/stp/stp are position-
 * independent; the adrp x22 is re-emitted absolutely from the runtime *_x22 slot. */
uint8_t tramp_rewired_gbd(void *player, int actionId, void *method);
__asm__(
    ".text\n.align 2\n"
    ".global tramp_rewired_gbd\n"
    ".type   tramp_rewired_gbd, %function\n"
    "tramp_rewired_gbd:\n"
    "   str x30, [sp, #-0x30]!\n"
    "   stp x22, x21, [sp, #0x10]\n"
    "   stp x20, x19, [sp, #0x20]\n"
    "   adrp x16, g_rwd_gbd_x22\n"
    "   add  x16, x16, #:lo12:g_rwd_gbd_x22\n"
    "   ldr  x22, [x16]\n"
    "   adrp x16, g_rwd_gbd_cont\n"
    "   add  x16, x16, #:lo12:g_rwd_gbd_cont\n"
    "   ldr  x16, [x16]\n"
    "   br   x16\n"
);
uint8_t tramp_rewired_gb(void *player, int actionId, void *method);
__asm__(
    ".text\n.align 2\n"
    ".global tramp_rewired_gb\n"
    ".type   tramp_rewired_gb, %function\n"
    "tramp_rewired_gb:\n"
    "   str x30, [sp, #-0x30]!\n"
    "   stp x22, x21, [sp, #0x10]\n"
    "   stp x20, x19, [sp, #0x20]\n"
    "   adrp x16, g_rwd_gb_x22\n"
    "   add  x16, x16, #:lo12:g_rwd_gb_x22\n"
    "   ldr  x22, [x16]\n"
    "   adrp x16, g_rwd_gb_cont\n"
    "   add  x16, x16, #:lo12:g_rwd_gb_cont\n"
    "   ldr  x16, [x16]\n"
    "   br   x16\n"
);

/* ---- detours patched into the Rewired methods (call trampoline, then force) ---- */
static uint8_t hk_rewired_gbd(void *player, int actionId, void *method) {   /* GetButtonDown */
  uint8_t r = tramp_rewired_gbd(player, actionId, method);
  if (actionId >= 0)
    for (int i = 0; i < G_ACTION_N; i++)                     /* rising edge */
      if (g_action_map[i].hid && g_action_map[i].action == actionId &&
          (g_buttons & g_action_map[i].hid) && !(g_prev_buttons & g_action_map[i].hid))
        r = 1;
  return r;
}
static uint8_t hk_rewired_gb(void *player, int actionId, void *method) {    /* GetButton (held) */
  uint8_t r = tramp_rewired_gb(player, actionId, method);
  if (actionId >= 0)
    for (int i = 0; i < G_ACTION_N; i++)                     /* held */
      if (g_action_map[i].hid && g_action_map[i].action == actionId &&
          (g_buttons & g_action_map[i].hid))
        r = 1;
  return r;
}

/* ---- STRING overloads: GetButton(string)/GetButtonDown(string) -----------------
 * The gameplay may read actions by NAME ("Jump", "Fire", ...) instead of by int id;
 * these have a SEPARATE path from the int overloads (they don't call them). Same
 * prologue, so same trampoline. We log the action NAME the first time each returns
 * true -> triggering an action in-game reveals its name directly. */
static int managed_str_to_ascii(void *s, char *out, size_t cap);   /* defined below */
uint64_t g_rwd_gbds_x22 = 0, g_rwd_gbds_cont = 0;   /* GetButtonDown(string) */
uint64_t g_rwd_gbs_x22  = 0, g_rwd_gbs_cont  = 0;   /* GetButton(string)     */
uint8_t tramp_gbd_str(void *player, void *nameStr, void *method);
__asm__(
    ".text\n.align 2\n.global tramp_gbd_str\n.type tramp_gbd_str,%function\n"
    "tramp_gbd_str:\n"
    "   str x30, [sp, #-0x30]!\n"
    "   stp x22, x21, [sp, #0x10]\n"
    "   stp x20, x19, [sp, #0x20]\n"
    "   adrp x16, g_rwd_gbds_x22\n"
    "   add  x16, x16, #:lo12:g_rwd_gbds_x22\n"
    "   ldr  x22, [x16]\n"
    "   adrp x16, g_rwd_gbds_cont\n"
    "   add  x16, x16, #:lo12:g_rwd_gbds_cont\n"
    "   ldr  x16, [x16]\n"
    "   br   x16\n"
);
uint8_t tramp_gb_str(void *player, void *nameStr, void *method);
__asm__(
    ".text\n.align 2\n.global tramp_gb_str\n.type tramp_gb_str,%function\n"
    "tramp_gb_str:\n"
    "   str x30, [sp, #-0x30]!\n"
    "   stp x22, x21, [sp, #0x10]\n"
    "   stp x20, x19, [sp, #0x20]\n"
    "   adrp x16, g_rwd_gbs_x22\n"
    "   add  x16, x16, #:lo12:g_rwd_gbs_x22\n"
    "   ldr  x22, [x16]\n"
    "   adrp x16, g_rwd_gbs_cont\n"
    "   add  x16, x16, #:lo12:g_rwd_gbs_cont\n"
    "   ldr  x16, [x16]\n"
    "   br   x16\n"
);
/* ---- NAME-based action map: force a game action from a Switch button --------
 * Gameplay reads actions by name via GetButton(string)/GetButtonDown(string). We
 * force the trigger-only actions from the Switch buttons that otherwise can't reach
 * them. GetButtonDown = rising edge, GetButton = held. Default Rewired bindings are
 * left intact, so the in-menu icons stay (LT/RT) and no rebind is needed. Add more
 * lines to remap any other action by its logged name. */
static int streq(const char *a, const char *b) {
  while (*a && *a == *b) { a++; b++; }
  return *a == *b;
}
/* Switch button -> game action, forced by NAME. The gameplay input systems
 * (UpdateInput / UpdateAttack / UpdateMovement) read actions through the string
 * overloads Rewired.Player.GetButton(string) / GetButtonDown(string), so Jump
 * and Fire -- the LT/RT triggers Rewired can't expose as buttons -- are forced
 * straight onto their action names from ZL/ZR, and Menu (B) closes the pause.
 *   state: -1 = any GameState, N = only that state (2 = Gameplay, 3 = Pause).
 *   edge : 1 = rising edge only (fires once); 0 = while held. */
static const struct { const char *name; uint64_t hid; int state; int edge; } g_name_map[] = {
  { "Jump", HidNpadButton_ZL, -1, 1 },   /* jump         <- ZL (edge, any state) */
  { "Fire", HidNpadButton_ZR, -1, 0 },   /* fire         <- ZR (held)            */
  { "Menu", HidNpadButton_B,   3, 0 },   /* close pause  <- B, Pause(3) only, held:
                                          * PauseController closes the moment it sees
                                          * Menu=true and the state flips to 2, so it
                                          * fires exactly once. Held rather than edge in
                                          * case Execute doesn't poll "Menu" on the exact
                                          * edge frame during the pause. */
  { 0, 0, 0, 0 }
};
#define G_NAME_N ((int)(sizeof g_name_map / sizeof g_name_map[0]))

static uint8_t hk_gbd_str(void *player, void *nameStr, void *method) {   /* GetButtonDown(str) */
  uint8_t r = tramp_gbd_str(player, nameStr, method);
  if (nameStr) {
    char nm[48];
    if (managed_str_to_ascii(nameStr, nm, sizeof nm))
      for (int i = 0; i < G_NAME_N; i++)                    /* rising edge */
        if (g_name_map[i].hid && streq(nm, g_name_map[i].name) &&
            (g_name_map[i].state < 0 || g_name_map[i].state == g_current_state) &&
            (g_buttons & g_name_map[i].hid) &&
            (!g_name_map[i].edge || !(g_edge_prev & g_name_map[i].hid)))
          r = 1;
  }
  return r;
}
static uint8_t hk_gb_str(void *player, void *nameStr, void *method) {    /* GetButton(str) held */
  uint8_t r = tramp_gb_str(player, nameStr, method);
  if (nameStr) {
    char nm[48];
    if (managed_str_to_ascii(nameStr, nm, sizeof nm))
      for (int i = 0; i < G_NAME_N; i++)                    /* held */
        if (g_name_map[i].hid && streq(nm, g_name_map[i].name) &&
            (g_name_map[i].state < 0 || g_name_map[i].state == g_current_state) &&
            (g_buttons & g_name_map[i].hid))
          r = 1;
  }
  return r;
}

/* Il2CppString: { Il2CppObject obj; int32_t length; uint16_t chars[]; }
 * so on 64-bit, length is at +0x10 and the UTF-16 body at +0x14. */
static int managed_str_to_ascii(void *s, char *out, size_t cap) {
  if (!s || cap == 0)
    return 0;
  int32_t len = *(int32_t *)((uintptr_t)s + 0x10);
  const uint16_t *w = (const uint16_t *)((uintptr_t)s + 0x14);
  if (len < 0)
    return 0;
  size_t n = 0;
  for (int32_t i = 0; i < len && n + 1 < cap; i++)
    out[n++] = (w[i] < 128) ? (char)w[i] : '?';
  out[n] = 0;
  return (int)n;
}

/* Rewired composes axis names at runtime, so match structurally rather than
 * against fixed literals: any name containing "analog"/"axis" plus a trailing
 * index is treated as joystick axis N.
 *
 * THE INDEX IS 1-BASED. Confirmed on hardware -- Rewired asks for exactly
 * "Joy1Axis1" .. "Joy1Axis9" (plus "MouseAxis1..3", which we ignore). The
 * previous table was 0-based, so every axis was answered one slot early:
 * axis 1 got left-Y, axis 2 got right-X, axis 3 got right-Y, axis 4 got nothing.
 * Pushing the left stick sideways therefore produced NO horizontal value at all
 * and only diagonal pushes moved anything -- which is why the in-game cursor
 * felt both reversed and extremely slow.
 *
 * THE Y AXES ARE NEGATED. libnx reports the analogue sticks with +Y up; Android
 * AXIS_Y is normalised the other way, -1 at the top, and Rewired's template is
 * written against the Android convention. Without the negation the sticks are
 * correct horizontally and inverted vertically.
 *
 * Right-stick placement is the one part not pinned by the log: axis 3 is
 * AXIS_Z on every Android pad, but vertical is AXIS_RZ, which Unity exposes as
 * axis 4 on most devices and axis 6 on some. 4 is the common case and is what
 * ships here. If the right stick reads horizontally but not vertically, move
 * `case 4` to `case 6` -- that is the whole fix, and nothing else depends on it.
 * Axes 5..9 stay 0: the D-pad is somewhere in there (hat X/Y) but which pair is
 * not established, and feeding a stick value to a trigger axis would be worse
 * than leaving it silent. */
static float axis_value(const char *name) {
  /* MOUSE AXES MUST READ ZERO.
   *
   * Rewired polls "MouseAxis1".."MouseAxis3" every frame alongside the joystick
   * axes, and the old trailing-digit parse answered them with live stick data.
   * That is wrong twice over. The touch hooks already report mousePresent=0 and
   * touchSupported=0, so claiming mouse movement contradicts them -- and Unity
   * mouse axes are per-frame DELTAS in pixels, not a normalised -1..1, so a
   * stick at full deflection was asking the cursor to move ONE PIXEL PER FRAME.
   * That is the "cursor crawls, like analog isn't mapped" symptom: the game has
   * native analog stick support and was being driven through the mouse channel
   * instead, at 1 px/frame, fighting it.
   *
   * The 1-based fix made this worse rather than better, because it fed the mouse
   * channel a clean left-stick X/Y instead of the previous scrambled pair. */
  if (strstr(name, "Mouse") || strstr(name, "mouse"))
    return 0.0f;

  const char *p = name + strlen(name);
  while (p > name && (p[-1] >= '0' && p[-1] <= '9'))
    p--;
  if (!*p)
    return 0.0f;
  int axis = atoi(p);
  float v;
  switch (axis) {
    case 1: v = g_lx; break;
    case 2: v = ZB_STICK_Y_INVERT ? -g_ly : g_ly; break;
    case 3: v = g_rx; break;
    case 4: v = ZB_STICK_Y_INVERT ? -g_ry : g_ry; break;
    /* Sticks are axes 1-4 (confirmed working). The game reads the TRIGGERS and the
     * D-pad as higher axes -- on the standard Android/Switch-Pro layout that Rewired
     * uses, LT/RT continue on 5/6 and the D-pad hat on 7/8. The game's scheme binds
     * Jump=LT, Shoot=RT, Move=D-pad, so without these the character can't jump,
     * shoot, or walk with the pad. ZL/ZR are digital on Switch -> 0.0/1.0. */
    /* The D-pad (Move) is a HAT AXIS. Hardware (39th turn): feeding ZL onto axis 5
     * made the character MOVE FORWARD -- so axis 5 is the horizontal move/hat axis,
     * NOT a trigger. The D-pad therefore goes on 5/6; the triggers (Jump=LT/Shoot=RT)
     * are handled as BUTTONS 6/7 only (feeding them as axes was injecting movement). */
    case 5: v = (g_buttons & HidNpadButton_Right) ? 1.0f
              : (g_buttons & HidNpadButton_Left)  ? -1.0f : 0.0f; break;  /* D-pad X (move) */
    case 6: v = (g_buttons & HidNpadButton_Up)    ? 1.0f
              : (g_buttons & HidNpadButton_Down)  ? -1.0f : 0.0f; break;  /* D-pad Y */
    /* Jump=LT / Shoot=RT are read as TRIGGER AXES, not buttons (ZL/ZR as buttons
     * 6/7 did nothing). Sticks=1-4, D-pad=5/6, so the triggers follow on 7/8. */
    case 7: v = 0.0f; break;
    case 8: v = 0.0f; break;
    case 9: v = 0.0f; break;
    default: return 0.0f;
  }
  return v;
}

static float hk_GetAxisRaw(void *name_str) {
  char buf[64];
  if (!managed_str_to_ascii(name_str, buf, sizeof buf))
    return orig_GetAxisRaw ? orig_GetAxisRaw(name_str) : 0.0f;
  if (!strstr(buf, "oystick") && !strstr(buf, "nalog") && !strstr(buf, "xis"))
    return orig_GetAxisRaw ? orig_GetAxisRaw(name_str) : 0.0f;
  return axis_value(buf);
}

static float hk_GetAxis(void *name_str) {
  return hk_GetAxisRaw(name_str);
}

/* Called from zombotron_input.c after zb_pad_set, once per frame. */
void zb_il2cpp_frame_end(void) {
  g_prev_buttons = g_buttons;
}

/* -------------------------------------------------------------- installer */

/* Bounds guard. Every RVA here comes from Zombotron's dump.cs. On a DIFFERENT
 * game (Zombotron) libil2cpp is a different size, so a Zombotron RVA can point
 * PAST the mapped image -- dereferencing base+rva then faults before the byte
 * guard can even run (this is exactly what crashed bring-up: GetJoystickNames
 * @ +0x56547b4 landed 30 MB past Zombotron's 60 MB image). Reject any RVA whose
 * read window would leave the module's mapping; a wrong RVA becomes SKIP+log,
 * never a data abort. The in-range-but-wrong case is still caught by the byte
 * guard below. 0x14 covers the two-word (+0x10) reads. */
static int rva_in_image(so_module *m, uint32_t rva) {
  return so_rva_in_image(m, rva, 0x14u);
}

static int install_one(so_module *m, const char *name, uint32_t rva,
                       uint32_t guard, void *hook, void **orig_out) {
  if (!rva_in_image(m, rva)) {
    debugPrintf("[il2cpp] %-20s SKIP (rva 0x%06x past image 0x%zx)\n",
                name, rva, m->load_size);
    return 0;
  }
  uintptr_t addr = (uintptr_t)m->load_virtbase + rva;
  uint32_t got = *(volatile uint32_t *)addr;
  if (got != guard) {
    debugPrintf("[il2cpp] %-20s SKIP (guard 0x%08x, found 0x%08x)\n",
                name, guard, got);
    return 0;
  }
  if (orig_out)
    *orig_out = NULL;   /* no trampoline: these hooks fully replace the method */
  hook_arm64(addr, (uintptr_t)hook);
  debugPrintf("[il2cpp] %-20s hooked @ +0x%06x\n", name, rva);
  return 1;
}

/* Two-word variant. VideoPlayer::Play, ::Stop and VideoClip::get_length all
 * begin with the identical `str x30,[sp,#-0x20]!` prologue, so a one-word guard
 * cannot tell a correct RVA from one that has slid onto a neighbouring method.
 * The word at +0x10 is each method's own initialised-class flag load and is
 * distinct; checking both is what makes a stale offset fail safe here. */
static int install_two(so_module *m, const char *name, uint32_t rva,
                       uint32_t guard0, uint32_t guard4, void *hook,
                       void **orig_out) {
  if (!rva_in_image(m, rva)) {
    debugPrintf("[il2cpp] %-24s SKIP (rva 0x%06x past image 0x%zx)\n",
                name, rva, m->load_size);
    return 0;
  }
  uintptr_t addr = (uintptr_t)m->load_virtbase + rva;
  uint32_t w0 = *(volatile uint32_t *)addr;
  uint32_t w4 = *(volatile uint32_t *)(addr + 0x10);
  if (w0 != guard0 || w4 != guard4) {
    debugPrintf("[il2cpp] %-24s SKIP (guard %08x/%08x, found %08x/%08x)\n",
                name, guard0, guard4, w0, w4);
    return 0;
  }
  if (orig_out)
    *orig_out = NULL;   /* full replacement, same as install_one */
  hook_arm64(addr, (uintptr_t)hook);
  debugPrintf("[il2cpp] %-24s hooked @ +0x%06x\n", name, rva);
  return 1;
}

/* Verify a method we intend to CALL rather than patch. Calling a stale address
 * is strictly worse than writing to one, so the same guard applies. */
static int verify_two(so_module *m, const char *name, uint32_t rva,
                      uint32_t guard0, uint32_t guard4) {
  if (!rva_in_image(m, rva)) {
    debugPrintf("[il2cpp] %-24s NOT CALLABLE (rva 0x%06x past image 0x%zx)\n",
                name, rva, m->load_size);
    return 0;
  }
  uintptr_t addr = (uintptr_t)m->load_virtbase + rva;
  uint32_t w0 = *(volatile uint32_t *)addr;
  uint32_t w4 = *(volatile uint32_t *)(addr + 0x10);
  if (w0 != guard0 || w4 != guard4) {
    debugPrintf("[il2cpp] %-24s NOT CALLABLE (guard %08x/%08x, found %08x/%08x)\n",
                name, guard0, guard4, w0, w4);
    return 0;
  }
  return 1;
}

/* See the header for why Noone rather than "pretend auth succeeded": claiming
 * the platform is initialised would send later achievement/leaderboard calls
 * into the Play Games path, which is the thing that cannot work. */
int zb_il2cpp_force_platform_none(so_module *il2cpp) {
  static const struct { const char *name; uint32_t rva; } SITES[] = {
    { "PlatformAPI::ApiKindGet",  RVA_PlatformAPI_ApiKindGet },
    { "Master::get__ApiKind",     RVA_Master_get__ApiKind    },
  };
  const int n = (int)(sizeof(SITES) / sizeof(SITES[0]));
  uintptr_t base = (uintptr_t)il2cpp->load_virtbase;

  for (int i = 0; i < n; i++) {
    if (!so_rva_in_image(il2cpp, SITES[i].rva, 4)) {
      debugPrintf("[platform] ABORT: %s @+0x%07x past image 0x%zx "
                  "-- ApiKind left as GooglePlay\n",
                  SITES[i].name, SITES[i].rva, il2cpp->load_size);
      return 0;
    }
    uint32_t got = *(volatile uint32_t *)(base + SITES[i].rva);
    if (got != GUARD_ApiKind_mov_w0_5) {
      debugPrintf("[platform] ABORT: %s @+0x%07x expected %08x, found %08x "
                  "-- ApiKind left as GooglePlay\n",
                  SITES[i].name, SITES[i].rva, GUARD_ApiKind_mov_w0_5, got);
      return 0;
    }
  }
  int done = 0;
  for (int i = 0; i < n; i++) {
    uint32_t w = PATCH_ApiKind_mov_w0_0;
    if (so_patch_code((void *)(base + SITES[i].rva), &w, 4) < 0) {
      debugPrintf("[platform] FAILED writing %s\n", SITES[i].name);
      break;
    }
    debugPrintf("[platform] %-24s ApiKind 5 (GooglePlay) -> 0 (Noone)\n",
                SITES[i].name);
    done++;
  }
  return done;
}


/* --- Unholy Fusion DLC entitlement ---------------------------------------
 * See ZB_OWN_UNHOLY_FUSION_DLC in config.h for what this asserts and why it is
 * necessary. Three call sites read this, all covered by hooking the entry:
 *   GeneralUiScript.<IntroAndTitleScreen_Coroutine>d__43::MoveNext()  (x2)
 *   PlatformAPI_Android::UpdateOwnsDlcs()
 *
 * Scoped by ProductId rather than blanket-returning true. Zombotron happens to
 * have exactly one product of this type -- ZombotronMobileInAppPurchases
 * .UnholyFusionDlc -- so the distinction makes no difference today, but a
 * getter that answers "unlocked" for every non-consumable is the kind of thing
 * that quietly becomes wrong when a game updates. Anything that is not the
 * Fusion DLC falls through to the original test.
 *
 * The ProductId is logged once so the substring match can be tightened to the
 * exact string on a later pass. */
static uint8_t (*orig_IAP_IsUnlocked)(void *) = NULL;

static uint8_t hk_IAP_IsUnlocked(void *self) {
  if (!self)
    return 0;
  /* Faithful reimplementation of the four instructions the detour overwrote:
   *     return this->OrderState == 4; */
  const int32_t order = *(volatile int32_t *)((char *)self + IAP_OFF_OrderState);
  const uint8_t real = (order == IAP_ORDERSTATE_PURCHASED) ? 1u : 0u;

#if ZB_OWN_UNHOLY_FUSION_DLC
  void *pid = *(void **)((char *)self + IAP_OFF_ProductId);
  char id[96];
  if (pid && managed_str_to_ascii(pid, id, sizeof id)) {
    int fusion = 0;
    for (char *q = id; *q; q++)
      if ((q[0] == 'f' || q[0] == 'F') && !strncasecmp(q, "fusion", 6)) { fusion = 1; break; }
    static int logged = 0;
    if (!logged) {
      logged = 1;
      debugPrintf("[dlc] IsUnlocked(\"%s\") store=%s -> %s\n", id,
                  real ? "owned" : "unknown",
                  fusion ? "OWNED (ZB_OWN_UNHOLY_FUSION_DLC=1)" : "passthrough");
    }
    if (fusion)
      return 1;
  }
#endif
  return real;
}

/* --- Splash video ---------------------------------------------------------
 *
 * These two REPLACE VideoPlayer::Play and ::Stop; nothing is called through.
 * That is deliberate on both counts:
 *
 *  - the engine's own Play would walk into AndroidVideoMedia, whose AMedia*
 *    imports are all stubbed to AMEDIA_ERROR_UNSUPPORTED. Letting it run buys
 *    nothing and risks the "surface creation stalled" wait libunity carries a
 *    message for;
 *  - IntroScript never reads VideoPlayer state back (no isPrepared, no
 *    isPlaying, no events -- see the header), so there is nothing for a
 *    passthrough to keep consistent.
 *
 * The clip is identified by asking the engine for its length and matching that
 * against the staged manifest, rather than by call order or by a field offset
 * into IntroScript. VideoClip::length is serialized metadata, so it answers
 * correctly even with the media backend dead -- it is the same value the game
 * itself uses to time the intro. */
typedef void   *(*fn_vp_get_clip)(void *self, void *method);
typedef double  (*fn_vc_get_length)(void *self, void *method);
static fn_vp_get_clip   vp_get_clip;
static fn_vc_get_length vc_get_length;

static void hk_VideoPlayer_Play(void *self, void *method) {
  (void)method;
  double len = 0.0;
  if (self && vp_get_clip && vc_get_length) {
    void *clip = vp_get_clip(self, NULL);
    if (clip)
      len = vc_get_length(clip, NULL);
  }
  zb_video_play(len);
}

static void hk_VideoPlayer_Stop(void *self, void *method) {
  (void)self;
  (void)method;
  zb_video_stop();
}

int zb_il2cpp_install_video_hooks(so_module *il2cpp) {
#if ZB_VIDEO
  uintptr_t base = (uintptr_t)il2cpp->load_virtbase;

  /* Identification is optional: without it the player falls back to play order,
   * which is still correct for a two-clip intro. So a guard failure here
   * degrades the feature instead of disabling it. */
  if (verify_two(il2cpp, "VideoPlayer.get_clip", RVA_VideoPlayer_get_clip,
                 GUARD_VideoPlayer_get_clip, GUARD_VideoPlayer_get_clip_w4) &&
      verify_two(il2cpp, "VideoClip.get_length", RVA_VideoClip_get_length,
                 GUARD_VideoClip_get_length, GUARD_VideoClip_get_length_w4)) {
    vp_get_clip   = (fn_vp_get_clip)(base + RVA_VideoPlayer_get_clip);
    vc_get_length = (fn_vc_get_length)(base + RVA_VideoClip_get_length);
  } else {
    debugPrintf("[video] clip identification unavailable -- will use play order\n");
  }

  /* VERIFY FIRST, BOTH OR NEITHER -- the same shape as
   * zb_il2cpp_force_platform_none. Half a pair is worse than none here: Play
   * replaced while Stop stayed live would raise the overlay with no way to take
   * it down, and a skipped intro would cover the game. Checking both before
   * writing either means a moved RVA leaves the engine completely stock. */
  if (!verify_two(il2cpp, "VideoPlayer.Play", RVA_VideoPlayer_Play,
                  GUARD_VideoPlayer_Play, GUARD_VideoPlayer_Play_w4) ||
      !verify_two(il2cpp, "VideoPlayer.Stop", RVA_VideoPlayer_Stop,
                  GUARD_VideoPlayer_Stop, GUARD_VideoPlayer_Stop_w4)) {
    debugPrintf("[video] VideoPlayer hooks NOT applied -- splash video INACTIVE "
                "(game unaffected; the intro stays black, as it does today)\n");
    return 0;
  }

  install_two(il2cpp, "VideoPlayer.Play", RVA_VideoPlayer_Play,
              GUARD_VideoPlayer_Play, GUARD_VideoPlayer_Play_w4,
              (void *)hk_VideoPlayer_Play, NULL);
  install_two(il2cpp, "VideoPlayer.Stop", RVA_VideoPlayer_Stop,
              GUARD_VideoPlayer_Stop, GUARD_VideoPlayer_Stop_w4,
              (void *)hk_VideoPlayer_Stop, NULL);
  debugPrintf("[video] VideoPlayer Play/Stop replaced -- splash video active\n");
  return 1;
#else
  (void)il2cpp;
  debugPrintf("[video] disabled at compile time (ZB_VIDEO=0)\n");
  return 0;
#endif
}

int zb_il2cpp_install_hooks(so_module *il2cpp) {
  if (!g_bound) {
    debugPrintf("[il2cpp] not bound -- no input hooks installed\n");
    return 0;
  }
  int n = 0;
  n += install_one(il2cpp, "GetJoystickNames", RVA_Input_GetJoystickNames,
                   GUARD_Input_GetJoystickNames, (void *)hk_GetJoystickNames,
                   (void **)&orig_GetJoystickNames);
  n += install_one(il2cpp, "GetKey", RVA_Input_GetKey,
                   GUARD_Input_GetKey, (void *)hk_GetKey, (void **)&orig_GetKey);
  n += install_one(il2cpp, "GetKeyDown", RVA_Input_GetKeyDown,
                   GUARD_Input_GetKeyDown, (void *)hk_GetKeyDown, (void **)&orig_GetKeyDown);
  n += install_one(il2cpp, "GetKeyUp", RVA_Input_GetKeyUp,
                   GUARD_Input_GetKeyUp, (void *)hk_GetKeyUp, (void **)&orig_GetKeyUp);
  n += install_one(il2cpp, "GetAxisRaw", RVA_Input_GetAxisRaw,
                   GUARD_Input_GetAxisRaw, (void *)hk_GetAxisRaw, (void **)&orig_GetAxisRaw);
  n += install_one(il2cpp, "GetAxis", RVA_Input_GetAxis,
                   GUARD_Input_GetAxis, (void *)hk_GetAxis, (void **)&orig_GetAxis);
  /* Direct action mapping: hook Rewired.Player.GetButton/GetButtonDown themselves
   * (the real gameplay path), via the re-exec-prologue trampolines above. */
  {
    uintptr_t b = (uintptr_t)il2cpp->load_virtbase;
    if (so_rva_in_image(il2cpp, RVA_Rewired_GetButtonDown, 16) &&
        *(volatile uint32_t *)(b + RVA_Rewired_GetButtonDown) == GUARD_Rewired_GetBtn) {
      g_rwd_gbd_x22  = (uint64_t)(b + REWIRED_ADRP22);
      g_rwd_gbd_cont = (uint64_t)(b + RVA_Rewired_GetButtonDown + 16);
      hook_arm64(b + RVA_Rewired_GetButtonDown, (uintptr_t)&hk_rewired_gbd);
      debugPrintf("[act] Rewired.Player.GetButtonDown hooked @ il2cpp+0x%x\n", RVA_Rewired_GetButtonDown);
      n++;
    } else debugPrintf("[act] GetButtonDown NOT hooked (guard/bounds)\n");
    if (so_rva_in_image(il2cpp, RVA_Rewired_GetButton, 16) &&
        *(volatile uint32_t *)(b + RVA_Rewired_GetButton) == GUARD_Rewired_GetBtn) {
      g_rwd_gb_x22  = (uint64_t)(b + REWIRED_ADRP22);
      g_rwd_gb_cont = (uint64_t)(b + RVA_Rewired_GetButton + 16);
      hook_arm64(b + RVA_Rewired_GetButton, (uintptr_t)&hk_rewired_gb);
      debugPrintf("[act] Rewired.Player.GetButton hooked @ il2cpp+0x%x\n", RVA_Rewired_GetButton);
      n++;
    } else debugPrintf("[act] GetButton NOT hooked (guard/bounds)\n");
    /* string overloads (gameplay may read actions by name) */
    if (so_rva_in_image(il2cpp, RVA_Rewired_GetButtonDown_str, 16) &&
        *(volatile uint32_t *)(b + RVA_Rewired_GetButtonDown_str) == GUARD_Rewired_GetBtn) {
      g_rwd_gbds_x22  = (uint64_t)(b + REWIRED_ADRP22);
      g_rwd_gbds_cont = (uint64_t)(b + RVA_Rewired_GetButtonDown_str + 16);
      hook_arm64(b + RVA_Rewired_GetButtonDown_str, (uintptr_t)&hk_gbd_str);
      debugPrintf("[act] Rewired.Player.GetButtonDown(string) hooked @ il2cpp+0x%x\n", RVA_Rewired_GetButtonDown_str);
      n++;
    } else debugPrintf("[act] GetButtonDown(string) NOT hooked (guard/bounds)\n");
    if (so_rva_in_image(il2cpp, RVA_Rewired_GetButton_str, 16) &&
        *(volatile uint32_t *)(b + RVA_Rewired_GetButton_str) == GUARD_Rewired_GetBtn) {
      g_rwd_gbs_x22  = (uint64_t)(b + REWIRED_ADRP22);
      g_rwd_gbs_cont = (uint64_t)(b + RVA_Rewired_GetButton_str + 16);
      hook_arm64(b + RVA_Rewired_GetButton_str, (uintptr_t)&hk_gb_str);
      debugPrintf("[act] Rewired.Player.GetButton(string) hooked @ il2cpp+0x%x\n", RVA_Rewired_GetButton_str);
      n++;
    } else debugPrintf("[act] GetButton(string) NOT hooked (guard/bounds)\n");
  }

  debugPrintf("[il2cpp] %d input hooks applied\n", n);

#if ZB_OWN_UNHOLY_FUSION_DLC
  if (install_one(il2cpp, "IAP.get_IsUnlocked", RVA_IAP_get_IsUnlocked,
                  GUARD_IAP_get_IsUnlocked, (void *)hk_IAP_IsUnlocked,
                  (void **)&orig_IAP_IsUnlocked)) {
    n++;
    debugPrintf("[dlc] Unholy Fusion entitlement hook installed -- Play Billing "
                "is unreachable here, so ownership is asserted by config\n");
  } else {
    debugPrintf("[dlc] entitlement hook NOT applied (guard mismatch) -- DLC will "
                "read as unowned\n");
  }
#endif
  return n;
}

/* --------------------------------------------------------------------------
 * Zombotron game-side service neutralisation (M8).
 *
 * With the engine booting and the render loop live, boot logs two one-time
 * NullReferenceExceptions during service init: the JNI-dependent SDKs (Yandex
 * AppMetrica, GameAnalytics) and the Google-Play-backed AchievementsService hit
 * the fake Android layer and throw. GDPR consent would also gate the first UI.
 * None of these are needed to run offline, so no-op their entry points. RVAs are
 * from Zombotron's own dump.cs; every site is verified (prologue word) and
 * bounds-checked first, so a wrong/stale RVA SKIPs instead of faulting.
 *   Initialize()      -> ret            (void; returns immediately)
 *   IsNeedToShow()    -> mov w0,#0; ret (bool false; no GDPR popup)
 * ------------------------------------------------------------------------ */
int zb_game_patches(so_module *il2cpp) {
  uintptr_t base = (uintptr_t)il2cpp->load_virtbase;
  const uint32_t GUARD = 0xa9be57feu;          /* stp x30,x21,[sp,#-0x20]! -- all three */
  const uint32_t RET   = 0xd65f03c0u;          /* ret            */
  const uint32_t MOVW0 = 0x52800000u;          /* mov w0, #0     */

  /* NOTE (18-21º turno): with PlayerPrefs fixed the game reaches the loading
   * scenario and stalls on the splash. Ruled out as the gate: the boot NREs
   * (PlayerPrefs, fixed), GameAnalytics (un-noop'd 18º, no effect), RemoteConfig
   * (21º probe: get_IsInitialized is never polled). Remaining stubbed service was
   * AchievementsService.Initialize -> now UN-NOOP'd so it runs for real (confirmed
   * non-blocking: it only fetches AntEngine/scenario refs and returns, no GPGS
   * sign-in wait), in case a loading system waits on its side effects. Kept:
   *  - GDPRPopupController.IsNeedToShow -> false: the real check could return true
   *    and pop an undismissable consent dialog (input/M7 not wired). false = "no
   *    consent needed, proceed", the standard non-EU path. */
  static const struct { const char *name; uint32_t rva; int ret_false; } P[] = {
    { "GDPRPopupController.IsNeedToShow", 0x19C170Cu, 1 },
  };
  const int n = (int)(sizeof(P) / sizeof(P[0]));
  int done = 0;
  for (int i = 0; i < n; i++) {
    if (!so_rva_in_image(il2cpp, P[i].rva, 8)) {
      debugPrintf("[gamepatch] %-32s SKIP (rva 0x%06x past image 0x%zx)\n",
                  P[i].name, P[i].rva, il2cpp->load_size);
      continue;
    }
    uintptr_t a = base + P[i].rva;
    uint32_t got = *(volatile uint32_t *)a;
    if (got != GUARD) {
      debugPrintf("[gamepatch] %-32s SKIP (guard %08x, found %08x)\n",
                  P[i].name, GUARD, got);
      continue;
    }
    int rc;
    if (P[i].ret_false) {
      uint32_t code[2] = { MOVW0, RET };
      rc = so_patch_code((void *)a, code, sizeof code);
    } else {
      rc = so_patch_code((void *)a, &RET, 4);
    }
    if (rc < 0) { debugPrintf("[gamepatch] %-32s FAILED to write\n", P[i].name); continue; }
    debugPrintf("[gamepatch] %-32s neutralised @+0x%06x\n", P[i].name, P[i].rva);
    done++;
  }

  /* Lofelt NiceVibrations DeviceCapabilities.cctor (@+0x2afb104) parses the Android
   * API level out of SystemInfo.operatingSystem ("...API-33..."). Our fake-Android
   * OS string lacks that pattern, so the 3-char substring handed to int.Parse
   * (bl @+0x2afb1c4) is non-numeric and throws FormatException. Once a type
   * initializer throws, EVERY later access re-throws TypeInitializationException --
   * and the game fires a haptic on death and on reward pickup, so that throw
   * propagates up and ABORTS the death sequence and the pickup completion (player
   * can't die; the collect effect sticks on screen and blocks input). Skip the
   * parse -- force the parsed version to 0 (mov w0,#0); the cctor stores it as
   * platformVersion and derives isVersionSupported = (0 > 16) = false, so haptics
   * disable cleanly with no throw. Verify the exact bl word first: a shifted RVA
   * SKIPs instead of corrupting code. */
  {
    const uint32_t PARSE_RVA = 0x2afb1c4u;
    const uint32_t PARSE_BL  = 0x940677bdu;   /* bl int.Parse */
    const uint32_t MOV_W0_0  = 0x52800000u;   /* mov w0, #0    */
    if (!so_rva_in_image(il2cpp, PARSE_RVA, 4)) {
      debugPrintf("[gamepatch] %-32s SKIP (rva 0x%06x past image)\n",
                  "NiceVibrations.DeviceCaps", PARSE_RVA);
    } else {
      uintptr_t a = base + PARSE_RVA;
      uint32_t got = *(volatile uint32_t *)a;
      if (got != PARSE_BL) {
        debugPrintf("[gamepatch] %-32s SKIP (word %08x, found %08x)\n",
                    "NiceVibrations.DeviceCaps", PARSE_BL, got);
      } else if (so_patch_code((void *)a, &MOV_W0_0, 4) < 0) {
        debugPrintf("[gamepatch] %-32s FAILED to write\n", "NiceVibrations.DeviceCaps");
      } else {
        debugPrintf("[gamepatch] %-32s haptics version parse skipped @+0x%06x\n",
                    "NiceVibrations.DeviceCaps", PARSE_RVA);
        done++;
      }
    }
  }
  debugPrintf("[gamepatch] %d game-side patches applied (incl. NiceVibrations fix)\n", done);
  return done;
}
