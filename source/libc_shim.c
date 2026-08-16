/* libc_shim.c -- bionic-compatible libc wrappers for libcrx.so + libc++_shared
 *
 * The Android engine and its C++ runtime are linked against bionic. Where the
 * bionic and newlib ABIs differ (struct layouts, flag values, missing
 * functions) we provide converting wrappers here; everything that matches is
 * passed straight through from imports.c. Online/IPC functionality (sockets,
 * fork/exec, dlopen of system libs) is dead on Switch and stubbed to fail
 * cleanly so the engine falls back to offline behaviour.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <malloc.h>
#include <wchar.h>
#include <wctype.h>
#include <time.h>
#include <sys/stat.h>
#include <switch.h>
#include <EGL/egl.h>     /* eglGetProcAddress: resolve the full GLES API for dlsym */

#include "config.h"
#include "zombotron_root.h"
#include "util.h"
#include "error.h"
#include "imports.h"   /* dynlib_find_export (dlsym shim lookup) */
#include "so_util.h"
#include "libc_shim.h"
#include "asset_pack.h"
#include "android_native_unity.h"
#include "diag.h"

// ---------------------------------------------------------------------------
// fortify (_chk) wrappers: ignore the object-size argument
// ---------------------------------------------------------------------------

void *__memcpy_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) { (void)dstlen; return memcpy(dst, src, n); }
void *__memmove_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) { (void)dstlen; return memmove(dst, src, n); }
void *__memset_chk_fake(void *dst, int c, size_t n, size_t dstlen) { (void)dstlen; return memset(dst, c, n); }
char *__strcat_chk_fake(char *dst, const char *src, size_t dstlen) { (void)dstlen; return strcat(dst, src); }
char *__strchr_chk_fake(const char *s, int c, size_t slen) { (void)slen; return strchr(s, c); }
char *__strcpy_chk_fake(char *dst, const char *src, size_t dstlen) { (void)dstlen; return strcpy(dst, src); }
size_t __strlen_chk_fake(const char *s, size_t slen) { (void)slen; return strlen(s); }
char *__strncat_chk_fake(char *dst, const char *src, size_t n, size_t dstlen) { (void)dstlen; return strncat(dst, src, n); }
char *__strncpy_chk_fake(char *dst, const char *src, size_t n, size_t dstlen) { (void)dstlen; return strncpy(dst, src, n); }
char *__strncpy_chk2_fake(char *dst, const char *src, size_t n, size_t dstlen, size_t srclen) { (void)dstlen; (void)srclen; return strncpy(dst, src, n); }
char *__strrchr_chk_fake(const char *s, int c, size_t slen) { (void)slen; return strrchr(s, c); }
int __vsnprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list va) { (void)flag; (void)slen; return vsnprintf(s, maxlen, fmt, va); }
int __vsprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, va_list va) { (void)flag; (void)slen; return vsprintf(s, fmt, va); }

int __snprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, ...) {
  (void)flag; (void)slen;
  va_list va; va_start(va, fmt);
  int r = vsnprintf(s, maxlen, fmt, va);
  va_end(va);
  return r;
}
int __sprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, ...) {
  (void)flag; (void)slen;
  va_list va; va_start(va, fmt);
  int r = vsprintf(s, fmt, va);
  va_end(va);
  return r;
}

// fortified read helpers ignore the buffer-size guard
int   __open_2_fake(const char *path, int flags) { return open_fake(path, flags); }
long  __read_chk_fake(int fd, void *buf, size_t count, size_t buflen) { (void)buflen; return read(fd, buf, count); }
long  __pread_chk_fake(int fd, void *buf, size_t count, long off, size_t buflen) {
  (void)buflen;
  long cur = lseek(fd, 0, SEEK_CUR);
  if (cur < 0 || lseek(fd, off, SEEK_SET) < 0) return -1;
  long r = read(fd, buf, count);
  lseek(fd, cur, SEEK_SET);
  return r;
}
void  __FD_SET_chk_fake(int fd, void *set, size_t setlen) { (void)setlen; if (set && fd >= 0 && fd < 1024) ((unsigned long *)set)[fd / (8 * sizeof(long))] |= (1ul << (fd % (8 * sizeof(long)))); }
int   __FD_ISSET_chk_fake(int fd, const void *set, size_t setlen) { (void)setlen; if (set && fd >= 0 && fd < 1024) return (((const unsigned long *)set)[fd / (8 * sizeof(long))] >> (fd % (8 * sizeof(long)))) & 1; return 0; }

// ---------------------------------------------------------------------------
// misc bionic functions
// ---------------------------------------------------------------------------

// Native twin of the android.os.Build.* JNI fields. libunity calls this to
// detect API level / ABI / device; returning "" (the old stub) made it
// mis-detect the platform. Hand back Switch-sane values for the keys engines
// actually query; everything else stays empty (= property unset, the normal
// Android case). Return value is the value length, per bionic contract.
int __system_property_get_fake(const char *name, char *value) {
  if (!value) return 0;
  const char *v = "";
  if (name) {
    if      (!strcmp(name, "ro.build.version.sdk"))        v = "33";
    else if (!strcmp(name, "ro.build.version.release"))    v = "13";
    else if (!strcmp(name, "ro.build.version.codename"))   v = "REL";
    else if (!strcmp(name, "ro.product.cpu.abi"))          v = "arm64-v8a";
    else if (!strcmp(name, "ro.product.cpu.abilist"))      v = "arm64-v8a";
    else if (!strcmp(name, "ro.product.cpu.abilist64"))    v = "arm64-v8a";
    else if (!strcmp(name, "ro.product.cpu.abi2"))         v = "";
    else if (!strcmp(name, "ro.product.model"))            v = "Switch";
    else if (!strcmp(name, "ro.product.manufacturer"))     v = "Nintendo";
    else if (!strcmp(name, "ro.product.brand"))            v = "Nintendo";
    else if (!strcmp(name, "ro.product.name"))             v = "Switch";
    else if (!strcmp(name, "ro.product.device"))           v = "Switch";
    else if (!strcmp(name, "ro.product.board"))            v = "nx";
    else if (!strcmp(name, "ro.hardware"))                 v = "nx";
    else if (!strcmp(name, "ro.board.platform"))           v = "nx";
    else if (!strcmp(name, "ro.build.fingerprint"))        v = "Nintendo/Switch/Switch:13/REL/10007:user/release-keys";
    else if (!strcmp(name, "ro.build.characteristics"))    v = "default";
    else if (!strcmp(name, "ro.build.type"))               v = "user";
    else if (!strcmp(name, "ro.build.tags"))               v = "release-keys";
    else if (!strcmp(name, "ro.debuggable"))               v = "0";
    else if (!strcmp(name, "ro.secure"))                   v = "1";
    else if (!strcmp(name, "ro.kernel.qemu"))              v = "0";
    else if (!strcmp(name, "ro.opengles.version"))         v = "196610"; /* GLES 3.2 */
    else if (!strcmp(name, "dalvik.vm.heapsize"))          v = "512m";
    else if (!strcmp(name, "persist.sys.timezone"))        v = "UTC";
  }
  size_t n = strlen(v);
  if (n > 91) n = 91;            /* PROP_VALUE_MAX-1 */
  memcpy(value, v, n); value[n] = '\0';
  return (int)n;
}
unsigned long getauxval_fake(unsigned long type) { (void)type; return 0; }

int gettid_fake(void) {
  u64 tid = 1;
  if (R_SUCCEEDED(svcGetThreadId(&tid, CUR_THREAD_HANDLE)) && tid)
    return (int)(tid & 0x7fffffff);
  return 1;
}

#define ARM64_SYS_GETTID            178
#define ARM64_SYS_FUTEX             98
#define ARM64_SYS_SCHED_SETAFFINITY 122
#define ARM64_SYS_PROCESS_VM_READV  270
#define ARM64_SYS_PROCESS_VM_WRITEV 271

// futex(2) emulation over libnx mutex+condvar. The il2cpp runtime synchronizes
// its GC, thread pool and locks with raw futex; returning ENOSYS made every
// waiter spin forever (the syscall(98) -> ENOSYS flood) and threading never made
// progress. Wait queues are hashed by uaddr into a bucket array; FUTEX_WAKE wakes
// the whole bucket (waiters re-check *uaddr, so over-broad wakes are harmless).
// The bucket mutex serializes compare-and-sleep against wakers so no wake is lost.
// Infinite waits are capped at 16ms and return as if woken: under load a wake can
// be missed (the Unity Job System / GC otherwise deadlock), and a bounded re-poll
// recovers it safely since the waiter re-checks *uaddr before proceeding.
#define FUTEX_WAIT        0
#define FUTEX_WAKE        1
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_CMD_MASK    0x7f  // strip FUTEX_PRIVATE_FLAG(128)/CLOCK_REALTIME(256)
#define FUTEX_BUCKETS     256

static Mutex   futex_lock[FUTEX_BUCKETS];   // libnx Mutex/CondVar are u32; 0 == ready
static CondVar futex_cond[FUTEX_BUCKETS];

static long futex_impl(volatile int32_t *uaddr, int op, int val, const struct timespec *to) {
  const int cmd = op & FUTEX_CMD_MASK;
  const unsigned h = (unsigned)(((uintptr_t)uaddr >> 4) & (FUTEX_BUCKETS - 1));
  if (cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET) {
    long ret = 0;
    mutexLock(&futex_lock[h]);
    if (*uaddr != val) {
      errno = EAGAIN; ret = -1;
    } else if (to) {
      const u64 ns = (u64)to->tv_sec * 1000000000ULL + (u64)to->tv_nsec;
      if (R_FAILED(condvarWaitTimeout(&futex_cond[h], &futex_lock[h], ns))) {
        errno = ETIMEDOUT; ret = -1;
      }
    } else {
      diag_futex_spin(uaddr); // beacon: alive-but-spinning if value never flips
      condvarWaitTimeout(&futex_cond[h], &futex_lock[h], 16000000ULL); // capped infinite wait
    }
    mutexUnlock(&futex_lock[h]);
    return ret;
  }
  if (cmd == FUTEX_WAKE || cmd == FUTEX_WAKE_BITSET) {
    mutexLock(&futex_lock[h]);
    condvarWakeAll(&futex_cond[h]);
    mutexUnlock(&futex_lock[h]);
    return val > 0 ? val : 0; // approximate count woken
  }
  errno = ENOSYS;
  return -1;
}

/* newlib has no <sys/uio.h>; the kernel iovec layout is just {ptr, len}. */
struct nx_iovec { void *iov_base; size_t iov_len; };

/* Validate that [addr, addr+len) is mapped and readable via svcQueryMemory, so a
 * self process_vm_readv can copy safely instead of risking a fault. */
static int nx_addr_readable(uintptr_t addr, size_t len) {
  uintptr_t a = addr, end = addr + len;
  while (a < end) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) return 0;
    if (mi.type == 0) return 0;                 /* MemType_Unmapped */
    if ((mi.perm & Perm_R) == 0) return 0;      /* not readable */
    uintptr_t be = (uintptr_t)mi.addr + mi.size;
    if (be <= a) return 0;
    a = be;
  }
  return 1;
}

long syscall_fake(long number, ...) {
  switch (number) {
    case ARM64_SYS_GETTID: return gettid_fake();
    case ARM64_SYS_FUTEX: {
      va_list va; va_start(va, number);
      volatile int32_t *uaddr = va_arg(va, volatile int32_t *);
      const int op  = va_arg(va, int);
      const int val = va_arg(va, int);
      const struct timespec *to = va_arg(va, const struct timespec *);
      va_end(va);
      return futex_impl(uaddr, op, val, to);
    }
    case ARM64_SYS_SCHED_SETAFFINITY:
      return 0; // affinity hints are advisory; pretend success
    case ARM64_SYS_PROCESS_VM_READV:
    case ARM64_SYS_PROCESS_VM_WRITEV: {
      /* Self memory copy used as a fault-safe read/write probe. Stubbing it to
       * ENOSYS made the caller spin once per frame (a process_vm_readv flood),
       * wedging the boot path. Implement it for the own-process case: validate
       * each remote range with svcQueryMemory, then copy the readable parts. */
      va_list va; va_start(va, number);
      long pid                   = va_arg(va, long); (void)pid;
      const struct nx_iovec *liov   = va_arg(va, const struct nx_iovec *);
      unsigned long lcnt         = va_arg(va, unsigned long);
      const struct nx_iovec *riov   = va_arg(va, const struct nx_iovec *);
      unsigned long rcnt         = va_arg(va, unsigned long);
      va_end(va);
      int writing = (number == ARM64_SYS_PROCESS_VM_WRITEV);
      static int dbg = 0;
      if (dbg < 5) {
        dbg++;
        debugPrintf("[sys%ld] %s lcnt=%lu rcnt=%lu remote0=%p rlen0=%zu caller=%p\n",
                    number, writing ? "vm_writev" : "vm_readv", lcnt, rcnt,
                    rcnt ? riov[0].iov_base : NULL, rcnt ? riov[0].iov_len : 0,
                    __builtin_return_address(0));
      }
      ssize_t total = 0;
      unsigned long li = 0, ri = 0; size_t lo = 0, ro = 0;
      while (li < lcnt && ri < rcnt) {
        char *lp = (char *)liov[li].iov_base + lo;
        char *rp = (char *)riov[ri].iov_base + ro;
        size_t lrem = liov[li].iov_len - lo, rrem = riov[ri].iov_len - ro;
        size_t n = lrem < rrem ? lrem : rrem;
        char *probe = writing ? lp : rp;   /* the side being read-from must be readable */
        if (!nx_addr_readable((uintptr_t)probe, n)) {
          if (total == 0) { errno = EFAULT; return -1; }
          return total;
        }
        if (writing) memcpy(rp, lp, n); else memcpy(lp, rp, n);
        total += (ssize_t)n; lo += n; ro += n;
        if (lo == liov[li].iov_len) { li++; lo = 0; }
        if (ro == riov[ri].iov_len) { ri++; ro = 0; }
      }
      return total;
    }
  }
  debugPrintf("libc: syscall(%ld) -> ENOSYS\n", number);
  errno = ENOSYS;
  return -1;
}

void sincosf_fake(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }
int sched_get_priority_max_fake(int policy) { (void)policy; return 0; }
int sched_get_priority_min_fake(int policy) { (void)policy; return 0; }
void android_set_abort_message_fake(const char *msg) { debugPrintf("abort message: %s\n", msg ? msg : "(null)"); }
size_t __ctype_get_mb_cur_max_fake(void) { return 1; }
int __register_atfork_fake(void) { return 0; }
int __cxa_thread_atexit_impl_fake(void (*fn)(void *), void *arg, void *dso) { (void)fn; (void)arg; (void)dso; return 0; }

#define BIONIC_SC_PAGESIZE 39
#define BIONIC_SC_PAGE_SIZE 40
#define BIONIC_SC_NPROCESSORS_CONF 96
#define BIONIC_SC_NPROCESSORS_ONLN 97
#define BIONIC_SC_PHYS_PAGES 98

long sysconf_fake(int name) {
  switch (name) {
    case BIONIC_SC_PAGESIZE:
    case BIONIC_SC_PAGE_SIZE: return 0x1000;
    case BIONIC_SC_NPROCESSORS_CONF:
    case BIONIC_SC_NPROCESSORS_ONLN: return 3;
    // Report 512 MB (matches synthetic /proc/meminfo MemTotal) to make Unity's
    // DynamicHeap reserve fewer 256MB regions; real backing (arena/OC) holds more.
    case BIONIC_SC_PHYS_PAGES: return (512ll * 1024 * 1024) / 0x1000;
    default: return -1;
  }
}
long pathconf_fake(const char *path, int name) { (void)path; (void)name; return -1; }

// ---------------------------------------------------------------------------
// open() flag translation (bionic/linux -> newlib)
// ---------------------------------------------------------------------------

#define LINUX_O_CREAT  0100
#define LINUX_O_EXCL   0200
#define LINUX_O_TRUNC  01000
#define LINUX_O_APPEND 02000

static int convert_open_flags(int flags) {
  int out = flags & 3;
  if (flags & LINUX_O_CREAT)  out |= O_CREAT;
  if (flags & LINUX_O_EXCL)   out |= O_EXCL;
  if (flags & LINUX_O_TRUNC)  out |= O_TRUNC;
  if (flags & LINUX_O_APPEND) out |= O_APPEND;
  return out;
}

