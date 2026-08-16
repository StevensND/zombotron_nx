/* zombotron_touch.c -- multi-touch slot tracker with Android-style phases.
 *
 * See zombotron_touch.h for why this replaces unity_input_hook.c.
 *
 * The tracking problem: HID hands us an unordered set of fingers each frame,
 * identified by a raw finger id that persists while the finger is down. Android
 * MotionEvent instead wants a stable pointer INDEX plus a phase transition. So
 * each raw id is assigned a slot on first sight, keeps it until the finger
 * lifts, and the slot is then freed for reuse. Without the slot layer, a finger
 * lifting would renumber the ones after it and the consumer would see phantom
 * moves.
 *
 * MIT.
 */
#include <string.h>

#include "zombotron_touch.h"

typedef struct {
  int   used;
  int   raw_id;
  float x, y;
  int   seen_this_frame;
  int   is_new;
  int   lifted;      /* set for exactly one frame after the finger goes away */
} slot;

static slot g_slots[NX_MAX_TOUCH];

static int find_slot(int raw_id) {
  for (int i = 0; i < NX_MAX_TOUCH; i++)
    if (g_slots[i].used && g_slots[i].raw_id == raw_id)
      return i;
  return -1;
}

static int alloc_slot(void) {
  for (int i = 0; i < NX_MAX_TOUCH; i++)
    if (!g_slots[i].used)
      return i;
  return -1;
}

void nx_input_hook_update_multi(const NxTouchIn *in, int n) {
  /* Clear last frame's transient flags. A slot that was reported UP last frame
   * is now genuinely gone. */
  for (int i = 0; i < NX_MAX_TOUCH; i++) {
    if (g_slots[i].lifted) {
      memset(&g_slots[i], 0, sizeof g_slots[i]);
      continue;
    }
    g_slots[i].seen_this_frame = 0;
    g_slots[i].is_new = 0;
  }

  if (n > NX_MAX_TOUCH)
    n = NX_MAX_TOUCH;

  for (int k = 0; k < n; k++) {
    int s = find_slot(in[k].id);
    if (s < 0) {
      s = alloc_slot();
      if (s < 0)
        continue;                 /* more fingers than slots: drop the extras */
      g_slots[s].used   = 1;
      g_slots[s].raw_id = in[k].id;
      g_slots[s].is_new = 1;
    }
    g_slots[s].x = in[k].x;
    g_slots[s].y = in[k].y;
    g_slots[s].seen_this_frame = 1;
  }

  /* Anything still allocated but not seen has lifted. Hold it one more frame so
   * the consumer gets exactly one ACTION_UP for it. */
  for (int i = 0; i < NX_MAX_TOUCH; i++)
    if (g_slots[i].used && !g_slots[i].seen_this_frame)
      g_slots[i].lifted = 1;
}

void nx_input_hook_update(int active, float ux, float uy) {
  NxTouchIn one = { 0, ux, uy };
  nx_input_hook_update_multi(active ? &one : NULL, active ? 1 : 0);
}

int zb_touch_poll(zb_touch *out, int max) {
  int n = 0;
  for (int i = 0; i < NX_MAX_TOUCH && n < max; i++) {
    if (!g_slots[i].used)
      continue;
    out[n].id     = i;
    out[n].raw_id = g_slots[i].raw_id;
    out[n].x      = g_slots[i].x;
    out[n].y      = g_slots[i].y;
    out[n].phase  = g_slots[i].lifted ? ZB_TOUCH_UP
                  : g_slots[i].is_new ? ZB_TOUCH_DOWN
                                      : ZB_TOUCH_MOVE;
    n++;
  }
  return n;
}

int zb_touch_any_down(void) {
  for (int i = 0; i < NX_MAX_TOUCH; i++)
    if (g_slots[i].used && !g_slots[i].lifted)
      return 1;
  return 0;
}
