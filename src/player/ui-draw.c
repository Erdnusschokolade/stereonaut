#define _POSIX_C_SOURCE 200809L
#include "ui-draw.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

static float clampf(float value, float low, float high)
{
    return value < low ? low : value > high ? high : value;
}

/* ---- pixels ------------------------------------------------------------ */

/* Source-over of a straight-alpha colour with partial coverage onto the
 * premultiplied canvas, in 8-bit fixed point. */
static inline void blend(struct ui_canvas *canvas, int x, int y,
                         struct ui_color color, float coverage)
{
    if (x < 0 || y < 0 || x >= canvas->width || y >= canvas->height)
        return;
    unsigned alpha = (unsigned)(coverage * (float)color.a + 0.5f);
    if (!alpha)
        return;
    if (alpha > 255)
        alpha = 255;
    uint8_t *pixel = canvas->pixels + ((size_t)y * (size_t)canvas->width +
                                       (size_t)x) * 4U;
    int red = canvas->bgra ? 2 : 0, blue = canvas->bgra ? 0 : 2;
    unsigned keep = 255U - alpha;
    pixel[red] = (uint8_t)((color.r * alpha + pixel[red] * keep + 127U) / 255U);
    pixel[1] = (uint8_t)((color.g * alpha + pixel[1] * keep + 127U) / 255U);
    pixel[blue] = (uint8_t)((color.b * alpha + pixel[blue] * keep + 127U) /
                            255U);
    pixel[3] = (uint8_t)(alpha + (pixel[3] * keep + 127U) / 255U);
}

/* Blends `color` at full coverage over columns [left, right) of one row. */
static void blend_span(struct ui_canvas *canvas, int row, int left, int right,
                       struct ui_color color)
{
    if (row < 0 || row >= canvas->height || !color.a)
        return;
    if (left < 0)
        left = 0;
    if (right > canvas->width)
        right = canvas->width;
    int red = canvas->bgra ? 2 : 0, blue = canvas->bgra ? 0 : 2;
    unsigned alpha = color.a, keep = 255U - alpha;
    unsigned r = color.r * alpha, g = color.g * alpha, b = color.b * alpha;
    uint8_t *pixel = canvas->pixels + ((size_t)row * (size_t)canvas->width +
                                       (size_t)left) * 4U;
    for (int column = left; column < right; ++column, pixel += 4) {
        pixel[red] = (uint8_t)((r + pixel[red] * keep + 127U) / 255U);
        pixel[1] = (uint8_t)((g + pixel[1] * keep + 127U) / 255U);
        pixel[blue] = (uint8_t)((b + pixel[blue] * keep + 127U) / 255U);
        pixel[3] = (uint8_t)(alpha + (pixel[3] * keep + 127U) / 255U);
    }
}

void ui_clear(struct ui_canvas *canvas, struct ui_color color)
{
    float alpha = color.a / 255.0f;
    uint8_t value[4] = {
        (uint8_t)(color.r * alpha + 0.5f), (uint8_t)(color.g * alpha + 0.5f),
        (uint8_t)(color.b * alpha + 0.5f), color.a,
    };
    if (canvas->bgra) {
        uint8_t swap = value[0];
        value[0] = value[2];
        value[2] = swap;
    }
    size_t count = (size_t)canvas->width * (size_t)canvas->height;
    for (size_t i = 0; i < count; ++i)
        memcpy(canvas->pixels + i * 4U, value, 4);
}

void ui_copy(struct ui_canvas *target, const struct ui_canvas *source)
{
    memcpy(target->pixels, source->pixels,
           (size_t)source->width * (size_t)source->height * 4U);
}

void ui_fill_rect(struct ui_canvas *canvas, int x, int y, int width,
                  int height, struct ui_color color)
{
    for (int row = y; row < y + height; ++row)
        for (int column = x; column < x + width; ++column)
            blend(canvas, column, row, color, 1.0f);
}

void ui_set_rect(struct ui_canvas *canvas, int x, int y, int width,
                 int height, struct ui_color color)
{
    float alpha = color.a / 255.0f;
    uint8_t value[4] = {
        (uint8_t)(color.r * alpha + 0.5f), (uint8_t)(color.g * alpha + 0.5f),
        (uint8_t)(color.b * alpha + 0.5f), color.a,
    };
    if (canvas->bgra) {
        uint8_t swap = value[0];
        value[0] = value[2];
        value[2] = swap;
    }
    ui_set_rect_raw(canvas, x, y, width, height, value);
}

void ui_get_pixel_raw(const struct ui_canvas *canvas, int x, int y,
                      uint8_t value[4])
{
    memcpy(value, canvas->pixels + ((size_t)y * (size_t)canvas->width +
                                    (size_t)x) * 4U, 4);
}

void ui_set_rect_raw(struct ui_canvas *canvas, int x, int y, int width,
                     int height, const uint8_t value[4])
{
    for (int row = y < 0 ? 0 : y; row < y + height && row < canvas->height;
         ++row)
        for (int column = x < 0 ? 0 : x;
             column < x + width && column < canvas->width; ++column)
            memcpy(canvas->pixels + ((size_t)row * (size_t)canvas->width +
                                     (size_t)column) * 4U,
                   value, 4);
}

