/* util.c -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "util.h"
#include <time.h>
#include "config.h"
#include "zombotron_root.h"

// File-only logger (DEBUG_LOG builds only): open once + flush per line so the tail survives a
// crash, mutex-serialised across engine threads. Drops the high-frequency dlsym/dlopen/JNI spam.
#if DEBUG_LOG
static Mutex g_log_lock;
static int log_is_noisy(const char *t) {
  return !strncmp(t, "dlsym", 5) || !strncmp(t, "dlopen", 6) ||
         !strncmp(t, "JNI ", 4)  || !strncmp(t, "JNI:", 4) || !strncmp(t, "[jni]", 5);
}
#endif

/* Log file, deliberately NOT flushed per line.
 *
 * WHY THIS MATTERS MORE THAN IT LOOKS
 * Unity's Debug.Log reaches us through __android_log_print -> debugPrintf, and
 * the engine logs heavily during startup (the Play Games plugin alone emits a
 * dozen lines). This used to fflush() on every call, and each flush is a
 * synchronous fs IPC to the SD card. The first on-hardware loading-screen hang
 * was exactly this: seven watchdog stalls over 48 seconds, and at EVERY one the
 * engine main thread was parked in
 *     svcSendSyncRequest <- fsFileWrite <- fsdev_write <- _write_r
 * i.e. it was not deadlocked, it was spending all of frame 0 writing our own log
 * one flush at a time.
 *
 * Now: a 64 KB buffer, flushed on a timer, on important lines, and explicitly by
 * the crash handler. A crash still gets a complete log because
 * debug_log_flush() is called from the exception path before anything else. */
static FILE   *g_logf = NULL;
static char    g_logbuf[64 * 1024];
static uint64_t g_log_last_flush_ns = 0;
static int      g_log_dirty = 0;

#define LOG_FLUSH_INTERVAL_NS  2000000000ull   /* 2s */

static uint64_t log_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Lines that must reach the card immediately: anything announcing a failure or
 * a phase change, since those are what a post-mortem reader needs. */
static int log_is_important(const char *t) {
  /* Tolerate a leading newline: several diagnostic lines start "\n[wd]" /
   * "\n[crash]", and a plain prefix test silently downgraded them to
   * "buffer it, flush later" -- i.e. the most urgent lines were the ones most
   * likely to be lost. */
  while (*t == '\n' || *t == '\r') t++;
  /* [loop] and [vsync] flush immediately: on a hard system freeze nothing runs
   * afterwards to flush them, so a buffered marker is a marker that never
   * arrives -- and its absence would be misread as "that code never ran". */
  return !strncmp(t, "[crash]", 7) || !strncmp(t, "[wd]", 4) ||
         !strncmp(t, "[loop]", 6) || !strncmp(t, "[vsync]", 7) ||
         !strncmp(t, "[gfx]", 5) || !strncmp(t, "[gc]", 4) ||
         /* [fmod] is the whole point of the OpenSL init trace: it fires a handful
          * of times at startup and then never again, so flushing it is free and
          * losing it costs a test cycle. [input]/[touch]/[screen] are likewise
          * one-shot -- no per-frame line remains under any of these prefixes. */
         /* [gpua] added for the frame-0 hang: the log stops before
          * "GPU arena reserved", and with only 3 MB free at boot, whether that
          * 512 MB reservation completes is exactly the open question. [dlc] so a
          * hook that does not install says so instead of vanishing. */
         /* [video] fires a handful of times per clip -- manifest load, hook
          * result, decoder name, a summary at the end -- and never per frame.
          * If the intro is where a build dies, these are the lines that say how
          * far it got, so buffering them would lose exactly the useful part. */
         !strncmp(t, "[video]", 7) ||
         !strncmp(t, "[gpua]", 6) || !strncmp(t, "[dlc]", 5) ||
         !strncmp(t, "[fmod]", 6) || !strncmp(t, "[input]", 7) ||
         !strncmp(t, "[touch]", 7) || !strncmp(t, "[screen]", 8) ||
         !strncmp(t, "[boot]", 6)  || !strncmp(t, "[region]", 8) ||
         strstr(t, "FATAL") || strstr(t, "ABORT") || strstr(t, "failed");
}

