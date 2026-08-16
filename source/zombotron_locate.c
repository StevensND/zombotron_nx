/* zombotron_locate.c -- resolve engine functions inside the game's own
 * libunity.so at boot.
 *
 * TWO TIERS, in this order:
 *
 *   1. FINGERPRINT SCAN. Masked opcode patterns derived from Zombotron 1.4.8's
 *      libunity.so, accepted only on exactly one match. This is the tier that
 *      survives a game update: as long as the game is rebuilt with the same
 *      Unity version and toolchain the codegen holds, and the scan finds the
 *      function at its new address.
 *
 *   2. VERIFIED RVA + GUARD. If the scan finds nothing, or finds too much, fall
 *      back to the address measured by hand for 1.4.8 -- but only if the word
 *      living there still matches the recorded guard. On 1.4.8 this agrees with
 *      tier 1; on a rebuilt binary the guard will almost certainly fail, and
 *      failing is the point.
 *
 * If both tiers fail the target stays 0 and its patch is skipped. That is a
 * deliberate ordering of harms: a missed patch is a bug report, a wrong patch is
 * memory corruption inside someone else's engine.
 *
 * WHY NOT JUST HARD-CODE THE RVAs, as badpiggies_nx and ZookeeperDX_NX do?
 * Because a raw offset table silently stops applying the moment the game
 * updates, and re-deriving it needs the new binary, a symbolized reference, and
 * someone who remembers the procedure. Tier 1 makes the common case
 * self-maintaining; tier 2 keeps the known-good answer for the build that was
 * actually tested on hardware.
 *
 * MIT.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <elf.h>

#include "so_util.h"
#include "util.h"
#include "zombotron_offsets.h"
#include "zombotron_locate.h"

uintptr_t g_cp_addr[ZB_TARGET_COUNT];
static zb_resolve_how g_cp_how[ZB_TARGET_COUNT];

/* so_module keeps the link-time program headers; a segment's runtime address is
 * load_virtbase + p_vaddr. Scan only the executable PT_LOAD -- walking .rodata
 * and .data too would be slower and would invite false positives on data that
 * happens to encode our opcodes. */
static int find_text(so_module *mod, uintptr_t *out_base, size_t *out_size) {
  for (int i = 0; i < mod->phnum; i++) {
    Elf64_Phdr *p = &mod->phdr[i];
    if (p->p_type == PT_LOAD && (p->p_flags & PF_X)) {
      *out_base = (uintptr_t)mod->load_virtbase + p->p_vaddr;
      *out_size = p->p_filesz;
      return 0;
    }
  }
  return -1;
}

/* NOTE ON ADDRESS BASES -- easy to get wrong, and wrong silently.
 * GAME_RVA_* in zombotron_offsets.h are MODULE-relative (libunity links at
 * vaddr 0, so a link-time vaddr IS the module RVA). The scan, however, walks
 * only the executable segment, whose base is well past the module base. So the
 * two must not be mixed: RVAs are always computed against load_virtbase, never
 * against the .text base. */

static inline int fp_match(const uint32_t *w, const fp_word *fp, int n) {
  for (int j = 0; j < n; j++)
    if ((w[j] & fp[j].mask) != fp[j].pattern)
      return 0;
  return 1;
}

int zb_locate_all(so_module *unity) {
  uintptr_t base;
  size_t size;

  memset(g_cp_addr, 0, sizeof(g_cp_addr));
  memset(g_cp_how, 0, sizeof(g_cp_how));

  if (find_text(unity, &base, &size) < 0) {
    debugPrintf("[locate] FATAL: libunity has no executable segment\n");
    return -1;
  }

  uintptr_t mod_base = (uintptr_t)unity->load_virtbase;
  const uint32_t *w = (const uint32_t *)base;
  size_t nwords = size / 4;
  int hits[ZB_TARGET_COUNT] = { 0 };
  uintptr_t first[ZB_TARGET_COUNT] = { 0 };

  debugPrintf("[locate] libunity .text @ %p (%u words)\n",
              (void *)base, (unsigned)nwords);

  /* One pass. Only each target's first word is tested inline; the full 16-word
   * compare runs on the rare first-word hit. */
  for (size_t i = 0; i + 24 < nwords; i++) {
    uint32_t cur = w[i];
    for (size_t t = 0; t < ZOMBOTRON_TARGETS_N; t++) {
      const fp_target *tg = &ZOMBOTRON_TARGETS[t];
      if ((cur & tg->fp[0].mask) != tg->fp[0].pattern)
        continue;
      if (!fp_match(&w[i], tg->fp, tg->n))
        continue;
      if (hits[t]++ == 0)
        first[t] = base + i * 4;
    }
  }

  int ok = 0;
  for (size_t t = 0; t < ZOMBOTRON_TARGETS_N; t++) {
    const fp_target *tg = &ZOMBOTRON_TARGETS[t];

    if (hits[t] == 1) {
      g_cp_addr[t] = first[t];
      g_cp_how[t] = ZB_BY_FINGERPRINT;
      ok++;
      uint32_t rva = (uint32_t)(first[t] - mod_base);
      debugPrintf("[locate] %-26s fp   rva=0x%06x%s\n", tg->name, rva,
                  rva == tg->game_rva ? "  (== 1.4.8)"
                                      : "  (moved -- game updated?)");
      continue;
    }

    /* Tier 2: the hand-verified 1.4.8 address, gated on its guard word. */
    uintptr_t cand = mod_base + tg->game_rva;
    if (cand >= base && cand + 4 <= base + size &&
        *(volatile uint32_t *)cand == tg->guard) {
      g_cp_addr[t] = cand;
      g_cp_how[t] = ZB_BY_RVA;
      ok++;
      debugPrintf("[locate] %-26s rva  0x%06x (fp %s, guard held)\n",
                  tg->name, tg->game_rva, hits[t] ? "ambiguous" : "missed");
    } else {
      g_cp_how[t] = ZB_UNRESOLVED;
      debugPrintf("[locate] %-26s UNRESOLVED (fp hits=%d, guard mismatch) -- stock\n",
                  tg->name, hits[t]);
    }
  }

  debugPrintf("[locate] %d/%u resolved\n", ok, (unsigned)ZOMBOTRON_TARGETS_N);
  return ok;
}

uintptr_t zb_addr(int id) {
  return (id < 0 || id >= ZB_TARGET_COUNT) ? 0 : g_cp_addr[id];
}

zb_resolve_how zb_how(int id) {
  return (id < 0 || id >= ZB_TARGET_COUNT) ? ZB_UNRESOLVED : g_cp_how[id];
}
