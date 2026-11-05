#ifndef STEREONAUT_EYE_VIEW_H
#define STEREONAUT_EYE_VIEW_H

#include <stdint.h>

/* One eye of a stereo picture and a perspective view straight ahead out of
 * a 180/360 degree eye, with the same mapping as the projection shader;
 * used by the scrub preview (trickplay.c) and the browser tiles
 * (thumbnails.c). */

struct ui_image;

/* Where each pixel of the view samples the eye: it depends only on the
 * sizes, the projection and the field of view, so it is kept and reused
 * (per pixel two arctangents and a square root made a strip of five
 * pictures five times slower). Zero-initialize; free with
 * eye_view_map_free. */
struct eye_view_map {
    int eye_width, eye_height, width, height;
    uint32_t projection;
    int fov;
    float *xy; /* source x, y per view pixel */
};

void eye_view_map_free(struct eye_view_map *map);

/* The left (side by side) or top (top and bottom) eye of `picture`, or a
 * copy of it for mono; `layout` is an enum nv12_stereo_layout. */
struct ui_image *eye_view_cut(const struct ui_image *picture, uint32_t layout);

/* View of `width` x `height` with a horizontal field of view of
 * `fov_degrees` out of an equirectangular 180 or 360 degree eye
 * (`projection`, enum nv12_projection_mode); NULL on failure. */
struct ui_image *eye_view_straighten(const struct ui_image *eye,
                                     struct eye_view_map *map,
                                     uint32_t projection, int fov_degrees,
                                     int width, int height);

#endif
