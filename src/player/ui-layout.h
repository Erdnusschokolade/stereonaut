#ifndef STEREONAUT_UI_LAYOUT_H
#define STEREONAUT_UI_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "third_party/clay/clay.h"
#include "ui-draw.h"

/* Declarative panel layout on Clay (third_party/clay): a view describes
 * its boxes with CLAY() and the helpers below between ui_layout_begin and
 * ui_layout_end. The result holds the draw commands and the boxes of the
 * controls, so drawing (ui_layout_draw) and hit testing (ui_layout_hit)
 * use the same layout. Drawing goes through ui-draw.c.
 *
 * Clay has one global context, so begin..end hold a lock (the player panel
 * is laid out in a thread of its own and hit tested in the frame loop),
 * and text is measured with fonts of the layout's own. A view only lays
 * out between begin and end; it draws afterwards, outside the lock.
 *
 * Texts are copied into the layout: a view may pass temporary buffers.
 * The layout points into itself, so it must not be copied. */

enum ui_layout_font {
    UI_LAYOUT_REGULAR,
    UI_LAYOUT_BOLD,
    UI_LAYOUT_FONT_COUNT,
};

#define UI_LAYOUT_MAX_COMMANDS 256
#define UI_LAYOUT_MAX_CONTROLS 128
#define UI_LAYOUT_MAX_ITEMS 128
#define UI_LAYOUT_TEXT_BYTES 8192

/* Text the adapter draws itself, because Clay has neither an ellipsis nor
 * a line limit: a single line shortened to its box, or a paragraph wrapped
 * into at most `max_lines` lines (the box is that high). */
enum ui_layout_item_kind {
    UI_LAYOUT_LABEL,
    UI_LAYOUT_PARAGRAPH,
};

struct ui_layout_item {
    enum ui_layout_item_kind kind;
    enum ui_layout_font font;
    int size;
    struct ui_color color;
    enum ui_align align;
    int line_height, max_lines; /* paragraphs */
    const char *text;
};

enum ui_layout_command_kind {
    UI_LAYOUT_RECT,
    UI_LAYOUT_BORDER,
    UI_LAYOUT_TEXT,
    UI_LAYOUT_ITEM,
};

struct ui_layout_command {
    enum ui_layout_command_kind kind;
    float x, y, width, height; /* rounded to whole pixels */
    struct ui_color color;
    float radius, thickness;
    enum ui_layout_font font;
    int size;
    const char *text; /* UI_LAYOUT_TEXT: `length` bytes, not terminated */
    size_t length;
    const struct ui_layout_item *item;
};

struct ui_layout_control {
    int control;
    Clay_ElementId id;
    float x, y, width, height;
};

struct ui_layout {
    struct ui_layout_command commands[UI_LAYOUT_MAX_COMMANDS];
    int command_count;
    struct ui_layout_control controls[UI_LAYOUT_MAX_CONTROLS];
    int control_count;
    struct ui_layout_item items[UI_LAYOUT_MAX_ITEMS];
    int item_count;
    char text[UI_LAYOUT_TEXT_BYTES];
    size_t text_used;
};

/* Starts a layout of a width x height canvas (takes the lock). */
void ui_layout_begin(struct ui_layout *layout, int width, int height);
/* Computes the boxes and keeps the result (releases the lock). */
void ui_layout_end(struct ui_layout *layout);

/* Id of the element of `control` (unique within a layout); the control
 * is hit where that element lies. */
Clay_ElementId ui_layout_control(struct ui_layout *layout, int control);

Clay_Color ui_layout_color(struct ui_color color);

/* A line of text as wide as it is (Clay measures it), never shortened. */
void ui_layout_text(struct ui_layout *layout, const char *text,
                    enum ui_layout_font font, int size,
                    struct ui_color color);
/* A line of text in a box `width` wide and size + 4 high, shortened with
 * an ellipsis where it does not fit. */
void ui_layout_label(struct ui_layout *layout, const char *text,
                     enum ui_layout_font font, int size,
                     struct ui_color color, enum ui_align align,
                     Clay_SizingAxis width);
/* Wrapped text across the parent's width, max_lines * line_height high. */
void ui_layout_paragraph(struct ui_layout *layout, const char *text,
                         enum ui_layout_font font, int size,
                         struct ui_color color, int line_height,
                         int max_lines);

void ui_layout_draw(const struct ui_layout *layout, struct ui_canvas *canvas,
                    struct ui_font *const fonts[UI_LAYOUT_FONT_COUNT]);
/* Control at canvas pixel x/y (the last laid out wins), or `none`. */
int ui_layout_hit(const struct ui_layout *layout, float x, float y, int none);

#endif
