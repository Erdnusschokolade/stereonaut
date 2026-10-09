#ifndef STEREONAUT_UI_DRAW_H
#define STEREONAUT_UI_DRAW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* CPU drawing toolkit for the panels: anti-aliased shapes, images, icons and
 * FreeType text with fallback faces (CJK). The canvas holds premultiplied
 * 8-bit RGBA; `bgra` swaps red and blue for swapchains in BGRA order. Nothing
 * here knows about OpenXR or Vulkan. */

struct ui_canvas {
    uint8_t *pixels; /* width * height * 4 */
    int width, height;
    bool bgra;
};

struct ui_color {
    uint8_t r, g, b, a;
};
#define UI_RGBA(red, green, blue, alpha) \
    ((struct ui_color){(red), (green), (blue), (alpha)})

/* Straight-alpha RGBA picture, e.g. a decoded thumbnail. */
struct ui_image {
    int width, height;
    uint8_t *rgba;
};

struct ui_image *ui_image_create(int width, int height);
void ui_image_free(struct ui_image *image);
/* Area-averaging downscale (returns a new image; the source is untouched). */
struct ui_image *ui_image_scaled(const struct ui_image *source, int width,
                                 int height);

void ui_clear(struct ui_canvas *canvas, struct ui_color color);
void ui_copy(struct ui_canvas *target, const struct ui_canvas *source);

void ui_fill_rect(struct ui_canvas *canvas, int x, int y, int width,
                  int height, struct ui_color color);
/* Overwrites the pixels instead of blending (resets a region of a cached
 * layer to its background). */
void ui_set_rect(struct ui_canvas *canvas, int x, int y, int width,
                 int height, struct ui_color color);
/* Same with a raw canvas pixel (e.g. sampled from a cached layer). */
void ui_set_rect_raw(struct ui_canvas *canvas, int x, int y, int width,
                     int height, const uint8_t value[4]);
void ui_get_pixel_raw(const struct ui_canvas *canvas, int x, int y,
                      uint8_t value[4]);
void ui_fill_round_rect(struct ui_canvas *canvas, float x, float y,
                        float width, float height, float radius,
                        struct ui_color color);
void ui_stroke_round_rect(struct ui_canvas *canvas, float x, float y,
                          float width, float height, float radius,
                          float thickness, struct ui_color color);
/* Soft shadow below a rounded rectangle (`blur` pixels of falloff). */
void ui_shadow_round_rect(struct ui_canvas *canvas, float x, float y,
                          float width, float height, float radius,
                          float blur, struct ui_color color);
void ui_fill_circle(struct ui_canvas *canvas, float cx, float cy,
                    float radius, struct ui_color color);
void ui_fill_gradient_v(struct ui_canvas *canvas, int x, int y, int width,
                        int height, struct ui_color top,
                        struct ui_color bottom);
void ui_fill_polygon(struct ui_canvas *canvas, const float *points,
                     size_t count, struct ui_color color);
void ui_draw_line(struct ui_canvas *canvas, float x0, float y0, float x1,
                  float y1, float thickness, struct ui_color color);

/* Draws `image` stretched over the rectangle (centre-cropped to cover it)
 * with rounded corners. `dim` (0..1) darkens it for hover/disabled states. */
void ui_draw_image(struct ui_canvas *canvas, const struct ui_image *image,
                   float x, float y, float width, float height, float radius,
                   float dim);

enum ui_icon {
    UI_ICON_FOLDER,
    UI_ICON_VIDEO,
    UI_ICON_BACK,
    UI_ICON_HOME,
    UI_ICON_DISK,
    UI_ICON_SERVER,
    UI_ICON_POWER,
    UI_ICON_CHEVRON_LEFT,
    UI_ICON_CHEVRON_RIGHT,
    UI_ICON_PLAY,
    UI_ICON_PAUSE,
    UI_ICON_SKIP_BACK,
    UI_ICON_SKIP_FORWARD,
    UI_ICON_EXIT,
    UI_ICON_CUBE,
    UI_ICON_SWAP,
    UI_ICON_INFO,
    UI_ICON_USER,
    UI_ICON_ARROW_UP,
    UI_ICON_ARROW_DOWN,
    UI_ICON_CHEVRON_DOWN,
};
/* Icon centred at (cx, cy), fitted into a `size` x `size` square. */
void ui_draw_icon(struct ui_canvas *canvas, enum ui_icon icon, float cx,
                  float cy, float size, struct ui_color color);

/* Fonts. Regular/bold Noto Sans with CJK and symbol fallbacks; the search
 * paths can be overridden with STEREONAUT_FONT_REGULAR / _BOLD / _FALLBACK. */
struct ui_font;
struct ui_font *ui_font_load(bool bold);
void ui_font_free(struct ui_font *font);
bool ui_font_ready(const struct ui_font *font);

enum ui_align { UI_ALIGN_LEFT, UI_ALIGN_CENTER, UI_ALIGN_RIGHT };

int ui_text_width(struct ui_font *font, int pixel_size, const char *text);
/* Draws a single line with its top at `y` (ascent-based baseline). With
 * `max_width` > 0 the text is shortened with an ellipsis. Returns the drawn
 * width. */
int ui_draw_text(struct ui_canvas *canvas, struct ui_font *font,
                 int pixel_size, int x, int y, int max_width,
                 enum ui_align align, struct ui_color color,
                 const char *text);
/* Greedy word wrap inside [x, x + width); returns the number of lines
 * drawn (limited by `max_lines`, the last one ellipsized). */
int ui_draw_text_wrapped(struct ui_canvas *canvas, struct ui_font *font,
                         int pixel_size, int x, int y, int width,
                         int line_height, int max_lines, enum ui_align align,
                         struct ui_color color, const char *text);

#endif