/* Signed distance from a pixel centre to a rounded rectangle. */
static float round_rect_distance(float px, float py, float x, float y,
                                 float width, float height, float radius)
{
    float half_w = width * 0.5f, half_h = height * 0.5f;
    radius = fminf(radius, fminf(half_w, half_h));
    float qx = fabsf(px - (x + half_w)) - (half_w - radius);
    float qy = fabsf(py - (y + half_h)) - (half_h - radius);
    if (qx <= 0.0f || qy <= 0.0f) /* straight edges: no square root */
        return fmaxf(qx, qy) - radius;
    return sqrtf(qx * qx + qy * qy) - radius;
}

/* Fills a convex shape row by row: the edge pixels are evaluated from both
 * sides until full coverage, the inside is a constant span. */
typedef float (*coverage_fn)(float px, float py, const float *shape);

static void fill_convex(struct ui_canvas *canvas, int left, int top,
                        int right, int bottom, coverage_fn coverage,
                        const float *shape, struct ui_color color)
{
    if (top < 0)
        top = 0;
    if (bottom > canvas->height)
        bottom = canvas->height;
    for (int row = top; row < bottom; ++row) {
        float py = row + 0.5f;
        int start = left, end = right;
        for (; start < right; ++start) {
            float c = coverage(start + 0.5f, py, shape);
            if (c >= 1.0f)
                break;
            blend(canvas, start, row, color, c);
        }
        if (start == right)
            continue;
        for (; end > start; --end) {
            float c = coverage(end - 0.5f, py, shape);
            if (c >= 1.0f)
                break;
            blend(canvas, end - 1, row, color, c);
        }
        blend_span(canvas, row, start, end, color);
    }
}

static float round_rect_coverage(float px, float py, const float *shape)
{
    return clampf(0.5f - round_rect_distance(px, py, shape[0], shape[1],
                                             shape[2], shape[3], shape[4]),
                  0, 1);
}

static float shadow_coverage(float px, float py, const float *shape)
{
    float distance = round_rect_distance(px, py, shape[0], shape[1],
                                         shape[2], shape[3], shape[4]);
    float blur = shape[5];
    float t = clampf(1.0f - (distance + blur * 0.25f) / blur, 0, 1);
    return t * t * (3.0f - 2.0f * t);
}

void ui_fill_round_rect(struct ui_canvas *canvas, float x, float y,
                        float width, float height, float radius,
                        struct ui_color color)
{
    const float shape[5] = {x, y, width, height, radius};
    fill_convex(canvas, (int)floorf(x), (int)floorf(y), (int)ceilf(x + width),
                (int)ceilf(y + height), round_rect_coverage, shape, color);
}

void ui_stroke_round_rect(struct ui_canvas *canvas, float x, float y,
                          float width, float height, float radius,
                          float thickness, struct ui_color color)
{
    int left = (int)floorf(x) - 1, top = (int)floorf(y) - 1;
    int right = (int)ceilf(x + width) + 1, bottom = (int)ceilf(y + height) + 1;
    for (int row = top; row < bottom; ++row)
        for (int column = left; column < right; ++column) {
            float distance = round_rect_distance(
                column + 0.5f, row + 0.5f, x, y, width, height, radius);
            float coverage = 0.5f + thickness * 0.5f -
                             fabsf(distance + thickness * 0.5f);
            blend(canvas, column, row, color, clampf(coverage, 0, 1));
        }
}

void ui_shadow_round_rect(struct ui_canvas *canvas, float x, float y,
                          float width, float height, float radius,
                          float blur, struct ui_color color)
{
    const float shape[6] = {x, y, width, height, radius, blur};
    fill_convex(canvas, (int)floorf(x - blur), (int)floorf(y - blur),
                (int)ceilf(x + width + blur), (int)ceilf(y + height + blur),
                shadow_coverage, shape, color);
}

void ui_fill_circle(struct ui_canvas *canvas, float cx, float cy,
                    float radius, struct ui_color color)
{
    ui_fill_round_rect(canvas, cx - radius, cy - radius, radius * 2.0f,
                       radius * 2.0f, radius, color);
}

void ui_fill_gradient_v(struct ui_canvas *canvas, int x, int y, int width,
                        int height, struct ui_color top,
                        struct ui_color bottom)
{
    for (int row = 0; row < height; ++row) {
        float t = height > 1 ? (float)row / (float)(height - 1) : 0.0f;
        struct ui_color color = {
            (uint8_t)(top.r + (bottom.r - top.r) * t),
            (uint8_t)(top.g + (bottom.g - top.g) * t),
            (uint8_t)(top.b + (bottom.b - top.b) * t),
            (uint8_t)(top.a + (bottom.a - top.a) * t),
        };
        for (int column = x; column < x + width; ++column)
            blend(canvas, column, y + row, color, 1.0f);
    }
}