// The engine addresses asset packs as "<packdir>/<file>.mvgl" but we ship the
// data flat in the game dir. If a read path with a subdirectory is missing,
// fall back to just its basename in the cwd (the game dir). Reads only -- never
// redirect a write -- and only when the basename actually exists.
static int basename_fallback(const char *path, char *out, size_t outsz) {
  const char *slash = strrchr(path, '/');
  if (!slash || !slash[1]) return 0;   // no subdir component to strip
  struct stat st;
  snprintf(out, outsz, "%s", slash + 1); // basename, resolved against the cwd
  return stat(out, &st) == 0;
}

// Create one directory, skipping paths newlib's mkdir() can't handle safely.
// A bare "device:" path (e.g. "sdmc:") makes newlib resolve to a device root
// with an empty in-device path and dereference a NULL devoptab -- a Data Abort
// reading devoptab->mkdir_r at +0x68. Refuse those (and null/empty).
static int safe_mkdir(const char *p) {
  if (!p || !*p) { errno = EINVAL; return -1; }
  const char *colon = strchr(p, ':');
  if (colon) {                       // has a "device:" prefix
    const char *in = colon + 1;      // the path inside the device
    while (*in == '/') in++;
    if (!*in) { errno = EEXIST; return 0; }  // "sdmc:" / "sdmc:/" -> root, skip
    // A single top-level component ("sdmc:/switch") also null-derefs newlib's
    // devoptab. Such dirs (the homebrew mount point) always pre-exist already.
    if (!strchr(in, '/')) { errno = EEXIST; return 0; }
  }
  return mkdir(p, 0777);
}

// mkdir -p: create `dir` and every missing parent. Save data lives in subdirs
// the engine only mkdir()s one level at a time, so a deeper missing parent left
// the whole chain (and the save write) failing.
//
// We must NOT try to create the game root or any ancestor of it ("sdmc:",
// "sdmc:/switch", "sdmc:/switch/zookeeper"): they already exist, they aren't
// ours, and newlib's mkdir() of a *top-level* path (one component under the
// device, e.g. "sdmc:/switch") null-derefs its devoptab -> Data Abort at the
// mkdir_r slot (+0x68). So begin the parent walk *after* GAME_HOME.
static void mkdir_p_dir(const char *dir) {
  if (!dir || !*dir) return;
  char tmp[512];
  if (snprintf(tmp, sizeof(tmp), "%s", dir) <= 0) return;
  size_t skip;
  const char *root = zb_game_root();
  const size_t glen = strlen(root);
  if (strncmp(tmp, root, glen) == 0 && (tmp[glen] == '/' || tmp[glen] == '\0')) {
    skip = glen;                                  // only create *under* the game root
  } else {
    const char *colon = strchr(tmp, ':');         // unknown base: at least skip "device:"
    skip = colon ? (size_t)(colon + 1 - tmp) : 0;
  }
  for (char *p = tmp + skip + 1; *p; p++)
    if (*p == '/') { *p = '\0'; safe_mkdir(tmp); *p = '/'; }
  if (tmp[skip]) safe_mkdir(tmp);
}
// create the parent directory chain of a file path
static void mkdir_parents(const char *filepath) {
  char tmp[512];
  snprintf(tmp, sizeof(tmp), "%s", filepath);
  char *last = strrchr(tmp, '/');
  if (!last || last == tmp) return;
  *last = '\0';
  mkdir_p_dir(tmp);
}

// mkdir wrapper: create the full chain and treat "already exists" as success
int mkdir_fake(const char *path, unsigned mode) {
  (void)mode;
  if (!path || !*path) { errno = EINVAL; return -1; }
  mkdir_p_dir(path);
  int r = safe_mkdir(path);
  if (r != 0 && errno == EEXIST) r = 0;
  return r;
}

int g_watch_fd = -1;   /* data.unity3d fd: trace its reads/seeks to debug header load */
void watch_dump(const char *tag, int fd, long a, long b, const void *buf, long got) {
  if (fd != g_watch_fd) return;
  char h[64]; int n = (got > 16 ? 16 : (got < 0 ? 0 : (int)got));
  int p = 0; for (int i = 0; i < n; i++) p += snprintf(h + p, sizeof(h) - p, "%02x ", ((const unsigned char *)buf)[i]);
  h[p] = 0;
  if (TRACE_IO) debugPrintf("[io] %s fd=%d a=%ld b=%ld -> %ld  [%s]\n", tag, fd, a, b, got, h);
}

/* ---- read-ahead cache for big archive files (data.unity3d, sharedassets) ----
 * Unity deserializes archives with THOUSANDS of tiny read()s (2-8 bytes each,
 * field by field). On Android the archive sits in the OS page cache so these are
 * memory-fast; on Switch there is no page cache, so every tiny read is a direct
 * SD access (~ms) and boot crawls (8000+ reads just for the header). We buffer
 * each big read-only fd through a 1MB window filled by one large read, and
 * virtualize the logical file position -- turning ~250k tiny SD reads per MB into
 * a single one. Keyed by fd; the real fd position is used only as our scratch. */
#define RA_SLOTS 32              /* per-fd position slots (tiny now: no per-fd buffer) */
/* Global asset block cache -- adapted from the pvz / fruitninja "rc" cache the user
 * supplied. Unity deserializes archives with thousands of scattered tiny reads; the
 * old design gave each big fd a single 2MB sliding window that RE-FILLED on every far
 * seek. This keeps up to RC_NBLOCK 64KB blocks live at once, hash-indexed by (fd,blk),
 * so scattered random reads across a large sharedassets file stay cached in RAM. The
 * per-fd logical position is still virtualized by the RaCache slots + z_lseek below. */
#define RC_BLOCK   (64u * 1024)
#define RC_NBLOCK  384u          /* 384 * 64KB = 24 MB */
static struct { int fd; uint64_t blk; uint32_t len; } rc_tag[RC_NBLOCK];
static unsigned char *rc_data;
static int rc_state;             /* 0 untried, 1 ready, -1 disabled */
static void rc_init(void) {
  if (rc_state) return;
  rc_data = (unsigned char *)malloc((size_t)RC_NBLOCK * RC_BLOCK);
  if (rc_data) {
    for (unsigned i = 0; i < RC_NBLOCK; i++) rc_tag[i].fd = -1;
    rc_state = 1;
    debugPrintf("[rc] asset block cache armed: %u x %u KB = %u MB\n",
                RC_NBLOCK, RC_BLOCK >> 10, (unsigned)(((size_t)RC_NBLOCK * RC_BLOCK) >> 20));
  } else {
    rc_state = -1;
    debugPrintf("[rc] asset block cache DISABLED (alloc failed)\n");
  }
}
/* Serve `count` bytes at absolute `off` from the block cache. Caller holds g_ra_lock;
 * the real fd position is used only as scratch (as the old window did). */
static long rc_pread_locked(int fd, void *buf, size_t count, long off) {
  rc_init();
  if (rc_state != 1 || count == 0) return -1;
  size_t done = 0;
  while (done < count) {
    uint64_t abs = (uint64_t)off + done, blk = abs / RC_BLOCK;
    unsigned idx = (unsigned)(((blk * 2654435761ull) ^ (unsigned)fd) % RC_NBLOCK);
    unsigned char *slot = rc_data + (size_t)idx * RC_BLOCK;
    if (rc_tag[idx].fd != fd || rc_tag[idx].blk != blk) {   /* miss -> fill one 64KB block */
      if (lseek(fd, (long)(blk * RC_BLOCK), SEEK_SET) < 0) break;
      size_t got = 0;
      while (got < RC_BLOCK) { long r = read(fd, slot + got, RC_BLOCK - got); if (r <= 0) break; got += (size_t)r; }
      rc_tag[idx].fd = fd; rc_tag[idx].blk = blk; rc_tag[idx].len = (uint32_t)got;
    }
    uint32_t blen = rc_tag[idx].len, inblk = (uint32_t)(abs - blk * RC_BLOCK);
    if (inblk >= blen) break;                    /* EOF inside this block */
    size_t n = blen - inblk; if (n > count - done) n = count - done;
    memcpy((char *)buf + done, slot + inblk, n);
    done += n;
    if (blen < RC_BLOCK) break;                  /* short block == EOF */
  }
  return (long)done;
}
static void rc_drop_fd(int fd) {                 /* fd numbers get reused -> evict stale blocks */
  if (rc_state != 1) return;
  for (unsigned i = 0; i < RC_NBLOCK; i++) if (rc_tag[i].fd == fd) rc_tag[i].fd = -1;
}

struct RaCache {
  int  fd;           /* -1 == free */
  long pos;          /* virtual file position (what read/lseek observe) */
  long size;         /* file size (for SEEK_END) */
};
static struct RaCache g_ra[RA_SLOTS];
static Mutex g_ra_lock;
static struct RaCache *ra_find(int fd) {
  if (fd < 0) return NULL;
  for (int i = 0; i < RA_SLOTS; i++) if (g_ra[i].fd == fd) return &g_ra[i];
  return NULL;
}
void ra_attach(int fd, long size) {
  mutexLock(&g_ra_lock);
  for (int i = 0; i < RA_SLOTS; i++) if (g_ra[i].fd < 0) {
    g_ra[i].fd = fd; g_ra[i].pos = 0; g_ra[i].size = size;
    break;
  }
  mutexUnlock(&g_ra_lock);
}
void ra_detach(int fd) {
  mutexLock(&g_ra_lock);
  struct RaCache *c = ra_find(fd);
  if (c) c->fd = -1;
  rc_drop_fd(fd);      /* the block cache is global -> drop this fd's blocks */
  mutexUnlock(&g_ra_lock);
}
static long ra_read(struct RaCache *c, int fd, void *buf, size_t count) {
  mutexLock(&g_ra_lock);
  long got = rc_pread_locked(fd, buf, count, c->pos);
  if (got > 0) c->pos += got;
  mutexUnlock(&g_ra_lock);
  return got < 0 ? 0 : got;
}

/* lseek for arm64: off_t is already 64-bit, so this also services lseek64.
 * lseek64 was previously stubbed to return 0 (no seek) -- that made libunity's
 * archive reader see data.unity3d as empty/mis-positioned ("Unable to read
 * header from archive file"), since it lseek64(SEEK_END)s to size the file. */
long z_lseek(int fd, long off, int whence) {
#if ASSET_PACK_ENABLE
  if (asset_pack_fd_is(fd)) return asset_pack_lseek_fd(fd, off, whence);
#endif
  struct RaCache *c = ra_find(fd);
  if (c) {   /* virtualized position -- don't touch the real fd here */
    mutexLock(&g_ra_lock);
    long np = (whence == SEEK_SET) ? off : (whence == SEEK_CUR) ? c->pos + off : c->size + off;
    c->pos = np;
    mutexUnlock(&g_ra_lock);
    return np;
  }
  return lseek(fd, off, whence);
}

static const char *synthetic_proc(const char *path);  /* defined below */

