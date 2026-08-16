/* zombotron_unityfs.c -- find the intro VideoClips in the game's own files.
 *
 * WHY THIS IS IN THE PORT AND NOT ONLY IN A PC SCRIPT
 * The first hardware run of the video path failed for the dullest possible
 * reason: tools/extract_videos.py had not been run against the data root, so
 * videos/manifest.txt did not exist and the player had nothing to play. A step
 * a user has to remember is a step that gets missed. With this, the port finds
 * the clips in the files it already has and plays them straight out of
 * sharedassets1.resource -- no staging, no extra copy on the SD card, and
 * nothing to go stale when the assets are replaced.
 *
 * WHAT IT COSTS AT RUNTIME: essentially nothing, because the byte ranges are
 * reachable without decompressing the bundle. data.unity3d is 67 MB compressed
 * / 251 MB decompressed in 1920 LZ4 blocks, but the block table gives every
 * block's COMPRESSED size, so blocks that do not overlap the node we want are
 * seeked past rather than read. For Zombotron 1.4.8 sharedassets1.assets lands
 * inside exactly ONE 128 KB block, so the whole scan is:
 *
 *     header + 7986-byte block table + one 128 KB block  ==  under 150 KB read
 *
 * against 45.4 MB of compressed data skipped by fseek.
 *
 * FORMATS IMPLEMENTED
 *   UnityFS container (version 8)  -> node table, block table
 *   LZ4 block decompression        -> minimal, bounds-checked, see lz4_block()
 *   SerializedFile (version 22)    -> object table, class ids
 *   VideoClip body (class 329)     -> name, size, frame rate/count, and the
 *                                     m_ExternalResources byte range
 *
 * All of it was prototyped in Python and validated against the real
 * data.unity3d before being written here, so the layouts are checked rather
 * than inferred from documentation.
 *
 * NO NEW PACKAGE. LZ4 is ~60 lines and is implemented below rather than pulled
 * in as switch-lz4: this port has already lost a build cycle to a missing
 * portlib, and a self-contained decoder cannot be the thing that is missing.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zombotron_unityfs.h"

#ifndef UNITYFS_HOST_TEST
#include "util.h"
#else
#include <stdarg.h>
#define debugPrintf printf
#endif

#define CLASS_VIDEOCLIP 329

/* ------------------------------------------------------------ big-endian rd */

static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t be64(const uint8_t *p) {
  return ((uint64_t)be32(p) << 32) | be32(p + 4);
}
static uint32_t le32(const uint8_t *p) {
  return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[1] << 8) | p[0];
}
static uint64_t le64(const uint8_t *p) {
  return ((uint64_t)le32(p + 4) << 32) | le32(p);
}

/* --------------------------------------------------------------- LZ4 block */

/* Minimal LZ4 block-format decompressor.
 *
 * Every read and every write is bounds-checked against the caller's buffers:
 * this parses a file from the SD card, and a truncated or corrupt bundle must
 * come back as "no clips found" rather than as a wild write. Returns the number
 * of bytes produced, or -1.
 *
 * Format: a sequence is a token byte (high nibble = literal length, low nibble
 * = match length - 4), optional 255-extended length bytes, the literals, then a
 * 2-byte little-endian back-offset and optional extended match length. The
 * final sequence has literals and no match.
 */