/* Even-odd polygon fill, 4x4 supersampled. */
void ui_fill_polygon(struct ui_canvas *canvas, const float *points,
                     size_t count, struct ui_color color)
{
    if (count < 3)
        return;
    float min_x = points[0], max_x = points[0];
    float min_y = points[1], max_y = points[1];
    for (size_t i = 1; i < count; ++i) {
        min_x = fminf(min_x, points[i * 2]);
        max_x = fmaxf(max_x, points[i * 2]);
        min_y = fminf(min_y, points[i * 2 + 1]);
        max_y = fmaxf(max_y, points[i * 2 + 1]);
    }
    for (int row = (int)floorf(min_y); row < (int)ceilf(max_y); ++row)
        for (int column = (int)floorf(min_x); column < (int)ceilf(max_x);
             ++column) {
            int inside_samples = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    float px = column + (sx + 0.5f) / 4.0f;
                    float py = row + (sy + 0.5f) / 4.0f;
                    bool inside = false;
                    for (size_t i = 0, j = count - 1; i < count; j = i++) {
                        float xi = points[i * 2], yi = points[i * 2 + 1];
                        float xj = points[j * 2], yj = points[j * 2 + 1];
                        if ((yi > py) != (yj > py) &&
                            px < (xj - xi) * (py - yi) / (yj - yi) + xi)
                            inside = !inside;
                    }
                    inside_samples += inside;
                }
            if (inside_samples)
                blend(canvas, column, row, color, inside_samples / 16.0f);
        }
}

void ui_draw_line(struct ui_canvas *canvas, float x0, float y0, float x1,
                  float y1, float thickness, struct ui_color color)
{
    float dx = x1 - x0, dy = y1 - y0;
    float length = hypotf(dx, dy);
    if (length <= 0.0f)
        return;
    /* Capsule: distance to the segment, round caps. */
    float half = thickness * 0.5f;
    int left = (int)floorf(fminf(x0, x1) - half) - 1;
    int right = (int)ceilf(fmaxf(x0, x1) + half) + 1;
    int top = (int)floorf(fminf(y0, y1) - half) - 1;
    int bottom = (int)ceilf(fmaxf(y0, y1) + half) + 1;
    for (int row = top; row < bottom; ++row)
        for (int column = left; column < right; ++column) {
            float px = column + 0.5f - x0, py = row + 0.5f - y0;
            float t = clampf((px * dx + py * dy) / (length * length), 0, 1);
            float distance = hypotf(px - dx * t, py - dy * t) - half;
            blend(canvas, column, row, color, clampf(0.5f - distance, 0, 1));
        }
}

/* ---- images ------------------------------------------------------------ */

struct ui_image *ui_image_create(int width, int height)
{
    struct ui_image *image = calloc(1, sizeof(*image));
    if (!image)
        return NULL;
    image->rgba = calloc((size_t)width * (size_t)height, 4U);
    if (!image->rgba) {
        free(image);
        return NULL;
    }
    image->width = width;
    image->height = height;
    return image;
}

void ui_image_free(struct ui_image *image)
{
    if (!image)
        return;
    free(image->rgba);
    free(image);
}

struct ui_image *ui_image_scaled(const struct ui_image *source, int width,
                                 int height)
{
    struct ui_image *result = ui_image_create(width, height);
    if (!result)
        return NULL;
    for (int y = 0; y < height; ++y) {
        int sy0 = y * source->height / height;
        int sy1 = (y + 1) * source->height / height;
        if (sy1 <= sy0)
            sy1 = sy0 + 1;
        for (int x = 0; x < width; ++x) {
            int sx0 = x * source->width / width;
            int sx1 = (x + 1) * source->width / width;
            if (sx1 <= sx0)
                sx1 = sx0 + 1;
            unsigned sum[4] = {0};
            unsigned samples = 0;
            for (int sy = sy0; sy < sy1 && sy < source->height; ++sy)
                for (int sx = sx0; sx < sx1 && sx < source->width; ++sx) {
                    const uint8_t *p = source->rgba +
                        ((size_t)sy * (size_t)source->width + (size_t)sx) * 4U;
                    for (int c = 0; c < 4; ++c)
                        sum[c] += p[c];
                    ++samples;
                }
            uint8_t *q = result->rgba + ((size_t)y * (size_t)width +
                                         (size_t)x) * 4U;
            for (int c = 0; c < 4; ++c)
                q[c] = (uint8_t)(samples ? sum[c] / samples : 0);
        }
    }
    return result;
}

