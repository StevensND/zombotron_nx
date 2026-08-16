/* zombotron_locate.h -- runtime resolution of engine functions in libunity.so */
#ifndef ZOMBOTRON_LOCATE_H
#define ZOMBOTRON_LOCATE_H

#include <stdint.h>
#include "so_util.h"

/* Order MUST match ZOMBOTRON_TARGETS[] in zombotron_offsets.h. */
typedef enum {
  ZB_TM_UPDATE = 0,      /* TimeManager::Update(double)                  */
  ZB_CHOREO_GET,         /* ChoreographerBase::Get()                     */
  ZB_AUDIO_OUTTYPE,      /* AndroidAudio::GetAndroidAudioOutputType(int) */
  ZB_FMOD_BUFGEOM,       /* FMOD OpenSL buffer-geometry bound check      */
  ZB_TARGET_COUNT
} zb_target_id;

typedef enum {
  ZB_UNRESOLVED = 0,
  ZB_BY_FINGERPRINT,     /* found by opcode scan -- update-tolerant       */
  ZB_BY_RVA              /* fell back to the verified 1.4.8 address       */
} zb_resolve_how;

extern uintptr_t g_cp_addr[ZB_TARGET_COUNT];

/* Resolve every target. Returns how many landed. Call once, after
 * so_finalize()/so_flush_caches() on libunity and BEFORE any patch installer. */
int zb_locate_all(so_module *unity);

uintptr_t      zb_addr(int id);   /* 0 if unresolved */
zb_resolve_how zb_how(int id);

/* 256MB -> 64MB allocator region granularity. Must run BEFORE the engine's
 * memory manager initialises, i.e. before the init arrays. */
int zb_install_region_patch(so_module *unity);

#endif /* ZOMBOTRON_LOCATE_H */
