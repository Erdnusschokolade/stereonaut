#ifndef STEREONAUT_TRICKPLAY_H
#define STEREONAUT_TRICKPLAY_H

#include <stdbool.h>
#include <stdint.h>

/* Seek bar previews from the trickplay tiles Jellyfin generates. A worker
 * thread loads the description and the tile images (kept compressed in
 * memory while the title plays) and cuts out single thumbnails, reduced to
 * one eye for stereo layouts; 180/360 degree pictures can be straightened
 * to a forward view. The host asks for positions while the seek bar is hovered
 * or dragged and polls for finished pictures: a strip of TRICKPLAY_STRIP
 * thumbnails, the middle one at the position, the others before and after
 * it. */

#define TRICKPLAY_STRIP 5

struct ui_image;
struct trickplay;
struct jf_details;

struct trickplay *trickplay_create(void);
void trickplay_destroy(struct trickplay *trickplay);
/* Item to preview (NULL: none). Drops everything of the previous item. */
void trickplay_open(struct trickplay *trickplay, const char *item_id);
/* Asks for the strip around `position_us`, thumbnails `spacing_us` apart
 * (at least the trickplay interval; 0: only the middle one, the others are
 * NULL); `stereo_layout` and `projection_mode`
 * are enum nv12_stereo_layout / nv12_projection_mode. With `fov_degrees` > 0
 * 180/360 degree pictures are straightened to a perspective view of that
 * horizontal field of view. Older requests are superseded. */
void trickplay_request(struct trickplay *trickplay, int64_t position_us,
                       int64_t spacing_us, uint32_t stereo_layout,
                       uint32_t projection_mode, int fov_degrees);
/* Takes the newest finished strip, earliest first: pictures owned by the
 * caller, NULL before the start, after the end or not loaded (yet). True
 * if there was a new one. The middle picture comes first, the others
 * follow in further strips. */
bool trickplay_poll(struct trickplay *trickplay,
                    struct ui_image *strip[TRICKPLAY_STRIP]);
/* The worker also loads the item's description (chapters, overview,
 * people); true once when it has arrived for the current item. */
bool trickplay_details(struct trickplay *trickplay,
                       struct jf_details *details);

#endif