// Serve /proc and /sys reads that arrive through raw open() (e.g.
// /proc/self/maps, which the engine opens to enumerate memory mappings).
// newlib's open() can't be memory-backed, so materialize the synthetic content
// into a small file under the game dir and hand back a real fd. Returns an fd,
// or -1 if `path` isn't a node we synthesize (caller proceeds normally).
static int synth_proc_open(const char *path) {
  if (!path) return -1;
  if (strncmp(path, "/proc/", 6) && strncmp(path, "/sys/", 5)) return -1;
  static char buf[16384];
  int len;
  if (!strcmp(path, "/proc/self/maps") || !strcmp(path, "/proc/self/smaps")) {
    len = so_dump_maps(buf, sizeof buf);
  } else {
    const char *s = synthetic_proc(path);
    if (!s) return -1;                                   // not /proc or /sys
    len = (int)strlen(s);
    if (len > (int)sizeof buf) len = (int)sizeof buf;
    memcpy(buf, s, (size_t)len);
  }
  char safe[160]; size_t j = 0;
  for (const char *p = path; *p && j < sizeof safe - 1; p++) safe[j++] = (*p == '/') ? '_' : *p;
  safe[j] = '\0';
  char tf[256];
  snprintf(tf, sizeof tf, "%s/.synth/%s", zb_game_root(), safe);
  int wfd = open(tf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (wfd >= 0) { if (write(wfd, buf, (size_t)len) < 0) { /* best effort */ } close(wfd); }
  return open(tf, O_RDONLY);
}

// ---------------------------------------------------------------------------
// Synthetic inode numbers. libnx's fsdev returns st_ino==0 for EVERY file, but
// il2cpp's System.IO share layer keys its open-file table on (st_dev,st_ino):
// with every file colliding on inode 0 it treats UNRELATED files as the same
// one, so Easy Save's concurrent SaveData.es3.tmp (write) + SaveData.es3 (read)
// looks like an incompatible double-open of one file and throws "IOException:
// Sharing violation on path SaveData.es3" every frame on the menu -- and each of
// those exception backtraces mmaps libunity/libil2cpp, leaking us to OOM/exit.
// Give each distinct path a stable, non-zero inode (only when the real one is 0,
// so a genuine inode from a future fsdev is preserved). fstat() has no path, so a
// small fd->inode map is filled at open() time and mirrors stat(path)'s value.
#define FD_INO_MAX 4096
static uint64_t g_fd_ino[FD_INO_MAX];
static uint64_t path_ino(const char *path) {
  uint64_t h = 1469598103934665603ULL;               // FNV-1a 64 offset basis
  for (const unsigned char *p = (const unsigned char *)path; *p; p++) { h ^= *p; h *= 1099511628211ULL; }
  return h ? h : 1;                                   // 0 means "no inode" -- avoid it
}
static void fd_ino_set(int fd, const char *path) { if (fd >= 0 && fd < FD_INO_MAX) g_fd_ino[fd] = path_ino(path); }
static void fd_ino_clear(int fd) { if (fd >= 0 && fd < FD_INO_MAX) g_fd_ino[fd] = 0; }

/* Close every fd we know to be open on `path`, returning the LOWEST one we closed (or -1).
 * Used by rename_fake: Horizon refuses to rename, delete, or even open-for-write a file that
 * has a live handle, and il2cpp's ReplaceFile holds one on the very file we must replace.
 * g_fd_ino is a 64-bit FNV hash of the path, maintained by open_fake/close_fake, so a false
 * match would need a 64-bit hash collision against a path opened in this same process. */
int close_fake(int fd);          /* fwd: clears g_fd_ino and commits the SD card */
static int fd_close_by_path(const char *path) {
  const uint64_t ino = path_ino(path);
  int first = -1;
  for (int fd = 0; fd < FD_INO_MAX; fd++) {
    if (g_fd_ino[fd] != ino) continue;
    if (TRACE_IO) debugPrintf("[io] rename: closing caller's open handle fd=%d on %s\n", fd, path);
    close_fake(fd);
    if (first < 0) first = fd;
  }
  return first;
}

// libnx buffers sdmc: writes -- they reach the physical card only on fsdevCommitDevice() or a
// clean unmount. Exiting via HOME kills the process (title override), so that commit never runs
// and saves written this session vanish on reboot. Track fds opened for writing and commit the
// card when they close, so a save persists the moment the game finishes writing it.
#define WFD_MAX 4096
static unsigned char g_write_fd[WFD_MAX];
int nx_sd_dirty = 0;   /* a file has been opened for writing since the last commit */
static void mark_write_fd(int fd)   { nx_sd_dirty = 1; if (fd >= 0 && fd < WFD_MAX) g_write_fd[fd] = 1; }
static void commit_write_fd(int fd) { if (fd >= 0 && fd < WFD_MAX && g_write_fd[fd]) {
                                        g_write_fd[fd] = 0; nx_sd_dirty = 0; fsdevCommitDevice("sdmc"); } }

/* Commit any pending writes to the physical card. Called periodically from the render loop and
 * after the game's save (nx_save_prefs), so the save survives a HOME-kill / reboot. */
void nx_sd_flush(void) {
  debug_log_flush();   /* push our own buffered log before committing the FS */
  if (!nx_sd_dirty) return;
  nx_sd_dirty = 0;
  fsdevCommitDevice("sdmc");
}

/* Unity probes filesystem case-sensitivity on every boot: it creates
 * CASESENSITIVETEST<guid> (O_CREAT|O_EXCL) in the data root, re-opens it under a
 * different case, and never cleans up -- a fresh GUID name each launch, so junk
 * accumulates on the SD card. Redirect every such name (any case, any guid) onto ONE
 * fixed hidden scratch file. Different-case probes then hit the same file, which is
 * exactly the case-INSENSITIVE answer FAT/exFAT gives anyway, and main.c sweeps the
 * scratch (plus any strays from older builds) at boot. (Copied from zookeeperdx_nx.) */
static const char *casetest_redirect(const char *path) {
  if (!path) return path;
  const char *b = strrchr(path, '/');
  b = b ? b + 1 : path;
  if (strncasecmp(b, "CASESENSITIVETEST", 17) == 0) {
    /* Was a compile-time literal; now built from the runtime root, so it needs
     * storage that outlives the call. One probe path, one buffer. */
    static char casetest[560];
    if (!casetest[0])
      snprintf(casetest, sizeof casetest, "%s/.synth/casetest", zb_game_root());
    return casetest;
  }
  return path;
}

/* Android APK asset root -> our asset dir on the SD card.
 *
 * The game builds ABSOLUTE "/assets/..." paths (on Android that's the APK's
 * asset root). On Switch a leading "/" makes newlib resolve against the DEFAULT
 * DEVICE ROOT, i.e. sdmc:/assets/... -- the root of the SD card -- which does not
 * exist. Our assets live under GAME_HOME/assets/. Result: every absolute asset
 * open returned -1 (AssetBundles/*.unity3d, Data/*.json) even though the files
 * were present, because they were being looked for in the wrong place.
 *
 * Relative "assets/..." paths already work (they resolve against the cwd, which
 * main.c chdir()s to GAME_HOME), so ONLY the absolute form needs rewriting.
 * basename_fallback() can't cover this: it drops the subdirectory, so
 * "/assets/AssetBundles/Episode_1_Levels.unity3d" would degrade to looking for
 * "Episode_1_Levels.unity3d" in the game root. Keep the full subpath instead. */
static const char *asset_redirect(const char *path, char *buf, size_t bufsz) {
  if (!path) return path;
  if (!strncmp(path, "/assets/", 8)) {
    snprintf(buf, bufsz, "%s/assets/%s", zb_game_root(), path + 8);
    return buf;
  }
  if (!strcmp(path, "/assets")) {
    snprintf(buf, bufsz, "%s/assets", zb_game_root());
    return buf;
  }
  return path;
}

int open_fake(const char *path, int flags, ...) {
  char _rbuf[512];
  path = casetest_redirect(path);
  path = asset_redirect(path, _rbuf, sizeof _rbuf);
  int mode = 0666;
  if (flags & LINUX_O_CREAT) { va_list va; va_start(va, flags); mode = va_arg(va, int); va_end(va); }
  const int cvt = convert_open_flags(flags);
  const int writing = (flags & 3) != 0 || (flags & LINUX_O_CREAT);
  if (!writing) {
#if ASSET_PACK_ENABLE
    /* Asset pack: if a path resolves to a packed asset, hand back a pack fd (a real
     * fd of the single pack file, tracked in the pack's handle table). read/lseek/
     * fstat/close route to the pack via asset_pack_fd_is(). Falls through to the
     * normal open when the path isn't packed (asset_pack_open_path -> -1). */
    if (asset_pack_active()) {
      int pfd = asset_pack_open_path(path);
      if (pfd >= 0) {
        if (TRACE_IO) debugPrintf("[io] open(%s) -> %d [pack]\n", path, pfd);
        return pfd;
      }
    }
#endif
    // /dev/urandom + /dev/random: Switch has no /dev node, but Mono/.NET (RNG
    // seeds, Guid.NewGuid, hashtable randomization) and asset crypto open these.
    // A failing open (-1) leaves those paths without entropy and can stall the
    // scene/asset load. Materialize a buffer of real CSPRNG bytes (libnx
    // randomGet) into a file and hand back a real fd so read() just works.
    if (!strcmp(path, "/dev/urandom") || !strcmp(path, "/dev/random")) {
      static char rbuf[65536];
      randomGet(rbuf, sizeof rbuf);
      char tf[256];
      snprintf(tf, sizeof tf, "%s/.synth/dev_random", zb_game_root());
      int wfd = open(tf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (wfd >= 0) { if (write(wfd, rbuf, sizeof rbuf) < 0) { /* best effort */ } close(wfd); }
      int rfd = open(tf, O_RDONLY);
      fd_ino_set(rfd, path);
      if (TRACE_IO) debugPrintf("[io] open(%s,0x%x) -> %d [urandom]\n", path, flags, rfd);
      return rfd;
    }
    // synthetic /proc, /sys (incl. self/maps)
    int sfd = synth_proc_open(path);
    if (sfd >= 0) { fd_ino_set(sfd, path); if (TRACE_IO) debugPrintf("[io] open(%s,0x%x) -> %d [synthetic]\n", path, flags, sfd); return sfd; }
  }
  int fd = open(path, cvt, mode);
  if (fd < 0 && writing) {
    // save files: the target subdir may not exist yet -- create it and retry
    mkdir_parents(path);
    fd = open(path, cvt, mode);
  }
  if (fd < 0 && (flags & 3) == 0 && !(flags & LINUX_O_CREAT)) {
    char alt[320];
    if (basename_fallback(path, alt, sizeof(alt)))
      fd = open(alt, cvt, mode);
  }
  if (fd >= 0) {
    fd_ino_set(fd, path);
    if (writing) mark_write_fd(fd);
    struct stat _st;
    if (fstat(fd, &_st) == 0) {
      if (TRACE_IO) debugPrintf("[io] open(%s,0x%x) -> %d size=%lld\n", path, flags, fd, (long long)_st.st_size);
      /* Big read-only asset files (data.unity3d ~424MB, sharedassets*.resource)
       * get a read-ahead cache so Unity's tiny per-field reads hit RAM, not SD.
       * Threshold 1MB (was 4MB): the split-layout level/sharedassets chunks and
       * global-metadata.dat are all read with the same tiny-read pattern. */
      if (!writing && _st.st_size >= (1 << 20))
        ra_attach(fd, (long)_st.st_size);
    } else {
      if (TRACE_IO) debugPrintf("[io] open(%s,0x%x) -> %d size=?\n", path, flags, fd);
    }
  } else {
    if (TRACE_IO) debugPrintf("[io] open(%s,0x%x) -> %d\n", path, flags, fd);
  }
  return fd;
}
int openat_fake(int dirfd, const char *path, int flags, ...) {
  (void)dirfd;
  int mode = 0666;
  if (flags & LINUX_O_CREAT) { va_list va; va_start(va, flags); mode = va_arg(va, int); va_end(va); }
  // Delegate to open_fake so /dev/urandom, synthetic /proc + /sys, save-dir
  // creation and basename fallback all apply (some libc paths route open->openat).
  return open_fake(path, flags, mode);
}
int unlinkat_fake(int dirfd, const char *path, int flags) { (void)dirfd; (void)flags; return unlink(path); }

// ---------------------------------------------------------------------------
// struct stat conversion (bionic aarch64 layout)
// ---------------------------------------------------------------------------

struct bionic_timespec { int64_t tv_sec; int64_t tv_nsec; };
struct bionic_stat {
  uint64_t st_dev; uint64_t st_ino; uint32_t st_mode; uint32_t st_nlink;
  uint32_t st_uid; uint32_t st_gid; uint64_t st_rdev; uint64_t __pad1;
  int64_t st_size; int32_t st_blksize; int32_t __pad2; int64_t st_blocks;
  struct bionic_timespec st_atim; struct bionic_timespec st_mtim; struct bionic_timespec st_ctim;
  uint32_t __unused4; uint32_t __unused5;
};

static void convert_stat(const struct stat *in, struct bionic_stat *out) {
  memset(out, 0, sizeof(*out));
  out->st_dev = in->st_dev; out->st_ino = in->st_ino; out->st_mode = in->st_mode;
  out->st_nlink = in->st_nlink; out->st_uid = in->st_uid; out->st_gid = in->st_gid;
  out->st_rdev = in->st_rdev; out->st_size = in->st_size; out->st_blksize = in->st_blksize;
  out->st_blocks = in->st_blocks;
  out->st_atim.tv_sec = in->st_atime; out->st_mtim.tv_sec = in->st_mtime; out->st_ctim.tv_sec = in->st_ctime;
}

/* POSIX rename() REPLACES an existing destination. Horizon's fsFsRenameFile does NOT -- it
 * fails if the destination exists, and libnx's fsdev passes that straight through. Worse,
 * Horizon will not rename OR delete a file that currently has an open handle.
 *
 * Bad Piggies saves via SettingsData.TransactionalFileWrite:
 *     write Progress.dat.tmp
 *     if (File.Exists(Progress.dat)) File.Replace(tmp, Progress.dat, Progress.dat.bak);
 *     else                           File.Move   (tmp, Progress.dat);
 * and il2cpp's native ReplaceFile does:
 *     fd = open(backup, O_RDONLY);          <-- keeps .bak OPEN across the next call
 *     rename(dest -> backup);
 *     rename(source -> dest);
 *     close(fd);
 *
 * So on Switch that first rename hits a destination which (a) already exists and (b) is held
 * open by the caller itself. It cannot be renamed away and it cannot be deleted -- which is
 * why every save silently jammed, leaving the real save stranded in Progress.dat.tmp while
 * the game kept loading a stale Progress.dat. (The game try/catches its save, hence no
 * managed exception in the log.) Settings.xml goes down the identical path.
 *
 * The fix therefore must never touch the destination's DIRECTORY ENTRY -- only its CONTENTS.
 * We try, in order:
 *   1. plain rename                    (destination absent: nothing to do)
 *   2. park the destination aside, rename, drop the parked copy, roll back on failure
 *      (correct and near-atomic; works whenever the destination is not held open)
 *   3. copy the source's BYTES over the destination and unlink the source
 *      (the only thing that works when the destination has an open handle -- writing to an
 *       open file is allowed, renaming/deleting it is not)
 * Step 2 is kept ahead of step 3 because it is atomic and never leaves a half-written file;
 * step 3 is the fallback that actually rescues this game's save. We deliberately read the
 * source fully BEFORE truncating the destination, so a failure to read cannot destroy it. */
int rename_fake(const char *oldp, const char *newp) {
  char rb1[512], rb2[512];
  oldp = asset_redirect(oldp, rb1, sizeof rb1);
  newp = asset_redirect(newp, rb2, sizeof rb2);

  if (rename(oldp, newp) == 0) return 0;          /* 1. destination absent: plain rename */

  struct stat st;
  if (stat(newp, &st) != 0) {                     /* dest really is absent -> genuine error */
    if (TRACE_IO) debugPrintf("[io] rename(%s -> %s) FAILED, dest absent (errno %d)\n", oldp, newp, errno);
    return -1;
  }

  /* 2. park the destination aside */
  char aside[576];
  snprintf(aside, sizeof aside, "%s.rnold", newp);
  remove(aside);                                  /* stale one from an interrupted swap */
  if (rename(newp, aside) == 0) {
    if (rename(oldp, newp) == 0) {
      remove(aside);
      if (TRACE_IO) debugPrintf("[io] rename(%s -> %s) ok (displaced existing)\n", oldp, newp);
      return 0;
    }
    rename(aside, newp);                          /* roll back: restore the old file */
    if (TRACE_IO) debugPrintf("[io] rename(%s -> %s) FAILED, original restored (errno %d)\n", oldp, newp, errno);
    return -1;
  }

  /* 2b. The destination is held OPEN by the caller, and Horizon will not rename it, delete it,
   *     or even open it for writing (EIO) while a handle is live. il2cpp's native ReplaceFile is
   *     literally holding the file we have to replace:
   *         fd = open(backup, O_RDONLY);   rename(dest -> backup);   ... ;   close(fd);
   *     Nothing can be done to that file until the handle is gone -- so close it ourselves.
   *
   *     This is safe: the handle is a pure existence probe. ReplaceFile never reads a byte from
   *     it; its only other use is the close() afterwards, which on a stale fd returns EBADF and
   *     is ignored. To stop that fd NUMBER being handed to another thread in the gap, we re-open
   *     the destination right after the swap -- fds are allocated lowest-free, so we normally get
   *     the same number straight back and the caller's close() then closes our handle, which is
   *     exactly what it meant to do. */
  int victim = fd_close_by_path(newp);
  if (victim >= 0) {
    int ok = 0;
    if (rename(newp, aside) == 0) {
      if (rename(oldp, newp) == 0) { remove(aside); ok = 1; }
      else rename(aside, newp);                 /* roll back */
    }
    int re = open(newp, O_RDONLY);              /* re-occupy the fd number for the caller */
    if (re >= 0) {
      if (re == victim) fd_ino_set(re, newp);   /* caller's close() will close this */
      else close(re);
    }
    if (ok) {
      if (TRACE_IO) debugPrintf("[io] rename(%s -> %s) ok (closed caller's handle, then displaced)\n", oldp, newp);
      return 0;
    }
  }

  /* 3. still stuck -> overwrite the destination's contents instead of its directory entry.
   *    Read the source completely first; only then truncate the destination. */
  int fsrc = open(oldp, O_RDONLY);
  if (fsrc < 0) {
    if (TRACE_IO) debugPrintf("[io] rename(%s -> %s) FAILED, cannot read source (errno %d)\n", oldp, newp, errno);
    return -1;
  }
  struct stat ss;
  if (fstat(fsrc, &ss) != 0 || ss.st_size < 0) { close(fsrc); return -1; }
  size_t n = (size_t)ss.st_size;
  unsigned char *buf = (unsigned char *)malloc(n ? n : 1);
  if (!buf) { close(fsrc); return -1; }
  size_t got = 0;
  while (got < n) {
    ssize_t r = read(fsrc, buf + got, n - got);
    if (r <= 0) break;
    got += (size_t)r;
  }
  close(fsrc);
  if (got != n) {
    free(buf);
    if (TRACE_IO) debugPrintf("[io] rename(%s -> %s) FAILED, short read of source (%zu/%zu)\n", oldp, newp, got, n);
    return -1;                                    /* destination untouched */
  }

  int fdst = open(newp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fdst < 0) {
    free(buf);
    if (TRACE_IO) debugPrintf("[io] rename(%s -> %s) FAILED, cannot write dest (errno %d)\n", oldp, newp, errno);
    return -1;
  }
  size_t put = 0;
  while (put < n) {
    ssize_t w = write(fdst, buf + put, n - put);
    if (w <= 0) break;
    put += (size_t)w;
  }
  fsync(fdst);
  close(fdst);
  free(buf);
  if (put != n) {
    if (TRACE_IO) debugPrintf("[io] rename(%s -> %s) FAILED, short write of dest (%zu/%zu)\n", oldp, newp, put, n);
    return -1;
  }

  /* The destination now holds the source's bytes; rename() also means the source is gone. */
  if (remove(oldp) != 0)
    if (TRACE_IO) debugPrintf("[io] rename: warning, could not unlink source %s (errno %d)\n", oldp, errno);
  if (TRACE_IO) debugPrintf("[io] rename(%s -> %s) ok (%zu bytes copied over open dest)\n", oldp, newp, n);
  return 0;
}

int stat_fake(const char *path, struct bionic_stat *st) {
  char _rbuf[512];
  path = asset_redirect(path, _rbuf, sizeof _rbuf);
#if ASSET_PACK_ENABLE
  if (asset_pack_active()) {
    uint64_t sz = 0, ino = 0; int isdir = 0;
    if (asset_pack_stat_path_info(path, &sz, &ino, &isdir)) {
      struct stat real; memset(&real, 0, sizeof real);
      real.st_size = (off_t)sz; real.st_ino = ino;
      real.st_mode = isdir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
      convert_stat(&real, st);
      if (st->st_ino == 0) st->st_ino = ino ? ino : path_ino(path);
      return 0;
    }
  }
#endif
  struct stat real; int r = stat(path, &real);
  if (r != 0) {
    char alt[320];
    if (basename_fallback(path, alt, sizeof(alt))) r = stat(alt, &real);
  }
  if (r == 0) {
    convert_stat(&real, st);
    if (st->st_ino == 0) st->st_ino = path_ino(path);   // fsdev gives 0 -> synth
  }
  return r;
}
int fstat_fake(int fd, struct bionic_stat *st) {
#if ASSET_PACK_ENABLE
  if (asset_pack_fd_is(fd)) {
    uint64_t sz = 0, ino = 0; int isdir = 0;
    if (asset_pack_fstat_fd(fd, &sz, &ino, &isdir) && st) {
      struct stat real; memset(&real, 0, sizeof real);
      real.st_size = (off_t)sz; real.st_ino = ino;
      real.st_mode = isdir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
      convert_stat(&real, st);
      if (st->st_ino == 0) st->st_ino = ino;
      return 0;
    }
    return -1;
  }
#endif
  struct stat real; const int r = fstat(fd, &real);
  if (r == 0) {
    convert_stat(&real, st);
    if (st->st_ino == 0) {                               // mirror stat(path)'s inode
      uint64_t ino = (fd >= 0 && fd < FD_INO_MAX) ? g_fd_ino[fd] : 0;
      st->st_ino = ino ? ino : ((uint64_t)(fd + 1) * 2654435761ULL) | 1;
    }
  }
  return r;
}
int lstat_fake(const char *path, struct bionic_stat *st) { return stat_fake(path, st); }

// ---------------------------------------------------------------------------
// dirent conversion (bionic dirent64 layout)
// ---------------------------------------------------------------------------

struct bionic_dirent {
  uint64_t d_ino; int64_t d_off; uint16_t d_reclen; uint8_t d_type; char d_name[256];
};

void *readdir_fake(void *dirp) {
  static struct bionic_dirent out; // not thread-safe (matches bionic readdir)
  struct dirent *e = readdir((DIR *)dirp);
  if (!e) return NULL;
  memset(&out, 0, sizeof(out));
  out.d_ino = e->d_ino;
  out.d_reclen = sizeof(out);
  out.d_type = e->d_type;
  snprintf(out.d_name, sizeof(out.d_name), "%s", e->d_name);
  return &out;
}

// ---------------------------------------------------------------------------
// locale: ignore the locale argument and use the C-locale versions
// ---------------------------------------------------------------------------

void *newlocale_fake(int mask, const char *locale, void *base) { (void)mask; (void)locale; (void)base; return (void *)1; }
void freelocale_fake(void *loc) { (void)loc; }
void *uselocale_fake(void *loc) { (void)loc; return (void *)1; }

#define WRAP_ISW_L(fn) int fn##_l_fake(int wc, void *loc) { (void)loc; return fn(wc); }
WRAP_ISW_L(iswalpha) WRAP_ISW_L(iswblank) WRAP_ISW_L(iswcntrl) WRAP_ISW_L(iswdigit)
WRAP_ISW_L(iswlower) WRAP_ISW_L(iswprint) WRAP_ISW_L(iswpunct) WRAP_ISW_L(iswspace)
WRAP_ISW_L(iswupper) WRAP_ISW_L(iswxdigit) WRAP_ISW_L(towlower) WRAP_ISW_L(towupper)

int strcoll_l_fake(const char *a, const char *b, void *loc) { (void)loc; return strcoll(a, b); }
size_t strxfrm_l_fake(char *dst, const char *src, size_t n, void *loc) { (void)loc; return strxfrm(dst, src, n); }
size_t strftime_l_fake(char *s, size_t max, const char *fmt, const void *tm, void *loc) { (void)loc; return strftime(s, max, fmt, (const struct tm *)tm); }
long double strtold_l_fake(const char *s, char **end, void *loc) { (void)loc; return strtold(s, end); }
long long strtoll_l_fake(const char *s, char **end, int base, void *loc) { (void)loc; return strtoll(s, end, base); }
unsigned long long strtoull_l_fake(const char *s, char **end, int base, void *loc) { (void)loc; return strtoull(s, end, base); }
int wcscoll_l_fake(const wchar_t *a, const wchar_t *b, void *loc) { (void)loc; return wcscoll(a, b); }
size_t wcsxfrm_l_fake(wchar_t *dst, const wchar_t *src, size_t n, void *loc) { (void)loc; return wcsxfrm(dst, src, n); }

size_t mbsnrtowcs_fake(wchar_t *dst, const char **src, size_t nms, size_t len, void *ps) {
  (void)ps;
  size_t i = 0; const char *s = *src;
  while (i < nms && s[i] && (!dst || i < len)) { if (dst) dst[i] = (unsigned char)s[i]; i++; }
  if (dst && i < len) { dst[i] = 0; *src = NULL; }
  return i;
}
size_t wcsnrtombs_fake(char *dst, const wchar_t **src, size_t nwc, size_t len, void *ps) {
  (void)ps;
  size_t i = 0; const wchar_t *s = *src;
  while (i < nwc && s[i] && (!dst || i < len)) { if (dst) dst[i] = (char)s[i]; i++; }
  if (dst && i < len) { dst[i] = 0; *src = NULL; }
  return i;
}

// ---------------------------------------------------------------------------
// memory
// ---------------------------------------------------------------------------

int posix_memalign_fake(void **out, size_t align, size_t size) {
  void *p = memalign(align, size);
  if (!p) return ENOMEM;
  *out = p;
  return 0;
}

// --- anonymous mmap arena (page-granular; supports sub-range munmap) ----------
//
// Switch has no mmap. Unity reserves big *256MB-aligned* pools by over-mmapping a
// larger region then munmapping the unaligned head/tail to keep an aligned middle.
// A plain malloc/free-per-mmap frees the WHOLE block when the head is trimmed (the
// trim's addr == the registered base) and the kept aligned middle is then reused
// out from under the engine -> the TLSF allocator's free block reads back zeroed
// (next_free == NULL) and faults. So we manage a dedicated arena (carved 256MB-
// aligned in __libnx_initheap) with a per-page used-bitmap: mmap = find a free run
// of pages and mark them; munmap = clear exactly the pages of the sub-range. Big
// requests are handed back 256MB-aligned so Unity only ever trims the tail.
// File-backed maps (Unity streams BGM/SE this way) are served from the same arena.
// ------------------------------------------------------------------------------
extern void  *g_mmap_arena_base;   // set by __libnx_initheap (main.c)
extern size_t g_mmap_arena_size;
extern int    g_overcommit;        // 1 = alias-region on-demand commit
extern u64    g_alias_base, g_alias_size;

#define BIONIC_MAP_ANONYMOUS 0x20
#define MMAP_PAGE       0x1000u
#define MMAP_BIG_ALIGN  MMAP_ARENA_ALIGN
#define MMAP_BIG_THRESH ((size_t)64 * 1024 * 1024)
#define BIONIC_PROT_NONE 0x0
#define BIONIC_PROT_WRITE 0x2
#define BIONIC_MADV_DONTNEED 4

static uint8_t *mmap_arena;    // 256MB-aligned usable base (published last)
static size_t   mmap_usable;   // usable bytes
static size_t   mmap_pages;    // usable / page
static uint8_t *mmap_used;     // 1 byte/page bitmap: reserved (address space)
static uint8_t *mmap_committed;// 1 byte/page bitmap: physically committed (overcommit only)
static size_t   g_committed_pages;   // running count of committed pages
static size_t   g_commit_peak;       // high-water mark (pages)
static Mutex    g_mmap_lock;   // zero-init == valid unlocked libnx mutex

// --- overcommit commit/decommit (caller holds g_mmap_lock) -------------------
// svcMapPhysicalMemory zero-fills and draws from the freed physical limit; it
// FAILS on already-mapped pages, so we only ever commit pages we track as
// uncommitted, in contiguous runs. Out-of-physical is logged, not fatal.
static void arena_commit_locked(size_t first, size_t cnt) {
  size_t i = 0;
  while (i < cnt) {
    if (mmap_committed[first + i]) { i++; continue; }
    size_t run = 0;
    while (i + run < cnt && !mmap_committed[first + i + run]) run++;
    u64 a = (u64)(uintptr_t)(mmap_arena + (first + i) * MMAP_PAGE);
    Result rc = svcMapPhysicalMemory((void *)a, (u64)run * MMAP_PAGE);
    if (R_SUCCEEDED(rc)) {
      for (size_t k = 0; k < run; k++) mmap_committed[first + i + k] = 1;
      g_committed_pages += run;
      if (g_committed_pages > g_commit_peak) {
        size_t prev = g_commit_peak;
        g_commit_peak = g_committed_pages;
        if ((g_commit_peak >> 16) != (prev >> 16))   // new 256MB high-water mark
          if (TRACE_MMAP) debugPrintf("[mmap] committed peak %u MB (live %u MB)\n",
                      (unsigned)((g_commit_peak * MMAP_PAGE) >> 20),
                      (unsigned)((g_committed_pages * MMAP_PAGE) >> 20));
      }
    } else {
      if (TRACE_MMAP) debugPrintf("[mmap] COMMIT FAIL %u KB @ 0x%lx rc=0x%x (committed %u MB peak %u MB)\n",
                  (unsigned)((run * MMAP_PAGE) >> 10), (unsigned long)a, rc,
                  (unsigned)((g_committed_pages * MMAP_PAGE) >> 20),
                  (unsigned)((g_commit_peak * MMAP_PAGE) >> 20));
    }
    i += run ? run : 1;
  }
}

static void arena_decommit_locked(size_t first, size_t cnt) {
  size_t i = 0;
  while (i < cnt) {
    if (!mmap_committed[first + i]) { i++; continue; }
    size_t run = 0;
    while (i + run < cnt && mmap_committed[first + i + run]) run++;
    u64 a = (u64)(uintptr_t)(mmap_arena + (first + i) * MMAP_PAGE);
    if (R_SUCCEEDED(svcUnmapPhysicalMemory((void *)a, (u64)run * MMAP_PAGE))) {
      for (size_t k = 0; k < run; k++) mmap_committed[first + i + k] = 0;
      g_committed_pages -= run;
    }
    i += run ? run : 1;
  }
}

// translate [addr,addr+len) to a clamped page range within the arena; returns 0 if
// outside the arena (e.g. a newlib-fallback pointer), else 1 with *first/*cnt set.
static int arena_page_range(void *addr, size_t len, size_t *first, size_t *cnt) {
  if (!mmap_arena || (uint8_t *)addr < mmap_arena) return 0;
  size_t off = (uint8_t *)addr - mmap_arena;
  if (off >= mmap_usable) return 0;
  size_t f = off / MMAP_PAGE;
  size_t c = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  if (f + c > mmap_pages) c = mmap_pages - f;
  *first = f; *cnt = c;
  return 1;
}

// commit [addr,len) on demand (mprotect RW / anon mmap). no-op if not overcommit.
static void arena_commit_range(void *addr, size_t len) {
  if (!g_overcommit) return;
  size_t first, cnt;
  mutexLock(&g_mmap_lock);
  if (arena_page_range(addr, len, &first, &cnt)) arena_commit_locked(first, cnt);
  mutexUnlock(&g_mmap_lock);
}

// decommit [addr,len) (mprotect PROT_NONE / munmap). reclaims physical. Safe
// because re-use of a decommitted page goes through mprotect(RW) -> recommit.
static void arena_decommit_range(void *addr, size_t len) {
  if (!g_overcommit) return;
  size_t first, cnt;
  mutexLock(&g_mmap_lock);
  if (arena_page_range(addr, len, &first, &cnt)) arena_decommit_locked(first, cnt);
  mutexUnlock(&g_mmap_lock);
}

// madvise(MADV_DONTNEED): zero the committed pages but KEEP them committed. The
// Switch has no fault handler, so decommitting here would crash if the engine
// re-touches without an intervening mprotect(RW) (allowed on Linux). Zeroing
// preserves the "reads back as zero after DONTNEED" contract safely.
static void arena_dontneed_range(void *addr, size_t len) {
  if (!g_overcommit) return;
  size_t first, cnt;
  mutexLock(&g_mmap_lock);
  if (arena_page_range(addr, len, &first, &cnt)) {
    for (size_t i = 0; i < cnt; ) {
      if (!mmap_committed[first + i]) { i++; continue; }
      size_t run = 0;
      while (i + run < cnt && mmap_committed[first + i + run]) run++;
      memset(mmap_arena + (first + i) * MMAP_PAGE, 0, run * MMAP_PAGE);
      i += run;
    }
  }
  mutexUnlock(&g_mmap_lock);
}

// ===========================================================================
// Stack-region overcommit (OC) arena.
// Boot probe established: svcMapMemory can alias heap pages into the STACK
// region (the alias region is rejected, kernel err 0xdc01), and Unity reserves
// ~2.8GB of PROT_NONE blocks while committing only ~80MB via mprotect(RW) with
// ZERO decommits. So we satisfy the big PROT_NONE reservations from a cheap
// stack-region address window and alias a small bump-allocated heap commit-pool
// in on mprotect(RW). Tried BEFORE the heap-backed arena for big anon PROT_NONE
// maps; anything else (and overflow when the window fills) falls through to the
// heap-backed arena, so if OC setup fails the engine runs exactly as before.
// Because decommits are never observed, the pool is a no-reclaim bump allocator.
// ===========================================================================
static uint8_t *oc_base;        // stack-region window base (256MB-aligned)
static size_t   oc_pages;       // window size in pages (0 => OC disabled)
static uint8_t *oc_used;        // 1/page: address space reserved by an mmap
static uint8_t *oc_committed;   // 1/page: physically backed via svcMapMemory
static uint8_t *oc_pool;        // commit-pool base (heap, page-aligned)
static size_t   oc_pool_pages;  // pool capacity in pages
static size_t   oc_pool_bump;   // next free pool page (bump; no reclaim)
static size_t   oc_live_pages;  // committed pages (diagnostic)

// Called once from main() after the newlib heap exists. window = a reserved
// stack-region range; pool = a heap buffer. Returns 1 if OC is armed.
int oc_arena_init(void *window, size_t window_bytes, void *pool, size_t pool_bytes) {
  if (!window || !pool || !window_bytes || !pool_bytes) return 0;
  size_t wp = window_bytes / MMAP_PAGE;
  uint8_t *u = (uint8_t *)calloc(wp, 1);
  uint8_t *c = (uint8_t *)calloc(wp, 1);
  if (!u || !c) { free(u); free(c); return 0; }
  mutexLock(&g_mmap_lock);
  oc_base = (uint8_t *)window; oc_pages = wp; oc_used = u; oc_committed = c;
  oc_pool = (uint8_t *)pool; oc_pool_pages = pool_bytes / MMAP_PAGE;
  oc_pool_bump = 0; oc_live_pages = 0;
  mutexUnlock(&g_mmap_lock);
  return 1;
}

static int oc_contains(void *addr) {
  return oc_pages && (uint8_t *)addr >= oc_base &&
         (uint8_t *)addr < oc_base + oc_pages * MMAP_PAGE;
}

// A thread stack (e.g. an audio worker's) can get mapped by libnx INSIDE the OC
// window after arm time -- the boot hole-scan can't see stacks created later, and
// the virtmem reservation on the window doesn't reliably deflect them. Reserving
// such a range and later svcMapMemory'ing over it fails (0xd401 InvalidCurrentMemory)
// and leaves Unity's allocation unbacked -> the engine faults. So before handing out
// a candidate, confirm every page is still Unmapped; if a mapped span is found, mark
// its OC pages used so the scan permanently routes around it. caller holds g_mmap_lock.
static int oc_range_occupied(size_t i, size_t need) {
  uint64_t base = (uint64_t)(uintptr_t)oc_base;
  uint64_t a   = base + (uint64_t)i * MMAP_PAGE;
  uint64_t end = a + (uint64_t)need * MMAP_PAGE;
  int occ = 0;
  while (a < end) {
    /* Pages WE committed (e.g. the pre-committed low zone that immunises the
     * window against kernel TLS placement) are ours -- available to hand out,
     * not intruders. Skip them without a syscall. */
    size_t pg = (size_t)((a - base) / MMAP_PAGE);
    if (pg < oc_pages && oc_committed[pg]) { a += MMAP_PAGE; continue; }
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) { occ = 1; break; }
    uint64_t span_end = mi.addr + mi.size;
    if (span_end <= a) { occ = 1; break; }
    if (mi.type != MemType_Unmapped) {
      occ = 1;
      uint64_t s = mi.addr > (uint64_t)(uintptr_t)oc_base ? mi.addr
                                                          : (uint64_t)(uintptr_t)oc_base;
      size_t p0 = (size_t)((s - (uint64_t)(uintptr_t)oc_base) / MMAP_PAGE);
      size_t p1 = (size_t)((span_end - (uint64_t)(uintptr_t)oc_base + MMAP_PAGE - 1) / MMAP_PAGE);
      for (size_t k = p0; k < p1 && k < oc_pages; k++)
        if (!oc_committed[k]) oc_used[k] = 1;   /* mark only true intruder pages */
      debugPrintf("[oc] window range 0x%llx has mapped span 0x%llx..0x%llx (type=0x%x) -> routing around\n",
                  (unsigned long long)s, (unsigned long long)mi.addr,
                  (unsigned long long)span_end, mi.type);
    }
    a = span_end;
  }
  return occ;
}

// Reserve address space in the OC window. Mirrors mmap_arena_alloc_locked's
// 256MB-aligned tail-overflow so Unity's 511MB over-map nets one 256MB slot.
// caller holds g_mmap_lock.
static void *oc_alloc_locked(size_t len, size_t *got) {
  *got = 0;
  if (!oc_pages) return NULL;
  size_t need = (len + MMAP_PAGE - 1) / MMAP_PAGE; if (!need) need = 1;
  const size_t step = MMAP_BIG_ALIGN / MMAP_PAGE;
  size_t kept = need > step ? need - step : need;
  for (size_t i = 0; i + need <= oc_pages; i += step) {        // pass 1: full over-map fits
    size_t run = 0; while (run < need && !oc_used[i + run]) run++;
    if (run == need) {
      if (oc_range_occupied(i, need)) continue;   // a thread stack landed here -> skip
      for (size_t k = 0; k < need; k++) oc_used[i + k] = 1;
      *got = need * MMAP_PAGE; return oc_base + i * MMAP_PAGE;
    }
  }
  for (size_t i = 0; i < oc_pages; i += step) {                // pass 2: tail slot
    if (i + need <= oc_pages) continue;
    size_t avail = oc_pages - i; if (avail < kept) continue;
    size_t run = 0; while (run < avail && !oc_used[i + run]) run++;
    if (run == avail) {
      if (oc_range_occupied(i, avail)) continue;  // a thread stack landed here -> skip
      for (size_t k = 0; k < avail; k++) oc_used[i + k] = 1;
      *got = avail * MMAP_PAGE; return oc_base + i * MMAP_PAGE;
    }
  }
  return NULL;
}

// Commit [addr,len): alias contiguous bump-pool runs into the reserved OC range
// via svcMapMemory (which remaps the pool source away -- we only access via the
// OC address). Already-committed pages are skipped. caller holds g_mmap_lock.
static void oc_commit_locked(void *addr, size_t len) {
  if ((uint8_t *)addr < oc_base) return;
  size_t first = ((uint8_t *)addr - oc_base) / MMAP_PAGE;
  size_t cnt   = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  if (first >= oc_pages) return;
  if (first + cnt > oc_pages) cnt = oc_pages - first;
  size_t i = 0;
  while (i < cnt) {
    if (oc_committed[first + i]) { i++; continue; }
    /* A kernel ThreadLocal region or a thread stack can land on this window page
     * after arm time (the userspace virtmem reservation does not deflect kernel
     * TLS). svcMapMemory over such a page fails 0xd401, and -- critically -- a
     * BATCHED map of a whole run fails if ANY page in it is occupied, leaving all
     * those pages unmapped so Unity faults. So probe per page and build a run of
     * only confirmed-Unmapped pages; skip an intruder page rather than dragging the
     * whole run down with it. */
    void *dst0 = oc_base + (first + i) * MMAP_PAGE;
    MemoryInfo mi; u32 pi;
    if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)(uintptr_t)dst0)) &&
        mi.type != MemType_Unmapped) {
      debugPrintf("[oc] commit: page %p occupied (type=0x%x) -> skip intruder\n",
                  dst0, mi.type);
      i++;                         /* leave it; it is mapped, so no immediate fault */
      continue;
    }
    size_t run = 1;
    while (i + run < cnt && !oc_committed[first + i + run]) {
      void *dN = oc_base + (first + i + run) * MMAP_PAGE;
      if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)(uintptr_t)dN)) &&
          mi.type != MemType_Unmapped) break;   /* stop the run at the next intruder */
      run++;
    }
    if (oc_pool_bump + run > oc_pool_pages) {
      debugPrintf("[oc] commit-pool EXHAUSTED: need %zu pages, %zu left (live %zu MB)\n",
                  run, oc_pool_pages - oc_pool_bump, (oc_live_pages * MMAP_PAGE) >> 20);
      return;   /* can't back; touching it would fault (shouldn't happen at pool size) */
    }
    void *dst = dst0;
    void *src = oc_pool + oc_pool_bump * MMAP_PAGE;
    Result rc = svcMapMemory(dst, src, (u64)run * MMAP_PAGE);
    if (R_FAILED(rc)) {
      /* A page in the run got occupied between probe and map. Retry a single page;
       * if even that lost the race, skip it (mapped by the intruder -> no fault). */
      debugPrintf("[oc] svcMapMemory FAIL dst=%p run=%zu rc=0x%x -> single-page retry\n",
                  dst, run, rc);
      run = 1;
      rc = svcMapMemory(dst, src, (u64)MMAP_PAGE);
      if (R_FAILED(rc)) { i++; continue; }
    }
    memset(dst, 0, run * MMAP_PAGE);   // freshly committed anon must read as zero
    for (size_t k = 0; k < run; k++) oc_committed[first + i + k] = 1;
    oc_pool_bump += run; oc_live_pages += run;
    if (((oc_live_pages * MMAP_PAGE) >> 24) != (((oc_live_pages - run) * MMAP_PAGE) >> 24))
      if (TRACE_MMAP) debugPrintf("[oc] committed %zu MB (pool %zu/%zu MB)\n",
                  (oc_live_pages * MMAP_PAGE) >> 20,
                  (oc_pool_bump * MMAP_PAGE) >> 20, (oc_pool_pages * MMAP_PAGE) >> 20);
    i += run;
  }
}

