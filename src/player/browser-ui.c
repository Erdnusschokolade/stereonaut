#define _POSIX_C_SOURCE 200809L
#include "browser-ui.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define PAGE_ENTRIES 9U

struct canvas {
    uint32_t *pixels;
    bool bgra;
};

static uint32_t rgba(const struct canvas *canvas, uint8_t r, uint8_t g,
                     uint8_t b, uint8_t a)
{
    if (canvas->bgra) {
        uint8_t temporary = r;
        r = b;
        b = temporary;
    }
    return (uint32_t)r | ((uint32_t)g << 8U) |
           ((uint32_t)b << 16U) | ((uint32_t)a << 24U);
}

static void rectangle(const struct canvas *canvas, int x, int y, int width,
                      int height, uint32_t color)
{
    int left = x < 0 ? 0 : x;
    int top = y < 0 ? 0 : y;
    int right = x + width > (int)BROWSER_UI_WIDTH ? (int)BROWSER_UI_WIDTH
                                                  : x + width;
    int bottom = y + height > (int)BROWSER_UI_HEIGHT ? (int)BROWSER_UI_HEIGHT
                                                     : y + height;
    for (int row = top; row < bottom; ++row)
        for (int column = left; column < right; ++column)
            canvas->pixels[(size_t)row * BROWSER_UI_WIDTH +
                           (size_t)column] = color;
}

static const uint8_t letter_rows[36][5] = {
    {2,5,7,5,5},{6,5,6,5,6},{3,4,4,4,3},{6,5,5,5,6},
    {7,4,6,4,7},{7,4,6,4,4},{3,4,5,5,3},{5,5,7,5,5},
    {7,2,2,2,7},{1,1,1,5,2},{5,5,6,5,5},{4,4,4,4,7},
    {5,7,7,5,5},{5,7,7,7,5},{2,5,5,5,2},{6,5,6,4,4},
    {2,5,5,3,1},{6,5,6,5,5},{3,4,2,1,6},{7,2,2,2,2},
    {5,5,5,5,7},{5,5,5,5,2},{5,5,7,7,5},{5,5,2,5,5},
    {5,5,2,2,2},{7,1,2,4,7},
    {2,5,5,5,2},{2,6,2,2,7},{6,1,2,4,7},{6,1,2,1,6},
    {5,5,7,1,1},{7,4,6,1,6},{3,4,6,5,2},{7,1,2,2,2},
    {2,5,2,5,2},{2,5,3,1,6},
};

static uint8_t glyph_row(unsigned char character, unsigned row)
{
    if (character >= 'a' && character <= 'z')
        character = (unsigned char)(character - 'a' + 'A');
    if (character >= 'A' && character <= 'Z')
        return letter_rows[character - 'A'][row];
    if (character >= '0' && character <= '9')
        return letter_rows[26U + character - '0'][row];
    if (character == '.')
        return row == 4 ? 2 : 0;
    if (character == '-')
        return row == 2 ? 7 : 0;
    if (character == '_')
        return row == 4 ? 7 : 0;
    if (character == '/')
        return (uint8_t)(1U << (row > 2 ? 0 : 2U - row));
    if (character == ':')
        return row == 1 || row == 3 ? 2 : 0;
    if (character == '>')
        return (uint8_t[]){4, 2, 1, 2, 4}[row];
    if (character == '<')
        return (uint8_t[]){1, 2, 4, 2, 1}[row];
    if (character == '(')
        return (uint8_t[]){1, 2, 2, 2, 1}[row];
    if (character == ')')
        return (uint8_t[]){4, 2, 2, 2, 4}[row];
    if (character == ',')
        return (uint8_t[]){0, 0, 0, 2, 4}[row];
    if (character == '+')
        return (uint8_t[]){0, 2, 7, 2, 0}[row];
    if (character == '!')
        return (uint8_t[]){2, 2, 2, 0, 2}[row];
    if (character == '?')
        return (uint8_t[]){6, 1, 2, 0, 2}[row];
    if (character == '&')
        return (uint8_t[]){2, 5, 2, 5, 3}[row];
    if (character == '\'')
        return (uint8_t[]){2, 2, 0, 0, 0}[row];
    if (character == ' ')
        return 0;
    return (uint8_t[]){7, 5, 5, 5, 7}[row];
}

