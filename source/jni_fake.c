/* jni_fake.c -- fake JNI environment for the MVGL engine (libcrx.so)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <pthread.h>
#include <switch.h>

#include "config.h"
#include "util.h"
extern const char *nx_prefs_get(const char *key);  /* KV lookup in unity_jni.c */
#include "jni_fake.h"
#include "data.h"
#include "text2bitmap.h"
#include "movie_player.h"
#include "editbox.h"
#include "android_native_unity.h"
#include "jni_unimpl.h"
#include "libc_shim.h"   /* managed_path: device-less paths for managed code */
#include "opensles.h"    /* audio_fmod_open/write: FMOD native-audio output sink */
#include "zombotron_video.h"  /* zb_video_mix_audio: splash audio into the same block */

#define JNI_OK 0
#define JNI_VERSION_1_6 0x00010006

typedef uint64_t juint;

void fmod_audio_start(void); // defined below; launched from FMODAudioDevice.start()

// ---------------------------------------------------------------------------
// fake object model
// ---------------------------------------------------------------------------

enum {
  TAG_OBJECT = 0x4f424a31, // 'OBJ1'  heap object (freeable)
  TAG_STRING = 0x53545231, // 'STR1'
  TAG_OBJARR = 0x4f415231, // 'OAR1'
  TAG_PRIARR = 0x50415231, // 'PAR1'
  TAG_ID     = 0x4d494431, // 'MID1'  pooled, never freed
  TAG_CLASS  = 0x434c5331, // 'CLS1'  pooled, never freed
  // text2bitmap.h BITMAP_TAG ('BMP1') is also handled by free_ref
};

typedef struct { uint32_t tag; char label[64]; } FakeObject;
typedef struct { uint32_t tag; char *utf; } FakeString;
typedef struct { uint32_t tag; int len; void **items; } FakeObjArray;
typedef struct { uint32_t tag; int len; int elem_size; void *data; } FakePriArray;
typedef struct { uint32_t tag; char cls[96]; char name[64]; char sig[160]; } FakeID;
typedef struct { uint32_t tag; char name[96]; } FakeClass;

volatile int jni_quit_requested = 0;

// ---------------------------------------------------------------------------
// local reference registry (matches the engine's Push/PopLocalFrame brackets)
// ---------------------------------------------------------------------------

#define MAX_LOCALS 1048576
#define MAX_FRAMES 64
static void *locals[MAX_LOCALS];
static int locals_top = 0;
static int frames[MAX_FRAMES];
static int frame_top = 0;
static Mutex locals_lock;

static void *reg_local(void *ref) {
  if (ref) {
    mutexLock(&locals_lock);
    if (locals_top < MAX_LOCALS)
      locals[locals_top++] = ref;
    mutexUnlock(&locals_lock);
  }
  return ref;
}

// interned-string pool: the engine re-creates the same constant strings (class
// names, the activity name) constantly; pool them by content so repeats don't
// fill the local-ref table. Pooled strings are never reg_local'd, and free_ref
// skips them (range check below).
#define MAX_ISTR 512
static FakeString istr_pool[MAX_ISTR];
static int istr_count = 0;

static void free_ref(void *ref) {
  if (!ref)
    return;
  if ((char *)ref >= (char *)istr_pool && (char *)ref < (char *)&istr_pool[MAX_ISTR])
    return;  // interned string -- pooled, never freed
  switch (*(uint32_t *)ref) {
    case TAG_STRING: { FakeString *s = ref; free(s->utf); free(s); break; }
    case TAG_PRIARR: { FakePriArray *a = ref; free(a->data); free(a); break; }
    case TAG_OBJARR: { FakeObjArray *a = ref; free(a->items); free(a); break; }
    case TAG_OBJECT: free(ref); break;
    case BITMAP_TAG: text2bitmap_free((FakeBitmap *)ref); break;
    default: break; // TAG_ID / TAG_CLASS are pooled
  }
}

static void delete_local(void *ref) {
  if (!ref)
    return;
  mutexLock(&locals_lock);
  for (int i = locals_top - 1; i >= 0; i--) {
    if (locals[i] == ref) {
      locals[i] = locals[--locals_top];
      free_ref(ref);
      break;
    }
  }
  mutexUnlock(&locals_lock);
}

// ---------------------------------------------------------------------------
// object constructors
// ---------------------------------------------------------------------------

// Intern objects by label -- one pooled object per class (TAG_CLASS so free_ref()
// leaves it alone, never reg_local'd) -- so the engine's frequent NewObject calls
// don't fill the local-ref table. Safe: our objects are opaque, stateless handles
// dispatched by method class, not by identity.
#define MAX_IOBJ 128
static FakeObject iobj_pool[MAX_IOBJ];
static int iobj_count = 0;
void *jni_make_object(const char *label) {
  const char *l = (label && label[0]) ? label : "obj";
  mutexLock(&locals_lock);
  void *r = NULL;
  for (int i = 0; i < iobj_count; i++)
    if (!strcmp(iobj_pool[i].label, l)) { r = &iobj_pool[i]; break; }
  if (!r) {
    if (iobj_count >= MAX_IOBJ) r = &iobj_pool[0];
    else {
      FakeObject *o = &iobj_pool[iobj_count++];
      o->tag = TAG_CLASS;             // pooled: free_ref() ignores TAG_CLASS
      strncpy(o->label, l, sizeof(o->label) - 1);
      r = o;
    }
  }
  mutexUnlock(&locals_lock);
  return r;
}

void *jni_make_string(const char *utf) {
  const char *u = utf ? utf : "";
  mutexLock(&locals_lock);
  for (int i = 0; i < istr_count; i++)            // repeats reuse the pooled string
    if (!strcmp(istr_pool[i].utf, u)) { void *r = &istr_pool[i]; mutexUnlock(&locals_lock); return r; }
  if (istr_count < MAX_ISTR) {
    FakeString *s = &istr_pool[istr_count++];
    s->tag = TAG_STRING;
    s->utf = strdup(u);
    mutexUnlock(&locals_lock);
    return s;                                      // pooled, not reg_local'd
  }
  mutexUnlock(&locals_lock);
  FakeString *s = calloc(1, sizeof(*s));           // pool full: one-off local string
  s->tag = TAG_STRING;
  s->utf = strdup(u);
  return reg_local(s);
}

static void *make_pri_array_adopt(void *data, int len, int elem_size) {
  FakePriArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_PRIARR;
  a->len = len;
  a->elem_size = elem_size;
  a->data = data;
  return reg_local(a);
}

static const char *obj_str(void *jstr) {
  FakeString *s = jstr;
  if (s && s->tag == TAG_STRING)
    return s->utf;
  return "";
}

// UTF-16 code-unit count of a modified-UTF-8 string (Java String.length()).
// ASCII -> byte count; astral planes count as a surrogate pair. Used by both
// GetStringLength and the String.length() upcall handler.
static juint utf16_len(const char *str) {
  const unsigned char *p = (const unsigned char *)(str ? str : "");
  juint n = 0;
  while (*p) {
    const unsigned char c = *p;
    juint adv; uint32_t cp;
    if (c < 0x80)      { cp = c;        adv = 1; }
    else if (c < 0xE0) { cp = c & 0x1F; adv = 2; }
    else if (c < 0xF0) { cp = c & 0x0F; adv = 3; }
    else               { cp = c & 0x07; adv = 4; }
    for (juint k = 1; k < adv; k++) {
      if (!p[k]) { adv = k; break; }
      cp = (cp << 6) | (p[k] & 0x3F);
    }
    n += (cp >= 0x10000) ? 2u : 1u;
    p += adv;
  }
  return n;
}

// register a text2bitmap result in the local table so the engine's recycle /
// DeleteLocalRef frees it
static void *reg_bitmap(FakeBitmap *b) { return reg_local(b); }

// ---------------------------------------------------------------------------
// interned classes + singletons
// ---------------------------------------------------------------------------

// A game like this interns 150-300 distinct classes, so give the pool ample headroom.
#define MAX_CLASSES 512
static FakeClass class_pool[MAX_CLASSES];
static int class_count = 0;

static void *intern_class(const char *name) {
  for (int i = 0; i < class_count; i++)
    if (!strcmp(class_pool[i].name, name))
      return &class_pool[i];
  if (class_count >= MAX_CLASSES)
    return &class_pool[0];
  FakeClass *c = &class_pool[class_count++];
  c->tag = TAG_CLASS;
  strncpy(c->name, name, sizeof(c->name) - 1);
  return c;
}

static const char *class_name_of(void *cls) {
  FakeClass *c = cls;
  return (c && c->tag == TAG_CLASS) ? c->name : "";
}

static FakeObject *g_activity_obj = NULL;   // the MyNativeActivity instance
static FakeObject *g_asset_mgr = NULL;      // android.content.res.AssetManager

void *jni_make_activity_object(void) {
  if (!g_activity_obj) {
    g_activity_obj = calloc(1, sizeof(*g_activity_obj));
    g_activity_obj->tag = TAG_CLASS; // pooled (never freed)
    strcpy(g_activity_obj->label, "MyNativeActivity");
  }
  return g_activity_obj;
}

static void *get_asset_manager_obj(void) {
  if (!g_asset_mgr) {
    g_asset_mgr = calloc(1, sizeof(*g_asset_mgr));
    g_asset_mgr->tag = TAG_CLASS;
    strcpy(g_asset_mgr->label, "AssetManager");
  }
  return g_asset_mgr;
}

// The engine fetches the ClassLoader every frame; hand back a cached singleton
// (pooled, never reg_local'd) so it doesn't fill the local-ref table.
static FakeObject *g_classloader = NULL;
static void *get_classloader_obj(void) {
  if (!g_classloader) {
    g_classloader = calloc(1, sizeof(*g_classloader));
    g_classloader->tag = TAG_CLASS;
    strcpy(g_classloader->label, "ClassLoader");
  }
  return g_classloader;
}

// ---------------------------------------------------------------------------
// method/field ID pool (class-aware)
// ---------------------------------------------------------------------------

#define MAX_IDS 512
static FakeID id_pool[MAX_IDS];
static int id_count = 0;

static FakeID *get_id(const char *cls, const char *name, const char *sig) {
  for (int i = 0; i < id_count; i++)
    if (!strcmp(id_pool[i].name, name) && !strcmp(id_pool[i].sig, sig) &&
        !strcmp(id_pool[i].cls, cls))
      return &id_pool[i];
  if (id_count >= MAX_IDS) {
    debugPrintf("JNI: id pool exhausted (%s.%s)\n", cls, name);
    return &id_pool[0];
  }
  FakeID *id = &id_pool[id_count++];
  id->tag = TAG_ID;
  strncpy(id->cls, cls ? cls : "", sizeof(id->cls) - 1);
  strncpy(id->name, name, sizeof(id->name) - 1);
  strncpy(id->sig, sig, sizeof(id->sig) - 1);
  debugPrintf("JNI id: %s.%s %s\n", id->cls, id->name, id->sig);
  return id;
}

// ---------------------------------------------------------------------------
// dispatch helpers
// ---------------------------------------------------------------------------

static int sig_returns(const char *sig, const char *ret) {
  const char *rp = strchr(sig, ')');
  return rp && strstr(rp + 1, ret) == rp + 1;
}

static int name_has(const char *name, const char *sub) { return strstr(name, sub) != NULL; }