// munmap of an OC range: reclaim only UNCOMMITTED pages (the tail-overflow slack
// Unity trims after each over-map). Committed pages stay reserved+mapped (a small
// bounded leak) so a later reservation can't collide with a live alias.
static void oc_free_locked(void *addr, size_t len) {
  if ((uint8_t *)addr < oc_base) return;
  size_t first = ((uint8_t *)addr - oc_base) / MMAP_PAGE;
  size_t cnt   = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  if (first >= oc_pages) return;
  if (first + cnt > oc_pages) cnt = oc_pages - first;
  for (size_t i = 0; i < cnt; i++)
    if (!oc_committed[first + i]) oc_used[first + i] = 0;
}

// Pre-map the low `bytes` of the OC window at arm time. Kernel thread-local (TLS)
// pages -- MemType_ThreadLocal, type 0xc -- get placed in the largest free stack-
// region hole, which is exactly our window; the userspace virtmem reservation does
// not deflect them. When one lands on a page Unity later commits, oc_commit can
// only skip it (leaving Unity to fault -- the intermittent frame-3 crash). By
// mapping the low zone up front those addresses become kernel-occupied, so the
// kernel routes TLS elsewhere. We cap the pre-commit so a lazy-commit reserve of
// the pool remains for anything Unity commits higher in the window. The pages are
// marked committed (not used), so oc_alloc still hands them out normally and
// oc_range_occupied (which now skips oc_committed pages) treats them as ours.
void oc_precommit(size_t bytes);
void oc_precommit(size_t bytes) {
  mutexLock(&g_mmap_lock);
  if (!oc_pages) { mutexUnlock(&g_mmap_lock); return; }
  size_t win_bytes  = oc_pages * MMAP_PAGE;
  size_t pool_bytes = oc_pool_pages * MMAP_PAGE;
  size_t reserve    = (size_t)384 * 1024 * 1024;      // keep this much pool for lazy commit
  size_t cap        = (pool_bytes > reserve) ? (pool_bytes - reserve) : 0;
  if (bytes > cap)       bytes = cap;
  if (bytes > win_bytes) bytes = win_bytes;
  size_t before = oc_pool_bump;
  if (bytes) oc_commit_locked(oc_base, bytes);        // maps the low zone (skips any intruder already there)
  size_t mapped = (oc_pool_bump - before) * MMAP_PAGE;
  debugPrintf("[oc] pre-committed %zu MB window low zone (TLS-immune); pool %zu/%zu MB used\n",
              (unsigned)(mapped >> 20), (unsigned)((oc_pool_bump * MMAP_PAGE) >> 20),
              (unsigned)(pool_bytes >> 20));
  mutexUnlock(&g_mmap_lock);
}