void ui_draw_image(struct ui_canvas *canvas, const struct ui_image *image,
                   float x, float y, float width, float height, float radius,
                   float dim)
{
    if (!image || image->width <= 0 || image->height <= 0)
        return;
    /* Cover: scale so the image fills the rectangle, crop the overflow. */
    float scale = fmaxf(width / image->width, height / image->height);
    float offset_x = (image->width * scale - width) * 0.5f;
    float offset_y = (image->height * scale - height) * 0.5f;
    float shade = 1.0f - clampf(dim, 0, 1);
    int left = (int)floorf(x), top = (int)floorf(y);
    int right = (int)ceilf(x + width), bottom = (int)ceilf(y + height);
    for (int row = top; row < bottom; ++row)
        for (int column = left; column < right; ++column) {
            float distance = round_rect_distance(
                column + 0.5f, row + 0.5f, x, y, width, height, radius);
            float coverage = clampf(0.5f - distance, 0, 1);
            if (coverage <= 0.0f)
                continue;
            float sx = (column + 0.5f - x + offset_x) / scale - 0.5f;
            float sy = (row + 0.5f - y + offset_y) / scale - 0.5f;
            int ix = (int)floorf(sx), iy = (int)floorf(sy);
            float fx = sx - ix, fy = sy - iy;
            float channel[4] = {0};
            for (int k = 0; k < 4; ++k) {
                int cx = ix + (k & 1), cy = iy + (k >> 1);
                cx = cx < 0 ? 0 : cx >= image->width ? image->width - 1 : cx;
                cy = cy < 0 ? 0 : cy >= image->height ? image->height - 1 : cy;
                float weight = ((k & 1) ? fx : 1 - fx) *
                               ((k >> 1) ? fy : 1 - fy);
                const uint8_t *p = image->rgba +
                    ((size_t)cy * (size_t)image->width + (size_t)cx) * 4U;
                for (int c = 0; c < 4; ++c)
                    channel[c] += p[c] * weight;
            }
            struct ui_color color = {
                (uint8_t)(channel[0] * shade), (uint8_t)(channel[1] * shade),
                (uint8_t)(channel[2] * shade), (uint8_t)channel[3],
            };
            blend(canvas, column, row, color, coverage);
        }
}

/* ---- icons ------------------------------------------------------------- */

static void icon_polygon(struct ui_canvas *canvas, float cx, float cy,
                         float size, const float *unit, size_t count,
                         struct ui_color color)
{
    float points[64];
    for (size_t i = 0; i < count && i < 32; ++i) {
        points[i * 2] = cx + (unit[i * 2] - 0.5f) * size;
        points[i * 2 + 1] = cy + (unit[i * 2 + 1] - 0.5f) * size;
    }
    ui_fill_polygon(canvas, points, count < 32 ? count : 32, color);
}

static void icon_line(struct ui_canvas *canvas, float cx, float cy,
                      float size, float x0, float y0, float x1, float y1,
                      float width, struct ui_color color)
{
    ui_draw_line(canvas, cx + (x0 - 0.5f) * size, cy + (y0 - 0.5f) * size,
                 cx + (x1 - 0.5f) * size, cy + (y1 - 0.5f) * size,
                 width * size, color);
}

static void icon_arc(struct ui_canvas *canvas, float cx, float cy, float size,
                     float ux, float uy, float radius, float start,
                     float end, float width, struct ui_color color)
{
    const int steps = 24;
    float px = 0, py = 0;
    for (int i = 0; i <= steps; ++i) {
        float angle = start + (end - start) * i / steps;
        float x = ux + cosf(angle) * radius, y = uy + sinf(angle) * radius;
        if (i)
            icon_line(canvas, cx, cy, size, px, py, x, y, width, color);
        px = x;
        py = y;
    }
}

