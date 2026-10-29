#ifndef STEREONAUT_LOCAL_THUMBNAIL_H
#define STEREONAUT_LOCAL_THUMBNAIL_H

#include <stdbool.h>
#include <stddef.h>

/* Tile picture of a local video as JPEG: a picture next to it the way
 * Jellyfin and Kodi store them ("NAME-poster.jpg", "NAME.jpg",
 * "NAME-thumb.jpg", "NAME-fanart.jpg"), else a key frame at about 10 % of
 * the title decoded in software (only 4:2:0 in 8 or 10 bit). Slow for 8K
 * (seconds): run it on a worker and cache the result. *data is malloc'd. */
int local_thumbnail_jpeg(const char *path, int max_width, char **data,
                         size_t *size);
/* Whether local_thumbnail_jpeg finds a sidecar picture for `path` (else it
 * decodes a key frame). */
bool local_thumbnail_has_sidecar(const char *path);
/* The key frame of a video at an http(s) URL; `headers` are extra request
 * lines ("Authorization: ...\r\n") or NULL. */
int local_thumbnail_url_jpeg(const char *url, const char *headers,
                             int max_width, char **data, size_t *size);

#endif
