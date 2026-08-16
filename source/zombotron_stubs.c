/* zombotron_stubs.c -- link-only no-op bodies for the text2bitmap / movie player /
 * editbox helpers that jni_fake.c's fallback handlers reference.
 *
 * Renamed from cr3_stubs.c: these paths belong to the Chaos Rings 3 MVGL engine
 * the substrate was originally written for. Unity drives none of them, and
 * Zombotron in particular has no FMV and no engine-side dynamic text rendering.
 * They exist purely so jni_fake.c links without pulling in FreeType/FFmpeg.
 *
 * data_dir() is the one live function here -- it feeds the JNI data-path getters
 * and now resolves to Zombotron's GAME_HOME. */

#include <stddef.h>
#include "config.h"
#include "zombotron_root.h"
#include "data.h"
#include "text2bitmap.h"
#include "movie_player.h"
#include "editbox.h"

/* Game data root, resolved at runtime from the .nro's own location -- see
 * zombotron_root.c. Used by the JNI path getters. */
const char *data_dir(void) { return zb_game_root(); }

/* Engine-thread-finished hook; our main loop uses jni_quit_requested instead, so no-op. */
void android_mark_main_finished(void) {}

/* ---- text rendering (engine's dynamic text path; Unity doesn't use it) ---- */
FakeBitmap *text2bitmap_render(const char *text, int pixel_size) { (void)text; (void)pixel_size; return NULL; }
int  text2bitmap_measure_width (const char *text, int pixel_size) { (void)text; (void)pixel_size; return 0; }
int  text2bitmap_measure_height(const char *text, int pixel_size) { (void)text; (void)pixel_size; return 0; }
void text2bitmap_free(FakeBitmap *bmp) { (void)bmp; }

/* ---- FMV playback (unused) ---- */
void movie_set_db(const char *db_path) { (void)db_path; }
int  movie_play(const char *name, int looping) { (void)name; (void)looping; return 0; }
void movie_stop(void)   {}
void movie_pause(void)  {}
void movie_resume(void) {}
int  movie_is_playing(void) { return 0; }

/* ---- software keyboard (Unity routes through SoftInputProvider stub) ---- */
void editbox_show(const char *initial, int maxlen) { (void)initial; (void)maxlen; }
int  editbox_is_open(void) { return 0; }
const char *editbox_text(void) { return ""; }
void editbox_close(void) {}