void ui_draw_icon(struct ui_canvas *canvas, enum ui_icon icon, float cx,
                  float cy, float size, struct ui_color color)
{
    const float stroke = 0.09f;
    const float pi = 3.14159265f;
    switch (icon) {
    case UI_ICON_FOLDER: {
        static const float tab[] = {0.08f, 0.22f, 0.40f, 0.22f, 0.48f, 0.32f,
                                    0.92f, 0.32f, 0.92f, 0.80f, 0.08f, 0.80f};
        icon_polygon(canvas, cx, cy, size, tab, 6, color);
        break;
    }
    case UI_ICON_VIDEO:
        ui_stroke_round_rect(canvas, cx - size * 0.42f, cy - size * 0.32f,
                             size * 0.84f, size * 0.64f, size * 0.1f,
                             size * stroke, color);
        {
            static const float play[] = {0.42f, 0.36f, 0.64f, 0.5f,
                                         0.42f, 0.64f};
            icon_polygon(canvas, cx, cy, size, play, 3, color);
        }
        break;
    case UI_ICON_BACK:
        icon_line(canvas, cx, cy, size, 0.2f, 0.5f, 0.82f, 0.5f, stroke,
                  color);
        icon_line(canvas, cx, cy, size, 0.2f, 0.5f, 0.45f, 0.25f, stroke,
                  color);
        icon_line(canvas, cx, cy, size, 0.2f, 0.5f, 0.45f, 0.75f, stroke,
                  color);
        break;
    case UI_ICON_HOME: {
        static const float roof[] = {0.5f, 0.12f, 0.92f, 0.5f, 0.8f, 0.5f,
                                     0.8f, 0.88f, 0.58f, 0.88f, 0.58f, 0.64f,
                                     0.42f, 0.64f, 0.42f, 0.88f, 0.2f, 0.88f,
                                     0.2f, 0.5f, 0.08f, 0.5f};
        icon_polygon(canvas, cx, cy, size, roof, 11, color);
        break;
    }
    case UI_ICON_DISK:
        ui_stroke_round_rect(canvas, cx - size * 0.4f, cy - size * 0.22f,
                             size * 0.8f, size * 0.44f, size * 0.08f,
                             size * stroke, color);
        ui_fill_circle(canvas, cx + size * 0.24f, cy, size * 0.05f, color);
        break;
    case UI_ICON_SERVER:
        for (int i = 0; i < 2; ++i) {
            float top = cy - size * 0.36f + i * size * 0.4f;
            ui_stroke_round_rect(canvas, cx - size * 0.4f, top, size * 0.8f,
                                 size * 0.32f, size * 0.07f, size * stroke,
                                 color);
            ui_fill_circle(canvas, cx - size * 0.22f, top + size * 0.16f,
                           size * 0.05f, color);
        }
        break;
    case UI_ICON_POWER:
        icon_arc(canvas, cx, cy, size, 0.5f, 0.54f, 0.32f, -pi * 0.3f,
                 pi * 1.3f, stroke, color);
        icon_line(canvas, cx, cy, size, 0.5f, 0.12f, 0.5f, 0.5f, stroke,
                  color);
        break;
    case UI_ICON_CHEVRON_LEFT:
        icon_line(canvas, cx, cy, size, 0.62f, 0.2f, 0.36f, 0.5f, stroke,
                  color);
        icon_line(canvas, cx, cy, size, 0.36f, 0.5f, 0.62f, 0.8f, stroke,
                  color);
        break;
    case UI_ICON_CHEVRON_RIGHT:
        icon_line(canvas, cx, cy, size, 0.38f, 0.2f, 0.64f, 0.5f, stroke,
                  color);
        icon_line(canvas, cx, cy, size, 0.64f, 0.5f, 0.38f, 0.8f, stroke,
                  color);
        break;
    case UI_ICON_PLAY: {
        static const float play[] = {0.28f, 0.14f, 0.86f, 0.5f, 0.28f, 0.86f};
        icon_polygon(canvas, cx, cy, size, play, 3, color);
        break;
    }
    case UI_ICON_PAUSE:
        ui_fill_round_rect(canvas, cx - size * 0.3f, cy - size * 0.36f,
                           size * 0.2f, size * 0.72f, size * 0.04f, color);
        ui_fill_round_rect(canvas, cx + size * 0.1f, cy - size * 0.36f,
                           size * 0.2f, size * 0.72f, size * 0.04f, color);
        break;
    case UI_ICON_SKIP_BACK:
    case UI_ICON_SKIP_FORWARD: {
        bool forward = icon == UI_ICON_SKIP_FORWARD;
        for (int i = 0; i < 2; ++i) {
            float o = i * 0.36f;
            float tri[6] = {0.12f + o, 0.22f, 0.5f + o, 0.5f, 0.12f + o, 0.78f};
            if (!forward)
                for (int k = 0; k < 3; ++k)
                    tri[k * 2] = 1.0f - tri[k * 2];
            icon_polygon(canvas, cx, cy, size, tri, 3, color);
        }
        break;
    }
    case UI_ICON_EXIT:
        icon_line(canvas, cx, cy, size, 0.62f, 0.18f, 0.24f, 0.18f, stroke,
                  color);
        icon_line(canvas, cx, cy, size, 0.24f, 0.18f, 0.24f, 0.82f, stroke,
                  color);
        icon_line(canvas, cx, cy, size, 0.24f, 0.82f, 0.62f, 0.82f, stroke,
                  color);
        icon_line(canvas, cx, cy, size, 0.44f, 0.5f, 0.9f, 0.5f, stroke,
                  color);
        icon_line(canvas, cx, cy, size, 0.44f, 0.5f, 0.62f, 0.32f, stroke,
                  color);
        icon_line(canvas, cx, cy, size, 0.44f, 0.5f, 0.62f, 0.68f, stroke,
                  color);
        break;
    case UI_ICON_CUBE:
        icon_line(canvas, cx, cy, size, 0.5f, 0.1f, 0.88f, 0.3f, stroke, color);
        icon_line(canvas, cx, cy, size, 0.88f, 0.3f, 0.88f, 0.72f, stroke,
                  color);
        icon_line(canvas, cx, cy, size, 0.88f, 0.72f, 0.5f, 0.92f, stroke,
                  color);
        icon_line(canvas, cx, cy, size, 0.5f, 0.92f, 0.12f, 0.72f, stroke,
                  color);
        icon_line(canvas, cx, cy, size, 0.12f, 0.72f, 0.12f, 0.3f, stroke,
                  color);
        icon_line(canvas, cx, cy, size, 0.12f, 0.3f, 0.5f, 0.1f, stroke, color);
        icon_line(canvas, cx, cy, size, 0.12f, 0.3f, 0.5f, 0.5f, stroke, color);
        icon_line(canvas, cx, cy, size, 0.88f, 0.3f, 0.5f, 0.5f, stroke, color);
        icon_line(canvas, cx, cy, size, 0.5f, 0.5f, 0.5f, 0.92f, stroke, color);
        break;
    case UI_ICON_SWAP:
        icon_line(canvas, cx, cy, size, 0.14f, 0.34f, 0.84f, 0.34f, stroke,
                  color);
        icon_line(canvas, cx, cy, size, 0.84f, 0.34f, 0.66f, 0.18f, stroke,
                  color);
        icon_line(canvas, cx, cy, size, 0.86f, 0.66f, 0.16f, 0.66f, stroke,
                  color);
        icon_line(canvas, cx, cy, size, 0.16f, 0.66f, 0.34f, 0.82f, stroke,
                  color);
        break;
    case UI_ICON_INFO:
        icon_arc(canvas, cx, cy, size, 0.5f, 0.5f, 0.4f, 0.0f, 2.0f * pi,
                 stroke, color);
        icon_line(canvas, cx, cy, size, 0.5f, 0.46f, 0.5f, 0.72f, stroke,
                  color);
        ui_fill_circle(canvas, cx, cy - size * 0.2f, size * 0.06f, color);
        break;
    case UI_ICON_ARROW_UP:
        icon_line(canvas, cx, cy, size, 0.5f, 0.84f, 0.5f, 0.18f, stroke,
                  color);
        icon_line(canvas, cx, cy, size, 0.5f, 0.18f, 0.26f, 0.44f, stroke,
                  color);
        icon_line(canvas, cx, cy, size, 0.5f, 0.18f, 0.74f, 0.44f, stroke,
                  color);
        break;
    case UI_ICON_ARROW_DOWN:
        icon_line(canvas, cx, cy, size, 0.5f, 0.16f, 0.5f, 0.82f, stroke,
                  color);
        icon_line(canvas, cx, cy, size, 0.5f, 0.82f, 0.26f, 0.56f, stroke,
                  color);
        icon_line(canvas, cx, cy, size, 0.5f, 0.82f, 0.74f, 0.56f, stroke,
                  color);
        break;
    case UI_ICON_CHEVRON_DOWN:
        icon_line(canvas, cx, cy, size, 0.2f, 0.36f, 0.5f, 0.66f, stroke,
                  color);
        icon_line(canvas, cx, cy, size, 0.5f, 0.66f, 0.8f, 0.36f, stroke,
                  color);
        break;
    case UI_ICON_USER:
        ui_fill_circle(canvas, cx, cy - size * 0.16f, size * 0.18f, color);
        icon_arc(canvas, cx, cy, size, 0.5f, 0.92f, 0.32f, pi, 2.0f * pi,
                 stroke * 1.6f, color);
        break;
    case UI_ICON_SEARCH:
        icon_arc(canvas, cx, cy, size, 0.42f, 0.42f, 0.26f, 0.0f, 2.0f * pi,
                 stroke, color);
        icon_line(canvas, cx, cy, size, 0.62f, 0.62f, 0.86f, 0.86f,
                  stroke * 1.3f, color);
        break;
    case UI_ICON_SETTINGS:
        icon_arc(canvas, cx, cy, size, 0.5f, 0.5f, 0.22f, 0.0f, 2.0f * pi,
                 stroke * 1.3f, color);
        for (int tooth = 0; tooth < 8; ++tooth) {
            float angle = (float)tooth * pi / 4.0f;
            float c = cosf(angle), s = sinf(angle);
            icon_line(canvas, cx, cy, size, 0.5f + 0.3f * c, 0.5f + 0.3f * s,
                      0.5f + 0.42f * c, 0.5f + 0.42f * s, stroke * 1.4f,
                      color);
        }
        break;
    case UI_ICON_EYE:
        /* Upper and lower lid as arcs of circles below and above. */
        icon_arc(canvas, cx, cy, size, 0.5f, 0.78f, 0.42f, pi + 0.73f,
                 2.0f * pi - 0.73f, stroke, color);
        icon_arc(canvas, cx, cy, size, 0.5f, 0.22f, 0.42f, 0.73f,
                 pi - 0.73f, stroke, color);
        ui_fill_circle(canvas, cx, cy, size * 0.1f, color);
        break;
    }
}

