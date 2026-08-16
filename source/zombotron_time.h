/* zombotron_time.h -- UnityEngine.Time driven from a wall clock.
 *
 * The engine's TimeManager::Update is never called on this port (proved by the
 * render-loop heartbeat), so every managed Time.* value stays frozen. These
 * hooks answer the managed getters from our own monotonic clock instead.
 * RVAs are from Il2CppDumper output for Zombotron 1.4.8 and each is guarded.
 */
#ifndef ZOMBOTRON_TIME_H
#define ZOMBOTRON_TIME_H

#include <stdint.h>
#include "so_util.h"

/* UnityEngine.Time accessors, Zombotron 1.4.8. All share the same prologue
 * guard (stp x30, x19, [sp, #-0x10]! == 0xa9bf4ffe). */
#define RVA_Time_get_time                   0x32873f4u  /* Zombotron: UnityEngine.Time (guard 0xa9bf4ffe verified) */
#define RVA_Time_get_deltaTime              0x328749cu
#define RVA_Time_get_unscaledTime           0x32874c4u
#define RVA_Time_get_unscaledDeltaTime      0x32874ecu
#define RVA_Time_get_fixedUnscaledDeltaTime 0x3287514u
#define RVA_Time_get_fixedDeltaTime         0x328753cu
#define RVA_Time_get_smoothDeltaTime        0x328759cu
#define RVA_Time_get_timeScale              0x32875c4u
#define RVA_Time_get_frameCount             0x327a700u
#define RVA_Time_get_renderedFrameCount     0x55e6314u  /* not present in Zombotron dump -> SKIPs harmlessly (unused by gameplay) */
#define RVA_Time_get_realtimeSinceStartup   0x3286560u

/* Install the hooks. Each is verified against its guard word independently, so a
 * moved getter is skipped rather than mispatched. Returns how many landed. */
int zb_time_install(so_module *il2cpp);

/* Sample deltaTime. Call ONCE per frame from the render loop. */
void zb_time_tick(void);

int     zb_time_installed(void);
float   zb_time_now(void);
float   zb_time_delta(void);
int32_t zb_time_frames(void);

#endif /* ZOMBOTRON_TIME_H */