// caller holds g_mmap_lock
static void mmap_arena_init_locked(void) {
  if (mmap_arena) return;
  uint8_t *base; size_t usable;
  if (g_mmap_arena_base) {
    base   = (uint8_t *)g_mmap_arena_base;   // dedicated, already 256MB-aligned
    usable = g_mmap_arena_size;
  } else {
    // fallback (small heap / applet): memalign a modest arena (< 2GB newlib limit)
    const size_t want = (size_t)768 * 1024 * 1024 + MMAP_BIG_ALIGN;
    uint8_t *raw = memalign(MMAP_PAGE, want);
    if (!raw) fatal_error("mmap arena alloc (%u MB) failed", (unsigned)(want >> 20));
    base   = (uint8_t *)(((uintptr_t)raw + (MMAP_BIG_ALIGN - 1)) & ~(MMAP_BIG_ALIGN - 1));
    usable = (size_t)768 * 1024 * 1024;
  }
  size_t pages  = usable / MMAP_PAGE;
  uint8_t *used = (uint8_t *)calloc(pages, 1);
  if (!used) fatal_error("mmap bitmap alloc failed");
  if (g_overcommit) {
    mmap_committed = (uint8_t *)calloc(pages, 1);
    if (!mmap_committed) fatal_error("mmap commit-bitmap alloc failed");
  }
  mmap_usable = usable; mmap_pages = pages; mmap_used = used;
  mmap_arena  = base;   // publish last (alloc/free key off this)
  if (TRACE_MMAP) debugPrintf("[mmap] arena: %u MB %s at %p\n", (unsigned)(usable >> 20),
              g_overcommit ? "virtual (alias, on-demand commit)" : "256MB-aligned heap-backed",
              base);
}

// caller holds g_mmap_lock.
// Returns the mapped base and writes the number of bytes ACTUALLY reserved
// (in-arena) to *got. For big alignment over-maps (Unity reserves block+align,
// then munmaps the unaligned head/tail), the request is much larger than the
// ~256MB block Unity actually keeps. Normally we reserve the whole over-map and
// let the tail-munmap give it back. But for the LAST 256MB slot the full over-map
// runs past the arena end, so a plain "need contiguous pages" search fails even
// though the kept block fits. In that case we reserve only [slot, arena_end) -- the
// kept block lives there; Unity's tail-munmap targets addresses beyond our arena
// and is a harmless no-op. This removes the transient peak so each block costs
// exactly its 256MB slot (floor(arena/256MB) blocks fit, no 2x headroom needed).
static void *mmap_arena_alloc_locked(size_t len, size_t *got) {
  size_t need = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  if (!need) need = 1;
  if (len >= MMAP_BIG_THRESH) {
    const size_t step = MMAP_BIG_ALIGN / MMAP_PAGE;   // 256MB in pages
    size_t kept = need > step ? need - step : need;   // pages Unity actually keeps
    // pass 1: full over-map fits within the arena (normal case for all but the last slot)
    for (size_t i = 0; i + need <= mmap_pages; i += step) {
      size_t run = 0;
      while (run < need && !mmap_used[i + run]) run++;
      if (run == need) {
        for (size_t k = 0; k < need; k++) mmap_used[i + k] = 1;
        *got = need * MMAP_PAGE;
        return mmap_arena + i * MMAP_PAGE;
      }
    }
    // pass 2: tail slot -- the over-map would spill past the arena end, but the kept
    // block fits in [slot, arena_end). Reserve only that; the spill is trimmed away.
    for (size_t i = 0; i < mmap_pages; i += step) {
      if (i + need <= mmap_pages) continue;          // handled by pass 1
      size_t avail = mmap_pages - i;
      if (avail < kept) continue;                    // kept block wouldn't fit
      size_t run = 0;
      while (run < avail && !mmap_used[i + run]) run++;
      if (run == avail) {
        for (size_t k = 0; k < avail; k++) mmap_used[i + k] = 1;
        *got = avail * MMAP_PAGE;                     // only the in-arena portion
        return mmap_arena + i * MMAP_PAGE;
      }
    }
  } else {
    for (size_t i = 0; i + need <= mmap_pages; ) {
      size_t run = 0;
      while (run < need && !mmap_used[i + run]) run++;
      if (run == need) {
        for (size_t k = 0; k < need; k++) mmap_used[i + k] = 1;
        *got = need * MMAP_PAGE;
        return mmap_arena + i * MMAP_PAGE;
      }
      i += run + 1;
    }
  }
  *got = 0;
  return NULL;
}

static void mmap_arena_free(void *addr, size_t len) {
  if (!mmap_arena || (uint8_t *)addr < mmap_arena) return;
  size_t off = (uint8_t *)addr - mmap_arena;
  if (off >= mmap_usable) return;
  size_t first = off / MMAP_PAGE;
  size_t cnt   = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  mutexLock(&g_mmap_lock);
  for (size_t k = 0; k < cnt && first + k < mmap_pages; k++)
    mmap_used[first + k] = 0;
  mutexUnlock(&g_mmap_lock);
}

// Stopgap: when the 256MB-block arena is exhausted by Unity's 9 pools, small
// (sub-threshold) il2cpp/GC mmaps still need to land somewhere. Serve them from
// newlib's free heap via memalign and track each so munmap can free it. This is
// not real overcommit (it consumes physical newlib heap), but it unblocks the
// sub-1MB il2cpp allocations that were failing and surfaces il2cpp's true mmap
// appetite in the log to size the proper fix.
#define MMAP_FALLBACK_MAX 4096
static struct { void *ptr; size_t len; } g_fb[MMAP_FALLBACK_MAX];
static int   g_fb_n = 0;
static size_t g_fb_bytes = 0;
static Mutex g_fb_lock;

static void *mmap_fallback(size_t length, int flags, int fd, long offset) {
  /* Big anonymous reservations are Unity Dynamic-Heap pools: its allocator masks
   * pointers down to a large-aligned pool base to derive block indices, so a
   * 4KB-aligned block makes that math walk off into unmapped memory (the
   * libunity+0xdce75c Data Abort). Hand big anon maps a MMAP_BIG_ALIGN-aligned
   * base so the index math lands correctly even out here in the newlib overflow. */
  size_t align = (length >= MMAP_BIG_THRESH && (flags & BIONIC_MAP_ANONYMOUS))
                   ? MMAP_BIG_ALIGN : MMAP_PAGE;
  void *q = memalign(align, length);
  if (!q) return NULL;
  long got = 0;
  if (flags & BIONIC_MAP_ANONYMOUS) {
    memset(q, 0, length);
  } else {
    if (fd >= 0) {
      long cur = lseek(fd, 0, SEEK_CUR);
      if (lseek(fd, offset, SEEK_SET) >= 0)
        while ((size_t)got < length) { long r = read(fd, (char *)q + got, length - got); if (r <= 0) break; got += r; }
      if (cur >= 0) lseek(fd, cur, SEEK_SET);
    }
    if ((size_t)got < length) memset((char *)q + got, 0, length - got);
  }
  mutexLock(&g_fb_lock);
  if (g_fb_n < MMAP_FALLBACK_MAX) { g_fb[g_fb_n].ptr = q; g_fb[g_fb_n].len = length; g_fb_n++; g_fb_bytes += length; }
  const size_t total = g_fb_bytes;
  mutexUnlock(&g_fb_lock);
  if (TRACE_MMAP) debugPrintf("[mmap] fallback %u KB -> %p  anon=%d fd=%d off=0x%lx got=%ld (total %u MB)\n",
              (unsigned)(length >> 10), q, !!(flags & BIONIC_MAP_ANONYMOUS), fd, offset, got,
              (unsigned)(total >> 20));
  return q;
}

// returns 1 and frees if addr was a fallback allocation
static int mmap_fallback_free(void *addr) {
  mutexLock(&g_fb_lock);
  for (int i = 0; i < g_fb_n; i++) {
    if (g_fb[i].ptr == addr) {
      free(addr);
      g_fb_bytes -= g_fb[i].len;
      g_fb[i] = g_fb[--g_fb_n];
      mutexUnlock(&g_fb_lock);
      return 1;
    }
  }
  mutexUnlock(&g_fb_lock);
  return 0;
}