static int lz4_block(const uint8_t *src, int srclen, uint8_t *dst, int dstcap) {
  int s = 0, d = 0;
  while (s < srclen) {
    const uint32_t token = src[s++];
    uint32_t litlen = token >> 4;
    if (litlen == 15) {
      uint32_t b;
      do {
        if (s >= srclen) return -1;
        b = src[s++];
        if (litlen > (uint32_t)dstcap) return -1;   /* overflow guard */
        litlen += b;
      } while (b == 255);
    }
    if (litlen > (uint32_t)(srclen - s) || litlen > (uint32_t)(dstcap - d))
      return -1;
    memcpy(dst + d, src + s, litlen);
    s += (int)litlen;
    d += (int)litlen;

    if (s == srclen)
      break;                                   /* last sequence: literals only */
    if (s + 2 > srclen) return -1;
    const uint32_t off = (uint32_t)src[s] | ((uint32_t)src[s + 1] << 8);
    s += 2;
    if (off == 0 || off > (uint32_t)d) return -1;

    uint32_t mlen = (token & 0x0F) + 4;
    if ((token & 0x0F) == 15) {
      uint32_t b;
      do {
        if (s >= srclen) return -1;
        b = src[s++];
        if (mlen > (uint32_t)dstcap) return -1;
        mlen += b;
      } while (b == 255);
    }
    if (mlen > (uint32_t)(dstcap - d)) return -1;
    /* byte-at-a-time: LZ4 matches may overlap the output being written */
    const uint8_t *m = dst + d - off;
    for (uint32_t i = 0; i < mlen; i++)
      dst[d + i] = m[i];
    d += (int)mlen;
  }
  return d;
}

/* Decompress one bundle block/section according to its flags. Compression 0 is
 * stored; 2 and 3 are LZ4 and LZ4HC, which share a bitstream. LZMA (1) is not
 * implemented -- Zombotron's bundle is entirely LZ4, and a bundle that is not
 * degrades to "no clips found" and the PC extractor. */
static int unpack(const uint8_t *src, int csize, uint8_t *dst, int usize,
                  unsigned flags) {
  const unsigned comp = flags & 0x3F;
  if (comp == 0) {
    if (csize > usize) return -1;
    memcpy(dst, src, (size_t)csize);
    return csize;
  }
  if (comp == 2 || comp == 3)
    return lz4_block(src, csize, dst, usize);
  return -1;
}

/* -------------------------------------------------------- UnityFS container */

typedef struct { uint32_t usize, csize; unsigned flags; } BlockInfo;

/* Read a NUL-terminated string from a file, capped. */
static int fcstr(FILE *f, char *out, size_t cap) {
  size_t n = 0;
  for (;;) {
    int c = fgetc(f);
    if (c == EOF) return -1;
    if (c == 0) break;
    if (n + 1 < cap) out[n++] = (char)c;
  }
  out[n] = 0;
  return (int)n;
}

