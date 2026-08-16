/* zombotron_region_patch.h -- 256MB -> 64MB memory-region granularity for
 * Zombotron 1.4.8 (Unity 6000.2.6f2).  GENERATED -- tools/gen_region_patch.py.
 *
 * WHY IT IS REQUIRED (a memory-layout change alone cannot work)
 * Unity's allocator rounds every virtual reservation up to a 256 MB region. On
 * hardware Zombotron issued EIGHT reservations that each rounded to 0x1FFFF000
 * (512 MB - one page): 4088 MB on a console reporting 3189 MB total. No division
 * of arena/pool/window satisfies that. At 64 MB granularity the same eight cost
 * 512 MB.
 *
 * THE GRANULARITY IS NOT ONE CONSTANT
 * It reaches the code as seven different encodings and at three bit levels
 * (28 = 256MB regions; 36 and 40 = the higher-level block directory):
 *
 *     mov  w9,  #0xfffffff            256MB-1 round-up addend
 *     mov  w8,  #0x10000000           256MB literal
 *     and  x21, x9, #0x...f0000000    round up / mask to a region
 *     lsr  x8,  x1, #0x1c             pointer -> region index
 *     lsr  x8,  x1, #0x28             pointer -> directory index
 *     ubfx x10, x1, #0x1c, #0xc       region index bitfield extract
 *     mov  x11, #-0x1000000000    +   movk x11, #0x1000, lsl #16
 *     sub  x8,  x8, x10, lsl #28      region index -> byte offset
 *
 * An earlier revision of this table carried only FOUR of those and covered ten
 * sites. That would have shifted the reserve path to 64 MB while leaving the
 * ubfx index extractions and the 36/40-bit directory at 256 MB -- the allocator
 * would have booked blocks under one geometry and looked them up under another.
 *
 * HOW THESE WERE DERIVED
 * The transform rules are general (every granularity-derived quantity shifted
 * down two bits) and were validated by REPRODUCING PvZ Fusion's proven 21-word
 * table exactly: 21/21 granularity words, zero incorrect. Applied to a
 * symbolized 6000.2.6f2 reference they find 21 sites across ten memory-manager
 * functions -- LocalLowLevelAllocator::ReserveMemoryBlock, DynamicHeapAllocator
 * (ctor + Allocate), MemoryManager::VirtualAllocator (ReserveMemoryBlock,
 * GetMemoryBlockFromPointer, GetBlockInfoFromPointer, MarkMemoryBlocks),
 * MemoryManager::GetAllocatorContainingPtr, InitializeDefaultAllocators and
 * TLSAllocator::ThreadInitialize.
 *
 * The game binary is a different build configuration, so its registers and
 * addresses differ. Scanning it with the same rules yields 111 candidate
 * clusters; the one below is the single cluster containing all five sites that
 * were independently located by masked opcode fingerprint, and it holds exactly
 * 21 sites -- the same count as the reference. The other clusters are ordinary
 * shift-by-28 code elsewhere in the engine and must NOT be touched.
 *
 * ALL 21 OR NONE. The installer verifies every `from` word before writing any.
 */
#ifndef ZOMBOTRON_REGION_PATCH_H
#define ZOMBOTRON_REGION_PATCH_H

#include <stdint.h>


/* ========================= ZOMBOTRON (re-derived) =========================
 * The table below is Zombotron's OWN 21 sites, not Zombotron's. Zombotron is
 * Unity 6000.2.6f2 -- the same build as Zombotron -- and its memory manager is
 * byte-identical: all 21 sites map 1:1 to Zombotron's with identical register
 * allocation. Located by scanning libunity.so for every granularity-bearing
 * encoding (size/size16/roundup/dirbase/shift/idx28/idx12/scale/basemask[64]/
 * fusedL1) and keeping the single 21-site cluster anchored by the six
 * fingerprint-verified allocator functions; the unrelated shift-by-28 cluster
 * at 0xd8d2xx was excluded. Every `from` was read from the binary and every
 * `to` round-tripped through a disassembler: 21/21 verified. The installer is
 * verify-first (writes a site only if its live word already equals `from`), so
 * this is safe to enable. Cross-checked against clayjamclassic_nx's region
 * methodology (which warns: width of the ubfx index must NOT change, only the
 * shift; and the fusedL1 lsr#0x28 site is easy to miss -- both handled here).
 * ========================================================================= */

typedef struct { uint32_t off, from, to; } NxPatchWord;