// --- Text2Bitmap ------------------------------------------------------------
// draw methods return a Bitmap; the first arg is the text String, the next int
// is the pixel size. measure methods return I (width or height by name).

static void *t2b_object(const FakeID *id, va_list va) {
  const char *text = obj_str(va_arg(va, void *));
  const int size = va_arg(va, int);
  FakeBitmap *b = text2bitmap_render(text, size);
  (void)id;
  return b ? reg_bitmap(b) : NULL;
}

static juint t2b_int(const FakeID *id, va_list va) {
  const char *text = obj_str(va_arg(va, void *));
  const int size = va_arg(va, int);
  if (name_has(id->name, "Height"))
    return (juint)text2bitmap_measure_height(text, size);
  if (name_has(id->name, "Width"))
    return (juint)text2bitmap_measure_width(text, size);
  return (juint)text2bitmap_measure_width(text, size);
}

// --- MoviePlayer ------------------------------------------------------------

static const char *first_string_arg(const char *sig, va_list va); // defined below

static void mov_void(const FakeID *id, va_list va) {
  if (!strcmp(id->name, "SetMovieDB")) { movie_set_db(first_string_arg(id->sig, va)); return; }
  if (name_has(id->name, "Stop") || name_has(id->name, "stop")) { movie_stop(); return; }
  if (name_has(id->name, "Pause")) { movie_pause(); return; }
  if (name_has(id->name, "Resume")) { movie_resume(); return; }
  if (name_has(id->name, "Play") || name_has(id->name, "play") ||
      name_has(id->name, "Start")) {
    movie_play(first_string_arg(id->sig, va), 0); // the String arg is the movie name
    return;
  }
}

static juint mov_int(const FakeID *id, va_list va) {
  (void)va;
  if (name_has(id->name, "Playing") || name_has(id->name, "playing"))
    return (juint)movie_is_playing();
  return 0;
}

// --- MyNativeActivity / general activity ------------------------------------

// Zombotron ships no OBB and no .mvgl archives -- every asset is inside the APK
// under assets/bin/Data. The inherited value here was Chaos Rings 3's archive
// base name; handing that to a getter that Unity is not expected to call at all
// could only ever mislead. Empty string == "no expansion file".
#define MAIN_OBB_BASE ""

/* --- Language selection --------------------------------------------------------------
 *
 * Bad Piggies is fully localized. Localizer.DetectLocale() (il2cpp 0x9F2D54) calls
 * UnityEngine.Application.systemLanguage, and Localizer.LoadLocalizationFile() reads a SINGLE
 * XML holding every language as text (there are NO per-language scene bundles here -- that was
 * a different game). This APK's global-metadata.dat carries the full locale table: ar cs da de
 * el en es fi fr hu id it ja ko nl no pl pt ro ru sv th tr uk vi zh. So all we have to do is
 * make Application.systemLanguage report the Switch's language, and the game localizes itself.
 *
 * How systemLanguage flows on Android (and thus here): native Unity's Application::
 * GetSystemLanguage calls back through JNI to Locale.getDefault().getLanguage(), then maps the
 * returned ISO-639-1 code to the UnityEngine.SystemLanguage enum. Our fake JNI answers that
 * Locale query from lang_code() below -- so lang_code() is the single source of truth, and
 * pointing it at the Switch's SetLanguage is the entire fix. No il2cpp hook needed.
 *
 * The initial language follows the Switch system language. Zombotron's intro
 * has its own language selector, and the player's choice there wins from then
 * on -- which is why there is no override file.
 */

/* SetLanguage (nn::settings) -> the game's own ISO code.
 *
 * Bad Piggies ships exactly SEVEN languages (per the store listing): English, French, German,
 * Italian, Japanese, Simplified Chinese, Spanish. Anything else falls back to English -- the
 * game does the same. Chinese is Simplified only, so BOTH the Simplified and Traditional Switch
 * settings map to it (a zh-Hant user gets the one Chinese that exists, not English). We report
 * it as "zh_CN" so downstream locale formatting is unambiguously Simplified. */
static const char *nx_setlang_to_iso(u64 sl) {
  switch (sl) {
    case SetLanguage_JA:                 return "ja";
    case SetLanguage_FR:
    case SetLanguage_FRCA:               return "fr";
    case SetLanguage_DE:                 return "de";
    case SetLanguage_IT:                 return "it";
    case SetLanguage_ES:
    case SetLanguage_ES419:              return "es";
    case SetLanguage_ZHCN:
    case SetLanguage_ZHHANS:
    case SetLanguage_ZHTW:
    case SetLanguage_ZHHANT:             return "zh_CN";  /* Simplified only; both settings map here */
    case SetLanguage_ENUS:
    case SetLanguage_ENGB:
    default:                             return "en";     /* English + anything unsupported */
  }
}

/* Resolve once: the Switch system language, else English. The
 * set of ISO codes here is exactly the localization set present in global-metadata.dat; if the
 * system language isn't among them the game itself falls back to English in DetectLocale. */
static const char *nx_resolve_lang(void) {
  static const char *cached = NULL;
  if (cached) return cached;

  /* System language only. There is no override file any more: Zombotron has a
   * language selector in its own intro, and whatever the player picks there is
   * what the game uses from then on. This just supplies the initial value. */

  u64 sl = SetLanguage_ENUS;
  Result rc = setInitialize();
  if (R_SUCCEEDED(rc)) {
    u64 code = 0; SetLanguage lang = SetLanguage_ENUS;
    if (R_SUCCEEDED(setGetSystemLanguage(&code)) &&
        R_SUCCEEDED(setMakeLanguage(code, &lang)))
      sl = (u64)lang;
    setExit();
  }
  cached = nx_setlang_to_iso(sl);
  debugPrintf("[lang] system language -> %s (auto)\n", cached);
  return cached;
}

static const char *lang_code(void) {
  return nx_resolve_lang();
}

/* Retained for source compatibility with the input-hook plumbing; Bad Piggies does not use
 * the Layton eLanguage/getCurrentLanguage scheme (its localization is text, not scenes). */
int nx_pick_elanguage(void) {
  return 1;
}

// Walk a JNI arg list per the signature and return the first non-empty String
// argument's text (used to seed the keyboard from ShowEditBox's initial text).
static const char *first_string_arg(const char *sig, va_list va) {
  const char *p = sig ? strchr(sig, '(') : NULL;
  if (!p) return "";
  for (p++; *p && *p != ')'; p++) {
    switch (*p) {
      case 'I': case 'Z': case 'B': case 'C': case 'S': (void)va_arg(va, int); break;
      case 'F': case 'D': (void)va_arg(va, double); break;
      case 'J': (void)va_arg(va, long long); break;
      case '[':
        (void)va_arg(va, void *);
        if (p[1] == 'L') { p++; while (*p && *p != ';') p++; } else if (p[1]) p++;
        break;
      case 'L': {
        const char *s = obj_str(va_arg(va, void *));
        while (*p && *p != ';') p++;
        if (s && s[0]) return s;
        break;
      }
      default: break;
    }
  }
  return "";
}

// EditBox / TextBox names the engine drives via JNI (both share our swkbd box)
static int is_editbox_show(const char *n)  { return name_has(n, "ShowEditBox")  || name_has(n, "OpenEditBox")  || name_has(n, "ShowTextBox") || name_has(n, "OpenTextBox"); }
static int is_editbox_open(const char *n)  { return name_has(n, "IsOpenEditBox") || name_has(n, "IsOpenTextBox"); }
static int is_editbox_text(const char *n)  { return name_has(n, "GetEditBoxText") || name_has(n, "GetTextBoxText"); }
static int is_editbox_close(const char *n) { return name_has(n, "CloseEditBox") || name_has(n, "CloseTextBox"); }

/* jni_string_utf is defined far below (shared with unity_jni.c) and unity_jni.h
 * is included after this point, so forward-declare it for act_object's
 * getProperty()/locale arg reads. */
const char *jni_string_utf(void *jstr);

/* AudioManager.getProperty(PROPERTY_OUTPUT_SAMPLE_RATE / FRAMES_PER_BUFFER): FMOD reads these to
 * size its OpenSL output. The key string doesn't reach us (arrives "") and the two properties need
 * opposite answers -- rate must be 48000, frames-per-buffer must be small (48000 there fails init
 * with error 60). FMOD retries the same output with 3 getProperty calls per attempt, so we hedge:
 * attempt 1 (calls 0-2) treats the 3rd call as the buffer query, attempt 2 (calls 3-5) the 2nd;
 * whichever is right, one attempt sees rate=48000 + fpb=256 and inits cleanly. */
static int g_last_output_prop = 0;   /* set by field_object from the static-field read, if it reaches us */

/* AudioManager.getProperty(PROPERTY_OUTPUT_SAMPLE_RATE / FRAMES_PER_BUFFER).
 *
 * REPORTS 24000/256 -- and that is deliberate, having been wrong once.
 *
 * An earlier revision "fixed" this to 48000 by reasoning from the note below
 * about FMOD sizing its output from the reported rate. That was wrong twice
 * over. pvz_fusion_nx, a working port whose opensles.c is byte-for-byte the same
 * as this one, reports 24000. And opensles.c here already opens the SDL device
 * at the Switch's native 48000 and RESAMPLES every player, so the number handed
 * to FMOD is the rate FMOD should mix at, not the rate the hardware runs at.
 *
 * Zombotron's engine confirms 24000 is what it wants:
 *     "Forced to initialize FMOD to the device driver's system output rate 48000
 *      ... compared to selected sample rate 24000"
 * That message appeared only after the 48000 change, together with
 * "Error initializing output device (60)".
 *
 * FRAMES_PER_BUFFER must stay small and nonzero: 0 or "" fails OpenSL init with
 * error 60 (zookeeperdx_nx hit exactly that). It is now
 * ZB_AUDIO_FRAMES_PER_BUFFER in config.h, because it also divides FMOD's
 * up-front buffer count and a value too LARGE silently yields zero buffers.
 *
 * NOT YET VERIFIED ON HARDWARE. If audio is silent, wrong-pitched, or FMOD logs
 * error 60, this pair is the first thing to try: 48000/256 -> 48000/512 ->
 * 44100/256. The OpenSL-vs-AudioTrack selection itself is forced in
 * zombotron_patches.c and is a separate question from these two numbers. */
static void *getproperty_value(const char *key) {
  static int call_n = 0;
  int which;
  if (key && strstr(key, "SAMPLE_RATE"))            which = 1;
  else if (key && strstr(key, "FRAMES_PER_BUFFER")) which = 2;
  else if (g_last_output_prop)                      which = g_last_output_prop;
  else                                              which = (call_n == 2 || call_n == 4) ? 2 : 1;
  call_n++;
  /* ZB_AUDIO_FRAMES_PER_BUFFER is the divisor in FMOD's up-front buffer-count
   * calculation, not merely a latency setting -- see the note in config.h. */
  #define ZB_STR2(x) #x
  #define ZB_STR(x)  ZB_STR2(x)
  const char *val = (which == 2) ? ZB_STR(ZB_AUDIO_FRAMES_PER_BUFFER) : "24000";
  /* Never hand back "" here: FMOD parses an empty PROPERTY_OUTPUT_FRAMES_PER_BUFFER as
   * framesPerBuffer == 0 and fails its OpenSL init with error 60 (zookeeperdx_nx hit
   * exactly that). Log the first few so the boot log shows the audio config is queried. */
  if (call_n <= 6)
    debugPrintf("[jni] getProperty(%s) -> %s\n", key ? key : "(null)", val);
  return jni_make_string(val);
}