/* ---- text -------------------------------------------------------------- */

#define FONT_FACES 4
#define GLYPH_CACHE_SIZE 2048

struct glyph {
    bool used;
    uint8_t face;
    uint16_t pixel_size;
    uint32_t index;
    int left, top, width, height, advance;
    uint8_t *bitmap;
};

struct ui_font {
    FT_Library library;
    FT_Face faces[FONT_FACES];
    int face_count;
    struct glyph cache[GLYPH_CACHE_SIZE];
};

/* Arch and SteamOS, Debian and Ubuntu, Fedora (whose Noto Sans is a
 * variable font only: DejaVu comes first there, it has a bold face). */
static const char *const regular_paths[] = {
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
    "/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf",
    "/usr/share/fonts/cantarell/Cantarell-VF.otf",
};
static const char *const bold_paths[] = {
    "/usr/share/fonts/noto/NotoSans-SemiBold.ttf",
    "/usr/share/fonts/noto/NotoSans-Bold.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
    "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf",
};
/* CJK (the first found), then symbols (the first found). */
static const char *const fallback_paths[] = {
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Medium.ttc",
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Light.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/google-noto-sans-cjk-fonts/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/google-noto-sans-cjk-vf-fonts/NotoSansCJK-VF.ttc",
};
static const char *const symbol_paths[] = {
    "/usr/share/fonts/noto/NotoSansSymbols2-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf",
    "/usr/share/fonts/google-noto/NotoSansSymbols2-Regular.ttf",
};

