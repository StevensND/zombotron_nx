/* zombotron_prefs.c -- KV-backed reimplementation of Unity PlayerPrefs.
 *
 * WHY: In Unity 6 (6000.2.x) UnityEngine.PlayerPrefs is a NATIVE libunity
 * binding (the managed GetString/GetInt/... marshal through ReadOnlySpan<char>
 * + ExceptionMarshaller into an internal call), NOT the Java SharedPreferences
 * path. So the loader's JNI SharedPreferences KV store (unity_jni.c) is bypassed
 * entirely: libunity's native prefs has no backing store on Switch and returns
 * "" / 0 instead of the caller's default. That made
 * PlayerPrefs.GetString(save, defaultSave) return "" on first boot ->
 * AntJSON.Parse(null) -> SettingsBoxLoader.Read NRE -> Game.Start() aborts
 * before InitializeUI -> BLACK SCREEN.
 *
 * FIX (mechanism borrowed from subwaysurfers_nx's nx_patch_path_icall /
 * nx_patch_il2cpp_method): overwrite each public PlayerPrefs method's entry with
 *   ldr x16, #8 ; br x16 ; .quad <C hook>
 * and service the get/set from the loader's existing KV store (nx_prefs_*,
 * backed by sdmc:/switch/zombotron_nx/prefs.kv). This returns the stored value
 * when present and the caller's DEFAULT on a miss (first-boot-correct), and --
 * because Set and Save are hooked too -- real save persistence, exactly like
 * the JNI path does for the older-Unity ports in this lineage.
 *
 * The hooks fully REPLACE the methods (they never call the original), so
 * clobbering the first 16 bytes is safe. Every site is fail-safe: bounds-checked
 * and prologue-sanity-checked before writing; a bad RVA is skipped, not crashed.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "config.h"
#include "util.h"
#include "so_util.h"

/* KV store (unity_jni.c) -- values are stored as strings with a type tag. */
extern void        nx_prefs_set(char type, const char *key, const char *val);
extern const char *nx_prefs_get(const char *key);
extern void        nx_prefs_del(const char *key);
extern void        nx_prefs_flush(void);

/* il2cpp String layout: [0]=klass [8]=monitor [0x10]=length(int32) [0x14]=UTF-16 */
static void *(*g_string_new)(const char *) = NULL;

static int str_utf8(void *str, char *out, size_t size) {
  if (!out || size == 0) return 0;
  out[0] = 0;
  if (!str) return 0;
  int len = *(int *)((char *)str + 0x10);
  const uint16_t *src = (const uint16_t *)((char *)str + 0x14);
  size_t o = 0;
  for (int i = 0; i < len && o + 4 < size; i++) {
    uint32_t c = src[i];
    if (c < 0x80) { out[o++] = (char)c; }
    else if (c < 0x800) { out[o++] = (char)(0xc0 | (c >> 6)); out[o++] = (char)(0x80 | (c & 0x3f)); }
    else { out[o++] = (char)(0xe0 | (c >> 12)); out[o++] = (char)(0x80 | ((c >> 6) & 0x3f)); out[o++] = (char)(0x80 | (c & 0x3f)); }
  }
  out[o] = 0;
  return 1;
}

/* ---- hooks: il2cpp static methods carry a trailing const MethodInfo* arg ---- */

static int g_get_log = 0;   /* log the first few reads to confirm the path */

static void *h_GetString2(void *key, void *def, void *mi) { (void)mi;
  char k[256];
  if (str_utf8(key, k, sizeof k)) {
    const char *v = nx_prefs_get(k);
    if (g_get_log < 8) { debugPrintf("[pphook] GetString(\"%s\") -> %s\n", k, v ? "stored" : "default"); g_get_log++; }
    if (v && g_string_new) return g_string_new(v);
  }
  return def;                     /* miss -> caller's default (first-boot correct) */
}
static void *h_GetString1(void *key, void *mi) { (void)mi;
  char k[256];
  if (str_utf8(key, k, sizeof k)) {
    const char *v = nx_prefs_get(k);
    if (v && g_string_new) return g_string_new(v);
  }
  return g_string_new ? g_string_new("") : NULL;
}
static void h_SetString(void *key, void *val, void *mi) { (void)mi;
  char k[256];
  if (!str_utf8(key, k, sizeof k)) return;
  /* Size the value buffer to the whole string. A fixed 2048-byte buffer here used
   * to TRUNCATE long values mid-string: the achievements record grows past 2 KB as
   * the player unlocks things, and a truncated value is invalid JSON in prefs.kv,
   * so the next boot failed to parse the save and hung on a black screen (it looked
   * like the save was lost -- it wasn't, it was being written cut off). str_utf8
   * emits at most 3 bytes per UTF-16 code unit, so 3*len+8 always fits. */
  int ulen = val ? *(int *)((char *)val + 0x10) : 0;
  if (ulen < 0) ulen = 0;
  size_t vsz = (size_t)ulen * 3 + 8;
  char *v = (char *)malloc(vsz);
  if (!v) return;                    /* OOM: skip rather than write a truncated (corrupt) value */
  str_utf8(val, v, vsz);
  nx_prefs_set('S', k, v);
  free(v);
}
static int h_GetInt(void *key, int def, void *mi) { (void)mi;
  char k[256];
  if (str_utf8(key, k, sizeof k)) { const char *v = nx_prefs_get(k); if (v) return (int)strtol(v, NULL, 10); }
  return def;
}
static void h_SetInt(void *key, int val, void *mi) { (void)mi;
  char k[256], b[32];
  if (str_utf8(key, k, sizeof k)) { snprintf(b, sizeof b, "%d", val); nx_prefs_set('I', k, b); }
}
static float h_GetFloat(void *key, float def, void *mi) { (void)mi;
  char k[256];
  if (str_utf8(key, k, sizeof k)) { const char *v = nx_prefs_get(k); if (v) return (float)strtod(v, NULL); }
  return def;
}
static void h_SetFloat(void *key, float val, void *mi) { (void)mi;
  char k[256], b[40];
  if (str_utf8(key, k, sizeof k)) { snprintf(b, sizeof b, "%.9g", (double)val); nx_prefs_set('F', k, b); }
}
static int h_HasKey(void *key, void *mi) { (void)mi;
  char k[256];
  if (str_utf8(key, k, sizeof k)) return nx_prefs_get(k) ? 1 : 0;
  return 0;
}
static void h_DeleteKey(void *key, void *mi) { (void)mi;
  char k[256];
  if (str_utf8(key, k, sizeof k)) nx_prefs_del(k);
}
static void h_Save(void *mi) { (void)mi; nx_prefs_flush(); }