static void *act_object(const FakeID *id, va_list va) {
  /* FRAME PACING -- report the Android Choreographer as UNAVAILABLE (return null).
   * On Android, Choreographer.getInstance() + postFrameCallback drives Unity's
   * per-frame vsync wait inside nativeRender (the "UnityChoreographer" object at
   * libunity 0x628fd4). With no Java Choreographer we can never deliver the vsync
   * frame callback, so that wait blocks FOREVER on the first real render frame --
   * the frame-2 black screen. A null Choreographer makes the engine log
   * "Choreographer NOT available" and take its non-vsync fallback (render as fast
   * as the loop drives it), which unblocks nativeRender. Also covers the Swappy
   * SwappyDisplayManager/ChoreographerCallback helpers. Must precede the generic
   * non-null object handlers below. */
  if ((name_has(id->cls, "Choreographer") || name_has(id->cls, "SwappyDisplayManager")) &&
      (name_has(id->name, "getInstance") || sig_returns(id->sig, "Landroid/view/Choreographer;")))
    return NULL;
  /* Uri.encode/decode: Unity round-trips PlayerPrefs keys through these. We are
   * the storage, so identity (return the input string) round-trips correctly and
   * keeps keys non-empty. Must precede the generic handlers. */
  if (name_has(id->cls, "net/Uri") && (name_has(id->name, "encode") || name_has(id->name, "decode")))
    return va_arg(va, void *);
  if (name_has(id->name, "AssetManager") || sig_returns(id->sig, "Landroid/content/res/AssetManager;"))
    return get_asset_manager_obj();
  if (name_has(id->name, "ClassLoader") || sig_returns(id->sig, "Ljava/lang/ClassLoader;"))
    return get_classloader_obj();
  if (sig_returns(id->sig, "Ljava/lang/Class;"))
    return intern_class("java/lang/Object");
  // version / package / device / storage strings
  if (name_has(id->name, "VersionName")) return jni_make_string(CS_VERSION_NAME);
  if (name_has(id->name, "PackageName")) return jni_make_string(CS_PACKAGE);
  if (name_has(id->name, "DeviceModel")) return jni_make_string("Switch");
  // archive name getters: the engine builds "<dir>/<name>.android.mvgl" for 5
  // slots (main + patch + 3 asset packs). We map them to the 5 shipped archives
  // (main.10007 + the four CRDB media DBs) so all of them mount.
  if (name_has(id->name, "ObbMainFileName"))  return jni_make_string(MAIN_OBB_BASE);
  if (name_has(id->name, "ObbPatchFileName")) return jni_make_string("CRDBbgm");
  if (name_has(id->name, "AssetPack1"))       return jni_make_string("CRDBvoice");
  if (name_has(id->name, "AssetPack2"))       return jni_make_string("CRDBse");
  if (name_has(id->name, "AssetPack3"))       return jni_make_string("CRDBmov");
  if (name_has(id->name, "getProperty"))
    return getproperty_value(jni_string_utf(va_arg(va, void *)));
  if (name_has(id->name, "Language") || name_has(id->name, "language")) {
    /* Locale.getLanguage() returns ONLY the language subtag (e.g. "zh"), never the region.
     * Unity's Application::GetSystemLanguage maps that bare code to the SystemLanguage enum, so
     * handing it "zh_CN" here would fail to match and fall back to English. The region ("CN")
     * belongs to getCountry() / the locale string below, not to getLanguage(). */
    const char *l = lang_code();
    return jni_make_string(!strcmp(l, "zh_CN") ? "zh" : l);
  }
  // Environment.getExternalStorageState() must return the SAME token as the
  // Environment.MEDIA_MOUNTED field ("mounted", see field_object) or the engine
  // decides external storage is unavailable and the save path never initialises.
  if (name_has(id->cls, "os/Environment")) {
    if (name_has(id->name, "ExternalStorageState")) return jni_make_string("mounted");
    if (name_has(id->name, "Directory")) return jni_make_object("java/io/File"); /* ->getAbsolutePath */
  }
  // Locale.getCountry/getISO3*/toString/getDisplayName: secondary locale queries
  // (the language itself comes from the getCurrentLanguage() hook). Mirror the
  // chosen language so any locale-derived formatting stays consistent. Guarded by
  // the Locale class so we don't hijack toString()/getCountry on other objects.
  if (name_has(id->cls, "Locale")) {
    const char *l = lang_code();
    /* Country/ISO3/locale strings for each language the game ships (7 total). These feed
     * secondary locale queries (formatting, getDisplayName); the language itself is `l`. */
    const char *ctry = "US", *iso3l = "eng", *iso3c = "USA", *loc = "en_US";
    if      (!strcmp(l, "fr"))    { ctry = "FR"; iso3l = "fra"; iso3c = "FRA"; loc = "fr_FR"; }
    else if (!strcmp(l, "es"))    { ctry = "ES"; iso3l = "spa"; iso3c = "ESP"; loc = "es_ES"; }
    else if (!strcmp(l, "de"))    { ctry = "DE"; iso3l = "deu"; iso3c = "DEU"; loc = "de_DE"; }
    else if (!strcmp(l, "it"))    { ctry = "IT"; iso3l = "ita"; iso3c = "ITA"; loc = "it_IT"; }
    else if (!strcmp(l, "ja"))    { ctry = "JP"; iso3l = "jpn"; iso3c = "JPN"; loc = "ja_JP"; }
    else if (!strcmp(l, "zh_CN")) { ctry = "CN"; iso3l = "zho"; iso3c = "CHN"; loc = "zh_CN"; }
    if (!strcmp(id->name, "getCountry"))     return jni_make_string(ctry);
    if (!strcmp(id->name, "getISO3Language"))return jni_make_string(iso3l);
    if (!strcmp(id->name, "getISO3Country")) return jni_make_string(iso3c);
    if (!strcmp(id->name, "toString") || name_has(id->name, "getDisplayName") ||
        name_has(id->name, "getDisplayLanguage"))
      return jni_make_string(loc);
  }
  if (name_has(id->name, "DataPath") || name_has(id->name, "StoragePath") ||
      name_has(id->name, "FilesDir") || name_has(id->name, "RootPath") ||
      name_has(id->name, "ObbDir") || name_has(id->name, "AssetPath") ||
      name_has(id->name, "Path"))
    return jni_make_string(managed_path(data_dir()));
  // text the user typed on the Switch software keyboard
  if (is_editbox_text(id->name))
    return jni_make_string(editbox_text());
  // asset-pack names ("" is fine: the engine appends the hardcoded CRDB* name,
  // and the data layer's basename fallback finds the flat file regardless)
  // Android object getters that must NOT be null, or the engine aborts the
  // chain. getPackageInfo()/getApplicationInfo() are how Unity reaches
  // PackageInfo.versionName/versionCode (-> Application.version); returning null
  // here is why the version stayed blank even with field access fixed -- Unity
  // got a null PackageInfo and never read the field. Hand back live (opaque)
  // objects; the subsequent field reads then resolve via field_object/field_int.
  if (name_has(id->name, "getPackageInfo"))     return jni_make_object("android/content/pm/PackageInfo");
  if (name_has(id->name, "getApplicationInfo")) return jni_make_object("android/content/pm/ApplicationInfo");
  if (name_has(id->name, "getPackageManager"))  return jni_make_object("android/content/pm/PackageManager");
  if (name_has(id->name, "getResources"))       return jni_make_object("android/content/res/Resources");
  if (name_has(id->name, "getConfiguration"))   return jni_make_object("android/content/res/Configuration");
  if (sig_returns(id->sig, "Ljava/lang/String;"))
    return jni_make_string(""); // UUID, asset-pack name, etc.
  (void)va;
  // BOOT GATE (Laytonbmr): the C# Payment code fetches a Level-5/Play-Billing plugin's
  // native instance via a JNI object-returning call; the old default NULL here set
  // nativeInstance=null -> "native handle can not be null" NullReferenceException -> the
  // startup coroutine never loads the title (permanent black boot scene). Return a live
  // (opaque) fake object by DEFAULT for any otherwise-unhandled object method so plugin
  // init "succeeds" and boot proceeds; the fake object's own methods then resolve
  // through act_* benign defaults. Explicit NULLs above (Choreographer, ...) still win.
  return jni_make_object(id->cls);
}

static juint act_int(const FakeID *id, va_list va) {
  // java.lang.Integer.parseInt(String[,radix]) / Long.parseLong: FMOD's audio
  // path parses getProperty()'s "48000"/"64" results through these. The old
  // act_int fall-through returned 0 -> framesPerBuffer parsed to 0 -> FMOD's
  // OpenSL output init failed with "Error initializing output device" (60).
  if (name_has(id->name, "parseInt") || name_has(id->name, "parseLong")) {
    const char *s = first_string_arg(id->sig, va);
    juint v = (juint)(s ? strtol(s, NULL, 10) : 0);
    debugPrintf("[jni] %s(\"%s\") -> %u\n", id->name, s ? s : "", v);
    return v;
  }
  if (is_editbox_open(id->name)) return (juint)editbox_is_open();
  // some builds expose Show/Open as an int (success) call rather than void
  if (is_editbox_show(id->name)) { editbox_show(first_string_arg(id->sig, va), 32); return 1; }
  // Play Asset Delivery: with NO Play Core on Switch, the engine MUST take the
  // "missing" path, where it treats every asset pack as install-time/local and
  // reads assets synchronously from the APK/bundle. Returning false (the old
  // default) tells Unity Play Core IS present, so it uses the ASYNC AssetPackManager
  // path -- it calls getAssetPackState() with a callback that we never invoke and
  // then waits forever for the pack, so the resident scene never loads and
  // ResidentSystem.Awake never runs (the live boot stall). Return true.
  if (name_has(id->name, "playCoreApiMissing")) return 1;
  // The other Google check whose default (0 == ConnectionResult.SUCCESS) wrongly
  // means "Play Services available": isGooglePlayServicesAvailable(). Return
  // SERVICE_MISSING (1) so the Play Games plugin cleanly disables itself instead
  // of trying to sign in against GMS that isn't there. (Not hit during boot --
  // Play Games activates on user action -- but correct for when it is.)
  if (name_has(id->name, "isGooglePlayServicesAvailable")) return 1; /* ConnectionResult.SERVICE_MISSING */
  (void)va;
  // every other "is something open / clicked / ok" probe -> false/0
  return 0;
}

static float act_float(const FakeID *id, va_list va) {
  (void)va;
  float x, y, z;
  android_get_orientation(&x, &y, &z);
  if (name_has(id->name, "OrientationX")) return x;
  if (name_has(id->name, "OrientationY")) return y;
  if (name_has(id->name, "OrientationZ")) return z;
  return 0.0f;
}

static void act_void(const FakeID *id, va_list va) {
  if (is_editbox_show(id->name)) { editbox_show(first_string_arg(id->sig, va), 32); return; }
  if (is_editbox_close(id->name)) { editbox_close(); return; }
  (void)va;
  if (!strcmp(id->name, "finish") || name_has(id->name, "appEnd") ||
      name_has(id->name, "exitApp"))
    jni_quit_requested = 1;
  // openStore / sendBroadcast / IME open / Mobage / web view: no-op
}