static bool add_face(struct ui_font *font, const char *path)
{
    if (font->face_count >= FONT_FACES || !path || access(path, R_OK) < 0)
        return false;
    FT_Face face;
    if (FT_New_Face(font->library, path, 0, &face) != 0)
        return false;
    font->faces[font->face_count++] = face;
    return true;
}

static void add_first(struct ui_font *font, const char *environment,
                      const char *const *paths, size_t count)
{
    if (add_face(font, getenv(environment)))
        return;
    for (size_t i = 0; i < count; ++i)
        if (add_face(font, paths[i]))
            return;
}

struct ui_font *ui_font_load(bool bold)
{
    struct ui_font *font = calloc(1, sizeof(*font));
    if (!font || FT_Init_FreeType(&font->library) != 0) {
        free(font);
        return NULL;
    }
    if (bold)
        add_first(font, "STEREONAUT_FONT_BOLD", bold_paths,
                  ARRAY_COUNT(bold_paths));
    if (!font->face_count)
        add_first(font, "STEREONAUT_FONT_REGULAR", regular_paths,
                  ARRAY_COUNT(regular_paths));
    add_first(font, "STEREONAUT_FONT_FALLBACK", fallback_paths,
              ARRAY_COUNT(fallback_paths));
    for (size_t i = 0; i < ARRAY_COUNT(symbol_paths); ++i)
        if (add_face(font, symbol_paths[i]))
            break;
    if (!font->face_count)
        fprintf(stderr, "ui: no usable font found\n");
    return font;
}

void ui_font_free(struct ui_font *font)
{
    if (!font)
        return;
    for (size_t i = 0; i < GLYPH_CACHE_SIZE; ++i)
        free(font->cache[i].bitmap);
    for (int i = 0; i < font->face_count; ++i)
        FT_Done_Face(font->faces[i]);
    FT_Done_FreeType(font->library);
    free(font);
}

bool ui_font_ready(const struct ui_font *font)
{
    return font && font->face_count > 0;
}

static uint32_t next_codepoint(const char **cursor)
{
    const unsigned char *s = (const unsigned char *)*cursor;
    uint32_t code;
    int extra;
    if (s[0] < 0x80) {
        code = s[0];
        extra = 0;
    } else if ((s[0] & 0xE0) == 0xC0) {
        code = s[0] & 0x1F;
        extra = 1;
    } else if ((s[0] & 0xF0) == 0xE0) {
        code = s[0] & 0x0F;
        extra = 2;
    } else if ((s[0] & 0xF8) == 0xF0) {
        code = s[0] & 0x07;
        extra = 3;
    } else {
        *cursor += 1;
        return 0xFFFD;
    }
    for (int i = 1; i <= extra; ++i) {
        if ((s[i] & 0xC0) != 0x80) {
            *cursor += i;
            return 0xFFFD;
        }
        code = (code << 6) | (s[i] & 0x3F);
    }
    *cursor += 1 + extra;
    return code;
}

static const struct glyph *load_glyph(struct ui_font *font, uint32_t code,
                                      int pixel_size)
{
    int face_index = 0;
    FT_UInt index = 0;
    for (int i = 0; i < font->face_count; ++i) {
        index = FT_Get_Char_Index(font->faces[i], code);
        if (index) {
            face_index = i;
            break;
        }
    }
    uint32_t hash = (code * 2654435761U) ^ ((uint32_t)pixel_size * 40503U) ^
                    (uint32_t)face_index;
    struct glyph *slot = &font->cache[hash % GLYPH_CACHE_SIZE];
    if (slot->used && slot->face == face_index &&
        slot->pixel_size == pixel_size && slot->index == index)
        return slot;
    FT_Face face = font->faces[face_index];
    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)pixel_size);
    if (FT_Load_Glyph(face, index, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) != 0)
        return NULL;
    FT_GlyphSlot rendered = face->glyph;
    free(slot->bitmap);
    *slot = (struct glyph){
        .used = true,
        .face = (uint8_t)face_index,
        .pixel_size = (uint16_t)pixel_size,
        .index = index,
        .left = rendered->bitmap_left,
        .top = rendered->bitmap_top,
        .width = (int)rendered->bitmap.width,
        .height = (int)rendered->bitmap.rows,
        .advance = (int)((rendered->advance.x + 32) >> 6),
    };
    size_t bytes = (size_t)slot->width * (size_t)slot->height;
    if (bytes) {
        slot->bitmap = malloc(bytes);
        if (!slot->bitmap) {
            slot->used = false;
            return NULL;
        }
        for (int row = 0; row < slot->height; ++row)
            memcpy(slot->bitmap + (size_t)row * (size_t)slot->width,
                   rendered->bitmap.buffer +
                       (ptrdiff_t)row * rendered->bitmap.pitch,
                   (size_t)slot->width);
    }
    return slot;
}

static int ascent(struct ui_font *font, int pixel_size)
{
    FT_Face face = font->faces[0];
    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)pixel_size);
    return (int)(face->size->metrics.ascender >> 6);
}