/* Overwrite a method entry with a branch to `hook`. Fail-safe. */
static int patch_method(so_module *m, uint32_t rva, void *hook, const char *name) {
  if (!so_rva_in_image(m, rva, 16)) {
    debugPrintf("[pphook] %-11s SKIP (rva 0x%06x past image)\n", name, rva);
    return 0;
  }
  uintptr_t site = (uintptr_t)m->load_virtbase + rva;
  uint32_t entry = *(volatile uint32_t *)site;
  if (entry == 0u || entry == 0xffffffffu) {
    debugPrintf("[pphook] %-11s SKIP (bad entry %08x)\n", name, entry);
    return 0;
  }
  uint32_t stub[4] = {
    0x58000050u,                                   /* ldr x16, #8 */
    0xd61f0200u,                                   /* br  x16     */
    (uint32_t)((uintptr_t)hook & 0xffffffffu),
    (uint32_t)((uintptr_t)hook >> 32),
  };
  if (so_patch_code((void *)site, stub, sizeof stub) < 0) {
    debugPrintf("[pphook] %-11s FAILED to write @+0x%06x\n", name, rva);
    return 0;
  }
  debugPrintf("[pphook] %-11s -> KV @+0x%06x\n", name, rva);
  return 1;
}

/* Public entry: install the KV-backed PlayerPrefs hooks. RVAs from Zombotron's
 * own dump.cs (public overloads have clean string/int/float ABIs; the private
 * *_Injected span wrappers underneath are left alone). */
int zb_playerprefs_hooks(so_module *il2cpp) {
  g_string_new = (void *(*)(const char *))so_try_find_addr_rx(il2cpp, "il2cpp_string_new");
  if (!g_string_new) {
    debugPrintf("[pphook] il2cpp_string_new unavailable -> PlayerPrefs hooks NOT installed\n");
    return 0;
  }
  int n = 0;
  n += patch_method(il2cpp, 0x3276020u, (void *)&h_GetString2, "GetString2");
  n += patch_method(il2cpp, 0x3276324u, (void *)&h_GetString1, "GetString1");
  n += patch_method(il2cpp, 0x3275FC8u, (void *)&h_SetString,  "SetString");
  n += patch_method(il2cpp, 0x3275BECu, (void *)&h_GetInt,     "GetInt");
  n += patch_method(il2cpp, 0x3275B94u, (void *)&h_SetInt,     "SetInt");
  n += patch_method(il2cpp, 0x3275E00u, (void *)&h_GetFloat,   "GetFloat");
  n += patch_method(il2cpp, 0x3275DA8u, (void *)&h_SetFloat,   "SetFloat");
  n += patch_method(il2cpp, 0x327636Cu, (void *)&h_HasKey,     "HasKey");
  n += patch_method(il2cpp, 0x327651Cu, (void *)&h_DeleteKey,  "DeleteKey");
  n += patch_method(il2cpp, 0x32766C0u, (void *)&h_Save,       "Save");
  debugPrintf("[pphook] PlayerPrefs KV-backed hooks installed: %d/10\n", n);
  return n;
}

/* ------------------------------------------------------------------------
 * Loading-gate probe (18/19º turno): the game boots to the splash then stalls
 * -- it reads its save slots and the AntEngine loading scenario never advances
 * to the menu (GPU flat, no scene load). Game.Start registers a RemoteConfig
 * update callback; on this title remote config is fed by GameAnalytics, which
 * cannot fetch offline. If the loading scenario gates on RemoteConfig being
 * initialized, it waits forever. Force RemoteConfig.get_IsInitialized() to log
 * (so we learn whether it is even polled) and return true (so a poll-based gate
 * proceeds on the baked RemoteConfigAsset defaults). Fail-safe; if it is not the
 * gate this only adds a log line and the exception tracer catches any fallout. */
static int g_rc_log = 0;
static int h_RC_IsInitialized(void *mi) { (void)mi;
  if (g_rc_log < 16) { debugPrintf("[rcgate] RemoteConfig.IsInitialized polled -> forcing true (#%d)\n", g_rc_log); g_rc_log++; }
  return 1;
}

int zb_gameflow_probe(so_module *il2cpp) {
  int n = patch_method(il2cpp, 0x1AA5B44u, (void *)&h_RC_IsInitialized, "RC.IsInitialized");
  debugPrintf("[rcgate] RemoteConfig.IsInitialized probe installed: %d/1\n", n);
  return n;
}