// ---------------------------------------------------------------------------
// top-level dispatch by class + return kind
// ---------------------------------------------------------------------------

#include "unity_jni.h"
#include "zombotron_jni.h"

static int is_t2b(const char *cls)  { return name_has(cls, "Text2Bitmap"); }
static int is_mov(const char *cls)  { return name_has(cls, "MoviePlayer"); }

// Breadcrumb: the game's own Java side (jp.kiteretsu.* save/load + license
// plugin) is reached only through JNI upcalls. The DEX shows the exact classes
// (loadsavedata.{SRecord,SCryption,SUtility,NativeLoad}, LicenseVerification),
// but not which the C# actually invokes or in what order. Log each unique
// app-class upcall once so the first run that reaches the save/license stage
// tells us precisely what to implement, instead of guessing. Behaviour is
// unchanged: after logging, the call still falls through to the act_* handlers.
/* A "framework" class we never need to trace (Android/JDK/Unity/Google-infra). Anything
 * else is the game's own app/plugin code -- log those to find boot gates. */
static int is_framework_cls(const char *c) {
  if (!c) return 1;
  return name_has(c, "android/") || name_has(c, "android.") ||
         name_has(c, "java/")    || name_has(c, "java.")    ||
         name_has(c, "javax/")   || name_has(c, "dalvik/")  ||
         name_has(c, "com/unity3d/") || name_has(c, "com.unity3d.") ||
         name_has(c, "org/khronos")  || name_has(c, "sun/");
}
static void log_app_upcall(const FakeID *id) {
  /* DIAGNOSTIC: log EVERY JNI method upcall once per class.method (dedup by the
   * interned class+name ptr pair), NO framework filter -- the boot gate (Payment
   * nativeInstance null) may go through com/android/vending or com/android/billingclient
   * (Google Play Billing), which a naive "android/" filter wrongly excludes. Bounded by
   * dedup (~a few hundred unique pairs). Once the culprit is known, re-narrow this. */
  (void)is_framework_cls;
  static const void *seen_c[400]; static const void *seen_n_[400]; static int seen_k = 0;
  for (int i = 0; i < seen_k; i++)
    if (seen_c[i] == id->cls && seen_n_[i] == id->name) return;
  if (seen_k < 400) { seen_c[seen_k] = id->cls; seen_n_[seen_k] = id->name; seen_k++; }
  debugPrintf("[jni] call: %s.%s%s\n", id->cls, id->name, id->sig);
}

static void *dispatch_object(void *recv, const FakeID *id, va_list va) {
  if (!id) return NULL;
  log_app_upcall(id);
  /* String.getBytes([charset]) -> byte[] of the string's UTF-8 bytes. Unity's
   * PlayerPrefs key-encoding is key.getBytes() -> new String([B,charset) ->
   * Uri.encode(...); without real bytes the whole chain collapsed to "" and
   * every encoded-key pref collided. Route by the FakeString receiver. */
  if (recv && *(uint32_t *)recv == TAG_STRING && name_has(id->name, "getBytes")) {
    const char *u = ((FakeString *)recv)->utf; int n = (int)strlen(u);
    char *d = malloc(n > 0 ? n : 1); if (n) memcpy(d, u, n);
    return make_pri_array_adopt(d, n, 1);
  }
  if (zombotron_owns_class(id->cls)) return zombotron_dispatch_object(recv, id, va);
  if (unity_owns_class(id->cls)) return unity_dispatch_object(recv, id, va);
  // any method returning a Bitmap is text rendering (Char2Bitmap / getShadowBitmap
  // / ...): the loaded class always reads back as java/lang/Object, so route by
  // return type rather than class name.
  const int wants_bitmap = sig_returns(id->sig, "Landroid/graphics/Bitmap;");
  return (is_t2b(id->cls) || wants_bitmap) ? t2b_object(id, va) : act_object(id, va);
}
static juint dispatch_int(void *recv, const FakeID *id, va_list va) {
  if (!id) return 0;
  log_app_upcall(id);
  // java.lang.String instance methods reached via CallIntMethod (Unity's
  // java::lang::String::length() does this to size path buffers). The receiver
  // is our FakeString; GetObjectClass reports it as java/lang/Object, so route
  // on the receiver tag + method name, NOT id->cls. Returning 0 here (the old
  // act_int fall-through) undersizes the OBB-path sprintf buffer and overflows.
  if (recv && *(uint32_t *)recv == TAG_STRING) {
    if (!strcmp(id->name, "length"))   return utf16_len(((FakeString *)recv)->utf);
    if (!strcmp(id->name, "hashCode")) return 0;
    if (!strcmp(id->name, "isEmpty"))  return ((FakeString *)recv)->utf[0] == '\0';
  }
  /* Boxed PlayerPrefs value (Integer/Long/Boolean) from getAll(): unbox by the
   * receiver so only our own boxes are affected. intValue/longValue/booleanValue
   * all land here (CallInt/Long/BooleanMethod -> dispatch_int). */
  if (unity_is_boxed(recv)) return unity_boxed_int(recv);
  if (zombotron_owns_class(id->cls)) return zombotron_dispatch_int(recv, id, va);
  if (unity_owns_class(id->cls)) return unity_dispatch_int(recv, id, va);
  if (is_t2b(id->cls)) return t2b_int(id, va);
  if (is_mov(id->cls)) return mov_int(id, va);
  return act_int(id, va);
}
static float dispatch_float(void *recv, const FakeID *id, va_list va) {
  if (!id) return 0.0f;
  log_app_upcall(id);
  if (unity_is_boxed(recv)) return unity_boxed_float(recv);   /* Float.floatValue */
  return act_float(id, va);
}
static void dispatch_void(void *recv, const FakeID *id, va_list va) {
  log_app_upcall(id);
  if (name_has(id->cls, "FMODAudioDevice")) {
    debugPrintf("[fmod] FMODAudioDevice.%s() CALLED\n", id->name);
    /* Path A (driving fmodProcess) is blocked: the mixer's source buffer is
     * allocated only by the AudioTrack output start sequence the Java run() loop
     * drives, which never runs here -- so fmodProcess always copies from a null
     * source (Data Abort at +0x28), confirmed even after a 120-frame warmup.
     * Pump left in the tree but disabled; audio is moving to FMOD OutputOpenSL
     * (Path B), which FMOD drives natively via opensles.c. */
    if (0 && !strcmp(id->name, "start")) fmod_audio_start();
  }
  if (zombotron_owns_class(id->cls)) { zombotron_dispatch_void(recv, id, va); return; }
  if (unity_owns_class(id->cls)) { unity_dispatch_void(recv, id, va); return; }
  if (is_mov(id->cls)) { mov_void(id, va); return; }
  act_void(id, va);
}

// ---------------------------------------------------------------------------
// JNIEnv function implementations
// ---------------------------------------------------------------------------

static juint j_GetVersion(void *env) { (void)env; return JNI_VERSION_1_6; }
static void *j_FindClass(void *env, const char *name) {
  (void)env;
  return intern_class(name ? name : "?");
}
static void *j_GetObjectClass(void *env, void *obj) {
  (void)env;
  if (obj && *(uint32_t *)obj == BITMAP_TAG) return intern_class("android/graphics/Bitmap");
  return intern_class("java/lang/Object");
}
static void *j_GetMethodID(void *env, void *cls, const char *name, const char *sig) {
  (void)env;
  return get_id(class_name_of(cls), name ? name : "", sig ? sig : "");
}
static void *j_GetFieldID(void *env, void *cls, const char *name, const char *sig) {
  (void)env; return get_id(class_name_of(cls), name ? name : "", sig ? sig : "");
}

/* String(byte[][,charset]) constructor: Unity builds PlayerPrefs keys as
 * bytes -> new String(bytes, charset) -> Uri.encode(...). Returning a
 * content-less object made every encoded key empty, so all such prefs collided
 * under "" (corrupted Screenmanager resolution prefs -> bad resolution ->
 * crash). Decode the byte array (UTF-8) into a real FakeString. Other ctors are
 * unaffected (still a labelled object). */
static void *new_object_dispatch(void *cls, void *mid, void *first_arg) {
  const char *cn = class_name_of(cls);
  if (cn && strstr(cn, "java/lang/String")) {
    FakeID *m = mid;
    if (m && strstr(m->sig, "[B")) {              /* String([B...) */
      int len = 0; char *b = jni_bytearray_data(first_arg, &len);
      if (b && len > 0) { char *t = malloc(len + 1); memcpy(t, b, len); t[len] = 0;
        void *s = jni_make_string(t); free(t); return s; }
      return jni_make_string("");
    }
  }
  return jni_make_object(cn);
}

static void *j_NewObject(void *env, void *cls, void *mid, ...) {
  (void)env;
  va_list va; va_start(va, mid); void *a0 = va_arg(va, void *); va_end(va);
  return new_object_dispatch(cls, mid, a0);
}
static void *j_NewObjectV(void *env, void *cls, void *mid, va_list va) {
  (void)env; void *a0 = va_arg(va, void *);
  return new_object_dispatch(cls, mid, a0);
}

static void *j_NewGlobalRef(void *env, void *obj) {
  (void)env;
  mutexLock(&locals_lock);
  for (int i = locals_top - 1; i >= 0; i--)
    if (locals[i] == obj) { locals[i] = locals[--locals_top]; break; }
  mutexUnlock(&locals_lock);
  return obj;
}
static void j_DeleteGlobalRef(void *env, void *obj) { (void)env; free_ref(obj); }
static void j_DeleteLocalRef(void *env, void *obj) { (void)env; delete_local(obj); }
static void *j_NewLocalRef(void *env, void *obj) { (void)env; return obj; }
static juint j_IsSameObject(void *env, void *a, void *b) { (void)env; return a == b; }

/* IsInstanceOf: we can't track the runtime type of opaque fake jobjects, so answer
 * optimistically (a failed cast traps the game in a retry loop) -- except boxed PlayerPrefs
 * values, which must be exact or getAll() misreads every value as the first type checked. */
static juint j_IsInstanceOf(void *env, void *obj, void *clazz) {
  (void)env;
  const char *cn = class_name_of(clazz);
  int ui = unity_isinstance(obj, cn);
  if (ui >= 0) return (juint)ui;
  if (obj && *(uint32_t *)obj == TAG_STRING) {
    if (strstr(cn, "String")) return 1;
    if (strstr(cn, "Integer") || strstr(cn, "Long") || strstr(cn, "Float") ||
        strstr(cn, "Double")  || strstr(cn, "Boolean") || strstr(cn, "Character") ||
        strstr(cn, "Short")   || strstr(cn, "Byte"))
      return 0;
  }
  return 1;
}
static juint j_EnsureLocalCapacity(void *env, int cap) { (void)env; (void)cap; return 0; }

static juint j_PushLocalFrame(void *env, int cap) {
  (void)env; (void)cap;
  mutexLock(&locals_lock);
  if (frame_top < MAX_FRAMES)
    frames[frame_top++] = locals_top;
  mutexUnlock(&locals_lock);
  return 0;
}
static void *j_PopLocalFrame(void *env, void *result) {
  (void)env;
  mutexLock(&locals_lock);
  const int mark = frame_top > 0 ? frames[--frame_top] : 0;
  for (int i = mark; i < locals_top; i++)
    if (locals[i] != result)
      free_ref(locals[i]);
  locals_top = mark;
  if (result && locals_top < MAX_LOCALS)
    locals[locals_top++] = result;
  mutexUnlock(&locals_lock);
  return result;
}