/* Width of the first `byte_limit` bytes of `text` (all of it if < 0). */
static int measure(struct ui_font *font, int pixel_size, const char *text,
                   int byte_limit)
{
    int width = 0;
    const char *cursor = text;
    while (*cursor && (byte_limit < 0 || cursor - text < byte_limit)) {
        const struct glyph *glyph = load_glyph(font, next_codepoint(&cursor),
                                               pixel_size);
        if (glyph)
            width += glyph->advance;
    }
    return width;
}

int ui_text_width(struct ui_font *font, int pixel_size, const char *text)
{
    if (!ui_font_ready(font) || !text)
        return 0;
    return measure(font, pixel_size, text, -1);
}

static int draw_run(struct ui_canvas *canvas, struct ui_font *font,
                    int pixel_size, int x, int baseline, struct ui_color color,
                    const char *text, int byte_limit)
{
    int pen = x;
    const char *cursor = text;
    while (*cursor && (byte_limit < 0 || cursor - text < byte_limit)) {
        const struct glyph *glyph = load_glyph(font, next_codepoint(&cursor),
                                               pixel_size);
        if (!glyph)
            continue;
        for (int row = 0; row < glyph->height; ++row)
            for (int column = 0; column < glyph->width; ++column) {
                uint8_t value = glyph->bitmap[(size_t)row *
                                              (size_t)glyph->width +
                                              (size_t)column];
                if (value)
                    blend(canvas, pen + glyph->left + column,
                          baseline - glyph->top + row, color, value / 255.0f);
            }
        pen += glyph->advance;
    }
    return pen - x;
}

int ui_draw_text(struct ui_canvas *canvas, struct ui_font *font,
                 int pixel_size, int x, int y, int max_width,
                 enum ui_align align, struct ui_color color, const char *text)
{
    if (!ui_font_ready(font) || !text)
        return 0;
    static const char ellipsis[] = "\xE2\x80\xA6";
    int width = measure(font, pixel_size, text, -1);
    int limit = -1;
    bool shortened = false;
    if (max_width > 0 && width > max_width) {
        int ellipsis_width = measure(font, pixel_size, ellipsis, -1);
        /* Longest prefix ending on a code point boundary that still fits. */
        int best = 0;
        const char *cursor = text;
        while (*cursor) {
            const char *previous = cursor;
            next_codepoint(&cursor);
            int prefix = (int)(cursor - text);
            if (measure(font, pixel_size, text, prefix) + ellipsis_width >
                max_width) {
                cursor = previous;
                break;
            }
            best = prefix;
        }
        limit = best;
        shortened = true;
        width = measure(font, pixel_size, text, limit) + ellipsis_width;
    }
    int start = align == UI_ALIGN_CENTER ? x - width / 2
                : align == UI_ALIGN_RIGHT ? x - width
                                          : x;
    int baseline = y + ascent(font, pixel_size);
    int drawn = draw_run(canvas, font, pixel_size, start, baseline, color,
                         text, limit);
    if (shortened)
        draw_run(canvas, font, pixel_size, start + drawn, baseline, color,
                 ellipsis, -1);
    return width;
}

static bool breakable_after(uint32_t code)
{
    return code == ' ' || code == '-' || code == '/' || code == '_' ||
           code >= 0x2E80; /* CJK and later scripts break anywhere */
}

int ui_draw_text_wrapped(struct ui_canvas *canvas, struct ui_font *font,
                         int pixel_size, int x, int y, int width,
                         int line_height, int max_lines, enum ui_align align,
                         struct ui_color color, const char *text)
{
    if (!ui_font_ready(font) || !text)
        return 0;
    int lines = 0;
    const char *line = text;
    while (*line && lines < max_lines) {
        while (*line == ' ')
            ++line;
        int anchor = align == UI_ALIGN_CENTER ? x + width / 2
                     : align == UI_ALIGN_RIGHT ? x + width
                                               : x;
        if (lines == max_lines - 1) {
            ui_draw_text(canvas, font, pixel_size, anchor,
                         y + lines * line_height, width, align, color, line);
            return lines + 1;
        }
        const char *cursor = line, *last_break = NULL;
        while (*cursor) {
            const char *before = cursor;
            uint32_t code = next_codepoint(&cursor);
            if (measure(font, pixel_size, line, (int)(cursor - line)) >
                width) {
                cursor = before;
                break;
            }
            if (breakable_after(code))
                last_break = cursor;
        }
        const char *end = !*cursor ? cursor
                          : last_break && last_break > line ? last_break
                                                            : cursor;
        if (end == line) /* a single glyph wider than the box */
            next_codepoint(&end);
        int baseline = y + lines * line_height + ascent(font, pixel_size);
        int length = (int)(end - line);
        while (length > 0 && line[length - 1] == ' ')
            --length;
        int line_width = measure(font, pixel_size, line, length);
        int start = align == UI_ALIGN_CENTER ? anchor - line_width / 2
                    : align == UI_ALIGN_RIGHT ? anchor - line_width
                                              : x;
        draw_run(canvas, font, pixel_size, start, baseline, color, line,
                 length);
        ++lines;
        line = end;
    }
    return lines;
}