// ---- read-only file-map dedup cache ---------------------------------------
// il2cpp builds a stack trace for every thrown managed exception (even caught
// ones -- GPG login retries, etc.), and the symbolizer mmaps libil2cpp.so
// (~40MB) + libunity.so (~25MB) read-only EACH TIME, never munmapping. Over a
// play session that is dozens of fresh copies (~1.4GB) that exhaust newlib ->
// the "arena FULL / out of RAM" self-exit. Dedup: hand back one shared, pinned
// buffer per (file inode, offset, length). Safe -- these maps are read-only.
#define MAPC_N 24
static struct { uint64_t ino; long off; size_t len; void *ptr; } g_mapc[MAPC_N];
static int g_mapc_n = 0;
static void *mapcache_get(uint64_t ino, long off, size_t len) {
  void *r = NULL;
  mutexLock(&g_fb_lock);
  for (int i = 0; i < g_mapc_n; i++)
    if (g_mapc[i].ino == ino && g_mapc[i].off == off && g_mapc[i].len == len) { r = g_mapc[i].ptr; break; }
  mutexUnlock(&g_fb_lock);
  return r;
}
static void mapcache_put(uint64_t ino, long off, size_t len, void *ptr) {
  mutexLock(&g_fb_lock);
  for (int i = 0; i < g_fb_n; i++)          // pin: drop from fallback free-list
    if (g_fb[i].ptr == ptr) { g_fb_bytes -= g_fb[i].len; g_fb[i] = g_fb[--g_fb_n]; break; }
  if (g_mapc_n < MAPC_N) { g_mapc[g_mapc_n].ino = ino; g_mapc[g_mapc_n].off = off;
                           g_mapc[g_mapc_n].len = len; g_mapc[g_mapc_n].ptr = ptr; g_mapc_n++; }
  mutexUnlock(&g_fb_lock);
}

void *mmap_fake(void *addr, size_t length, int prot, int flags, int fd, long offset) {
  (void)addr;
  if (length == 0) length = 1;

  // Big anonymous PROT_NONE reservations -> stack-region OC arena: cheap address
  // space now, physical aliased in on the later mprotect(RW). On a full window we
  // fall through to the heap-backed arena below (no behaviour change there).
  if (oc_pages && length >= MMAP_BIG_THRESH &&
      (flags & BIONIC_MAP_ANONYMOUS) && prot == BIONIC_PROT_NONE) {
    size_t ocres = 0;
    mutexLock(&g_mmap_lock);
    void *op = oc_alloc_locked(length, &ocres);
    mutexUnlock(&g_mmap_lock);
    if (op) {
      if (TRACE_MMAP) debugPrintf("[mmap] %u MB (prot=0x0 anon=1) -> %p  [OC reserve %u MB]\n",
                  (unsigned)(length >> 20), op, (unsigned)(ocres >> 20));
      return op;   // reserved-only; committed lazily via mprotect_fake
    }
    if (TRACE_MMAP) debugPrintf("[mmap] OC window full for %u MB -> heap-backed arena\n",
                (unsigned)(length >> 20));
  }

  size_t reserved = 0;
  mutexLock(&g_mmap_lock);
  mmap_arena_init_locked();
  void *p = mmap_arena_alloc_locked(length, &reserved);
  mutexUnlock(&g_mmap_lock);
  if (length >= MMAP_BIG_THRESH)
    if (TRACE_MMAP) debugPrintf("[mmap] %u MB (prot=0x%x anon=%d) -> %p  [reserved %u MB]\n",
                (unsigned)(length >> 20), prot, !!(flags & BIONIC_MAP_ANONYMOUS), p,
                (unsigned)(reserved >> 20));
  /* A file-backed map must be contiguous and fully readable. When the arena can
   * only give a tail-overflow reservation (reserved < length) we'd read just
   * `fill` bytes and leave the tail unfilled -- silently truncating the file in
   * RAM. For global-metadata.dat that nulls out System.Object (Class::Init NULL).
   * Hand any short-reserved file map to newlib, which backs the whole length. */
  if (p && !(flags & BIONIC_MAP_ANONYMOUS) && fd >= 0 && reserved < length) {
    mutexLock(&g_mmap_lock);
    mmap_arena_free(p, length);
    mutexUnlock(&g_mmap_lock);
    if (TRACE_MMAP) debugPrintf("[mmap] file fd=%d len=%zu: arena tail-overflow (reserved=%zu) -> newlib\n",
                fd, length, reserved);
    p = NULL;
  }
  if (!p) {
    // Arena exhausted (Unity's pools fill it). Route the request to newlib's free
    // heap regardless of size -- il2cpp's resource-extraction maps can exceed 64MB,
    // and rejecting them is what NULL-derefs the engine. Only a genuinely huge map
    // (> newlib free) will fail, and we log that distinctly.
    // Read-only file maps get deduped (backtrace .so symbolication leak, see above).
    int ro_file = fd >= 0 && !(flags & BIONIC_MAP_ANONYMOUS) && !(prot & BIONIC_PROT_WRITE);
    uint64_t mino = (ro_file && fd < FD_INO_MAX) ? g_fd_ino[fd] : 0;
    if (mino) { void *hit = mapcache_get(mino, offset, length); if (hit) return hit; }
    void *q = mmap_fallback(length, flags, fd, offset);
    if (q) { if (mino) mapcache_put(mino, offset, length, q); return q; }
    if (TRACE_MMAP) debugPrintf("[mmap] arena FULL and newlib fallback FAILED for %u MB (out of RAM)\n",
                (unsigned)(length >> 20));
    errno = ENOMEM; return (void *)-1;
  }

  // Never touch beyond what we actually reserved in-arena (tail over-maps reserve
  // less than the requested length; the spill lives past the arena and is trimmed).
  size_t fill = length < reserved ? length : reserved;

  if (g_overcommit) {
    // PROT_NONE reservation: address space only, no physical -- the whole point.
    // The engine commits the sub-ranges it uses later via mprotect(RW).
    if (prot == BIONIC_PROT_NONE) return p;
    // Otherwise commit now (anon RW, file maps): svcMapPhysicalMemory zero-fills,
    // so anon needs no memset; file maps read their contents over the zeros.
    arena_commit_range(p, fill);
    if (!(flags & BIONIC_MAP_ANONYMOUS) && fd >= 0) {
      long got = 0, cur = lseek(fd, 0, SEEK_CUR);
      if (lseek(fd, offset, SEEK_SET) >= 0)
        while ((size_t)got < fill) { long r = read(fd, (char *)p + got, fill - got); if (r <= 0) break; got += r; }
      if (cur >= 0) lseek(fd, cur, SEEK_SET);
    }
    return p;
  }

  if (flags & BIONIC_MAP_ANONYMOUS) {
    memset(p, 0, fill);   // anonymous memory must read back as zero
  } else {
    // File-backed mapping: pull [offset, offset+fill) into RAM (no real mmap).
    long got = 0;
    if (fd >= 0) {
      long cur = lseek(fd, 0, SEEK_CUR);
      if (lseek(fd, offset, SEEK_SET) >= 0) {
        while ((size_t)got < fill) {
          long r = read(fd, (char *)p + got, fill - (size_t)got);
          if (r <= 0) break;
          got += r;
        }
      }
      if (cur >= 0) lseek(fd, cur, SEEK_SET);
    }
    if ((size_t)got < fill) memset((char *)p + got, 0, fill - (size_t)got);
    if (fd >= 0)
      if (TRACE_MMAP) debugPrintf("[mmap] file map fd=%d len=%zu reserved=%zu fill=%zu got=%ld%s\n",
                  fd, length, reserved, fill, got,
                  ((size_t)got < length) ? "  *** TRUNCATED ***" : "");
  }
  return p;
}

int munmap_fake(void *addr, size_t length) {
  if (mmap_fallback_free(addr)) return 0;   // newlib fallback allocation
  if (oc_contains(addr)) {                   // stack-region OC reservation
    mutexLock(&g_mmap_lock);
    oc_free_locked(addr, length);
    mutexUnlock(&g_mmap_lock);
    return 0;
  }
  arena_decommit_range(addr, length);       // reclaim physical (overcommit only)
  mmap_arena_free(addr, length);            // unreserve address space
  return 0;
}

// In overcommit mode mprotect drives commit/decommit: RW/R commits physical at
// the alias address, PROT_NONE decommits it (safe -- reuse re-mprotects to RW).
// In heap-backed mode the arena is always RW so this is a no-op.
int mprotect_fake(void *addr, size_t len, int prot) {
  /* Diagnostic (fires even heap-backed): measure Unity's commit pattern so we can
   * confirm it commits PROT_NONE reservations via mprotect(RW) and size the
   * overcommit commit-pool. Tracks cumulative RW-commit vs PROT_NONE-decommit
   * bytes that fall inside the mmap arena (= Unity's live committed footprint). */
  {
    static size_t rw_b = 0, none_b = 0;
    static unsigned rw_n = 0, none_n = 0, oth_n = 0;
    int in_arena = g_mmap_arena_base &&
                   (uint8_t *)addr >= (uint8_t *)g_mmap_arena_base &&
                   (uint8_t *)addr <  (uint8_t *)g_mmap_arena_base + g_mmap_arena_size;
    if (prot == BIONIC_PROT_NONE)       { none_n++; if (in_arena) none_b += len; }
    else if (prot & BIONIC_PROT_WRITE)  { rw_n++;   if (in_arena) rw_b   += len; }
    else                                  oth_n++;
    if (len >= 4u * 1024 * 1024 || ((rw_n + none_n) & 0x7F) == 0)
      if (TRACE_MMAP) debugPrintf("[mprot] addr=%p len=%zuKB prot=0x%x arena=%d | RW %u/%zuMB NONE %u/%zuMB oth %u  net=%zdMB\n",
                  addr, len >> 10, prot, in_arena, rw_n, rw_b >> 20, none_n, none_b >> 20, oth_n,
                  (ssize_t)(rw_b - none_b) >> 20);
  }
  if (oc_contains(addr)) {
    // OC reservation being committed/decommitted. Unity only ever commits (RW);
    // decommits aren't observed, so PROT_NONE here is left mapped (cheap + safe).
    if (prot != BIONIC_PROT_NONE) {
      mutexLock(&g_mmap_lock);
      oc_commit_locked(addr, len);
      mutexUnlock(&g_mmap_lock);
    }
    return 0;
  }
  if (!g_overcommit) return 0;
  if (prot == BIONIC_PROT_NONE) arena_decommit_range(addr, len);
  else                          arena_commit_range(addr, len);
  return 0;
}
// madvise(MADV_DONTNEED): overcommit zeroes-but-keeps (see arena_dontneed_range);
// heap-backed leaves pages as-is (always RW-backed).
int madvise_fake(void *addr, size_t len, int advice) {
  if (g_overcommit && advice == BIONIC_MADV_DONTNEED) arena_dontneed_range(addr, len);
  return 0;
}

// ---------------------------------------------------------------------------
// filesystem odds and ends
// ---------------------------------------------------------------------------

char *realpath_fake(const char *path, char *resolved) {
  if (!path) return NULL;          /* POSIX: realpath(NULL,..) is an error, not a crash */
  if (!resolved) resolved = malloc(0x1000);
  strcpy(resolved, path);
  return resolved;
}
int strerror_r_fake(int err, char *buf, size_t len) { snprintf(buf, len, "%s", strerror(err)); return 0; }
int statvfs_fake(const char *path, void *buf) { (void)path; memset(buf, 0, 0x70); return 0; }
int statfs_fake(const char *path, void *buf) { (void)path; memset(buf, 0, 0x78); return 0; }

// Synthetic /proc and /sys files. Unity reads /proc/meminfo (MemTotal) to size
// its allocator reservations and /proc/cpuinfo + /sys cpu range to count cores
// for the job system. We report ~1 GB (NOT the real ~3 GB) so the engine's big
// 256MB-block dynamic-heap reservations stay within our mmap arena -- the arena
// is the real backing and has headroom, but Unity must not try to reserve 3 GB
// of address space up front. 3 cores (homebrew gets 0-2).
static const char *synthetic_proc(const char *path) {
  if (!path) return NULL;
  if (!strcmp(path, "/proc/meminfo"))
    return "MemTotal:        524288 kB\n"
           "MemFree:         393216 kB\n"
           "MemAvailable:    393216 kB\n"
           "Buffers:              0 kB\n"
           "Cached:               0 kB\n"
           "SwapTotal:            0 kB\n"
           "SwapFree:             0 kB\n";
  if (!strcmp(path, "/proc/cpuinfo"))
    return "processor\t: 0\nprocessor\t: 1\nprocessor\t: 2\n"
           "Features\t: fp asimd aes pmull sha1 sha2 crc32\n"
           "CPU implementer\t: 0x41\nCPU architecture: 8\nCPU variant\t: 0x1\n"
           "CPU part\t: 0xd07\nCPU revision\t: 1\n";
  if (strstr(path, "cpu_capacity")) return "1024\n";
  if (strstr(path, "cpuinfo_max_freq") || strstr(path, "scaling_max_freq")) return "1785000\n";
  if (strstr(path, "cpuinfo_min_freq") || strstr(path, "scaling_min_freq")) return "1020000\n";
  if (strstr(path, "/cpu/possible") || strstr(path, "/cpu/present") || strstr(path, "/cpu/online"))
    return "0-2\n";
  if (!strncmp(path, "/proc/", 6) || !strncmp(path, "/sys/", 5)) return ""; // empty for the rest
  return NULL;
}

// a buffered fopen for the big .mvgl archives: the engine issues many small
// reads/seeks and the fsdev round-trips dominate without a large buffer.
FILE *fopen_fake(const char *path, const char *mode) {
  char _rbuf[512];
  path = asset_redirect(path, _rbuf, sizeof _rbuf);
  const char *synth = synthetic_proc(path);
  if (synth) {
    size_t n = strlen(synth);
    return fmemopen((void *)strdup(synth), n ? n : 1, "r");
  }
  const int writing = strpbrk(mode, "wa+") != NULL;
  FILE *f = fopen(path, mode);
  if (!f && writing) {            // save file: create the subdir and retry
    mkdir_parents(path);
    f = fopen(path, mode);
  }
  if (!f && !writing && strchr(mode, 'r')) {
    char alt[320];
    if (basename_fallback(path, alt, sizeof(alt)))
      f = fopen(alt, mode);
  }
  if (!f)
    return NULL;
  if (writing) mark_write_fd(fileno(f));
  if (TRACE_IO) debugPrintf("[io] fopen(%s,%s) -> %p\n", path, mode, (void *)f);
  if (strchr(mode, 'r')) {
    /* Large-sequential-read buffering.
     *
     * CHANGED FOR ZOMBOTRON: was keyed on ".mvgl", which is a Chaos Rings 3
     * archive extension. Zombotron has no .mvgl files at all, so this branch
     * never fired and every big asset read went unbuffered off the SD card.
     *
     * Zombotron's large sequential reads are:
     *     assets/bin/Data/data.unity3d                 67.8 MB
     *     assets/bin/Data/*.resource                   streamed audio/texture
     *     assets/bin/Data/Managed/Metadata/global-metadata.dat   15.7 MB
     *
     * global-metadata.dat is read once at il2cpp init but it is read hard, and
     * data.unity3d is read throughout the session. */
    const char *ext = strrchr(path, '.');
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if ((ext && (strcasecmp(ext, ".unity3d")  == 0 ||
                 strcasecmp(ext, ".resource") == 0 ||
                 strcasecmp(ext, ".resS")     == 0)) ||
        strcasecmp(base, "global-metadata.dat") == 0)
      /* 1 MB, not 256 KB. data.unity3d is 67 MB and global-metadata.dat 15.7 MB,
       * read sequentially off an SD card during the load frames that measured
       * 1188 ms and 2987 ms on hardware. Larger buffers mean fewer fs IPC round
       * trips, which is the dominant cost on this path. */
      setvbuf(f, NULL, _IOFBF, 1024 * 1024);
  }
  return f;
}

// ---------------------------------------------------------------------------
// stdio over the fake bionic __sF (stdin/stdout/stderr). libc++_shared wires
// std::cout/cerr/cin to &__sF[1]/[2]/[0]; these wrappers absorb writes to those
// fake FILEs and forward everything else to newlib.
// ---------------------------------------------------------------------------

uint8_t fake_sF[3][0x100]; // referenced by imports.c (__sF / std{in,out,err})

static int is_fake_file(const void *f) {
  const uint8_t *p = f;
  const uint8_t *base = (const uint8_t *)fake_sF;
  return p >= base && p < base + sizeof(fake_sF);
}