// --- Call<type>Method (instance + static share class-aware dispatch) --------

#define CALL_VARIADIC(fn, ret_t, dispatch) \
  static ret_t fn(void *env, void *recv, FakeID *id, ...) { \
    (void)env; va_list va; va_start(va, id); \
    ret_t r = dispatch(recv, id, va); va_end(va); return r; } \
  static ret_t fn##V(void *env, void *recv, FakeID *id, va_list va) { \
    (void)env; return dispatch(recv, id, va); }

CALL_VARIADIC(j_CallObjectMethod, void *, dispatch_object)
CALL_VARIADIC(j_CallIntMethod, juint, dispatch_int)
CALL_VARIADIC(j_CallBooleanMethod, juint, dispatch_int)
CALL_VARIADIC(j_CallLongMethod, juint, dispatch_int)
CALL_VARIADIC(j_CallFloatMethod, float, dispatch_float)

static void j_CallVoidMethod(void *env, void *recv, FakeID *id, ...) {
  (void)env; va_list va; va_start(va, id); dispatch_void(recv, id, va); va_end(va);
}
static void j_CallVoidMethodV(void *env, void *recv, FakeID *id, va_list va) {
  (void)env; dispatch_void(recv, id, va);
}

#define j_CallStaticObjectMethod   j_CallObjectMethod
#define j_CallStaticObjectMethodV  j_CallObjectMethodV
#define j_CallStaticIntMethod      j_CallIntMethod
#define j_CallStaticIntMethodV     j_CallIntMethodV
#define j_CallStaticBooleanMethod  j_CallBooleanMethod
#define j_CallStaticBooleanMethodV j_CallBooleanMethodV
#define j_CallStaticLongMethod     j_CallLongMethod
#define j_CallStaticLongMethodV    j_CallLongMethodV
#define j_CallStaticFloatMethod    j_CallFloatMethod
#define j_CallStaticFloatMethodV   j_CallFloatMethodV
#define j_CallStaticVoidMethod     j_CallVoidMethod
#define j_CallStaticVoidMethodV    j_CallVoidMethodV

// --- Call<type>MethodA / NewObjectA (jvalue[] args) -------------------------
// SWIG bindings and AndroidJavaObject.CallStatic<T>()/Call<T>() marshal their
// arguments into a jvalue[] array and invoke the "A" variants. A va_list cannot
// be reconstructed from jvalue[] portably, so we forward to the variadic form
// with no varargs: the dispatch keys off the resolved method name and the
// object/value getters ignore positional args (defaulting to a non-null handle
// of the right class), which is what these init paths need. Previously these
// slots fell through to the unimplemented stub and returned 0/null, hanging the
// first scene (e.g. UNIMPL slot 116 == CallStaticObjectMethodA).
static void *j_CallObjectMethodA (void *e, void *r, FakeID *id, const void *a){
  // getProperty()'s String key lives in jvalue[0] and does NOT survive the
  // va_list-less forward below, so pull it directly. FMOD's OpenSL output reads
  // PROPERTY_OUTPUT_FRAMES_PER_BUFFER through this "A" path; without the key it
  // got "" -> framesPerBuffer 0 -> FMOD error 60.
  if (a && name_has(id->name, "getProperty"))
    return getproperty_value(jni_string_utf(((void *const *)a)[0]));
  if (a && name_has(id->cls, "net/Uri") && (name_has(id->name, "encode") || name_has(id->name, "decode")))
    return (void *)((void *const *)a)[0];        /* identity, jvalue path */
  /* SharedPreferences.getString(key, defValue) via the jvalue[] path: the
   * generic forward below drops the args, so the key/default never reach the
   * variadic handler in unity_jni.c and a missing key wrongly yielded "" ->
   * PlayerPrefs.GetString(save, defaultSave)=="" -> AntJSON.Parse null ->
   * SettingsBoxLoader.Read NRE -> Game.Start aborts -> black screen. Resolve it
   * here: return the stored value, else the caller's default (jvalue[1]). */
  if (a && name_has(id->cls, "SharedPreferences") && !name_has(id->cls, "Editor") &&
      name_has(id->name, "getString")) {
    const char *key = jni_string_utf(((void *const *)a)[0]);
    const char *val = key ? nx_prefs_get(key) : NULL;
    void       *defo = ((void *const *)a)[1];
    const char *def  = defo ? jni_string_utf(defo) : NULL;
    debugPrintf("[prefs] getString(\"%s\") -> %s (len %zu) [A]\n",
                key ? key : "", val ? "stored" : "default",
                (size_t)strlen(val ? val : (def ? def : "")));
    if (val) return jni_make_string(val);
    return jni_make_string(def ? def : "");
  }
  return j_CallObjectMethod(e, r, id);
}
static juint j_CallBooleanMethodA(void *e, void *r, FakeID *id, const void *a){ (void)a; return j_CallBooleanMethod(e, r, id); }
static juint j_CallIntMethodA    (void *e, void *r, FakeID *id, const void *a){
  // parseInt/parseLong via the jvalue[] path: read the String from jvalue[0].
  if (a && (name_has(id->name, "parseInt") || name_has(id->name, "parseLong"))) {
    const char *s = jni_string_utf(((void *const *)a)[0]);
    juint v = (juint)(s ? strtol(s, NULL, 10) : 0);
    debugPrintf("[jni] %s(\"%s\") -> %u [A]\n", id->name, s ? s : "", v);
    return v;
  }
  (void)a; return j_CallIntMethod(e, r, id);
}
static juint j_CallLongMethodA   (void *e, void *r, FakeID *id, const void *a){ (void)a; return j_CallLongMethod(e, r, id); }
static float j_CallFloatMethodA  (void *e, void *r, FakeID *id, const void *a){ (void)a; return j_CallFloatMethod(e, r, id); }
static void  j_CallVoidMethodA   (void *e, void *r, FakeID *id, const void *a){ (void)a; j_CallVoidMethod(e, r, id); }
static void *j_NewObjectA        (void *e, void *cls, void *mid, const void *a){ (void)e;
  return new_object_dispatch(cls, mid, a ? ((void *const *)a)[0] : NULL); }
#define j_CallStaticObjectMethodA  j_CallObjectMethodA
#define j_CallStaticBooleanMethodA j_CallBooleanMethodA
#define j_CallStaticIntMethodA     j_CallIntMethodA
#define j_CallStaticLongMethodA    j_CallLongMethodA
#define j_CallStaticFloatMethodA   j_CallFloatMethodA
#define j_CallStaticVoidMethodA    j_CallVoidMethodA

// --- strings ----------------------------------------------------------------

static void *j_NewStringUTF(void *env, const char *utf) { (void)env; return jni_make_string(utf); }
static void *j_NewString(void *env, const uint16_t *u, int len) {
  (void)env;
  if (!u || len < 0) return jni_make_string("");
  char *tmp = malloc((size_t)len * 4 + 1);
  int o = 0;
  for (int i = 0; i < len; i++) { // naive UTF-16 -> UTF-8 (BMP)
    const uint32_t c = u[i];
    if (c < 0x80) tmp[o++] = (char)c;
    else if (c < 0x800) { tmp[o++] = 0xC0 | (c >> 6); tmp[o++] = 0x80 | (c & 0x3F); }
    else { tmp[o++] = 0xE0 | (c >> 12); tmp[o++] = 0x80 | ((c >> 6) & 0x3F); tmp[o++] = 0x80 | (c & 0x3F); }
  }
  tmp[o] = 0;
  void *s = jni_make_string(tmp);
  free(tmp);
  return s;
}
static const char *j_GetStringUTFChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0; return obj_str(jstr);
}
static void j_ReleaseStringUTFChars(void *env, void *jstr, const char *utf) { (void)env; (void)jstr; (void)utf; }
static juint j_GetStringUTFLength(void *env, void *jstr) { (void)env; return strlen(obj_str(jstr)); }

// GetStringUTFRegion: the engine reads ALL its strings through this (not
// GetStringUTFChars), so it must work. Copies the [start, start+len) region as
// modified UTF-8 into buf. Our strings are ASCII (paths / archive names), where
// UTF-16 char offsets == UTF-8 byte offsets, so a byte copy is exact.
static void j_GetStringUTFRegion(void *env, void *jstr, int start, int len, char *buf) {
  (void)env;
  if (!buf) return;
  const char *s = obj_str(jstr);
  const int slen = (int)strlen(s);
  if (start < 0) start = 0;
  if (start > slen) start = slen;
  if (len < 0) len = 0;
  if (start + len > slen) len = slen - start;
  memcpy(buf, s + start, (size_t)len);
  buf[len] = '\0';
}
// GetStringRegion: UTF-16 variant; widen ASCII bytes into jchar (uint16) buf.
static void j_GetStringRegion(void *env, void *jstr, int start, int len, uint16_t *buf) {
  (void)env;
  if (!buf) return;
  const char *s = obj_str(jstr);
  const int slen = (int)strlen(s);
  if (start < 0) start = 0;
  if (start > slen) start = slen;
  if (len < 0) len = 0;
  if (start + len > slen) len = slen - start;
  for (int i = 0; i < len; i++) buf[i] = (uint8_t)s[start + i];
}
// GetStringLength must return the UTF-16 code-unit count, not the byte count
// (CJK text is multi-byte in UTF-8); engine code sizes UTF-16 buffers with it.
static juint j_GetStringLength(void *env, void *jstr) {
  (void)env;
  return utf16_len(obj_str(jstr));
}

// --- arrays -----------------------------------------------------------------

static juint j_GetArrayLength(void *env, void *arr) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && (a->tag == TAG_PRIARR || a->tag == TAG_OBJARR))
    return a->len;
  return 0;
}

static void *new_pri_array(int len, int elem_size) {
  void *data = calloc(len ? len : 1, elem_size);
  return make_pri_array_adopt(data, len, elem_size);
}
static void *j_NewByteArray(void *env, int len) { (void)env; return new_pri_array(len, 1); }
static void *j_NewIntArray(void *env, int len) { (void)env; return new_pri_array(len, 4); }
static void *j_NewFloatArray(void *env, int len) { (void)env; return new_pri_array(len, 4); }

static void *j_NewObjectArray(void *env, int len, void *cls, void *init) {
  (void)env; (void)cls;
  FakeObjArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_OBJARR;
  a->len = len;
  a->items = calloc(len ? len : 1, sizeof(void *));
  for (int i = 0; i < len; i++) a->items[i] = init;
  return reg_local(a);
}
static void *j_GetObjectArrayElement(void *env, void *arr, int i) {
  (void)env;
  FakeObjArray *a = arr;
  return (a && a->tag == TAG_OBJARR && i >= 0 && i < a->len) ? a->items[i] : NULL;
}
static void j_SetObjectArrayElement(void *env, void *arr, int i, void *val) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && a->tag == TAG_OBJARR && i >= 0 && i < a->len) a->items[i] = val;
}

static void *j_GetPriArrayElements(void *env, void *arr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0;
  FakePriArray *a = arr;
  return (a && a->tag == TAG_PRIARR) ? a->data : NULL;
}
static void j_ReleasePriArrayElements(void *env, void *arr, void *elems, int mode) {
  (void)env; (void)arr; (void)elems; (void)mode;
}
static void j_GetPriArrayRegion(void *env, void *arr, int start, int len, void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy(buf, (char *)a->data + (size_t)start * a->elem_size, (size_t)len * a->elem_size);
}
static void j_SetPriArrayRegion(void *env, void *arr, int start, int len, const void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy((char *)a->data + (size_t)start * a->elem_size, buf, (size_t)len * a->elem_size);
}