/* INDEPENDENT log for the watchdog.
 *
 * The watchdog must NOT use debugPrintf. debugPrintf holds g_log_lock across
 * fflush(), and fflush is a blocking SD write that the engine main thread has
 * been observed parked inside. A watchdog using the shared path therefore blocks
 * on a lock held by precisely the thread it exists to report on -- which is what
 * happened: seven stall dumps in run 3, then total silence once buffering was
 * added, in exactly the runs where a stall dump mattered most.
 *
 * Separate FILE*, separate lock, separate file. Slow (open/write/close per line)
 * but it only runs during a stall, and it cannot be starved by the main log. */
static Mutex g_stall_lock;
static int   g_stall_init;

int stallPrintf(char *text, ...) {
#if DEBUG_LOG
  va_list list;
  char path[600];
  if (!g_stall_init) { mutexInit(&g_stall_lock); g_stall_init = 1; }
  mutexLock(&g_stall_lock);
  snprintf(path, sizeof path, "%s/stall.log", zb_game_root());
  FILE *f = fopen(path, "a");
  if (f) {
    va_start(list, text);
    vfprintf(f, text, list);
    va_end(list);
    fclose(f);                 /* close each time: never hold a handle open */
  }
  mutexUnlock(&g_stall_lock);
#else
  (void)text;
#endif
  return 0;
}

void debug_log_flush(void) {
#if DEBUG_LOG
  mutexLock(&g_log_lock);
  if (g_logf && g_log_dirty) { fflush(g_logf); g_log_dirty = 0; }
  g_log_last_flush_ns = log_now_ns();
  mutexUnlock(&g_log_lock);
#endif
}

int debugPrintf(char *text, ...) {
#if DEBUG_LOG
  va_list list;
  int want_flush = 0;
  if (log_is_noisy(text)) return 0;
  mutexLock(&g_log_lock);
  if (!g_logf) {
    g_logf = fopen(zb_log_path(), "a");
    if (g_logf) setvbuf(g_logf, g_logbuf, _IOFBF, sizeof g_logbuf);
    g_log_last_flush_ns = log_now_ns();
  }
  if (g_logf) {
    va_start(list, text);
    vfprintf(g_logf, text, list);
    va_end(list);
    g_log_dirty = 1;
    uint64_t now = log_now_ns();
    want_flush = (log_is_important(text) ||
                  now - g_log_last_flush_ns >= LOG_FLUSH_INTERVAL_NS);
    if (want_flush) { g_log_dirty = 0; g_log_last_flush_ns = now; }
  }
  mutexUnlock(&g_log_lock);

  /* Flush OUTSIDE the lock. Holding a global mutex across a blocking SD write
   * makes every other thread that logs wait on whichever thread is currently
   * stuck in the filesystem -- which silenced the watchdog in exactly the runs
   * where it was needed. newlib serialises the FILE* internally. */
  if (want_flush && g_logf) fflush(g_logf);
#else
  (void)text;
#endif
  return 0;
}

// Per-thread bionic TLS. The engine reads its stack canary from tpidr_el0+0x28;
// every thread that runs engine code needs its OWN zeroed block here. A single
// shared block races: one thread's TLS writes (including the guard slot) corrupt
// another thread's in-flight canary, tripping a false __stack_chk_fail. `buf`
// must outlive the thread (TPIDR_EL0 points into it until the thread exits).
void install_bionic_tls(void *buf) {
  memset(buf, 0, BIONIC_TLS_SIZE);
  armSetTlsRw((uint8_t *)buf + BIONIC_TLS_TP_OFFSET);
}

// boost the CPU to 1785MHz while loading
void cpu_boost(int on) {
  appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
}

int ret0(void) { return 0; }

int retm1(void) { return -1; }