size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f)) {
#if DEBUG_LOG
    static char buf[0x400];
    const size_t total = size * n < sizeof(buf) - 1 ? size * n : sizeof(buf) - 1;
    memcpy(buf, ptr, total); buf[total] = '\0';
    debugPrintf("stdio: %s", buf);
#endif
    return n;
  }
  return fwrite(ptr, size, n, f);
}
size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f)) return 0;
  return fread(ptr, size, n, f);
}
int fputc_fake(int c, FILE *f) { if (is_fake_file(f)) return c; return fputc(c, f); }
int fputs_fake(const char *s, FILE *f) { if (is_fake_file(f)) { debugPrintf("stdio: %s", s); return 0; } return fputs(s, f); }
int fflush_fake(FILE *f) { if (is_fake_file(f) || f == NULL) return 0; return fflush(f); }
int fclose_fake(FILE *f) { if (is_fake_file(f)) return 0;
  int fd = fileno(f); fd_ino_clear(fd);   /* keep g_fd_ino free of stale entries: fd_close_by_path
                                             (rename_fake) closes fds by that table */
  int r = fclose(f); commit_write_fd(fd); return r; }
int ferror_fake(FILE *f) { if (is_fake_file(f)) return 0; return ferror(f); }
int feof_fake(FILE *f) { if (is_fake_file(f)) return 1; return feof(f); }
int fileno_fake(FILE *f) { if (is_fake_file(f)) return ((const uint8_t *)f - &fake_sF[0][0]) / 0x100; return fileno(f); }
int fseek_fake(FILE *f, long off, int whence) { if (is_fake_file(f)) return -1; return fseek(f, off, whence); }
long ftell_fake(FILE *f) { if (is_fake_file(f)) return -1; return ftell(f); }
int getc_fake(FILE *f) { if (is_fake_file(f)) return -1; return getc(f); }
int fgetc_fake(FILE *f) { if (is_fake_file(f)) return -1; return fgetc(f); }
char *fgets_fake(char *s, int n, FILE *f) { if (is_fake_file(f)) return NULL; return fgets(s, n, f); }
int ungetc_fake(int c, FILE *f) { if (is_fake_file(f)) return -1; return ungetc(c, f); }
void setbuf_fake(FILE *f, char *buf) { if (is_fake_file(f)) return; setbuf(f, buf); }

int fprintf_fake(FILE *f, const char *fmt, ...) {
  va_list va; va_start(va, fmt);
  int ret;
  if (is_fake_file(f)) {
#if DEBUG_LOG
    static char buf[0x400];
    ret = vsnprintf(buf, sizeof(buf), fmt, va);
    debugPrintf("stdio: %s", buf);
#else
    ret = 0;
#endif
  } else {
    ret = vfprintf(f, fmt, va);
  }
  va_end(va);
  return ret;
}
int vfprintf_fake(FILE *f, const char *fmt, va_list va) {
  if (is_fake_file(f)) {
#if DEBUG_LOG
    static char buf[0x400];
    int ret = vsnprintf(buf, sizeof(buf), fmt, va);
    debugPrintf("stdio: %s", buf);
    return ret;
#else
    return 0;
#endif
  }
  return vfprintf(f, fmt, va);
}

// ---------------------------------------------------------------------------
// fd routing: the native_app_glue command pipe lives in the fake-fd layer
// (android_native.c). Real files (small fds from open()) pass through to newlib.
// ---------------------------------------------------------------------------

long read_fake(int fd, void *buf, size_t count) {
  if (fakefd_is_fake(fd)) return fakefd_read(fd, buf, count);
#if ASSET_PACK_ENABLE
  if (asset_pack_fd_is(fd)) return asset_pack_read_fd(fd, buf, count);
#endif
  { struct RaCache *c = ra_find(fd); if (c) return ra_read(c, fd, buf, count); }
  /* fsdev can return fewer bytes than requested for a large read; il2cpp's
   * global-metadata.dat loader (and others) assume a single read() fills the
   * buffer. Loop until `count` is satisfied or we hit EOF/error so the metadata
   * is never silently truncated (a short read leaves System.Object et al.
   * unresolvable -> Class::Init(NULL)). */
  size_t total = 0;
  while (total < count) {
    long r = read(fd, (char *)buf + total, count - total);
    if (r < 0) { if (total) break; return -1; }
    if (r == 0) break; /* EOF */
    total += (size_t)r;
  }
  if (count >= (1u << 20))
    if (TRACE_IO) debugPrintf("[io] read(fd=%d, %zu) -> %zu%s\n", fd, count, total,
                total < count ? "  *** SHORT READ ***" : "");
  watch_dump("read", fd, (long)count, 0, buf, (long)total);
  return (long)total;
}
long write_fake(int fd, const void *buf, size_t count) {
  if (fakefd_is_fake(fd)) return fakefd_write(fd, buf, count);
  return write(fd, buf, count);
}
int close_fake(int fd) {
  ra_detach(fd);
  fd_ino_clear(fd);
  if (fakefd_is_fake(fd)) return fakefd_close(fd);
#if ASSET_PACK_ENABLE
  if (asset_pack_fd_is(fd)) return asset_pack_close_fd(fd);
#endif
  int r = close(fd);
  commit_write_fd(fd);   /* flush a just-written save to the physical SD */
  return r;
}
int pipe_fake(int fds[2]) { return fakefd_pipe(fds); }
int poll_fake(void *fds, unsigned long nfds, int timeout) { (void)fds; (void)nfds; (void)timeout; return 0; }
int select_fake(int n, void *r, void *w, void *e, void *t) { (void)n; (void)r; (void)w; (void)e; (void)t; return 0; }

// ---------------------------------------------------------------------------
// networking: online play (Mobage / Silicon Studio servers) is dead. Stub the
// socket layer so connections fail and the engine stays in offline mode.
// ---------------------------------------------------------------------------

int socket_fake(int d, int t, int p) { (void)d; (void)t; (void)p; errno = EAFNOSUPPORT; return -1; }
int connect_fake(int s, const void *a, unsigned l) { (void)s; (void)a; (void)l; errno = ECONNREFUSED; return -1; }
int bind_fake(int s, const void *a, unsigned l) { (void)s; (void)a; (void)l; errno = EACCES; return -1; }
int listen_fake(int s, int b) { (void)s; (void)b; return -1; }
int accept_fake(int s, void *a, void *l) { (void)s; (void)a; (void)l; errno = EINVAL; return -1; }
long send_fake(int s, const void *b, size_t l, int f) { (void)s; (void)b; (void)l; (void)f; errno = EPIPE; return -1; }
long recv_fake(int s, void *b, size_t l, int f) { (void)s; (void)b; (void)l; (void)f; return 0; }
long sendto_fake(int s, const void *b, size_t l, int f, const void *a, unsigned al) { (void)s; (void)b; (void)l; (void)f; (void)a; (void)al; errno = EPIPE; return -1; }
long recvfrom_fake(int s, void *b, size_t l, int f, void *a, void *al) { (void)s; (void)b; (void)l; (void)f; (void)a; (void)al; return 0; }
int shutdown_fake(int s, int how) { (void)s; (void)how; return 0; }
int setsockopt_fake(int s, int lv, int n, const void *v, unsigned l) { (void)s; (void)lv; (void)n; (void)v; (void)l; return 0; }
/* socketpair: the game only needs it for optional self-pipe/wakeup plumbing; sockets are
 * stubbed offline like the rest, so fail cleanly (callers treat -1 as "no pipe" and degrade). */
int socketpair_fake(int d, int t, int p, int sv[2]) { (void)d; (void)t; (void)p; (void)sv; errno = EAFNOSUPPORT; return -1; }

/* `environ`: libil2cpp imports the POSIX environment pointer (getenv-style reads). Give it an
 * empty, NULL-terminated vector -- reads see no variables, nothing faults. The resolver binds
 * the game's `environ` data symbol to &fake_environ (address of this char** variable). */
char  *g_env_empty[1] = { NULL };
char **fake_environ   = g_env_empty;
int getsockopt_fake(int s, int lv, int n, void *v, void *l) { (void)s; (void)lv; (void)n; (void)v; (void)l; return -1; }
int getsockname_fake(int s, void *a, void *l) { (void)s; (void)a; (void)l; return -1; }
int getpeername_fake(int s, void *a, void *l) { (void)s; (void)a; (void)l; return -1; }
int getaddrinfo_fake(const char *node, const char *svc, const void *hints, void **res) { (void)node; (void)svc; (void)hints; if (res) *res = NULL; return -2 /* EAI_NONAME */; }
void freeaddrinfo_fake(void *res) { (void)res; }
int getnameinfo_fake(const void *a, unsigned al, char *h, unsigned hl, char *s, unsigned sl, int f) { (void)a; (void)al; (void)f; if (h && hl) h[0] = 0; if (s && sl) s[0] = 0; return -1; }
int gethostname_fake(char *name, size_t len) { if (name && len) snprintf(name, len, "switch"); return 0; }
void *getservbyname_fake(const char *n, const char *p) { (void)n; (void)p; return NULL; }
unsigned if_nametoindex_fake(const char *n) { (void)n; return 0; }
char *if_indextoname_fake(unsigned i, char *buf) { (void)i; if (buf) buf[0] = 0; return buf; }
static volatile int g_h_errno = 0;
int *__get_h_errno_fake(void) { return (int *)&g_h_errno; }

// ---------------------------------------------------------------------------
// process control: fork/exec/etc. are unavailable; report failure.
// ---------------------------------------------------------------------------

int fork_fake(void) { errno = ENOSYS; return -1; }
int execvp_fake(const char *f, char *const argv[]) { (void)f; (void)argv; errno = ENOSYS; return -1; }
int waitpid_fake(int pid, int *status, int opts) { (void)pid; (void)opts; if (status) *status = 0; errno = ECHILD; return -1; }
int kill_fake(int pid, int sig) { (void)pid; (void)sig; return 0; }
int getpid_fake(void) { return 1; }
int sched_yield_fake(void) { svcSleepThread(0); return 0; }
// bionic struct passwd layout (pw_dir at +0x20, as the engine derefs).
struct bionic_passwd {
  char *pw_name;     /* 0x00 */
  char *pw_passwd;   /* 0x08 */
  uint32_t pw_uid;   /* 0x10 */
  uint32_t pw_gid;   /* 0x14 */
  char *pw_gecos;    /* 0x18 */
  char *pw_dir;      /* 0x20 */
  char *pw_shell;    /* 0x28 */
};
void *getpwuid_fake(int uid) {
  (void)uid;
  static struct bionic_passwd pw;
  static char nm[] = "switch", sh[] = "/bin/sh", empty[] = "";
  /* pw_dir was a compile-time array of GAME_HOME; the root is resolved at
   * runtime now, so fill it on first use. */
  static char dir[512];
  if (!dir[0]) snprintf(dir, sizeof dir, "%s", zb_game_root());
  pw.pw_name = nm; pw.pw_passwd = empty; pw.pw_uid = 0; pw.pw_gid = 0;
  pw.pw_gecos = empty; pw.pw_dir = dir; pw.pw_shell = sh;
  return &pw;
}

// Unity computes its home/cache dir via getenv("HOME") (then getpwuid fallback).
// Serve the writable game root for HOME/TMPDIR; delegate everything else to newlib.
const char *managed_path(const char *p) {
  if (!p) return p;
  const char *c = strchr(p, ':');
  return (c && c[1] == '/') ? c + 1 : p;     // "sdmc:/switch/.." -> "/switch/.."
}
char *getenv_fake(const char *name) {
  if (name) {
    /* Unity derives its home/cache dir from these. Must track the runtime root,
     * or it writes its cache next to a folder that does not exist. */
    if (!strcmp(name, "HOME") || !strcmp(name, "TMPDIR"))
      return (char *)managed_path(zb_game_root());
  }
  return getenv(name);
}
// Report a Unix-rooted cwd ("/switch/zookeeper", no "sdmc:") so managed Path
// APIs don't treat it as relative in Path.Combine. newlib's *internal* cwd is
// unchanged, so relative file resolution still works via the default device.
char *getcwd_fake(char *buf, size_t size) {
  char *r = getcwd(buf, size);
  if (!r) return r;
  const char *c = strchr(r, ':');
  if (c && c[1] == '/') memmove(r, c + 1, strlen(c + 1) + 1);  // drop "sdmc:"
  return r;
}
int getrusage_fake(int who, void *usage) { (void)who; if (usage) memset(usage, 0, 144); return 0; }

// ---------------------------------------------------------------------------
// dlopen/dlsym over the already-loaded modules (no real dynamic loading).
// dlsym lets the engine look up its own exports / our shims.
// ---------------------------------------------------------------------------

void *dlopen_fake(const char *name, int flags) {
  (void)flags; debugPrintf("dlopen(%s)\n", name ? name : "(self)");

  /* ZOMBOTRON / Unity 6 GATING.
   *
   * libunity reaches these three through dlopen rather than DT_NEEDED, so
   * refusing them here is how the port selects the code paths it can actually
   * service. This is not an optimisation -- without it the engine takes paths
   * that have no backing on Switch and fails in ways that look like hangs.
   *
   *   libswappywrapper.so  Unity logs "InitSwappyWrapper() failed" and uses its
   *                        non-Swappy present path. Also avoids loading a fourth
   *                        module and binding 60 libc++ symbols, since the port
   *                        drives its own frame loop anyway.
   *   libvulkan.so         forces the GLES backend, which switch-mesa provides.
   *                        Zombotron ships GLES3 shader variants (91 GLSL
   *                        sources in data.unity3d), so this is safe.
   *   libaaudio.so         forces the FMOD/OpenSL output that opensles.c backs,
   *                        rather than an AAudio device that does not exist.
   *
   * Anything else keeps the old behaviour of succeeding with a token handle;
   * dlsym_fake ignores the handle and resolves against loaded modules + shims. */
  if (name) {
    static const char *const refuse[] = {
      "libswappywrapper.so", "libvulkan.so", "libaaudio.so",
    };
    for (size_t i = 0; i < sizeof(refuse) / sizeof(*refuse); i++) {
      if (strstr(name, refuse[i])) {
        debugPrintf("dlopen(%s) -> NULL [zombotron: forced fallback path]\n", name);
        return NULL;
      }
    }
  }

  return (void *)0x1;
}
int dlclose_fake(void *h) { (void)h; return 0; }
const char *dlerror_fake(void) { return NULL; }
void *dlsym_fake(void *handle, const char *symbol) {
  (void)handle;
  if (!symbol) return NULL;
  /* Firebase SWIG stub resolver (firebase_stub.c) -- see step 2b below. */
  extern void *firebase_stub_lookup(const char *symbol);
  /* 1) a real export from a loaded module (il2cpp/unity/main) */
  void *p = so_resolve_external(symbol);
  if (p) return p;
  /* 2) one of our libc/GLES/EGL shims (the engine dlopen()s libGLESv2.so etc.
   *    and dlsym()s glGetString/glGetIntegerv, which are shims, not exports) */
  uintptr_t shim = dynlib_find_export(symbol);
  if (shim) { debugPrintf("dlsym(%s) -> %p [shim]\n", symbol, (void *)shim); return (void *)shim; }
  /* 2b) Firebase SWIG P/Invokes. The real Firebase .so files are intentionally
   *     NOT loaded (they crash our loader at boot and, lacking Play Services,
   *     could never report DependencyStatus.Available on a Switch anyway). We
   *     answer the managed SDK's native lookups with trivial stubs so the
   *     dependency check resolves to Available(0) and the bootstrap advances. */
  void *fb = firebase_stub_lookup(symbol);
  if (fb) return fb;
  /* 3) the full GLES/EGL API (~150 entry points) lives in mesa, beyond our
   *    static table -- resolve any gl or egl symbol via eglGetProcAddress. */
  if (!strncmp(symbol, "gl", 2) || !strncmp(symbol, "egl", 3)) {
    p = (void *)eglGetProcAddress(symbol);
    if (p) { debugPrintf("dlsym(%s) -> %p [egl]\n", symbol, p); return p; }
  }
  debugPrintf("dlsym(%s) -> NULL\n", symbol);
  return NULL;
}

// ---------------------------------------------------------------------------
// pthread extras: rwlocks, semaphores, timed locks
// ---------------------------------------------------------------------------

typedef struct { RwLock lock; } FakeRwLock;

static FakeRwLock *get_rwlock(void **storage) {
  if (!*storage) { FakeRwLock *l = calloc(1, sizeof(*l)); rwlockInit(&l->lock); *storage = l; }
  return *storage;
}
int pthread_rwlock_rdlock_fake(void **rw) { RwLock *l=&get_rwlock(rw)->lock; diag_wait_enter(DIAG_W_RWLOCK,l); rwlockReadLock(l); diag_wait_exit(); return 0; }
int pthread_rwlock_wrlock_fake(void **rw) { RwLock *l=&get_rwlock(rw)->lock; diag_wait_enter(DIAG_W_RWLOCK,l); rwlockWriteLock(l); diag_wait_exit(); return 0; }
int pthread_rwlock_unlock_fake(void **rw) {
  FakeRwLock *l = get_rwlock(rw);
  if (rwlockIsWriteLockHeldByCurrentThread(&l->lock)) rwlockWriteUnlock(&l->lock);
  else rwlockReadUnlock(&l->lock);
  return 0;
}

