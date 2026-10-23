#define _POSIX_C_SOURCE 200809L
#define CLAY_IMPLEMENTATION
#include "ui-layout.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Elements per layout; Clay sizes its arena (and its cache of measured
 * words, twice as many) from it. */
#define MAX_ELEMENTS 1024

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool initialized;
static struct ui_font *measure_fonts[UI_LAYOUT_FONT_COUNT];

static void report_error(Clay_ErrorData error)
{
    static bool reported[16];
    if ((unsigned)error.errorType < sizeof(reported) / sizeof(reported[0])) {
        if (reported[error.errorType])
            return;
        reported[error.errorType] = true;
    }
    fprintf(stderr, "ui-layout: %.*s\n", (int)error.errorText.length,
            error.errorText.chars);
}

/* Text boxes are size + 4 pixels high (ui_draw_text draws from the top);
 * this centres a line in a chip as the hand-placed layouts did. */
static Clay_Dimensions measure_text(Clay_StringSlice text,
                                    Clay_TextElementConfig *config,
                                    void *user_data)
{
    (void)user_data;
    struct ui_font *font =
        measure_fonts[config->fontId < UI_LAYOUT_FONT_COUNT ? config->fontId
                                                            : 0];
    return (Clay_Dimensions){
        (float)ui_text_width_n(font, config->fontSize, text.chars,
                               (size_t)text.length),
        (float)(config->fontSize + 4),
    };
}

static void initialize(void)
{
    measure_fonts[UI_LAYOUT_REGULAR] = ui_font_load(false);
    measure_fonts[UI_LAYOUT_BOLD] = ui_font_load(true);
    Clay_SetMaxElementCount(MAX_ELEMENTS);
    uint32_t size = Clay_MinMemorySize();
    void *memory = malloc(size);
    if (!memory) {
        fprintf(stderr, "ui-layout: no memory for the layout arena\n");
        abort();
    }
    Clay_Initialize(Clay_CreateArenaWithCapacityAndMemory(size, memory),
                    (Clay_Dimensions){1, 1},
                    (Clay_ErrorHandler){report_error, NULL});
    Clay_SetMeasureTextFunction(measure_text, NULL);
    initialized = true;
}

void ui_layout_begin(struct ui_layout *layout, int width, int height)
{
    pthread_mutex_lock(&lock);
    if (!initialized)
        initialize();
    layout->command_count = 0;
    layout->control_count = 0;
    layout->item_count = 0;
    layout->text_used = 0;
    Clay_SetLayoutDimensions((Clay_Dimensions){(float)width, (float)height});
    Clay_BeginLayout();
}

static struct ui_color to_color(Clay_Color color)
{
    return UI_RGBA((uint8_t)color.r, (uint8_t)color.g, (uint8_t)color.b,
                   (uint8_t)color.a);
}

void ui_layout_end(struct ui_layout *layout)
{
    Clay_RenderCommandArray commands = Clay_EndLayout();
    for (int32_t i = 0; i < commands.length; ++i) {
        const Clay_RenderCommand *source =
            Clay_RenderCommandArray_Get(&commands, i);
        if (layout->command_count >= UI_LAYOUT_MAX_COMMANDS) {
            fprintf(stderr, "ui-layout: more than %d draw commands\n",
                    UI_LAYOUT_MAX_COMMANDS);
            break;
        }
        struct ui_layout_command *command =
            &layout->commands[layout->command_count];
        float x = roundf(source->boundingBox.x);
        float y = roundf(source->boundingBox.y);
        *command = (struct ui_layout_command){
            .x = x,
            .y = y,
            .width = roundf(source->boundingBox.x +
                            source->boundingBox.width) - x,
            .height = roundf(source->boundingBox.y +
                             source->boundingBox.height) - y,
        };
        const Clay_RenderData *data = &source->renderData;
        switch (source->commandType) {
        case CLAY_RENDER_COMMAND_TYPE_RECTANGLE:
            command->kind = UI_LAYOUT_RECT;
            command->color = to_color(data->rectangle.backgroundColor);
            command->radius = data->rectangle.cornerRadius.topLeft;
            break;
        case CLAY_RENDER_COMMAND_TYPE_BORDER:
            command->kind = UI_LAYOUT_BORDER;
            command->color = to_color(data->border.color);
            command->radius = data->border.cornerRadius.topLeft;
            command->thickness = data->border.width.left;
            break;
        case CLAY_RENDER_COMMAND_TYPE_TEXT:
            command->kind = UI_LAYOUT_TEXT;
            command->color = to_color(data->text.textColor);
            command->font = data->text.fontId < UI_LAYOUT_FONT_COUNT
                                ? (enum ui_layout_font)data->text.fontId
                                : UI_LAYOUT_REGULAR;
            command->size = data->text.fontSize;
            command->text = data->text.stringContents.chars;
            command->length = (size_t)data->text.stringContents.length;
            break;
        case CLAY_RENDER_COMMAND_TYPE_CUSTOM:
            command->kind = UI_LAYOUT_ITEM;
            command->item = data->custom.customData;
            break;
        default: /* images, clipping: not used by the panels yet */
            continue;
        }
        ++layout->command_count;
    }
    for (int i = 0; i < layout->control_count; ++i) {
        struct ui_layout_control *control = &layout->controls[i];
        Clay_ElementData element = Clay_GetElementData(control->id);
        if (!element.found)
            continue;
        control->x = element.boundingBox.x;
        control->y = element.boundingBox.y;
        control->width = element.boundingBox.width;
        control->height = element.boundingBox.height;
    }
    pthread_mutex_unlock(&lock);
}