// --- fields -----------------------------------------------------------------
// The engine and the game DO read Java fields: android.os.Build.* (device id),
// Build.VERSION.SDK_INT (API gating), PackageInfo.versionName/versionCode (the
// Application.version the boot path logs as blank today), DisplayMetrics.*, and
// Configuration.*. Returning null/0 universally (the old stub) blanks the app
// version -- which can throw in version-parsing boot code -- and zeroes display
// metrics. Route every field read through a name-based dispatcher. fid is the
// FakeID GetFieldID handed back, so cls/name/sig are all available.
//
// Placeholders marked CHECK are safe defaults, not the shipped values; the game
// never verifies them against anything but a stubbed RemoteConfig, so exact
// numbers don't gate boot. Easy to correct once known.
#define APP_VERSION_NAME CS_VERSION_NAME  /* "1.1.3" */
#define APP_VERSION_CODE CS_VERSION_CODE  /* Color Sheep versionCode */
#define NX_SDK_INT       33        /* Android 13 -- high enough to pass any minSdk gate  */

static int fld_is(const FakeID *id, const char *cls_sub, const char *name) {
  return name_has(id->cls, cls_sub) && !strcmp(id->name, name);
}

// One-line-per-unique-field diagnostic: tells the next run exactly which Java
// fields the game reads (and lets us confirm versionName/currentActivity/etc.
// are being exercised). Dedup by interned name pointer, like log_app_upcall.
static void log_field_read(const FakeID *id, char kind) {
  static const void *seen[128]; static int seen_n = 0;
  for (int i = 0; i < seen_n; i++) if (seen[i] == id->name) return;
  if (seen_n < 128) seen[seen_n++] = id->name;
  debugPrintf("[jni] field(%c): %s.%s %s\n", kind, id->cls, id->name, id->sig);
}

static void *field_object(const FakeID *id) {
  const char *n = id->name, *c = id->cls;
  // PackageInfo / ApplicationInfo version string
  if (!strcmp(n, "versionName")) return jni_make_string(APP_VERSION_NAME);
  // UnityPlayer statics: currentActivity is THE Activity -- null here NPEs every
  // UnityPlayer.currentActivity.getXxx() in managed code, so hand back a live
  // (opaque) Activity that our method dispatch then services.
  if (name_has(c, "unity3d/player/UnityPlayer")) {
    if (!strcmp(n, "currentActivity")) return jni_make_object("android/app/Activity");
    if (!strcmp(n, "MANUFACTURER"))    return jni_make_string("Nintendo");
  }
  // AudioManager.PROPERTY_OUTPUT_* are static String field keys the engine reads
  // just before AudioManager.getProperty(key) to size FMOD's audio path. Return
  // the real Android property-name strings AND record which one was read
  // (g_last_output_prop) so getProperty() can answer even when the key argument
  // is lost on the JNI call path (see getproperty_value). These are AudioManager
  // fields, NOT UnityPlayer -- gating them on the wrong class meant they never
  // matched, getProperty saw "", and FMOD got framesPerBuffer 0 -> error 60.
  if (name_has(c, "media/AudioManager")) {
    if (!strcmp(n, "PROPERTY_OUTPUT_FRAMES_PER_BUFFER")) { g_last_output_prop = 2; return jni_make_string("android.media.property.OUTPUT_FRAMES_PER_BUFFER"); }
    if (!strcmp(n, "PROPERTY_OUTPUT_SAMPLE_RATE"))       { g_last_output_prop = 1; return jni_make_string("android.media.property.OUTPUT_SAMPLE_RATE"); }
  }
  // Context.*_SERVICE name constants -> the strings getSystemService() expects
  if (name_has(c, "content/Context")) {
    if (!strcmp(n, "AUDIO_SERVICE"))        return jni_make_string("audio");
    if (!strcmp(n, "DISPLAY_SERVICE"))      return jni_make_string("display");
    if (!strcmp(n, "WINDOW_SERVICE"))       return jni_make_string("window");
    if (!strcmp(n, "LOCATION_SERVICE"))     return jni_make_string("location");
    if (!strcmp(n, "CONNECTIVITY_SERVICE")) return jni_make_string("connectivity");
    if (!strcmp(n, "MEDIA_ROUTER_SERVICE")) return jni_make_string("media_router");
    if (!strcmp(n, "VIBRATOR_SERVICE"))     return jni_make_string("vibrator");
  }
  // Environment.MEDIA_MOUNTED MUST equal getExternalStorageState()'s return
  // ("mounted", set in act_object) or the storage check fails and save data is
  // disabled. Keep both in lockstep.
  if (name_has(c, "os/Environment")) {
    if (!strcmp(n, "MEDIA_MOUNTED"))           return jni_make_string("mounted");
    if (!strcmp(n, "MEDIA_MOUNTED_READ_ONLY")) return jni_make_string("mounted_ro");
  }
  if (name_has(c, "pm/PackageManager")) {
    if (!strcmp(n, "FEATURE_AUDIO_LOW_LATENCY")) return jni_make_string("android.hardware.audio.low_latency");
    if (!strcmp(n, "FEATURE_AUDIO_PRO"))         return jni_make_string("android.hardware.audio.pro");
  }
  // android.os.Build identity strings (all public static final String)
  if (name_has(c, "os/Build")) {
    if (!strcmp(n, "MODEL"))        return jni_make_string("Switch");
    if (!strcmp(n, "MANUFACTURER")) return jni_make_string("Nintendo");
    if (!strcmp(n, "BRAND"))        return jni_make_string("Nintendo");
    if (!strcmp(n, "DEVICE"))       return jni_make_string("Switch");
    if (!strcmp(n, "PRODUCT"))      return jni_make_string("Switch");
    if (!strcmp(n, "HARDWARE"))     return jni_make_string("nx");
    if (!strcmp(n, "BOARD"))        return jni_make_string("nx");
    if (!strcmp(n, "DISPLAY"))      return jni_make_string("nx");
    if (!strcmp(n, "ID"))           return jni_make_string("REL");
    if (!strcmp(n, "TYPE"))         return jni_make_string("user");
    if (!strcmp(n, "TAGS"))         return jni_make_string("release-keys");
    if (!strcmp(n, "FINGERPRINT"))  return jni_make_string("Nintendo/Switch/Switch:13/REL/10007:user/release-keys");
    if (!strcmp(n, "BOOTLOADER"))   return jni_make_string("unknown");
    if (!strcmp(n, "HOST"))         return jni_make_string("localhost");
    if (!strcmp(n, "USER"))         return jni_make_string("nx");
    if (!strcmp(n, "SERIAL"))       return jni_make_string("unknown");
    if (!strcmp(n, "RELEASE"))      return jni_make_string("13");        /* Build.VERSION.* */
    if (!strcmp(n, "CODENAME"))     return jni_make_string("REL");
    if (!strcmp(n, "INCREMENTAL"))  return jni_make_string("10007");
    if (!strcmp(n, "SECURITY_PATCH")) return jni_make_string("2023-01-01");
    if (!strcmp(n, "BASE_OS"))      return jni_make_string("");
  }
  // Any other String-typed field -> "" (non-null avoids NPEs in string ops).
  if (sig_returns(id->sig, "Ljava/lang/String;")) return jni_make_string("");
  // Any other object field stays null; array fields handled by the caller.
  return NULL;
}

/* The render resolution main.c resolved (ZB_FORCE_SCREEN_W/H). All
 * screen-metric reporting derives from it so the game lays out for exactly the buffer we
 * render into. Density tracks the resolution to keep the logical width ~360dp (a real
 * phone at any of these resolutions), matching standard Android density buckets:
 *   720->320dpi/2.0, 1080->480dpi/3.0, 1440->640dpi/4.0. */
extern int screen_width, screen_height;
static int   nx_report_density_dpi(void){ int w = screen_width > 0 ? screen_width : 720;
                                          return (int)(w * 160.0f / 360.0f + 0.5f); }
static float nx_report_density(void){     int w = screen_width > 0 ? screen_width : 720;
                                          return w / 360.0f; }

static juint field_int(const FakeID *id) {
  const char *n = id->name, *c = id->cls;
  if (!strcmp(n, "versionCode")) return APP_VERSION_CODE;
  // UnityPlayer integer statics
  if (name_has(c, "unity3d/player/UnityPlayer")) {
    if (!strcmp(n, "SDK_INT"))     return NX_SDK_INT;
    if (!strcmp(n, "densityDpi"))  return nx_report_density_dpi();
    if (!strcmp(n, "widthPixels")) return screen_width  > 0 ? (juint)screen_width  : 720;
    if (!strcmp(n, "heightPixels"))return screen_height > 0 ? (juint)screen_height : 1280;
    if (!strcmp(n, "STREAM_MUSIC"))return 3;   /* AudioManager.STREAM_MUSIC      */
    if (!strcmp(n, "GET_DEVICES_OUTPUTS")) return 2; /* AudioManager.GET_DEVICES_OUTPUTS */
    if (!strcmp(n, "ROUTE_TYPE_LIVE_VIDEO")) return 1;
    if (!strcmp(n, "SCREEN_ORIENTATION_UNSPECIFIED"))       return -1;
    if (!strcmp(n, "SCREEN_ORIENTATION_LANDSCAPE"))         return 0;
    if (!strcmp(n, "SCREEN_ORIENTATION_PORTRAIT"))          return 1;
    if (!strcmp(n, "SCREEN_ORIENTATION_REVERSE_LANDSCAPE")) return 8;
    if (!strcmp(n, "SCREEN_ORIENTATION_REVERSE_PORTRAIT"))  return 9;
    if (!strcmp(n, "SCREEN_ORIENTATION_FULL_USER"))         return 13;
    if (!strcmp(n, "SCREEN_ORIENTATION_FULL_SENSOR"))       return 10;
  }
  if (name_has(c, "content/Context") && !strcmp(n, "MODE_PRIVATE")) return 0;
  if (name_has(c, "pm/PackageManager")) {
    if (!strcmp(n, "PERMISSION_GRANTED")) return 0;   /* == granted              */
    if (!strcmp(n, "PERMISSION_DENIED"))  return (juint)-1;
  }
  if (name_has(c, "os/Build")) {
    if (!strcmp(n, "SDK_INT"))          return NX_SDK_INT;
    if (!strcmp(n, "PREVIEW_SDK_INT"))  return 0;
  }
  // DisplayMetrics integer fields (width/height/dpi)
  if (name_has(c, "DisplayMetrics")) {
    if (!strcmp(n, "widthPixels"))  return screen_width  > 0 ? (juint)screen_width  : 720;
    if (!strcmp(n, "heightPixels")) return screen_height > 0 ? (juint)screen_height : 1280;
    if (!strcmp(n, "densityDpi"))   return nx_report_density_dpi();
  }
  return 0;
}

/* DisplayMetrics.density / xdpi / ydpi / scaledDensity are float fields. 0 would
 * make dp->px scaling collapse, so hand back a sane xhdpi density (2.0). */