typedef struct { Semaphore sem; } FakeSem;
int sem_init_fake(void **s, int pshared, unsigned int value) { (void)pshared; FakeSem *fs = calloc(1, sizeof(*fs)); semaphoreInit(&fs->sem, value); *s = fs; return 0; }
int sem_destroy_fake(void **s) { if (s && *s) { free(*s); *s = NULL; } return 0; }
int sem_post_fake(void **s) { if (s && *s) semaphoreSignal(&((FakeSem *)*s)->sem); return 0; }
int sem_wait_fake(void **s) { if (s && *s) { Semaphore *sm=&((FakeSem *)*s)->sem; diag_wait_enter(DIAG_W_SEM,sm); semaphoreWait(sm); diag_wait_exit(); } return 0; }
int sem_trywait_fake(void **s) { if (s && *s && semaphoreTryWait(&((FakeSem *)*s)->sem)) return 0; errno = EAGAIN; return -1; }
int sem_getvalue_fake(void **s, int *val) { if (s && *s) *val = (int)((FakeSem *)*s)->sem.count; else *val = 0; return 0; }
// no native timed wait on libnx Semaphore; poll with a short backoff to the
// deadline. The engine uses it as a yield-with-timeout in its task scheduler.
int sem_timedwait_fake(void **s, const struct timespec *abs) {
  (void)abs;
  for (int i = 0; i < 1000; i++) {
    if (sem_trywait_fake(s) == 0) return 0;
    svcSleepThread(1000000ull); // 1 ms
  }
  errno = ETIMEDOUT;
  return -1;
}

/* --- Boehm GC stop-the-world bridge -------------------------------------
 * il2cpp's Boehm GC stops the world by sending every other thread a suspend
 * signal via pthread_kill; each target's signal handler sem_posts an ack and
 * parks in sigsuspend, and GC_stop_world / GC_start_world sem_wait on those
 * acks. POSIX signals are never delivered on Switch (pthread_kill is a no-op),
 * so the acks never arrive and the first collection hangs forever inside
 * GC_stop_world -- the verified boot wall.
 *
 * sem_post/sem_wait themselves work here (real libnx Semaphore underneath), so
 * we make pthread_kill itself post the ack that the never-delivered handler
 * would have posted. Every thread the GC suspends is already parked in our own
 * shim (idle worker / background waits), so not literally suspending them is
 * fine for the brief mark window. The signal numbers, the start-world ack gate
 * and the ack semaphore are all il2cpp globals (offsets below, next to the
 * #defines). Before GC_stop_init runs they hold bdwgc's sentinels -- the two
 * signal numbers are -1 (SIGNAL_UNSET), which no live signal can equal, and the
 * ack-sem storage is NULL, on which sem_post_fake no-ops -- so the bridge is
 * inert until the GC is actually up. */
uintptr_t g_il2cpp_base = 0;
size_t    g_il2cpp_size = 0;   /* mapped size of libil2cpp; 0 = unknown (guard inert) */

/* Offsets recovered by disassembling THIS build's libil2cpp (Zombotron, Unity
 * 6000.2.6f2). *** RE-DERIVED FOR ZOMBOTRON (this build). Previously Zombotron's
 * values (0x6028fbc/fc0/fb8, 0x6251e18); Zombotron's libil2cpp is ~60 MB so those
 * ack-sem offsets fell PAST the mapping (0x6251e20 > 0x3b97000) and the bounds
 * guard disabled the bridge -> GC stop-the-world deadlocked at frame 0. Earlier
 * history: Bad Piggies'
 * values (0x1dd9a98/a9c/aa0, 0x1de73c0); Zombotron's libil2cpp is 100 MB, so
 * those fell *inside* the mapping, the bounds guard passed, the reads landed on
 * unrelated data, no ack was ever posted and the first collection deadlocked at
 * frame 0. Never copy these between games. ***
 *
 * Derivation (tools/re/gc_globals.py reproduces all of it from the .so alone;
 * tools/re/verify_gc_offsets.py re-checks what this file ships against a .so):
 *
 * libil2cpp has no symbols, so the GC was found by its libc call sites. There
 * are exactly TWO pthread_kill call sites in the whole 100 MB module, and they
 * sit in the same 1 KB of .text as the only sem_wait/sem_init/sigsuspend sites
 * -- that block is bdwgc's pthread_stop_world.c. Each function is then named
 * outright by the string its abort path passes:
 *
 *   0x27c1258 GC_suspend_handler_inner "GC Warning: Duplicate suspend signal..."
 *             adrp x0,#0x6251000 ; add #0xe18 ; bl sem_post   @0x27c135c
 *                                             => GC_suspend_ack_sem  0x6251e18
 *             ldr w8,[x8,#0xfb8] ; cbz -> skip 2nd sem_post   @0x27c1394
 *                                             => restart-ack gate    0x6028fb8
 *   0x27c13cc GC_restart_handler       "Bad signal in restart handler"
 *             ldr w8,[x8,#0xfc0] ; cmp w8,w0  => GC_sig_thr_restart  0x6028fc0
 *   0x27c13fc GC_suspend_all           "pthread_kill failed at suspend"
 *             adrp x24,#0x6028000 @0x27c142c
 *             ldr w1,[x24,#0xfbc] ; bl pthread_kill @0x27c146c
 *                                             => GC_sig_suspend      0x6028fbc
 *   0x27c15d0 suspend_restart_barrier  "sem_wait failed"
 *             adrp x20,#0x6251000 ; add #0xe18 ; bl sem_wait  @0x27c15f4
 *   0x27c1640 GC_restart_all           "pthread_kill failed at resume"
 *             ldr w8,[x23,#0xfb8]  (gate, read 7 insns earlier)      0x6028fb8
 *             ldr w1,[x24,#0xfc0] ; bl pthread_kill @0x27c16cc       0x6028fc0
 *   0x27c1758 GC_stop_init
 *             ldr w8,[x19,#0xfbc] ; cmn w8,#1 ; mov w8,#0x1e ; str   (SIGPWR)
 *             ldr w9,[x20,#0xfc0] ; cmn w9,#1 ; mov w9,#0x18 ; str   (SIGXCPU)
 *             adrp x0,#0x6251000 ; add #0xe18 ; bl sem_init  @0x27c17ac
 *
 * Five independent cross-checks, all of which hold:
 *  1. The three ints are contiguous -- gate 0xfb8, suspend 0xfbc, restart 0xfc0
 *     -- exactly the (gate, suspend, restart) layout seen in the Bad Piggies,
 *     PvZ Fusion and Layton builds.
 *  2. The ack semaphore is reached from FOUR sites: sem_post in the handler
 *     (both the suspend ack and the gated restart ack), sem_wait in the
 *     barrier, sem_timedwait in the retry path, and sem_init in GC_stop_init.
 *  3. GC_stop_init's `cmn wN,#1` is bdwgc's `== SIGNAL_UNSET (-1)` test, and
 *     both ints are in fact initialised to -1 in .data -- so these really are
 *     the two settable signal-number globals and nothing else.
 *  4. The values it stores are SIG_SUSPEND=SIGPWR(30) and
 *     SIG_THR_RESTART=SIGXCPU(24), bdwgc's Linux defaults. At runtime the
 *     bridge should therefore read suspend_sig=30 restart_sig=24.
 *  5. All four lie inside libil2cpp's second RW PT_LOAD (0x5cf9c70..0x6254ed0):
 *     the three ints in .data, the semaphore in .bss, as globals must.
 *
 * NOTE for the next port: the earlier scan missed these because it compared a
 * BL's decoded target against a PLT stub address computed in the wrong space.
 * libil2cpp's segments are NOT identity-mapped -- p_vaddr = p_offset + 0x4000
 * for .text and + 0xC000 for .data -- so a scan that walks file offsets while
 * comparing virtual addresses is off by exactly one segment delta and matches
 * nothing. gc_globals.py translates explicitly via the program headers. */
#define GC_SUSPEND_SIG_OFF 0x3971314   /* GC_sig_suspend      (int)   -> 30 */
#define GC_RESTART_SIG_OFF 0x3971318   /* GC_sig_thr_restart  (int)   -> 24 */
#define GC_START_ACK_OFF   0x3971310   /* GC_retry_signals, restart-ack gate */
#define GC_ACK_SEM_OFF     0x3b93688   /* GC_suspend_ack_sem  (void*)        */

/* Largest offset this bridge dereferences; used for the bounds guard below. */
#define GC_MAX_OFF         (GC_ACK_SEM_OFF + sizeof(void *))

/* GUARD WORDS. Same fail-safe pattern as every code patch in this port: assert a
 * known instruction word at each site the four offsets were read from, so that a
 * stale offset against a different libil2cpp disables the bridge instead of
 * dereferencing an address that no longer means anything. Checked once, before
 * anything is dereferenced. The encodings pin both the base register and the
 * 12-bit displacement, so together with the adrp they fully determine the
 * address -- if all six match, the offsets above cannot be wrong.
 *
 * .text takes no relocations and this port patches nothing in libil2cpp's
 * .text, so these words are identical in the file and in memory. */
typedef struct { uint32_t off; uint32_t word; const char *what; } GcGuard;
static const GcGuard gc_guards[] = {
  { 0x17b3520, 0xd0010df8, "GC_suspend_all adrp x24,#0x3971000" },
  { 0x17b355c, 0xb9431701, "GC_suspend_all ldr w1,[x24,#0x314]" },
  { 0x17b3760, 0xd0010df7, "GC_restart_all adrp x23,#0x3971000" },
  { 0x17b3764, 0xd0010df8, "GC_restart_all adrp x24,#0x3971000" },
  { 0x17b37a0, 0xb94312e8, "GC_restart_all ldr w8,[x23,#0x310]" },
  { 0x17b37bc, 0xb9431b01, "GC_restart_all ldr w1,[x24,#0x318]" },
  { 0x17b3890, 0x90011f00, "GC_stop_init   adrp x0,#0x3b93000"  },
  { 0x17b3894, 0x911a2000, "GC_stop_init   add x0,x0,#0x688"    },
};

/* Master switch for actually suspending mutator threads during the GC
 * stop-the-world (vs. the old ack-only bridge that left them running through
 * the mark window). 1 = real suspension via svcSetThreadActivity; set to 0 to
 * fall straight back to the previous ack-only behaviour if this ever regresses.
 * Fail-safe regardless: a thread the registry doesn't know is acked only. */
#define GC_REAL_SUSPEND 1

int pthread_kill_gc(pthread_t t, int sig) {
  uintptr_t b = g_il2cpp_base;
  /* FAIL-SAFE: never dereference outside the loaded module. If the offsets above
   * are ever stale for the binary in hand (the Layton bug), we degrade to a no-op
   * instead of taking an unmapped read -- the GC then stalls rather than crashing,
   * which is both diagnosable and recoverable. */
  if (b && g_il2cpp_size && GC_MAX_OFF > g_il2cpp_size) {
    static int logged_oob = 0;
    if (!logged_oob) { logged_oob = 1;
      debugPrintf("[gc] BRIDGE DISABLED: offsets exceed libil2cpp size (max 0x%x > 0x%zx)"
                  " -- stale offsets from another build?\n",
                  (unsigned)GC_MAX_OFF, g_il2cpp_size); }
    return 0;
  }
  /* FAIL-SAFE 2: the four offsets were read out of eight specific instructions
   * (an adrp + its ldr/add, for each of the four globals). Verify those
   * instructions are still there before trusting what they pointed at. A
   * mismatch means this is not the libil2cpp the offsets were derived from, so
   * the bridge disables itself permanently rather than dereferencing garbage --
   * the GC then stalls (diagnosable, recoverable) instead of faulting.
   *
   * Reading here is safe: so_finalize maps executable runs Perm_Rx, and this is
   * the collecting thread under the GC lock, so the static latch needs no
   * synchronisation. */
  if (b) {
    static int guards_state = 0;   /* 0 = unchecked, 1 = pass, -1 = fail */
    if (guards_state == 0) {
      const unsigned n = (unsigned)(sizeof gc_guards / sizeof gc_guards[0]);
      guards_state = 1;
      for (unsigned i = 0; i < n; i++) {
        uint32_t got = *(volatile uint32_t *)(b + gc_guards[i].off);
        if (got != gc_guards[i].word) {
          guards_state = -1;
          debugPrintf("[gc] BRIDGE DISABLED: guard %u/%u failed at il2cpp+0x%x -- "
                      "expected %08x (%s), found %08x. These GC offsets were "
                      "derived from a different libil2cpp; re-derive with "
                      "tools/re/gc_globals.py.\n",
                      i + 1u, n, (unsigned)gc_guards[i].off,
                      (unsigned)gc_guards[i].word, gc_guards[i].what,
                      (unsigned)got);
          break;
        }
      }
      if (guards_state == 1)
        debugPrintf("[gc] bridge guards OK (%u instruction words matched)\n", n);
    }
    if (guards_state < 0) return 0;
  }
  if (b && sig) {
    int suspend_sig = *(volatile int *)(b + GC_SUSPEND_SIG_OFF);
    int restart_sig = *(volatile int *)(b + GC_RESTART_SIG_OFF);

    /* SANITY-CHECK THE VALUES. The guard words above prove we are reading the
     * right addresses; this proves the GC has actually initialised them. A real
     * signal number is 1..64 and the two differ. Before GC_stop_init runs both
     * read -1 (bdwgc's SIGNAL_UNSET), which never matches a live signal, so the
     * bridge is inert until the GC is up -- that is expected, not a fault.
     *
     * For this binary GC_stop_init stores SIGPWR(30) and SIGXCPU(24), so the
     * expected line is: suspend_sig=30 restart_sig=24. */
    static int checked = 0;
    if (!checked) {
      checked = 1;
      int ok = (suspend_sig > 0 && suspend_sig < 65 &&
                restart_sig > 0 && restart_sig < 65 &&
                suspend_sig != restart_sig);
      debugPrintf("[gc] bridge offsets %s: suspend_sig=%d restart_sig=%d "
                  "(il2cpp+0x%x / 0x%x)%s\n",
                  ok ? "look VALID" : "look WRONG",
                  suspend_sig, restart_sig,
                  (unsigned)GC_SUSPEND_SIG_OFF, (unsigned)GC_RESTART_SIG_OFF,
                  ok ? " -- expected 30/24" :
                       " -- expected 30/24 (SIGPWR/SIGXCPU); if this fired "
                       "before GC_stop_init both read -1, otherwise the "
                       "offsets are for a different libil2cpp");
    }
    void **ack_sem  = (void **)(b + GC_ACK_SEM_OFF);
    if (sig == suspend_sig) {            /* stop-the-world: pause the thread, THEN ack */
      /* Pause before posting the ack, so once GC_suspend_all's barrier drains
       * the acks the collector marks against genuinely-stopped mutators. */
      int paused = GC_REAL_SUSPEND ? diag_gc_suspend_thread((void *)(uintptr_t)t) : -1;
      static int logged_s = 0;
      if (!logged_s) { logged_s = 1;
        debugPrintf("[gc] stop-world suspend sig=%d -> %s, ack via sem@il2cpp+0x%x (first time)\n",
                    sig,
                    paused < 0 ? "ack-only (real-suspend off)" :
                    paused     ? "PAUSED thread" : "thread not in registry, ack-only",
                    (unsigned)GC_ACK_SEM_OFF); }
      sem_post_fake(ack_sem);
      return 0;
    }
    if (sig == restart_sig) {            /* start-the-world: resume the thread, ack iff handler would */
      /* Resume before the (gated) ack -- mirror of the suspend path. */
      int resumed = GC_REAL_SUSPEND ? diag_gc_resume_thread((void *)(uintptr_t)t) : -1;
      static int logged_r = 0;
      if (!logged_r) { logged_r = 1;
        debugPrintf("[gc] start-world restart sig=%d gate=%d -> %s (first time)\n",
                    sig, *(volatile int *)(b + GC_START_ACK_OFF),
                    resumed < 0 ? "ack-only (real-suspend off)" :
                    resumed     ? "RESUMED thread" : "thread not in registry"); }
      if (*(volatile int *)(b + GC_START_ACK_OFF)) sem_post_fake(ack_sem);
      return 0;
    }
  }
  return 0;   /* any other signal: no-op, as before */
}
