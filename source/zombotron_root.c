/* zombotron_root.c -- work out where the game data actually lives, at runtime.
 *
 * WHY THIS EXISTS
 * The inherited substrate compiles its data path in as a literal
 * (`sdmc:/switch/<port>`), so the folder on the SD card has to be named exactly
 * what the source says or nothing loads. The failure is also actively
 * misleading: `check_data()` reports "Missing data file: libmain.so" while the
 * file is plainly sitting there in Explorer -- it is looking in a directory that
 * does not exist. Worse, `debug.log` is written to that same nonexistent
 * directory, so the one artifact you would use to diagnose it never appears.
 *
 * That is a bad trap to leave in a port other people install by hand, so the
 * root is resolved at startup instead:
 *
 *   1. The directory of argv[0]. hbmenu passes the full path of the .nro it
 *      launched, so this is right by construction no matter what the folder is
 *      called -- rename it, nest it, put it on a different device, still works.
 *   2. Failing that, a list of plausible folder names.
 *   3. Failing that, the compiled-in default, so behaviour matches the old code.
 *
 * A candidate only wins if libmain.so is actually in it, which also means a
 * half-copied folder is rejected rather than half-loaded.
 *
 * MIT.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "config.h"
#include "zombotron_root.h"

static char g_root[512] = GAME_HOME;   /* compiled-in default until resolved */
static char g_log[576]  = LOG_NAME;
static int  g_resolved;

const char *zb_game_root(void) { return g_root; }
const char *zb_log_path(void)  { return g_log; }

static int has_libmain(const char *dir) {
  char p[600];
  struct stat st;
  snprintf(p, sizeof p, "%s/libmain.so", dir);
  return stat(p, &st) == 0;
}

static void adopt(const char *dir) {
  snprintf(g_root, sizeof g_root, "%s", dir);
  /* Strip a trailing slash so every "%s/%s" join stays well-formed. */
  size_t n = strlen(g_root);
  while (n > 1 && g_root[n - 1] == '/' && g_root[n - 2] != ':')
    g_root[--n] = '\0';
  snprintf(g_log, sizeof g_log, "%s/debug.log", g_root);
}

/* Candidates tried in order if argv[0] is unusable. Deliberately includes the
 * plain name and a couple of casings: people name the folder after the game,
 * not after the port. */
static const char *const CANDIDATES[] = {
  "sdmc:/switch/zombotron_nx",
  "sdmc:/switch/zombotron",
  "sdmc:/switch/Zombotron",
  "sdmc:/switch/Zombotron_NX",
  "sdmc:/zombotron_nx",
  "sdmc:/zombotron",
  "sdmc:/switch",
  ".",
};

const char *zb_resolve_game_root(int argc, char **argv) {
  if (g_resolved)
    return g_root;
  g_resolved = 1;

  /* 1. argv[0]'s directory. This is the authoritative answer when hbmenu
   *    provides it, because it is literally where the running .nro lives. */
  if (argc > 0 && argv && argv[0] && argv[0][0]) {
    char dir[512];
    snprintf(dir, sizeof dir, "%s", argv[0]);
    char *slash = strrchr(dir, '/');
    if (slash) {
      *slash = '\0';
      if (dir[0] && has_libmain(dir)) {
        adopt(dir);
        return g_root;
      }
    }
  }

  /* 2. Known-plausible folder names. */
  for (unsigned i = 0; i < sizeof(CANDIDATES) / sizeof(*CANDIDATES); i++) {
    if (has_libmain(CANDIDATES[i])) {
      adopt(CANDIDATES[i]);
      return g_root;
    }
  }

  /* 3. Nothing validated. Keep the compiled-in default so the subsequent
   *    check_data() failure names a real path the user can compare against
   *    what they see on the card. */
  return g_root;
}

/* Diagnostics for the log, once logging is pointed somewhere real. */
void zb_root_report(int argc, char **argv) {
  extern int debugPrintf(char *text, ...);
  debugPrintf("[root] argv[0]=%s\n",
              (argc > 0 && argv && argv[0]) ? argv[0] : "(none)");
  debugPrintf("[root] data root = %s%s\n", g_root,
              has_libmain(g_root) ? "" : "   <-- libmain.so NOT found here");
  debugPrintf("[root] log        = %s\n", g_log);
}