static float field_float(const FakeID *id) {
  const char *n = id->name;
  if (name_has(id->cls, "DisplayMetrics")) {
    if (!strcmp(n, "density") || !strcmp(n, "scaledDensity")) return nx_report_density();
    if (!strcmp(n, "xdpi") || !strcmp(n, "ydpi"))             return (float)nx_report_density_dpi();
  }
  (void)fld_is;
  return 0.0f;
}

static void *j_GetObjectField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return NULL;
  log_field_read((const FakeID *)fid, 'O');
  return field_object((const FakeID *)fid); }
static juint j_GetIntField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0;
  log_field_read((const FakeID *)fid, 'I');
  return field_int((const FakeID *)fid); }
static juint j_GetLongField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0; return (juint)field_int((const FakeID *)fid); }
static juint j_GetBooleanField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0; return field_int((const FakeID *)fid) ? 1 : 0; }
static float j_GetFloatField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0.0f; return field_float((const FakeID *)fid); }

// --- reflection bridge (proxy support) --------------------------------------
// Unity's AndroidJavaProxy / JNIBridge.newInterfaceProxy converts the reflected
// Method/Field objects of an interface into jmethod/jfieldIDs via these. Slot 7
// (FromReflectedMethod) and slot 8 (FromReflectedField) were unimplemented, so
// the proxy couldn't bind its methods (the "UNIMPL slot 7" lines). We don't
// carry real reflection, but returning a non-null opaque ID lets the proxy set
// up and be stored; if such a proxy callback is ever actually invoked it routes
// through act_* and no-ops, which is the right behaviour for our stubbed events.
static void *j_FromReflectedMethod(void *env, void *m) {
  (void)env; (void)m; return get_id("java/lang/reflect/Method", "invoke", "()V"); }
static void *j_FromReflectedField(void *env, void *f) {
  (void)env; (void)f; return get_id("java/lang/reflect/Field", "field", "()V"); }
static void *j_ToReflectedMethod(void *env, void *cls, void *mid, juint isStatic) {
  (void)env; (void)cls; (void)isStatic; return mid ? mid : jni_make_object("java/lang/reflect/Method"); }
static void *j_ToReflectedField(void *env, void *cls, void *fid, juint isStatic) {
  (void)env; (void)cls; (void)isStatic; return fid ? fid : jni_make_object("java/lang/reflect/Field"); }

// --- misc -------------------------------------------------------------------

/* JNI registers org/fmod/FMODAudioDevice's native bridge (fmodGetInfo /
 * fmodProcess / fmodProcessMicData) here -- these are file-local in libunity, so
 * RegisterNatives is the only place their addresses are exposed. Capture them so
 * a native playback thread can pull PCM from FMOD (the Java run() loop never runs
 * because we have no JVM). */
typedef struct { const char *name; const char *sig; void *fn; } JNINativeMethod_;
void *g_fmod_getinfo = 0, *g_fmod_process = 0, *g_fmod_micdata = 0;

/* Color Sheep: capture the UnityPlayer native method table so main.c can resolve
 * initJni/nativeRender/... BY NAME (they are file-local in libunity -- RegisterNatives
 * is the only place their addresses are exposed). The {name} pointers passed here
 * point into libunity's mapped rodata (persistent), so we store them directly. */
static struct { const char *name; void *fn; } g_unity_natives[96];
static int g_unity_native_n = 0;

void *jni_lookup_unity_native(const char *name) {
  for (int i = 0; i < g_unity_native_n; i++)
    if (g_unity_natives[i].name && !strcmp(g_unity_natives[i].name, name))
      return g_unity_natives[i].fn;
  return NULL;
}

static juint j_RegisterNatives(void *env, void *cls, void *methods, int n) {
  (void)env;
  const char *cn = class_name_of(cls);
  const JNINativeMethod_ *m = methods;
  int is_fmod  = name_has(cn, "fmod") || name_has(cn, "FMOD");
  int is_unity = name_has(cn, "UnityPlayer") || name_has(cn, "unity3d/player");
  debugPrintf("[jni] RegisterNatives %s (%d methods)%s%s\n", cn, n,
              is_fmod ? "  <-- fmod" : "", is_unity ? "  <-- UnityPlayer" : "");
  if (is_fmod && m) {
    for (int i = 0; i < n; i++) {
      debugPrintf("[jni]   %s %s -> %p\n",
                  m[i].name ? m[i].name : "?", m[i].sig ? m[i].sig : "?", m[i].fn);
      if (!m[i].name) continue;
      if      (!strcmp(m[i].name, "fmodGetInfo"))        g_fmod_getinfo = m[i].fn;
      else if (!strcmp(m[i].name, "fmodProcess"))        g_fmod_process = m[i].fn;
      else if (!strcmp(m[i].name, "fmodProcessMicData")) g_fmod_micdata = m[i].fn;
    }
    debugPrintf("[fmod] captured getInfo=%p process=%p micData=%p\n",
                g_fmod_getinfo, g_fmod_process, g_fmod_micdata);
  }
  if (is_unity && m) {
    for (int i = 0; i < n; i++) {
      if (!m[i].name) continue;
      if (g_unity_native_n < (int)(sizeof g_unity_natives / sizeof *g_unity_natives)) {
        g_unity_natives[g_unity_native_n].name = m[i].name;
        g_unity_natives[g_unity_native_n].fn   = m[i].fn;
        g_unity_native_n++;
      }
      debugPrintf("[jni]   unity %s -> %p\n", m[i].name, m[i].fn);
    }
    debugPrintf("[unity] captured %d UnityPlayer natives\n", g_unity_native_n);
  }
  return 0;
}
static juint j_GetJavaVM(void *env, void **vm) { (void)env; *vm = fake_vm; return JNI_OK; }
static juint j_ExceptionCheck(void *env) { (void)env; return 0; }
static void *j_ExceptionOccurred(void *env) { (void)env; return NULL; }
static void j_void1(void *env) { (void)env; }

// ---------------------------------------------------------------------------
// FMOD native-audio pump
// ---------------------------------------------------------------------------
// fmodProcess(env, this, ByteBuffer) renders one fixed-size FMOD mixer block
// (size comes from the output singleton set up at start(), NOT from the buffer
// capacity) straight into env->GetDirectBufferAddress(ByteBuffer), then returns
// 0. We have no JVM, so the Java FMODAudioDevice.run() loop never calls it --
// this native thread does instead, and pushes the PCM to the SDL sink.
//
// The only JNIEnv entry fmodProcess uses is GetDirectBufferAddress (slot 230);
// fmodGetInfo(which) uses none. So the shim only has to hand back our staging
// buffer and feed the captured function pointers a non-NULL `this`/buffer token.

#define FMOD_STAGING_BYTES (64 * 1024)   // generous: must exceed one mixer block
static unsigned char g_fmod_staging[FMOD_STAGING_BYTES];
static int  g_fmod_bb_token   = 0;       // stand-in jobject for the ByteBuffer
static int  g_fmod_this_token = 0;       // stand-in jobject for `this`
static int  g_fmod_started    = 0;

typedef int (*fmod_getinfo_fn)(void *env, void *thiz, int which);
typedef int (*fmod_process_fn)(void *env, void *thiz, void *bytebuffer);

// slot 230: every ByteBuffer we ever pass is our own staging buffer.
static void *j_GetDirectBufferAddress(void *env, void *buf) {
  (void)env; (void)buf; return g_fmod_staging;
}
// slot 231 (defensive -- the disasm shows fmodProcess never calls it).
static long j_GetDirectBufferCapacity(void *env, void *buf) {
  (void)env; (void)buf; return (long)FMOD_STAGING_BYTES;
}

// Discover how many bytes fmodProcess actually wrote, once, by sentinel-fill.
// Silence (0x0000) still differs from the 0xCD fill, so a silent first block is
// detected correctly.
static int probe_block_bytes(fmod_process_fn process, int frame_bytes) {
  memset(g_fmod_staging, 0xCD, FMOD_STAGING_BYTES);
  process(fake_env, &g_fmod_this_token, &g_fmod_bb_token);
  int last = -1;
  for (int i = FMOD_STAGING_BYTES - 1; i >= 0; i--) {
    if (g_fmod_staging[i] != 0xCD) { last = i; break; }
  }
  if (last < 0) return 0;
  int bytes = last + 1;
  if (frame_bytes > 0)                    // round up to a whole frame
    bytes = ((bytes + frame_bytes - 1) / frame_bytes) * frame_bytes;
  if (bytes > FMOD_STAGING_BYTES) bytes = FMOD_STAGING_BYTES;
  return bytes;
}

static int16_t block_peak(int bytes) {
  const int16_t *s = (const int16_t *)g_fmod_staging;
  int n = bytes / 2; int16_t peak = 0;
  for (int i = 0; i < n; i++) {
    int16_t v = s[i] < 0 ? (int16_t)-s[i] : s[i];
    if (v > peak) peak = v;
  }
  return peak;
}

static void *fmod_audio_thread(void *arg) {
  (void)arg;
  fmod_getinfo_fn getinfo = (fmod_getinfo_fn)g_fmod_getinfo;
  fmod_process_fn process = (fmod_process_fn)g_fmod_process;
  if (!process) { debugPrintf("[fmod] pump: no process ptr, abort\n"); return NULL; }

  int rate = 48000, channels = 2;
  if (getinfo) {
    int r = getinfo(fake_env, &g_fmod_this_token, 0);
    int c = getinfo(fake_env, &g_fmod_this_token, 1);
    debugPrintf("[fmod] getInfo: [0]=%d [1]=%d [2]=%d [3]=%d [4]=%d\n",
                r, c, getinfo(fake_env, &g_fmod_this_token, 2),
                getinfo(fake_env, &g_fmod_this_token, 3),
                getinfo(fake_env, &g_fmod_this_token, 4));
    if (r >= 8000 && r <= 192000) rate = r;
    if (c == 1 || c == 2 || c == 6) channels = c;
  }
  const int frame_bytes = channels * 2; // S16

  // CRITICAL: start() fires before Unity's render loop has driven a single
  // System::update(), so the FMOD mixer's DSP buffers aren't allocated yet --
  // calling fmodProcess now faults (null deref deep in the mix/copy path). Wait
  // for the engine to tick a batch of frames (each drives a System::update that
  // finalizes the mixer) before the first call. A faulting call can't be caught
  // (no working SEH here), so this warmup is the only protection.
  extern uint32_t port_frame_count(void);
  #define FMOD_WARMUP_FRAMES 120u
  uint32_t f0 = port_frame_count();
  debugPrintf("[fmod] warmup: waiting %u frames (start frame=%u)\n", FMOD_WARMUP_FRAMES, f0);
  for (int guard = 0; guard < 1500; guard++) {            // ~15s hard cap
    if (port_frame_count() - f0 >= FMOD_WARMUP_FRAMES) break;
    svcSleepThread(10000000ULL);                          // 10 ms
  }
  debugPrintf("[fmod] warmup done at frame=%u, probing\n", port_frame_count());

  // start() may still be wiring the FMOD output singleton; fmodProcess writes
  // nothing until it's live. Retry the probe briefly before giving up.
  int block = 0;
  for (int tries = 0; tries < 100 && block <= 0; tries++) {
    block = probe_block_bytes(process, frame_bytes);
    if (block <= 0) svcSleepThread(10000000ULL); // 10 ms
  }
  debugPrintf("[fmod] pump start: %d Hz, %d ch, block=%d bytes (%d frames)\n",
              rate, channels, block, block / (frame_bytes ? frame_bytes : 1));
  if (block <= 0) {
    debugPrintf("[fmod] pump: fmodProcess wrote nothing after retries, abort\n");
    return NULL;
  }

  int dev_rate = audio_fmod_open(rate, channels);
  if (!dev_rate) { debugPrintf("[fmod] pump: device open failed, abort\n"); return NULL; }

  // pace to realtime via the device queue; target ~4 blocks buffered.
  const uint32_t hi = (uint32_t)block * 6;
  const uint32_t lo = (uint32_t)block * 3;
  long iters = 0;
  for (;;) {
    while (audio_fmod_queued() > hi)
      svcSleepThread(2000000ULL); // 2 ms
    // refill toward the low watermark
    do {
      process(fake_env, &g_fmod_this_token, &g_fmod_bb_token);
      /* Splash-video audio rides the block FMOD just filled, rather than a
       * second SDL device. No-op unless a clip is decoding, and it never logs
       * or touches the filesystem -- this is the audio pump thread. */
      zb_video_mix_audio((short *)g_fmod_staging, block / frame_bytes, channels);
      uint32_t q = audio_fmod_write(g_fmod_staging, block);
      if (iters < 4) {
        debugPrintf("[fmod] block %ld: peak=%d queued=%u\n",
                    iters, (int)block_peak(block), q);
      }
      iters++;
      if (q > hi) break;
    } while (audio_fmod_queued() < lo);
    svcSleepThread(2000000ULL); // 2 ms
  }
  return NULL;
}