Clay_ElementId ui_layout_control(struct ui_layout *layout, int control)
{
    Clay_ElementId id = CLAY_IDI("control", (uint32_t)control);
    if (layout->control_count < UI_LAYOUT_MAX_CONTROLS)
        layout->controls[layout->control_count++] =
            (struct ui_layout_control){.control = control, .id = id};
    else
        fprintf(stderr, "ui-layout: more than %d controls\n",
                UI_LAYOUT_MAX_CONTROLS);
    return id;
}

Clay_Color ui_layout_color(struct ui_color color)
{
    return (Clay_Color){color.r, color.g, color.b, color.a};
}

/* Copy of `text` in the layout ("" once it is full). */
static Clay_String copy_text(struct ui_layout *layout, const char *text)
{
    size_t length = text ? strlen(text) : 0;
    if (length >= UI_LAYOUT_TEXT_BYTES - layout->text_used) {
        fprintf(stderr, "ui-layout: text storage full\n");
        return (Clay_String){.length = 0, .chars = ""};
    }
    char *copy = layout->text + layout->text_used;
    memcpy(copy, text ? text : "", length);
    copy[length] = '\0';
    layout->text_used += length + 1;
    return (Clay_String){.length = (int32_t)length, .chars = copy};
}

void ui_layout_text(struct ui_layout *layout, const char *text,
                    enum ui_layout_font font, int size,
                    struct ui_color color)
{
    CLAY_TEXT(copy_text(layout, text),
              CLAY_TEXT_CONFIG({.textColor = ui_layout_color(color),
                                .fontId = (uint16_t)font,
                                .fontSize = (uint16_t)size,
                                .wrapMode = CLAY_TEXT_WRAP_NONE}));
}

static struct ui_layout_item *add_item(struct ui_layout *layout,
                                       enum ui_layout_item_kind kind,
                                       const char *text,
                                       enum ui_layout_font font, int size,
                                       struct ui_color color)
{
    static struct ui_layout_item overflow;
    if (layout->item_count >= UI_LAYOUT_MAX_ITEMS) {
        fprintf(stderr, "ui-layout: more than %d drawn items\n",
                UI_LAYOUT_MAX_ITEMS);
        overflow = (struct ui_layout_item){.text = ""};
        return &overflow;
    }
    struct ui_layout_item *item = &layout->items[layout->item_count++];
    *item = (struct ui_layout_item){
        .kind = kind,
        .font = font,
        .size = size,
        .color = color,
        .align = UI_ALIGN_LEFT,
        .text = copy_text(layout, text).chars,
    };
    return item;
}

void ui_layout_label(struct ui_layout *layout, const char *text,
                     enum ui_layout_font font, int size,
                     struct ui_color color, enum ui_align align,
                     Clay_SizingAxis width)
{
    struct ui_layout_item *item =
        add_item(layout, UI_LAYOUT_LABEL, text, font, size, color);
    item->align = align;
    CLAY({.layout = {.sizing = {width, CLAY_SIZING_FIXED(size + 4)}},
          .custom = {.customData = item}}) {}
}

