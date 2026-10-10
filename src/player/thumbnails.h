#ifndef STEREONAUT_THUMBNAILS_H
#define STEREONAUT_THUMBNAILS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Background thumbnail loader for browser tiles. Sources are Jellyfin images
 * ("ITEM_ID|IMAGE[|FALLBACK]", see jf_item); downloads are cached in
 * $XDG_CACHE_HOME/stereonaut/thumbs. Results are decoded, side-by-side frames
 * reduced to the left eye, posters to their top 16:9 part, and scaled to
 * THUMBNAIL_WIDTH. */

#define THUMBNAIL_WIDTH 324

struct ui_image;
struct thumbnail_loader;

struct thumbnail_result {
    uint64_t generation;
    size_t index;
    struct ui_image *image; /* NULL if the image could not be loaded */
};

struct thumbnail_loader *thumbnail_loader_create(void);
void thumbnail_loader_destroy(struct thumbnail_loader *loader);
/* Queues a source. A request with a newer generation drops every pending
 * request of older generations (the listing changed). */
void thumbnail_loader_request(struct thumbnail_loader *loader,
                              uint64_t generation, size_t index,
                              const char *source);
/* Takes one finished result; false if there is none. */
bool thumbnail_loader_poll(struct thumbnail_loader *loader,
                           struct thumbnail_result *result);
/* Decodes a JPEG into an opaque RGBA image; NULL on failure. */
struct ui_image *thumbnail_decode_jpeg(const char *data, size_t size);

#endif
