#ifndef STEREONAUT_TRICKPLAY_H
#define STEREONAUT_TRICKPLAY_H

#include <stdbool.h>
#include <stdint.h>

/* Seek bar previews from the trickplay tiles Jellyfin generates. A worker
 * thread loads the description and the tile images (kept compressed in
 * memory while the title plays) and cuts out single thumbnails, reduced to
 * one eye for stereo layouts. The host asks for positions while the seek
 * bar is hovered or dragged and polls for finished pictures. */

struct ui_image;
struct trickplay;

struct trickplay *trickplay_create(void);
void trickplay_destroy(struct trickplay *trickplay);
/* Item to preview (NULL: none). Drops everything of the previous item. */
void trickplay_open(struct trickplay *trickplay, const char *item_id);
/* Asks for the thumbnail at `position_us`; `stereo_layout` is an
 * enum nv12_stereo_layout. Older requests are superseded. */
void trickplay_request(struct trickplay *trickplay, int64_t position_us,
                       uint32_t stereo_layout);
/* Takes the newest finished picture (owned by the caller). */
bool trickplay_poll(struct trickplay *trickplay, struct ui_image **image);

#endif