void ui_layout_paragraph(struct ui_layout *layout, const char *text,
                         enum ui_layout_font font, int size,
                         struct ui_color color, int line_height,
                         int max_lines)
{
    struct ui_layout_item *item =
        add_item(layout, UI_LAYOUT_PARAGRAPH, text, font, size, color);
    item->line_height = line_height;
    item->max_lines = max_lines;
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0),
                                CLAY_SIZING_FIXED(line_height * max_lines)}},
          .custom = {.customData = item}}) {}
}

void *ui_layout_card(struct ui_layout *layout,
                     const struct ui_layout_card *card)
{
    struct ui_layout_item *item = add_item(layout, UI_LAYOUT_CARD, NULL,
                                           UI_LAYOUT_REGULAR, 0,
                                           card->fill);
    item->card = *card;
    return item;
}

int ui_layout_line_count(const char *text, enum ui_layout_font font,
                         int size, int width, int max_lines)
{
    return ui_text_line_count(measure_fonts[font], size, width, max_lines,
                              text);
}

static void draw_item(const struct ui_layout_command *command,
                      struct ui_canvas *canvas,
                      struct ui_font *const fonts[UI_LAYOUT_FONT_COUNT])
{
    const struct ui_layout_item *item = command->item;
    int x = (int)command->x, y = (int)command->y;
    int width = (int)command->width;
    if (item->kind == UI_LAYOUT_CARD) {
        const struct ui_layout_card *card = &item->card;
        if (card->shadow.a)
            ui_shadow_round_rect(canvas, command->x,
                                 command->y + card->shadow_offset,
                                 command->width, command->height,
                                 card->radius, card->shadow_blur,
                                 card->shadow);
        ui_fill_round_rect(canvas, command->x, command->y, command->width,
                           command->height, card->radius, card->fill);
        if (card->border_width > 0.0f)
            ui_stroke_round_rect(canvas, command->x, command->y,
                                 command->width, command->height,
                                 card->radius, card->border_width,
                                 card->border);
        return;
    }
    if (item->kind == UI_LAYOUT_PARAGRAPH) {
        ui_draw_text_wrapped(canvas, fonts[item->font], item->size, x, y,
                             width, item->line_height, item->max_lines,
                             item->align, item->color, item->text);
        return;
    }
    int anchor = item->align == UI_ALIGN_CENTER ? x + width / 2
                 : item->align == UI_ALIGN_RIGHT ? x + width
                                                 : x;
    ui_draw_text(canvas, fonts[item->font], item->size, anchor, y, width,
                 item->align, item->color, item->text);
}

void ui_layout_draw(const struct ui_layout *layout, struct ui_canvas *canvas,
                    struct ui_font *const fonts[UI_LAYOUT_FONT_COUNT])
{
    for (int i = 0; i < layout->command_count; ++i) {
        const struct ui_layout_command *command = &layout->commands[i];
        switch (command->kind) {
        case UI_LAYOUT_RECT:
            if (command->radius > 0.0f)
                ui_fill_round_rect(canvas, command->x, command->y,
                                   command->width, command->height,
                                   command->radius, command->color);
            else
                ui_fill_rect(canvas, (int)command->x, (int)command->y,
                             (int)command->width, (int)command->height,
                             command->color);
            break;
        case UI_LAYOUT_BORDER:
            ui_stroke_round_rect(canvas, command->x, command->y,
                                 command->width, command->height,
                                 command->radius, command->thickness,
                                 command->color);
            break;
        case UI_LAYOUT_TEXT: {
            char line[1024];
            size_t length = command->length < sizeof(line) - 1
                                ? command->length
                                : sizeof(line) - 1;
            memcpy(line, command->text, length);
            line[length] = '\0';
            ui_draw_text(canvas, fonts[command->font], command->size,
                         (int)command->x, (int)command->y, 0, UI_ALIGN_LEFT,
                         command->color, line);
            break;
        }
        case UI_LAYOUT_ITEM:
            draw_item(command, canvas, fonts);
            break;
        }
    }
}

int ui_layout_hit(const struct ui_layout *layout, float x, float y, int none)
{
    for (int i = layout->control_count - 1; i >= 0; --i) {
        const struct ui_layout_control *control = &layout->controls[i];
        if (x >= control->x && x < control->x + control->width &&
            y >= control->y && y < control->y + control->height)
            return control->control;
    }
    return none;
}