static void draw_text(const struct canvas *canvas, int x, int y,
                      int scale, const char *text, uint32_t color,
                      size_t limit)
{
    size_t count = 0;
    for (const unsigned char *cursor = (const unsigned char *)text;
         *cursor && count < limit; ++cursor, ++count) {
        for (unsigned row = 0; row < 5; ++row) {
            uint8_t bits = glyph_row(*cursor, row);
            for (unsigned column = 0; column < 3; ++column)
                if (bits & (1U << (2U - column)))
                    rectangle(canvas, x + (int)column * scale,
                              y + (int)row * scale, scale, scale, color);
        }
        x += 4 * scale;
    }
}

static size_t page_count(const struct browser_ui *ui)
{
    return ui->listing.entry_count
               ? (ui->listing.entry_count + PAGE_ENTRIES - 1U) / PAGE_ENTRIES
               : 1U;
}

void browser_ui_render(struct browser_ui *ui, uint32_t *pixels, bool bgra)
{
    struct canvas canvas = {.pixels = pixels, .bgra = bgra};
    uint32_t background = rgba(&canvas, 8, 13, 24, 255);
    uint32_t panel = rgba(&canvas, 18, 29, 48, 255);
    uint32_t normal = rgba(&canvas, 35, 54, 82, 255);
    uint32_t selected = rgba(&canvas, 23, 128, 190, 255);
    uint32_t text = rgba(&canvas, 238, 245, 255, 255);
    rectangle(&canvas, 0, 0, BROWSER_UI_WIDTH, BROWSER_UI_HEIGHT, background);
    rectangle(&canvas, 32, 24, 960, 720, panel);
    draw_text(&canvas, 64, 48, 5, "STEREONAUT", text, 32);
    draw_text(&canvas, 64, 92, 3,
              ui->listing.path ? ui->listing.path : "LADE VERZEICHNIS",
              text, 72);
    if (ui->listing.error[0])
        draw_text(&canvas, 64, 130, 3, ui->listing.error, text, 72);
    else if (ui->status_message[0])
        draw_text(&canvas, 64, 130, 3, ui->status_message, text, 72);
    size_t first = ui->page * PAGE_ENTRIES;
    for (size_t row = 0; row < PAGE_ENTRIES; ++row) {
        size_t index = first + row;
        if (index >= ui->listing.entry_count)
            break;
        int y = 165 + (int)row * 54;
        rectangle(&canvas, 58, y, 908, 46,
                  ui->hover == (int)row ? selected : normal);
        const struct file_browser_entry *entry = &ui->listing.entries[index];
        const char *prefix = entry->kind == FILE_BROWSER_MEDIA ? "> " : "/ ";
        draw_text(&canvas, 76, y + 11, 4, prefix, text, 2);
        draw_text(&canvas, 112, y + 11, 4, entry->name, text, 52);
    }
    rectangle(&canvas, 58, 666, 260, 48, ui->hover == 9 ? selected : normal);
    rectangle(&canvas, 330, 666, 260, 48, ui->hover == 10 ? selected : normal);
    rectangle(&canvas, 602, 666, 364, 48, ui->hover == 11 ? selected : normal);
    draw_text(&canvas, 76, 678, 4, "< VORHERIGE", text, 20);
    draw_text(&canvas, 352, 678, 4, "NAECHSTE >", text, 20);
    char page_text[48];
    snprintf(page_text, sizeof(page_text), "SEITE %zu/%zu   BEENDEN",
             ui->page + 1U, page_count(ui));
    draw_text(&canvas, 624, 678, 4, page_text, text, 28);
    if (ui->pointer_visible) {
        uint32_t pointer = rgba(&canvas, 255, 244, 96, 255);
        uint32_t center = rgba(&canvas, 255, 255, 255, 255);
        rectangle(&canvas, ui->pointer_x - 7, ui->pointer_y - 2, 15, 5,
                  pointer);
        rectangle(&canvas, ui->pointer_x - 2, ui->pointer_y - 7, 5, 15,
                  pointer);
        rectangle(&canvas, ui->pointer_x - 1, ui->pointer_y - 1, 3, 3, center);
    }
    ui->dirty = false;
}