/* Extract one node out of the bundle by name. Returns malloc'd bytes. */
static uint8_t *bundle_extract(const char *path, const char *want,
                               int *out_size) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;

  uint8_t *node = NULL, *bi = NULL, *cbuf = NULL, *ubuf = NULL;
  BlockInfo *blocks = NULL;

  char sig[32], uver[32], urev[32];
  if (fcstr(f, sig, sizeof sig) < 0 || strcmp(sig, "UnityFS") != 0)
    goto fail;
  uint8_t hdr[20];
  if (fread(hdr, 1, 4, f) != 4) goto fail;
  const uint32_t version = be32(hdr);
  if (fcstr(f, uver, sizeof uver) < 0) goto fail;
  if (fcstr(f, urev, sizeof urev) < 0) goto fail;
  if (fread(hdr, 1, 20, f) != 20) goto fail;
  const uint64_t total = be64(hdr);
  const uint32_t cbi = be32(hdr + 8);
  const uint32_t ubi = be32(hdr + 12);
  const uint32_t flags = be32(hdr + 16);

  if (version >= 7) {
    long p = ftell(f);
    if (fseek(f, (p + 15) & ~15L, SEEK_SET) != 0) goto fail;
  }

  if (cbi == 0 || cbi > (1u << 24) || ubi == 0 || ubi > (1u << 26))
    goto fail;

  long data_start;
  cbuf = malloc(cbi);
  bi = malloc(ubi);
  if (!cbuf || !bi) goto fail;
  if (flags & 0x80) {                       /* block table lives at the end */
    long p = ftell(f);
    if (fseek(f, (long)(total - cbi), SEEK_SET) != 0) goto fail;
    if (fread(cbuf, 1, cbi, f) != cbi) goto fail;
    data_start = p;
  } else {
    if (fread(cbuf, 1, cbi, f) != cbi) goto fail;
    data_start = ftell(f);
  }
  if (unpack(cbuf, (int)cbi, bi, (int)ubi, flags) < 0) goto fail;
  free(cbuf);
  cbuf = NULL;
  if (flags & 0x200)
    data_start = (data_start + 15) & ~15L;

  /* block table: 16-byte hash, i32 count, then {u32 usize, u32 csize, u16 flags} */
  uint32_t p = 16;
  if (p + 4 > ubi) goto fail;
  const uint32_t nblocks = be32(bi + p);
  p += 4;
  if (nblocks == 0 || nblocks > 1000000u || p + (uint64_t)nblocks * 10 > ubi)
    goto fail;
  blocks = malloc(sizeof(BlockInfo) * nblocks);
  if (!blocks) goto fail;
  uint32_t maxu = 0;
  for (uint32_t i = 0; i < nblocks; i++) {
    blocks[i].usize = be32(bi + p);
    blocks[i].csize = be32(bi + p + 4);
    blocks[i].flags = ((unsigned)bi[p + 8] << 8) | bi[p + 9];
    if (blocks[i].usize > maxu) maxu = blocks[i].usize;
    p += 10;
  }

  /* node table: i32 count, then {i64 offset, i64 size, u32 flags, cstring} */
  if (p + 4 > ubi) goto fail;
  const uint32_t nnodes = be32(bi + p);
  p += 4;
  uint64_t want_off = 0, want_sz = 0;
  int found = 0;
  for (uint32_t i = 0; i < nnodes; i++) {
    if (p + 20 > ubi) goto fail;
    const uint64_t off = be64(bi + p);
    const uint64_t sz = be64(bi + p + 8);
    p += 20;
    const char *nm = (const char *)(bi + p);
    uint32_t l = 0;
    while (p + l < ubi && bi[p + l]) l++;
    if (p + l >= ubi) goto fail;
    if (!found && strcmp(nm, want) == 0) {
      want_off = off;
      want_sz = sz;
      found = 1;
    }
    p += l + 1;
  }
  if (!found || want_sz == 0 || want_sz > (1u << 26)) goto fail;

  /* Walk the blocks, SEEKING past everything that does not overlap the node.
   * This is what makes the scan cost 150 KB instead of 67 MB. */
  node = malloc((size_t)want_sz);
  ubuf = malloc(maxu);
  cbuf = malloc(maxu + 65536);
  if (!node || !ubuf || !cbuf) goto fail;
  if (fseek(f, data_start, SEEK_SET) != 0) goto fail;

  uint64_t cur = 0, got = 0;
  long pending_skip = 0;                        /* coalesced, see below */
  for (uint32_t i = 0; i < nblocks && got < want_sz; i++) {
    const uint64_t bend = cur + blocks[i].usize;
    if (bend <= want_off) {
      /* Entirely before the node. Accumulate rather than seeking per block:
       * the node sits ~116 MB into the decompressed stream, which is 889
       * blocks, and one fseek beats 889 of them on a FAT filesystem. */
      pending_skip += (long)blocks[i].csize;
      cur = bend;
      continue;
    }
    if (pending_skip) {
      if (fseek(f, pending_skip, SEEK_CUR) != 0) goto fail;
      pending_skip = 0;
    }
    if (cur >= want_off + want_sz)
      break;
    if (fread(cbuf, 1, blocks[i].csize, f) != blocks[i].csize) goto fail;
    const int n = unpack(cbuf, (int)blocks[i].csize, ubuf,
                         (int)blocks[i].usize, blocks[i].flags);
    if (n < 0 || (uint32_t)n != blocks[i].usize) goto fail;
    const uint64_t s = (cur < want_off) ? (want_off - cur) : 0;
    uint64_t e = blocks[i].usize;
    if (cur + e > want_off + want_sz)
      e = want_off + want_sz - cur;
    memcpy(node + got, ubuf + s, (size_t)(e - s));
    got += e - s;
    cur = bend;
  }
  if (got != want_sz) goto fail;

  free(bi); free(blocks); free(ubuf); free(cbuf);
  fclose(f);
  *out_size = (int)want_sz;
  return node;

