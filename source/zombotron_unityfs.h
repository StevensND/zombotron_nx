/* zombotron_unityfs.h -- locate the intro VideoClips inside the game's own
 * assets, so no PC-side staging step is required. See zombotron_unityfs.c.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */
#ifndef ZOMBOTRON_UNITYFS_H
#define ZOMBOTRON_UNITYFS_H

#include <stdint.h>

typedef struct {
  char     name[96];     /* Unity asset name, for the log and for matching  */
  char     source[64];   /* e.g. "sharedassets1.resource", beside the bundle */
  uint64_t offset;       /* byte range of the clip inside that .resource     */
  uint64_t size;
  double   fps;
  uint64_t frames;
  double   seconds;      /* frames / fps -- what VideoClip.length returns    */
  int      w, h;
} zb_videoclip;

/* Scan <data_dir>/data.unity3d for class-329 objects. `data_dir` is the folder
 * holding data.unity3d and the .resource files, i.e. <root>/assets/bin/Data.
 * Returns how many clips were written to `out`. Never throws, never blocks on
 * anything but a short read: a missing, truncated or unexpected bundle simply
 * yields 0 and the caller carries on without video. */
int zb_unityfs_find_videoclips(const char *data_dir, zb_videoclip *out, int max);

#endif /* ZOMBOTRON_UNITYFS_H */
