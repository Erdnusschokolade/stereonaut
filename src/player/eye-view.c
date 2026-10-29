#include "eye-view.h"

#include "nv12-playback-ipc.h"
#include "ui-draw.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846

void eye_view_map_free(struct eye_view_map *map)
{
    free(map->xy);
    map->xy = NULL;
}

struct ui_image *eye_view_cut(const struct ui_image *picture, uint32_t layout)
{
    int width = picture->width, height = picture->height;
    if (layout == NV12_STEREO_SBS)
        width /= 2;
    else if (layout == NV12_STEREO_TB)
        height /= 2;
    struct ui_image *eye =
        width > 0 && height > 0 ? ui_image_create(width, height) : NULL;
    if (!eye)
        return NULL;
    for (int y = 0; y < height; ++y)
        memcpy(eye->rgba + (size_t)y * (size_t)width * 4U,
               picture->rgba + (size_t)y * (size_t)picture->width * 4U,
               (size_t)width * 4U);
    return eye;
}

/* Bilinear sample of `image` at pixel coordinates (x, y), clamped. */
static void sample(const struct ui_image *image, double x, double y,
                   uint8_t out[4])
{
    x = fmin(fmax(x - 0.5, 0.0), image->width - 1.0);
    y = fmin(fmax(y - 0.5, 0.0), image->height - 1.0);
    int x0 = (int)x, y0 = (int)y;
    int x1 = x0 + 1 < image->width ? x0 + 1 : x0;
    int y1 = y0 + 1 < image->height ? y0 + 1 : y0;
    double fx = x - x0, fy = y - y0;
    const uint8_t *p00 = image->rgba + ((size_t)y0 * image->width + x0) * 4U;
    const uint8_t *p01 = image->rgba + ((size_t)y0 * image->width + x1) * 4U;
    const uint8_t *p10 = image->rgba + ((size_t)y1 * image->width + x0) * 4U;
    const uint8_t *p11 = image->rgba + ((size_t)y1 * image->width + x1) * 4U;
    for (int c = 0; c < 4; ++c) {
        double top = p00[c] + (p01[c] - p00[c]) * fx;
        double bottom = p10[c] + (p11[c] - p10[c]) * fx;
        out[c] = (uint8_t)lround(top + (bottom - top) * fy);
    }
}

static const float *view_map(struct eye_view_map *map,
                             const struct ui_image *eye, uint32_t projection,
                             int fov_degrees, int width, int height)
{
    if (map->xy && map->eye_width == eye->width &&
        map->eye_height == eye->height && map->width == width &&
        map->height == height && map->projection == projection &&
        map->fov == fov_degrees)
        return map->xy;
    free(map->xy);
    map->xy = malloc((size_t)width * (size_t)height * 2U * sizeof(float));
    if (!map->xy)
        return NULL;
    double half_x = tan(fov_degrees * PI / 360.0);
    double half_y = half_x * height / width;
    double span = projection == NV12_PROJECTION_EQUIRECT_360 ? 2.0 * PI : PI;
    for (int y = 0; y < height; ++y) {
        double dy = (1.0 - (y + 0.5) * 2.0 / height) * half_y;
        for (int x = 0; x < width; ++x) {
            double dx = ((x + 0.5) * 2.0 / width - 1.0) * half_x;
            double longitude = atan2(dx, 1.0);
            double latitude = atan2(dy, sqrt(dx * dx + 1.0));
            float *out = map->xy + ((size_t)y * width + x) * 2U;
            out[0] = (float)((longitude / span + 0.5) * eye->width);
            out[1] = (float)((0.5 - latitude / PI) * eye->height);
        }
    }
    map->eye_width = eye->width;
    map->eye_height = eye->height;
    map->width = width;
    map->height = height;
    map->projection = projection;
    map->fov = fov_degrees;
    return map->xy;
}

struct ui_image *eye_view_straighten(const struct ui_image *eye,
                                     struct eye_view_map *map,
                                     uint32_t projection, int fov_degrees,
                                     int width, int height)
{
    if (width <= 0 || height <= 0)
        return NULL;
    const float *xy =
        view_map(map, eye, projection, fov_degrees, width, height);
    struct ui_image *view = xy ? ui_image_create(width, height) : NULL;
    if (!view)
        return NULL;
    for (size_t i = 0; i < (size_t)width * (size_t)height; ++i)
        sample(eye, xy[2 * i], xy[2 * i + 1], view->rgba + i * 4U);
    return view;
}