fail:
  free(node); free(bi); free(blocks); free(ubuf); free(cbuf);
  fclose(f);
  return NULL;
}

/* -------------------------------------------------------- SerializedFile */

/* Parse the object table and hand back every object of `want_class`.
 * Layout verified against Zombotron 1.4.8's sharedassets1.assets: format
 * version 22, little-endian body, type tree disabled (release build). */
typedef struct { uint64_t start; uint32_t size; } ObjRef;

static int serialized_objects(const uint8_t *d, uint32_t len, int want_class,
                              ObjRef *out, int max) {
  if (len < 48) return 0;
  const uint32_t version = be32(d + 8);
  if (version < 16 || version > 30) return 0;   /* unknown dialect: bail */
  if (d[16] != 0) return 0;                     /* big-endian body: not Android */

  uint64_t data_offset;
  uint32_t p;
  if (version >= 22) {
    data_offset = be64(d + 32);
    p = 48;
  } else {
    data_offset = be32(d + 12);
    p = 20;
  }

  while (p < len && d[p]) p++;                  /* unity version string */
  if (p >= len) return 0;
  p++;
  if (p + 5 > len) return 0;
  p += 4;                                       /* target platform */
  const uint8_t type_tree = d[p++];
  if (type_tree) return 0;                      /* fixed-layout reader only */

  if (p + 4 > len) return 0;
  const uint32_t ntypes = le32(d + p);
  p += 4;
  if (ntypes > 100000u) return 0;
  int *classes = malloc(sizeof(int) * (ntypes ? ntypes : 1));
  if (!classes) return 0;
  for (uint32_t i = 0; i < ntypes; i++) {
    if (p + 4 > len) { free(classes); return 0; }
    const int32_t cls = (int32_t)le32(d + p);
    p += 4;
    p += 1;                                     /* m_IsStrippedType */
    p += 2;                                     /* m_ScriptTypeIndex  */
    if (cls == 114) p += 16;                    /* m_ScriptID         */
    p += 16;                                    /* m_OldTypeHash      */
    if (p > len) { free(classes); return 0; }
    classes[i] = cls;
  }

  if (p + 4 > len) { free(classes); return 0; }
  const uint32_t nobj = le32(d + p);
  p += 4;
  if (nobj > 1000000u) { free(classes); return 0; }

  int n = 0;
  for (uint32_t i = 0; i < nobj; i++) {
    p = (p + 3) & ~3u;
    if (p + 24 > len) break;
    p += 8;                                     /* m_PathID */
    uint64_t start;
    if (version >= 22) { start = le64(d + p); p += 8; }
    else               { start = le32(d + p); p += 4; }
    const uint32_t size = le32(d + p);
    p += 4;
    const int32_t tid = (int32_t)le32(d + p);
    p += 4;
    if (tid < 0 || (uint32_t)tid >= ntypes) continue;
    if (classes[tid] != want_class) continue;
    if (n < max) {
      out[n].start = start + data_offset;
      out[n].size = size;
      n++;
    }
  }
  free(classes);
  return n;
}

/* ------------------------------------------------------------- VideoClip */

/* Body layout, no type tree. Unity aligns to 4 after every string and after
 * every array whose element is not 4 bytes wide, which is why the align steps
 * below are load-bearing rather than decorative. */