// Called from dispatch_void when FMODAudioDevice.start() fires (pointers are
// already captured by then -- RegisterNatives precedes start()).
void fmod_audio_start(void) {
  if (g_fmod_started) return;
  if (!g_fmod_process) { debugPrintf("[fmod] start(): process ptr not captured yet\n"); return; }
  g_fmod_started = 1;
  pthread_t th;
  if (pthread_create(&th, NULL, fmod_audio_thread, NULL) != 0) {
    debugPrintf("[fmod] pthread_create failed\n");
    g_fmod_started = 0;
    return;
  }
  pthread_detach(th);
  debugPrintf("[fmod] native playback thread launched\n");
}

// ---------------------------------------------------------------------------
// table assembly (indices per the JNI specification)
// ---------------------------------------------------------------------------

static void *env_table[233];
static void **env_table_ptr = env_table;
/* ZOOKEEPER DX port: accessors so unity_jni.c/unity_input.c can read into the
 * (otherwise static) FakeString / FakePriArray without duplicating the structs. */
void *jni_bytearray_data(void *arr, int *len_out) {
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR) { if (len_out) *len_out = a->len; return a->data; }
  if (len_out) *len_out = 0;
  return NULL;
}
const char *jni_string_utf(void *jstr) {
  FakeString *s = jstr;
  return (s && s->tag == TAG_STRING) ? s->utf : "";
}
/* ZOMBOTRON: build a real int[] so zombotron_jni.c can answer
 * InputManager.getInputDeviceIds() without reimplementing FakePriArray. */
void *jni_make_intarray(const int32_t *vals, int n) {
  if (n < 0) n = 0;
  FakePriArray *a = new_pri_array(n, 4);
  if (a && vals && n) memcpy(a->data, vals, (size_t)n * 4);
  return a;
}

void *fake_env = &env_table_ptr;

static juint vm_DestroyJavaVM(void *vm) { (void)vm; return JNI_OK; }
static juint vm_AttachCurrentThread(void *vm, void **env, void *args) {
  (void)vm; (void)args; if (env) *env = fake_env; return JNI_OK;
}
static juint vm_DetachCurrentThread(void *vm) { (void)vm; return JNI_OK; }
static juint vm_GetEnv(void *vm, void **env, int version) {
  (void)vm; (void)version; if (env) *env = fake_env; return JNI_OK;
}
static void *vm_table[8];
static void **vm_table_ptr = vm_table;
void *fake_vm = &vm_table_ptr;

void jni_init(void) {
  mutexInit(&locals_lock);

  jni_fill_unimpl(env_table); // indexed stubs: log the exact unimplemented slot

  env_table[4]   = (void *)j_GetVersion;
  env_table[6]   = (void *)j_FindClass;
  env_table[7]   = (void *)j_FromReflectedMethod;    // was UNIMPL (proxy bind)
  env_table[8]   = (void *)j_FromReflectedField;
  env_table[9]   = (void *)j_ToReflectedMethod;
  env_table[12]  = (void *)j_ToReflectedField;
  env_table[15]  = (void *)j_ExceptionOccurred;
  env_table[16]  = (void *)j_void1; // ExceptionDescribe
  env_table[17]  = (void *)j_void1; // ExceptionClear
  env_table[19]  = (void *)j_PushLocalFrame;
  env_table[20]  = (void *)j_PopLocalFrame;
  env_table[21]  = (void *)j_NewGlobalRef;
  env_table[22]  = (void *)j_DeleteGlobalRef;
  env_table[23]  = (void *)j_DeleteLocalRef;
  env_table[24]  = (void *)j_IsSameObject;
  env_table[25]  = (void *)j_NewLocalRef;
  env_table[26]  = (void *)j_EnsureLocalCapacity;
  env_table[28]  = (void *)j_NewObject;
  env_table[29]  = (void *)j_NewObjectV;
  env_table[31]  = (void *)j_GetObjectClass;
  env_table[32]  = (void *)j_IsInstanceOf;
  env_table[33]  = (void *)j_GetMethodID;
  env_table[34]  = (void *)j_CallObjectMethod;
  env_table[35]  = (void *)j_CallObjectMethodV;
  env_table[37]  = (void *)j_CallBooleanMethod;
  env_table[38]  = (void *)j_CallBooleanMethodV;
  env_table[49]  = (void *)j_CallIntMethod;
  env_table[50]  = (void *)j_CallIntMethodV;
  env_table[52]  = (void *)j_CallLongMethod;
  env_table[53]  = (void *)j_CallLongMethodV;
  env_table[55]  = (void *)j_CallFloatMethod;
  env_table[56]  = (void *)j_CallFloatMethodV;
  env_table[61]  = (void *)j_CallVoidMethod;
  env_table[62]  = (void *)j_CallVoidMethodV;
  // "A" (jvalue[]) variants -- instance
  env_table[30]  = (void *)j_NewObjectA;
  env_table[36]  = (void *)j_CallObjectMethodA;
  env_table[39]  = (void *)j_CallBooleanMethodA;
  env_table[51]  = (void *)j_CallIntMethodA;
  env_table[54]  = (void *)j_CallLongMethodA;
  env_table[57]  = (void *)j_CallFloatMethodA;
  env_table[63]  = (void *)j_CallVoidMethodA;
  env_table[94]  = (void *)j_GetFieldID;
  env_table[95]  = (void *)j_GetObjectField;
  env_table[96]  = (void *)j_GetBooleanField;        // GetBooleanField
  env_table[100] = (void *)j_GetIntField;
  env_table[101] = (void *)j_GetLongField;           // GetLongField
  env_table[102] = (void *)j_GetFloatField;          // GetFloatField
  env_table[113] = (void *)j_GetMethodID;            // GetStaticMethodID
  env_table[114] = (void *)j_CallStaticObjectMethod;
  env_table[115] = (void *)j_CallStaticObjectMethodV;
  env_table[117] = (void *)j_CallStaticBooleanMethod;
  env_table[118] = (void *)j_CallStaticBooleanMethodV;
  env_table[129] = (void *)j_CallStaticIntMethod;
  env_table[130] = (void *)j_CallStaticIntMethodV;
  env_table[132] = (void *)j_CallStaticLongMethod;
  env_table[133] = (void *)j_CallStaticLongMethodV;
  env_table[135] = (void *)j_CallStaticFloatMethod;
  env_table[136] = (void *)j_CallStaticFloatMethodV;
  env_table[141] = (void *)j_CallStaticVoidMethod;
  env_table[142] = (void *)j_CallStaticVoidMethodV;
  // "A" (jvalue[]) variants -- static (SWIG / AndroidJavaObject.CallStatic<T>)
  env_table[116] = (void *)j_CallStaticObjectMethodA;
  env_table[119] = (void *)j_CallStaticBooleanMethodA;
  env_table[131] = (void *)j_CallStaticIntMethodA;
  env_table[134] = (void *)j_CallStaticLongMethodA;
  env_table[137] = (void *)j_CallStaticFloatMethodA;
  env_table[143] = (void *)j_CallStaticVoidMethodA;
  env_table[144] = (void *)j_GetFieldID;             // GetStaticFieldID
  env_table[145] = (void *)j_GetObjectField;         // GetStaticObjectField
  env_table[146] = (void *)j_GetBooleanField;        // GetStaticBooleanField
  env_table[150] = (void *)j_GetIntField;            // GetStaticIntField
  env_table[151] = (void *)j_GetLongField;           // GetStaticLongField
  env_table[152] = (void *)j_GetFloatField;          // GetStaticFloatField
  env_table[163] = (void *)j_NewString;
  env_table[164] = (void *)j_GetStringLength;
  env_table[167] = (void *)j_NewStringUTF;
  env_table[168] = (void *)j_GetStringUTFLength;
  env_table[169] = (void *)j_GetStringUTFChars;
  env_table[170] = (void *)j_ReleaseStringUTFChars;
  env_table[171] = (void *)j_GetArrayLength;
  env_table[172] = (void *)j_NewObjectArray;
  env_table[173] = (void *)j_GetObjectArrayElement;
  env_table[174] = (void *)j_SetObjectArrayElement;
  env_table[176] = (void *)j_NewByteArray;
  env_table[179] = (void *)j_NewIntArray;
  env_table[181] = (void *)j_NewFloatArray;
  for (int i = 183; i <= 190; i++) env_table[i] = (void *)j_GetPriArrayElements;
  for (int i = 191; i <= 198; i++) env_table[i] = (void *)j_ReleasePriArrayElements;
  for (int i = 199; i <= 206; i++) env_table[i] = (void *)j_GetPriArrayRegion;
  for (int i = 207; i <= 214; i++) env_table[i] = (void *)j_SetPriArrayRegion;
  env_table[215] = (void *)j_RegisterNatives;
  env_table[219] = (void *)j_GetJavaVM;
  env_table[220] = (void *)j_GetStringRegion;
  env_table[221] = (void *)j_GetStringUTFRegion; // engine reads every string via this
  env_table[222] = (void *)j_GetPriArrayElements;     // GetPrimitiveArrayCritical
  env_table[223] = (void *)j_ReleasePriArrayElements; // ReleasePrimitiveArrayCritical
  env_table[226] = (void *)j_NewGlobalRef;            // NewWeakGlobalRef
  env_table[227] = (void *)j_DeleteGlobalRef;         // DeleteWeakGlobalRef
  env_table[228] = (void *)j_ExceptionCheck;
  env_table[230] = (void *)j_GetDirectBufferAddress;  // fmodProcess drains via this
  env_table[231] = (void *)j_GetDirectBufferCapacity; // defensive (unused by fmodProcess)

  vm_table[3] = (void *)vm_DestroyJavaVM;
  vm_table[4] = (void *)vm_AttachCurrentThread;
  vm_table[5] = (void *)vm_DetachCurrentThread;
  vm_table[6] = (void *)vm_GetEnv;
  vm_table[7] = (void *)vm_AttachCurrentThread; // AttachCurrentThreadAsDaemon
}