int browser_ui_hit_test(float u, float v)
{
    int pixel_x = (int)(u * BROWSER_UI_WIDTH);
    int pixel_y = (int)(v * BROWSER_UI_HEIGHT);
    if (pixel_x >= 58 && pixel_x < 966) {
        for (int row = 0; row < (int)PAGE_ENTRIES; ++row) {
            int top = 165 + row * 54;
            if (pixel_y >= top && pixel_y < top + 46)
                return row;
        }
    }
    if (pixel_y >= 666 && pixel_y < 714) {
        if (pixel_x >= 58 && pixel_x < 318)
            return 9;
        if (pixel_x >= 330 && pixel_x < 590)
            return 10;
        if (pixel_x >= 602 && pixel_x < 966)
            return 11;
    }
    return -1;
}

void browser_ui_set_pointer(struct browser_ui *ui, bool visible, int x, int y,
                            int hover)
{
    if (hover != ui->hover) {
        ui->hover = hover;
        ui->dirty = true;
    }
    if (visible != ui->pointer_visible ||
        (visible && (x != ui->pointer_x || y != ui->pointer_y))) {
        ui->pointer_visible = visible;
        ui->pointer_x = x;
        ui->pointer_y = y;
        ui->dirty = true;
    }
}

enum browser_ui_action browser_ui_activate(struct browser_ui *ui, char *path,
                                           size_t path_size)
{
    if (ui->hover >= 0 && ui->hover < (int)PAGE_ENTRIES) {
        size_t index = ui->page * PAGE_ENTRIES + (size_t)ui->hover;
        if (index >= ui->listing.entry_count)
            return BROWSER_UI_NONE;
        const struct file_browser_entry *entry = &ui->listing.entries[index];
        if (entry->kind == FILE_BROWSER_MEDIA) {
            ui->selected_page = ui->page;
            snprintf(path, path_size, "%s", entry->path);
            return BROWSER_UI_FILE_SELECTED;
        }
        char error[256] = {0};
        if (file_browser_request(ui->browser, entry->path, error,
                                 sizeof(error)) < 0) {
            snprintf(ui->listing.error, sizeof(ui->listing.error), "%s",
                     error);
            ui->dirty = true;
        }
    } else if (ui->hover == 9) {
        if (ui->page > 0)
            --ui->page;
        ui->dirty = true;
    } else if (ui->hover == 10) {
        if (ui->page + 1U < page_count(ui))
            ++ui->page;
        ui->dirty = true;
    } else if (ui->hover == 11) {
        return BROWSER_UI_EXIT_REQUESTED;
    }
    return BROWSER_UI_NONE;
}

bool browser_ui_update(struct browser_ui *ui)
{
    struct file_browser_listing updated = {0};
    int result = file_browser_poll(ui->browser, &updated);
    if (result <= 0)
        return false;
    file_browser_listing_clear(&ui->listing);
    ui->listing = updated;
    ui->page = ui->restore_page;
    ui->restore_page = 0;
    if (ui->page >= page_count(ui))
        ui->page = 0;
    ui->hover = -1;
    ui->dirty = true;
    printf("browser_listing generation=%" PRIu64 " path=%s entries=%zu"
           " error=%s\n",
           ui->listing.generation, ui->listing.path ? ui->listing.path : "",
           ui->listing.entry_count,
           ui->listing.error[0] ? ui->listing.error : "none");
    return true;
}

int browser_ui_open(struct browser_ui *ui, const char *location,
                    const char *message, char *error, size_t error_size)
{
    memset(ui, 0, sizeof(*ui));
    ui->hover = -1;
    ui->dirty = true;
    if (message)
        snprintf(ui->status_message, sizeof(ui->status_message), "%s",
                 message);
    return file_browser_open(&ui->browser, location, error, error_size);
}

void browser_ui_navigate(struct browser_ui *ui, const char *location,
                         const char *message)
{
    snprintf(ui->status_message, sizeof(ui->status_message), "%s",
             message ? message : "");
    /* Coming back from playback shows the page the title was picked on. */
    ui->restore_page = ui->selected_page;
    ui->selected_page = 0;
    ui->hover = -1;
    ui->dirty = true;
    char error[256] = {0};
    if (file_browser_request(ui->browser, location, error,
                             sizeof(error)) < 0)
        snprintf(ui->listing.error, sizeof(ui->listing.error), "%s", error);
}

void browser_ui_close(struct browser_ui *ui)
{
    file_browser_listing_clear(&ui->listing);
    file_browser_close(&ui->browser);
}