static int parse_videoclip(const uint8_t *b, uint32_t len, zb_videoclip *v) {
  uint32_t p = 0;
#define NEED(n) do { if (p + (n) > len) return 0; } while (0)
#define ALIGN() do { p = (p + 3) & ~3u; } while (0)

  /* m_Name */
  NEED(4);
  uint32_t n = le32(b + p); p += 4;
  NEED(n);
  {
    uint32_t c = n < sizeof v->name - 1 ? n : (uint32_t)sizeof v->name - 1;
    memcpy(v->name, b + p, c);
    v->name[c] = 0;
  }
  p += n; ALIGN();

  /* m_OriginalPath, skipped */
  NEED(4);
  n = le32(b + p); p += 4;
  NEED(n);
  p += n; ALIGN();

  NEED(24);
  v->w = (int)le32(b + p + 8);
  v->h = (int)le32(b + p + 12);
  p += 24;

  NEED(8);
  {
    union { uint64_t u; double d; } fr;
    fr.u = le64(b + p);
    v->fps = fr.d;
  }
  p += 8;

  NEED(8);
  v->frames = le64(b + p);
  p += 8;
  NEED(4);
  p += 4;                                       /* m_Format */

  NEED(4);
  n = le32(b + p); p += 4;                      /* m_AudioChannelCount  u16[] */
  NEED(n * 2);
  p += n * 2; ALIGN();

  NEED(4);
  n = le32(b + p); p += 4;                      /* m_AudioSampleRate    u32[] */
  NEED(n * 4);
  p += n * 4;

  NEED(4);
  n = le32(b + p); p += 4;                      /* m_AudioLanguage   string[] */
  for (uint32_t i = 0; i < n; i++) {
    NEED(4);
    uint32_t l = le32(b + p); p += 4;
    NEED(l);
    p += l; ALIGN();
  }

  NEED(4);
  n = le32(b + p); p += 4;                      /* m_VideoShaders  PPtr[] @12 */
  NEED(n * 12);
  p += n * 12;

  /* StreamedResource m_ExternalResources */
  NEED(4);
  n = le32(b + p); p += 4;
  NEED(n);
  {
    uint32_t c = n < sizeof v->source - 1 ? n : (uint32_t)sizeof v->source - 1;
    memcpy(v->source, b + p, c);
    v->source[c] = 0;
  }
  p += n; ALIGN();

  NEED(16);
  v->offset = le64(b + p);
  v->size = le64(b + p + 8);

  v->seconds = (v->fps > 0.0) ? (double)v->frames / v->fps : 0.0;
  return (v->size > 0 && v->source[0]) ? 1 : 0;
#undef NEED
#undef ALIGN
}

/* ------------------------------------------------------------------ public */

int zb_unityfs_find_videoclips(const char *data_dir, zb_videoclip *out,
                               int max) {
  static const char *NODES[] = {
    "sharedassets1.assets", "sharedassets0.assets", "sharedassets2.assets",
    "resources.assets", "globalgamemanagers.assets",
  };
  char bundle[320];
  snprintf(bundle, sizeof bundle, "%s/data.unity3d", data_dir);

  int total = 0;
  for (unsigned k = 0; k < sizeof NODES / sizeof NODES[0] && total < max; k++) {
    int nsz = 0;
    uint8_t *node = bundle_extract(bundle, NODES[k], &nsz);
    if (!node)
      continue;

    ObjRef refs[32];
    const int nref = serialized_objects(node, (uint32_t)nsz, CLASS_VIDEOCLIP,
                                        refs, 32);
    for (int i = 0; i < nref && total < max; i++) {
      if (refs[i].start + refs[i].size > (uint64_t)nsz)
        continue;
      zb_videoclip v;
      memset(&v, 0, sizeof v);
      if (!parse_videoclip(node + refs[i].start, refs[i].size, &v))
        continue;
      out[total++] = v;
    }
    free(node);
    if (total)
      break;             /* Zombotron keeps both intro clips in one .assets */
  }
  return total;
}