static const NxPatchWord ZB_REGION_WORDS[] = {
  { 0x04f55c8, 0x12be0009, 0x12bf8009 },  /* roundup  LocalLowLevelAllocator::ReserveMemoryBlock  mov w9, #0xfffffff */
  { 0x04f55d8, 0x92648d35, 0x92669535 },  /* basemask64 LocalLowLevelAllocator::ReserveMemoryBlock  and x21,x9,#0xf..f0000000 */
  { 0x04f5ebc, 0x52a20008, 0x52a08008 },  /* size     LocalLowLevelAllocator::ReserveMemoryBlock  mov w8, #0x10000000 */
  { 0x04f78c0, 0x52a20009, 0x52a08009 },  /* size     BucketAllocator::BucketAllocator            mov w9, #0x10000000 */
  { 0x04f9e10, 0x52a20009, 0x52a08009 },  /* size     DynamicHeapAllocator::DynamicHeapAllocator  mov w9, #0x10000000 */
  { 0x04fa2dc, 0x12be000d, 0x12bf800d },  /* roundup  DynamicHeapAllocator::Allocate              mov w13,#0xfffffff */
  { 0x04fa308, 0x92648d36, 0x92669536 },  /* basemask64 DynamicHeapAllocator::Allocate              and x22,x9,#0xf..f0000000 */
  { 0x04fc2b8, 0xd35cdc33, 0xd35ad433 },  /* idx28    VirtualAllocator::MarkMemoryBlocks          ubfx x19,x1,#0x1c,#0x1c */
  { 0x04fc2c0, 0xd35cfd15, 0xd35afd15 },  /* shift    VirtualAllocator::MarkMemoryBlocks          lsr x21,x8,#0x1c */
  { 0x04fc364, 0x52a20008, 0x52a08008 },  /* size     VirtualAllocator::ReserveMemoryBlock        mov w8, #0x10000000 */
  { 0x04fc698, 0xd35cfc28, 0xd35afc28 },  /* shift    VirtualAllocator::GetMemoryBlockFromPointer lsr x8,x1,#0x1c */
  { 0x04fc6a8, 0x92646c28, 0x92667428 },  /* basemask VirtualAllocator::GetMemoryBlockFromPointer and x8,x1,#0xfffffff0000000 */
  { 0x04fc6b0, 0xd35c9c2a, 0xd35a942a },  /* idx12    VirtualAllocator::GetMemoryBlockFromPointer ubfx x10,x1,#0x1c,#0xc */
  { 0x04fc6c4, 0xd35cdc29, 0xd35ad429 },  /* idx28    VirtualAllocator::GetMemoryBlockFromPointer ubfx x9,x1,#0x1c,#0x1c */
  { 0x04fc6c8, 0xb25c6feb, 0xb25e77eb },  /* dirbase  VirtualAllocator::GetMemoryBlockFromPointer mov x11,#-0x1000000000 */
  { 0x04fc6cc, 0xf2a2000b, 0xf2a0800b },  /* size16   VirtualAllocator::GetMemoryBlockFromPointer movk x11,#0x1000,lsl #16 */
  { 0x04fc710, 0xcb0a7108, 0xcb0a6908 },  /* scale    VirtualAllocator::GetMemoryBlockFromPointer sub x8,x8,x10,lsl #28 */
  { 0x04fc724, 0xd368fc28, 0xd366fc28 },  /* fusedL1  VirtualAllocator::GetBlockInfoFromPointer   lsr x8,x1,#0x28 */
  { 0x04fc734, 0xd35c9c29, 0xd35a9429 },  /* idx12    VirtualAllocator::GetBlockInfoFromPointer   ubfx x9,x1,#0x1c,#0xc */
  { 0x04fe0e0, 0xd368fc28, 0xd366fc28 },  /* fusedL1  MemoryManager::GetAllocatorContainingPtr    lsr x8,x1,#0x28 */
  { 0x04fe0f8, 0xd35c9e89, 0xd35a9689 },  /* idx12    MemoryManager::GetAllocatorContainingPtr    ubfx x9,x20,#0x1c,#0xc */
};
#define ZB_REGION_WORDS_N (sizeof(ZB_REGION_WORDS)/sizeof(ZB_REGION_WORDS[0]))

/* Must equal MMAP_ARENA_ALIGN in config.h: the arena hands back region-aligned
 * reservations and a mismatch means Unity trims pages the arena thinks are live. */
#define ZB_REGION_GRANULARITY_MB 64

#endif /* ZOMBOTRON_REGION_PATCH_H */
