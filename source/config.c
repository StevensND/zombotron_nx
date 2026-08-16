/* config.c -- the resolved render resolution, and nothing else.
 *
 * This file used to read and write config.txt, inherited from the
 * badpiggies_nx lineage. The game exposes language, resolution and frame rate
 * in its own options menu, so a config file was just a second place for the
 * same settings to live and disagree. It is gone; the Switch system language is
 * the default and the in-game selectors override it.
 *
 * screen_width / screen_height stay here because config.h declares them and
 * because they are exactly what the name suggests: the resolution the port
 * resolved at boot from ZB_FORCE_SCREEN_W/H. main.c sets them; the DPI,
 * viewport and video-letterbox paths all read them.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */
#include "config.h"

int screen_width  = 0;
int screen_height = 0;
