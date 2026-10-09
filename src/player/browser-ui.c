#define _POSIX_C_SOURCE 200809L
#include "browser-ui.h"

#include "jellyfin.h"
#include "thumbnails.h"
#include "ui-draw.h"

#include <inttypes.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Layout of the 2048 x 1024 panel: navigation card (left), grid card
 * (centre) with a pager below, detail card (right). */
#define GRID_COLUMNS 6U
#define GRID_ROWS 2U
#define PAGE_ENTRIES (GRID_COLUMNS * GRID_ROWS)

#define RAIL_X 24
#define CENTER_X 420
#define DETAIL_X 1652
#define CARD_Y 24
#define CARD_H 900
#define SIDE_W 372
#define CENTER_W 1208

#define GRID_X 452
#define GRID_Y 196
/* Poster tiles (2:3, Jellyfin primary images) with a two-line title. */
#define TILE_W 168
#define THUMB_H 252
#define LABEL_SIZE 22
#define LABEL_LINE 28
#define TILE_H (THUMB_H + 8 + 2 * LABEL_LINE)
#define TILE_GAP_X 28
#define TILE_GAP_Y 20

#define PAGER_Y 940
#define PAGER_H 60
#define PAGE_BUTTONS 7

#define RAIL_ITEM_X 40
#define RAIL_ITEM_W 340
#define RAIL_ITEM_H 76
#define RAIL_FIRST_Y 172
#define RAIL_EXIT_Y 832

#define BACK_X 488
#define BACK_Y 74

/* Sort chip in the header and its menu. */
#define SORT_W 300
#define SORT_H 52
#define SORT_X (CENTER_X + CENTER_W - SORT_W - 32)
#define SORT_Y 48
#define MENU_ITEM_H 52
#define MENU_W 470
#define MENU_X (CENTER_X + CENTER_W - MENU_W - 32)
#define MENU_Y (SORT_Y + SORT_H + 8)
#define MENU_H (JF_SORT_COUNT * MENU_ITEM_H + 16)

/* Control ids returned by browser_ui_hit_test. */
enum {
    ID_TILE = 0, /* 0 .. PAGE_ENTRIES-1 */
    ID_PREVIOUS = 20,
    ID_NEXT = 21,
    ID_PAGE = 22, /* 22 .. 22+PAGE_BUTTONS-1 */
    ID_PAGE_AREA = 29, /* hit test only: resolved with the model */
    ID_BACK = 30,
    ID_RAIL = 40, /* start, jellyfin, local */
    ID_EXIT = 43,
    ID_SORT = 50,       /* the chip */
    ID_SORT_ITEM = 60,  /* 60 .. 60+JF_SORT_COUNT-1 */
    ID_SORT_AREA = 79,  /* hit test only: resolved with the model */
};

static const char *const sort_labels[JF_SORT_COUNT] = {
    "Name", "Zufällig", "Community-Bewertung", "Kritikerbewertung",
    "Hinzugefügt am", "Abgespielt am", "Altersfreigabe", "Wiedergabezähler",
    "Veröffentlichungsdatum", "Laufzeit",
};

/* Dates, ratings and counts are most useful newest/highest first. */
static bool default_descending(int sort)
{
    return sort != JF_SORT_NAME && sort != JF_SORT_RANDOM &&
           sort != JF_SORT_OFFICIAL_RATING && sort != JF_SORT_RUNTIME;
}

/* Only folder listings of a Jellyfin library can be sorted. */
static bool sortable(const struct browser_ui *ui)
{
    return strncmp(ui->location, "jf:view:", 8) == 0;
}

enum { RAIL_START, RAIL_JELLYFIN, RAIL_LOCAL, RAIL_COUNT };

static const struct ui_color color_card = UI_RGBA(17, 18, 22, 240);
static const struct ui_color color_border = UI_RGBA(255, 255, 255, 18);
static const struct ui_color color_tile = UI_RGBA(32, 34, 41, 255);
static const struct ui_color color_tile_hover = UI_RGBA(46, 50, 60, 255);
static const struct ui_color color_text = UI_RGBA(236, 239, 244, 255);
static const struct ui_color color_label = UI_RGBA(205, 210, 220, 255);
static const struct ui_color color_dim = UI_RGBA(140, 147, 160, 255);
static const struct ui_color color_accent = UI_RGBA(56, 160, 255, 255);
static const struct ui_color color_accent_soft = UI_RGBA(56, 160, 255, 60);
static const struct ui_color color_chip = UI_RGBA(42, 45, 54, 255);
static const struct ui_color color_shadow = UI_RGBA(0, 0, 0, 110);

static size_t page_count(const struct browser_ui *ui)
{
    return ui->grid_count ? (ui->grid_count + PAGE_ENTRIES - 1U) / PAGE_ENTRIES
                          : 1U;
}

/* Listing index of grid slot `slot` on the current page, or -1. */
static int slot_entry(const struct browser_ui *ui, int slot)
{
    if (slot < 0 || slot >= (int)PAGE_ENTRIES)
        return -1;
    size_t position = ui->page * PAGE_ENTRIES + (size_t)slot;
    return position < ui->grid_count ? (int)ui->grid[position] : -1;
}

static int rail_active(const struct browser_ui *ui)
{
    if (strncmp(ui->location, "jf:", 3) == 0)
        return RAIL_JELLYFIN;
    if (!ui->location[0] || strcmp(ui->location, "root:") == 0)
        return RAIL_START;
    return RAIL_LOCAL;
}

static void tile_rect(int slot, int *x, int *y)
{
    *x = GRID_X + (slot % (int)GRID_COLUMNS) * (TILE_W + TILE_GAP_X);
    *y = GRID_Y + (slot / (int)GRID_COLUMNS) * (TILE_H + TILE_GAP_Y);
}

/* First page number button shown and how many there are. */
static void page_window(const struct browser_ui *ui, size_t *first,
                        size_t *count)
{
    size_t pages = page_count(ui);
    *count = pages < PAGE_BUTTONS ? pages : PAGE_BUTTONS;
    size_t start = ui->page >= PAGE_BUTTONS / 2 ? ui->page - PAGE_BUTTONS / 2
                                                : 0;
    if (start + *count > pages)
        start = pages - *count;
    *first = start;
}

static void page_button_rect(size_t count, size_t i, int *x, int *w)
{
    const int width = 64, gap = 10;
    int total = (int)count * width + ((int)count - 1) * gap;
    *x = CENTER_X + CENTER_W / 2 - total / 2 + (int)i * (width + gap);
    *w = width;
}

static const char *extension_badge(const struct file_browser_entry *entry,
                                   char badge[8])
{
    badge[0] = '\0';
    if (entry->kind != FILE_BROWSER_MEDIA || strncmp(entry->path, "jf:", 3) == 0)
        return badge;
    const char *dot = strrchr(entry->path, '.');
    if (!dot || strlen(dot + 1) > 4)
        return badge;
    size_t i = 0;
    for (const char *c = dot + 1; *c && i < 6; ++c)
        badge[i++] = (char)(*c >= 'a' && *c <= 'z' ? *c - 32 : *c);
    badge[i] = '\0';
    return badge;
}

static const char *format_duration(int64_t duration_us, char text[16])
{
    text[0] = '\0';
    if (duration_us <= 0)
        return text;
    int64_t seconds = duration_us / 1000000;
    if (seconds >= 3600)
        snprintf(text, 16, "%d:%02d:%02d", (int)(seconds / 3600),
                 (int)(seconds / 60 % 60), (int)(seconds % 60));
    else
        snprintf(text, 16, "%d:%02d", (int)(seconds / 60),
                 (int)(seconds % 60));
    return text;
}

/* ---- drawing ----------------------------------------------------------- */

static void draw_card(struct ui_canvas *canvas, int x, int width)
{
    ui_shadow_round_rect(canvas, x, CARD_Y + 8, width, CARD_H, 28, 28,
                         color_shadow);
    ui_fill_round_rect(canvas, x, CARD_Y, width, CARD_H, 28, color_card);
    ui_stroke_round_rect(canvas, x, CARD_Y, width, CARD_H, 28, 2,
                         color_border);
}

static void draw_rail_item(struct browser_ui *ui, struct ui_canvas *canvas,
                           int y, enum ui_icon icon, const char *label,
                           bool active, bool hover)
{
    ui_set_rect_raw(canvas, RAIL_ITEM_X, y, RAIL_ITEM_W, RAIL_ITEM_H,
                    ui->card_pixel);
    if (active || hover)
        ui_fill_round_rect(canvas, RAIL_ITEM_X, y, RAIL_ITEM_W, RAIL_ITEM_H,
                           18, hover ? color_tile_hover : color_tile);
    if (active)
        ui_fill_round_rect(canvas, RAIL_ITEM_X + 6, y + 18, 6,
                           RAIL_ITEM_H - 36, 3, color_accent);
    struct ui_color tint = active || hover ? color_text : color_dim;
    ui_draw_icon(canvas, icon, RAIL_ITEM_X + 50, y + RAIL_ITEM_H / 2.0f, 40,
                 active ? color_accent : tint);
    ui_draw_text(canvas, active ? ui->bold : ui->font, 30,
                 RAIL_ITEM_X + 88, y + 18, RAIL_ITEM_W - 100, UI_ALIGN_LEFT,
                 tint, label);
}

static void draw_rail(struct browser_ui *ui, struct ui_canvas *canvas,
                      int hover)
{
    static const struct {
        enum ui_icon icon;
        const char *label;
    } items[RAIL_COUNT] = {
        {UI_ICON_HOME, "Start"},
        {UI_ICON_SERVER, "Jellyfin"},
        {UI_ICON_DISK, "Lokale Dateien"},
    };
    int active = rail_active(ui);
    for (int i = 0; i < RAIL_COUNT; ++i)
        if (hover < 0 || hover == ID_RAIL + i)
            draw_rail_item(ui, canvas,
                           RAIL_FIRST_Y + i * (RAIL_ITEM_H + 12),
                           items[i].icon, items[i].label, i == active,
                           hover == ID_RAIL + i);
    if (hover < 0 || hover == ID_EXIT)
        draw_rail_item(ui, canvas, RAIL_EXIT_Y, UI_ICON_POWER, "Beenden",
                       false, hover == ID_EXIT);
}

static void draw_tile(struct browser_ui *ui, struct ui_canvas *canvas,
                      int slot, bool hover)
{
    int index = slot_entry(ui, slot);
    if (index < 0)
        return;
    const struct file_browser_entry *entry = &ui->listing.entries[index];
    int x, y;
    tile_rect(slot, &x, &y);
    if (hover) /* start from the card colour so the outline is clean */
        ui_set_rect_raw(canvas, x - 8, y - 8, TILE_W + 16, TILE_H + 12,
                        ui->card_pixel);
    const struct ui_image *thumb = ui->thumbs ? ui->thumbs[index] : NULL;
    if (thumb) {
        ui_draw_image(canvas, thumb, x, y, TILE_W, THUMB_H, 14,
                      hover ? 0.0f : 0.12f);
    } else {
        ui_fill_round_rect(canvas, x, y, TILE_W, THUMB_H, 14,
                           hover ? color_tile_hover : color_tile);
        ui_draw_icon(canvas,
                     entry->kind == FILE_BROWSER_MEDIA ? UI_ICON_VIDEO
                                                       : UI_ICON_FOLDER,
                     x + TILE_W / 2.0f, y + THUMB_H / 2.0f, 64,
                     hover ? color_accent : color_dim);
    }
    char badge[8];
    if (extension_badge(entry, badge)[0]) {
        int width = ui_text_width(ui->bold, 18, badge) + 18;
        ui_fill_round_rect(canvas, x + 10, y + THUMB_H - 38, width, 28, 8,
                           UI_RGBA(0, 0, 0, 170));
        ui_draw_text(canvas, ui->bold, 18, x + 19, y + THUMB_H - 35, 0,
                     UI_ALIGN_LEFT, color_text, badge);
    }
    char duration[16];
    if (format_duration(entry->duration_us, duration)[0]) {
        int width = ui_text_width(ui->bold, 18, duration) + 18;
        ui_fill_round_rect(canvas, x + TILE_W - 10 - width, y + THUMB_H - 38,
                           width, 28, 8, UI_RGBA(0, 0, 0, 170));
        ui_draw_text(canvas, ui->bold, 18, x + TILE_W - 19, y + THUMB_H - 35,
                     0, UI_ALIGN_RIGHT, color_text, duration);
    }
    if (hover)
        ui_stroke_round_rect(canvas, x - 5, y - 5, TILE_W + 10, THUMB_H + 10,
                             18, 4, color_accent);
    ui_draw_text_wrapped(canvas, hover ? ui->bold : ui->font, LABEL_SIZE,
                         x - 4, y + THUMB_H + 6, TILE_W + 8, LABEL_LINE, 2,
                         UI_ALIGN_CENTER, hover ? color_text : color_label,
                         entry->name);
}

static void draw_breadcrumbs(struct browser_ui *ui, struct ui_canvas *canvas)
{
    const char *path = ui->listing.path ? ui->listing.path : "";
    char segment[256];
    int x = GRID_X;
    const char *cursor = path;
    while (*cursor && x < CENTER_X + CENTER_W - 60) {
        const char *separator = strstr(cursor, " / ");
        size_t length = separator ? (size_t)(separator - cursor)
                                  : strlen(cursor);
        if (length >= sizeof(segment))
            length = sizeof(segment) - 1;
        memcpy(segment, cursor, length);
        segment[length] = '\0';
        bool last = !separator;
        int width = ui_text_width(last ? ui->bold : ui->font, 24, segment) +
                    44;
        if (width > 420)
            width = 420;
        ui_fill_round_rect(canvas, x, 126, width, 46, 23,
                           last ? color_accent : color_chip);
        ui_draw_text(canvas, last ? ui->bold : ui->font, 24, x + width / 2,
                     134, width - 32, UI_ALIGN_CENTER,
                     last ? UI_RGBA(255, 255, 255, 255) : color_text, segment);
        x += width + 10;
        if (!separator)
            break;
        cursor = separator + 3;
    }
}

static const char *current_title(const struct browser_ui *ui)
{
    const char *path = ui->listing.path;
    if (!path || !path[0])
        return "Lade …";
    const char *last = path;
    for (const char *s = strstr(path, " / "); s; s = strstr(s + 3, " / "))
        last = s + 3;
    return last;
}

static void draw_back_button(struct browser_ui *ui, struct ui_canvas *canvas,
                             bool hover)
{
    if (ui->parent < 0)
        return;
    ui_fill_circle(canvas, BACK_X, BACK_Y, 32,
                   hover ? color_accent : color_chip);
    ui_draw_icon(canvas, UI_ICON_BACK, BACK_X, BACK_Y, 34, color_text);
}

static void draw_sort_chip(struct browser_ui *ui, struct ui_canvas *canvas,
                           bool hover)
{
    if (!sortable(ui))
        return;
    ui_set_rect_raw(canvas, SORT_X - 4, SORT_Y - 4, SORT_W + 8, SORT_H + 8,
                    ui->card_pixel);
    ui_fill_round_rect(canvas, SORT_X, SORT_Y, SORT_W, SORT_H, SORT_H / 2.0f,
                       hover || ui->sort_menu_open ? color_tile_hover
                                                   : color_chip);
    ui_draw_text(canvas, ui->font, 24, SORT_X + 24, SORT_Y + 10,
                 SORT_W - 112, UI_ALIGN_LEFT, color_text,
                 sort_labels[ui->sort]);
    if (ui->sort != JF_SORT_RANDOM)
        ui_draw_icon(canvas,
                     ui->sort_descending ? UI_ICON_ARROW_DOWN
                                         : UI_ICON_ARROW_UP,
                     SORT_X + SORT_W - 70, SORT_Y + SORT_H / 2.0f, 28,
                     color_accent);
    ui_draw_icon(canvas, UI_ICON_CHEVRON_DOWN, SORT_X + SORT_W - 32,
                 SORT_Y + SORT_H / 2.0f, 28, color_dim);
}

static void draw_sort_menu(struct browser_ui *ui, struct ui_canvas *canvas,
                           int hover)
{
    ui_shadow_round_rect(canvas, MENU_X, MENU_Y + 6, MENU_W, MENU_H, 22, 24,
                         color_shadow);
    ui_fill_round_rect(canvas, MENU_X, MENU_Y, MENU_W, MENU_H, 22,
                       UI_RGBA(30, 32, 39, 255));
    ui_stroke_round_rect(canvas, MENU_X, MENU_Y, MENU_W, MENU_H, 22, 2,
                         color_border);
    for (int i = 0; i < JF_SORT_COUNT; ++i) {
        int y = MENU_Y + 8 + i * MENU_ITEM_H;
        bool current = i == ui->sort;
        if (hover == ID_SORT_ITEM + i)
            ui_fill_round_rect(canvas, MENU_X + 8, y, MENU_W - 16,
                               MENU_ITEM_H, 14, color_tile_hover);
        ui_draw_text(canvas, current ? ui->bold : ui->font, 25, MENU_X + 28,
                     y + 11, MENU_W - 96, UI_ALIGN_LEFT,
                     current ? color_accent : color_text, sort_labels[i]);
        if (current && i != JF_SORT_RANDOM)
            ui_draw_icon(canvas,
                         ui->sort_descending ? UI_ICON_ARROW_DOWN
                                             : UI_ICON_ARROW_UP,
                         MENU_X + MENU_W - 40, y + MENU_ITEM_H / 2.0f, 28,
                         color_accent);
    }
}

static void draw_pager(struct browser_ui *ui, struct ui_canvas *canvas,
                       int hover)
{
    if (page_count(ui) <= 1)
        return;
    if (hover < 0 || hover == ID_PREVIOUS) {
        bool enabled = ui->page > 0;
        ui_fill_round_rect(canvas, CENTER_X, PAGER_Y, 190, PAGER_H, 16,
                           hover == ID_PREVIOUS && enabled ? color_tile_hover
                                                           : color_card);
        ui_draw_icon(canvas, UI_ICON_CHEVRON_LEFT, CENTER_X + 34,
                     PAGER_Y + PAGER_H / 2.0f, 30,
                     enabled ? color_text : color_dim);
        ui_draw_text(canvas, ui->font, 26, CENTER_X + 58, PAGER_Y + 13, 0,
                     UI_ALIGN_LEFT, enabled ? color_text : color_dim,
                     "Vorherige");
    }
    if (hover < 0 || hover == ID_NEXT) {
        bool enabled = ui->page + 1 < page_count(ui);
        int x = CENTER_X + CENTER_W - 190;
        ui_fill_round_rect(canvas, x, PAGER_Y, 190, PAGER_H, 16,
                           hover == ID_NEXT && enabled ? color_tile_hover
                                                       : color_card);
        ui_draw_text(canvas, ui->font, 26, x + 136, PAGER_Y + 13, 0,
                     UI_ALIGN_RIGHT, enabled ? color_text : color_dim,
                     "Nächste");
        ui_draw_icon(canvas, UI_ICON_CHEVRON_RIGHT, x + 158,
                     PAGER_Y + PAGER_H / 2.0f, 30,
                     enabled ? color_text : color_dim);
    }
    size_t first, count;
    page_window(ui, &first, &count);
    for (size_t i = 0; i < count; ++i) {
        int id = ID_PAGE + (int)i;
        if (hover >= 0 && hover != id)
            continue;
        int x, width;
        page_button_rect(count, i, &x, &width);
        bool current = first + i == ui->page;
        ui_fill_round_rect(canvas, x, PAGER_Y, width, PAGER_H, 16,
                           current ? color_accent
                           : hover == id ? color_tile_hover
                                         : color_card);
        char number[16];
        snprintf(number, sizeof(number), "%zu", first + i + 1);
        ui_draw_text(canvas, current ? ui->bold : ui->font, 26,
                     x + width / 2, PAGER_Y + 13, 0, UI_ALIGN_CENTER,
                     color_text, number);
    }
}

static void draw_base(struct browser_ui *ui, struct ui_canvas *canvas)
{
    ui_clear(canvas, UI_RGBA(0, 0, 0, 0));
    draw_card(canvas, RAIL_X, SIDE_W);
    draw_card(canvas, CENTER_X, CENTER_W);
    draw_card(canvas, DETAIL_X, SIDE_W);
    /* Card background (over its shadow) for resetting regions later. */
    ui_get_pixel_raw(canvas, CENTER_X + 200, CARD_Y + 400, ui->card_pixel);

    ui_draw_text(canvas, ui->bold, 42, 60, 52, 0, UI_ALIGN_LEFT, color_text,
                 "Stereonaut");
    ui_draw_text(canvas, ui->font, 24, 62, 108, 0, UI_ALIGN_LEFT, color_dim,
                 "VR-Videoplayer");
    draw_rail(ui, canvas, -1);

    draw_back_button(ui, canvas, false);
    ui_draw_text(canvas, ui->bold, 40, CENTER_X + CENTER_W / 2, 46,
                 sortable(ui) ? 480 : 820, UI_ALIGN_CENTER, color_text,
                 current_title(ui));
    draw_sort_chip(ui, canvas, false);
    draw_breadcrumbs(ui, canvas);
    for (int slot = 0; slot < (int)PAGE_ENTRIES; ++slot)
        draw_tile(ui, canvas, slot, false);
    if (!ui->grid_count && ui->listing.path)
        ui_draw_text(canvas, ui->font, 30, CENTER_X + CENTER_W / 2, 480, 0,
                     UI_ALIGN_CENTER, color_dim, "Keine Einträge");

    const char *message = ui->listing.error[0] ? ui->listing.error
                                               : ui->status_message;
    if (message[0]) {
        ui_fill_round_rect(canvas, GRID_X, 856, CENTER_W - 64, 52, 16,
                           color_accent_soft);
        ui_draw_icon(canvas, UI_ICON_INFO, GRID_X + 32, 882, 32, color_text);
        ui_draw_text(canvas, ui->font, 26, GRID_X + 62, 866, CENTER_W - 140,
                     UI_ALIGN_LEFT, color_text, message);
    }
    if (page_count(ui) > 1) {
        int track = (int)GRID_ROWS * (TILE_H + TILE_GAP_Y) - TILE_GAP_Y;
        int height = track / (int)page_count(ui);
        if (height < 40)
            height = 40;
        int top = GRID_Y + (int)((size_t)(track - height) * ui->page /
                                 (page_count(ui) - 1));
        ui_fill_round_rect(canvas, CENTER_X + CENTER_W - 20, GRID_Y, 6,
                           track, 3, color_chip);
        ui_fill_round_rect(canvas, CENTER_X + CENTER_W - 20, top, 6, height,
                           3, color_dim);
    }
    draw_pager(ui, canvas, -1);
}

static void draw_detail(struct browser_ui *ui, struct ui_canvas *canvas)
{
    const int x = DETAIL_X + 24, width = SIDE_W - 48;
    ui_draw_text(canvas, ui->font, 24, x, 48, 0, UI_ALIGN_LEFT, color_dim,
                 "Details");
    int index = ui->detail;
    if (index < 0 || (size_t)index >= ui->listing.entry_count) {
        ui_draw_text_wrapped(canvas, ui->font, 26, x, 110, width, 36, 4,
                             UI_ALIGN_LEFT, color_dim,
                             "Zeige mit dem Controller auf einen Titel, um "
                             "Details zu sehen.");
        return;
    }
    const struct file_browser_entry *entry = &ui->listing.entries[index];
    const struct ui_image *thumb = ui->thumbs ? ui->thumbs[index] : NULL;
    const int poster_w = 216, poster_h = 324;
    const int poster_x = x + (width - poster_w) / 2, poster_y = 92;
    if (thumb) {
        ui_draw_image(canvas, thumb, poster_x, poster_y, poster_w, poster_h,
                      16, 0.0f);
    } else {
        ui_fill_round_rect(canvas, poster_x, poster_y, poster_w, poster_h, 16,
                           color_tile);
        ui_draw_icon(canvas,
                     entry->kind == FILE_BROWSER_MEDIA ? UI_ICON_VIDEO
                                                       : UI_ICON_FOLDER,
                     poster_x + poster_w / 2.0f, poster_y + poster_h / 2.0f,
                     96, color_dim);
    }
    int lines = ui_draw_text_wrapped(canvas, ui->bold, 30, x, 440, width, 40,
                                     3, UI_ALIGN_LEFT, color_text,
                                     entry->name);
    int y = 440 + lines * 40 + 24;
    bool jellyfin = strncmp(entry->path, "jf:", 3) == 0;
    char badge[8], duration[16], year[16] = "";
    if (entry->year > 0)
        snprintf(year, sizeof(year), "%d", entry->year);
    struct {
        const char *label, *value;
    } rows[] = {
        {"Typ", entry->kind == FILE_BROWSER_MEDIA ? "Video" : "Ordner"},
        {"Dauer", format_duration(entry->duration_us, duration)},
        {"Jahr", year},
        {"Quelle", jellyfin ? "Jellyfin" : "Lokal"},
        {"Format", extension_badge(entry, badge)},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        if (!rows[i].value[0])
            continue;
        ui_draw_text(canvas, ui->font, 24, x, y, 0, UI_ALIGN_LEFT, color_dim,
                     rows[i].label);
        ui_draw_text(canvas, ui->bold, 24, x + 120, y, width - 120,
                     UI_ALIGN_LEFT, color_text, rows[i].value);
        y += 40;
    }
    if (entry->kind == FILE_BROWSER_MEDIA)
        ui_draw_text_wrapped(canvas, ui->font, 24, x, CARD_Y + CARD_H - 80,
                             width, 32, 2, UI_ALIGN_LEFT, color_dim,
                             "Trigger drücken zum Abspielen");
}

void browser_ui_render(struct browser_ui *ui, uint32_t *pixels, bool bgra)
{
    if (!ui->font)
        ui->font = ui_font_load(false);
    if (!ui->bold)
        ui->bold = ui_font_load(true);
    struct ui_canvas canvas = {(uint8_t *)pixels, BROWSER_UI_WIDTH,
                               BROWSER_UI_HEIGHT, bgra};
    if (!ui->base) {
        ui->base = malloc((size_t)BROWSER_UI_WIDTH * BROWSER_UI_HEIGHT * 4U);
        ui->content_dirty = true;
    }
    if (ui->base) {
        struct ui_canvas base = {ui->base, BROWSER_UI_WIDTH,
                                 BROWSER_UI_HEIGHT, bgra};
        if (ui->content_dirty) {
            draw_base(ui, &base);
        } else {
            for (int slot = 0; slot < (int)PAGE_ENTRIES; ++slot)
                if (ui->stale_tiles & (1U << slot)) {
                    int x, y;
                    tile_rect(slot, &x, &y);
                    ui_set_rect_raw(&base, x - 8, y - 8, TILE_W + 16,
                                    TILE_H + 12, ui->card_pixel);
                    draw_tile(ui, &base, slot, false);
                }
        }
        ui->stale_tiles = 0;
        ui_copy(&canvas, &base);
    } else {
        draw_base(ui, &canvas);
    }
    ui->content_dirty = false;
    /* Hover states are redrawn over the cached layer. */
    int hover = ui->hover;
    if (hover == ID_SORT)
        draw_sort_chip(ui, &canvas, true);
    else if (hover >= ID_TILE && hover < ID_TILE + (int)PAGE_ENTRIES)
        draw_tile(ui, &canvas, hover - ID_TILE, true);
    else if (hover == ID_BACK)
        draw_back_button(ui, &canvas, true);
    else if ((hover >= ID_RAIL && hover < ID_RAIL + RAIL_COUNT) ||
             hover == ID_EXIT)
        draw_rail(ui, &canvas, hover);
    else if (hover >= ID_PREVIOUS && hover < ID_PAGE + PAGE_BUTTONS)
        draw_pager(ui, &canvas, hover);
    draw_detail(ui, &canvas);
    if (ui->sort_menu_open)
        draw_sort_menu(ui, &canvas, hover);
    ui->dirty = false;
}

/* ---- input ------------------------------------------------------------- */

int browser_ui_hit_test(float u, float v)
{
    int x = (int)(u * BROWSER_UI_WIDTH);
    int y = (int)(v * BROWSER_UI_HEIGHT);
    if (x >= SORT_X && x < SORT_X + SORT_W && y >= SORT_Y &&
        y < SORT_Y + SORT_H)
        return ID_SORT;
    if (x >= MENU_X && x < MENU_X + MENU_W && y >= MENU_Y &&
        y < MENU_Y + MENU_H)
        return ID_SORT_AREA; /* only counts while the menu is open */
    for (int slot = 0; slot < (int)PAGE_ENTRIES; ++slot) {
        int tx, ty;
        tile_rect(slot, &tx, &ty);
        if (x >= tx && x < tx + TILE_W && y >= ty && y < ty + TILE_H)
            return ID_TILE + slot;
    }
    for (int i = 0; i < RAIL_COUNT; ++i) {
        int top = RAIL_FIRST_Y + i * (RAIL_ITEM_H + 12);
        if (x >= RAIL_ITEM_X && x < RAIL_ITEM_X + RAIL_ITEM_W && y >= top &&
            y < top + RAIL_ITEM_H)
            return ID_RAIL + i;
    }
    if (x >= RAIL_ITEM_X && x < RAIL_ITEM_X + RAIL_ITEM_W &&
        y >= RAIL_EXIT_Y && y < RAIL_EXIT_Y + RAIL_ITEM_H)
        return ID_EXIT;
    if ((x - BACK_X) * (x - BACK_X) + (y - BACK_Y) * (y - BACK_Y) <= 36 * 36)
        return ID_BACK;
    if (y >= PAGER_Y && y < PAGER_Y + PAGER_H) {
        if (x >= CENTER_X && x < CENTER_X + 190)
            return ID_PREVIOUS;
        if (x >= CENTER_X + CENTER_W - 190 && x < CENTER_X + CENTER_W)
            return ID_NEXT;
        return ID_PAGE_AREA;
    }
    return -1;
}

/* Page buttons depend on the page count, which the stateless hit test does
 * not know; resolve them against the model. */
static int resolve_page_button(const struct browser_ui *ui, int x)
{
    if (page_count(ui) <= 1)
        return -1;
    size_t first, count;
    page_window(ui, &first, &count);
    for (size_t i = 0; i < count; ++i) {
        int bx, width;
        page_button_rect(count, i, &bx, &width);
        if (x >= bx && x < bx + width)
            return ID_PAGE + (int)i;
    }
    return -1;
}

void browser_ui_set_pointer(struct browser_ui *ui, bool visible, int x, int y,
                            int hover)
{
    if (hover == ID_SORT_AREA) {
        /* The menu hides everything below it while open; closed, the area
         * is just grid. */
        if (ui->sort_menu_open) {
            int row = (y - MENU_Y - 8) / MENU_ITEM_H;
            hover = row >= 0 && row < JF_SORT_COUNT ? ID_SORT_ITEM + row : -1;
        } else {
            hover = -1;
        }
    } else if (ui->sort_menu_open) {
        /* Clicks outside the menu only close it. */
        if (hover != ID_SORT)
            hover = -1;
    }
    if (hover == ID_SORT && !sortable(ui))
        hover = -1;
    if (hover == ID_PAGE_AREA)
        hover = resolve_page_button(ui, x);
    if ((hover == ID_PREVIOUS || hover == ID_NEXT) && page_count(ui) <= 1)
        hover = -1;
    if (hover == ID_BACK && ui->parent < 0)
        hover = -1;
    if (hover >= ID_TILE && hover < ID_TILE + (int)PAGE_ENTRIES &&
        slot_entry(ui, hover) < 0)
        hover = -1;
    if (hover != ui->hover) {
        ui->hover = hover;
        if (hover >= ID_TILE && hover < ID_TILE + (int)PAGE_ENTRIES)
            ui->detail = slot_entry(ui, hover);
        ui->dirty = true;
    }
    /* The host draws the pointer as its own layer; moving it does not
     * change the panel picture. */
    ui->pointer_visible = visible;
    ui->pointer_x = x;
    ui->pointer_y = y;
}

static void request(struct browser_ui *ui, const char *location)
{
    char error[256] = {0};
    snprintf(ui->location, sizeof(ui->location), "%s", location);
    if (file_browser_request(ui->browser, location, error,
                             sizeof(error)) < 0) {
        snprintf(ui->listing.error, sizeof(ui->listing.error), "%s", error);
        ui->content_dirty = true;
        ui->dirty = true;
    }
}

/* Loads the thumbnails of the current and the next page and releases those
 * more than one page away, so memory stays bounded for long listings. */
static void request_thumbnails(struct browser_ui *ui)
{
    if (!ui->thumbs || !ui->thumb_requested)
        return;
    for (size_t position = 0; position < ui->grid_count; ++position) {
        size_t page = position / PAGE_ENTRIES;
        size_t index = ui->grid[position];
        if (page + 1 < ui->page || page > ui->page + 1) {
            ui_image_free(ui->thumbs[index]);
            ui->thumbs[index] = NULL;
            ui->thumb_requested[index] = false;
        }
    }
    for (size_t page = ui->page; page <= ui->page + 1; ++page)
        for (size_t slot = 0; slot < PAGE_ENTRIES; ++slot) {
            size_t position = page * PAGE_ENTRIES + slot;
            if (position >= ui->grid_count)
                return;
            size_t index = ui->grid[position];
            const char *image = ui->listing.entries[index].image;
            if (!image || ui->thumb_requested[index])
                continue;
            ui->thumb_requested[index] = true;
            thumbnail_loader_request(ui->loader, ui->listing.generation, index,
                                     image);
        }
}

/* ---- persisted sort choice -------------------------------------------- */

static bool settings_path(char *path, size_t size)
{
    const char *base = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (base && base[0])
        return snprintf(path, size, "%s/stereonaut/settings.json", base) <
               (int)size;
    if (home && home[0])
        return snprintf(path, size, "%s/.config/stereonaut/settings.json",
                        home) < (int)size;
    return false;
}

static void load_sort(struct browser_ui *ui)
{
    ui->sort = JF_SORT_NAME;
    ui->sort_descending = false;
    char path[4200];
    if (!settings_path(path, sizeof(path)))
        return;
    json_object *root = json_object_from_file(path);
    json_object *value = NULL;
    if (!root)
        return;
    if (json_object_object_get_ex(root, "sort", &value) &&
        json_object_is_type(value, json_type_int)) {
        int sort = json_object_get_int(value);
        if (sort >= 0 && sort < JF_SORT_COUNT)
            ui->sort = sort;
    }
    if (json_object_object_get_ex(root, "sort_descending", &value) &&
        json_object_is_type(value, json_type_boolean))
        ui->sort_descending = json_object_get_boolean(value);
    json_object_put(root);
}

/* Keeps other keys of the settings file; failure to save is not fatal. */
static void save_sort(const struct browser_ui *ui)
{
    char path[4200];
    if (!settings_path(path, sizeof(path)))
        return;
    json_object *root = access(path, F_OK) == 0
                            ? json_object_from_file(path) : NULL;
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root)
            json_object_put(root);
        if (access(path, F_OK) == 0)
            return; /* unreadable: do not overwrite */
        root = json_object_new_object();
    }
    json_object_object_add(root, "sort", json_object_new_int(ui->sort));
    json_object_object_add(root, "sort_descending",
                           json_object_new_boolean(ui->sort_descending));
    char directory[4200];
    snprintf(directory, sizeof(directory), "%s", path);
    char *slash = strrchr(directory, '/');
    if (slash) {
        *slash = '\0';
        char parent[4200];
        snprintf(parent, sizeof(parent), "%s", directory);
        char *parent_slash = strrchr(parent, '/');
        if (parent_slash) {
            *parent_slash = '\0';
            mkdir(parent, 0700);
        }
        mkdir(directory, 0700);
    }
    char temporary[4300];
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if (json_object_to_file_ext(temporary, root, JSON_C_TO_STRING_PRETTY) == 0)
        chmod(temporary, 0600),
        rename(temporary, path);
    json_object_put(root);
}

/* Chooses a criterion; choosing the current one flips the direction. */
static void choose_sort(struct browser_ui *ui, int sort)
{
    if (sort == ui->sort && sort != JF_SORT_RANDOM)
        ui->sort_descending = !ui->sort_descending;
    else {
        ui->sort = sort;
        ui->sort_descending = default_descending(sort);
    }
    save_sort(ui);
    file_browser_set_sort(ui->browser, ui->sort, ui->sort_descending);
    ui->restore_page = 0;
    request(ui, ui->location);
}

static void set_page(struct browser_ui *ui, size_t page)
{
    if (page >= page_count(ui) || page == ui->page)
        return;
    ui->page = page;
    request_thumbnails(ui);
    ui->hover = -1;
    ui->content_dirty = true;
    ui->dirty = true;
}

void browser_ui_close_menus(struct browser_ui *ui)
{
    if (ui->sort_menu_open) {
        ui->sort_menu_open = false;
        ui->dirty = true;
    }
}

void browser_ui_flip(struct browser_ui *ui, int direction)
{
    if (ui->sort_menu_open)
        return;
    if (direction < 0 && ui->page > 0)
        set_page(ui, ui->page - 1);
    else if (direction > 0)
        set_page(ui, ui->page + 1);
}

enum browser_ui_action browser_ui_activate(struct browser_ui *ui, char *path,
                                           size_t path_size)
{
    int hover = ui->hover;
    if (ui->sort_menu_open) {
        if (hover >= ID_SORT_ITEM && hover < ID_SORT_ITEM + JF_SORT_COUNT)
            choose_sort(ui, hover - ID_SORT_ITEM);
        ui->sort_menu_open = false;
        ui->hover = -1;
        ui->dirty = true;
        return BROWSER_UI_NONE;
    }
    if (hover == ID_SORT && sortable(ui)) {
        ui->sort_menu_open = true;
        ui->dirty = true;
        return BROWSER_UI_NONE;
    }
    if (hover >= ID_TILE && hover < ID_TILE + (int)PAGE_ENTRIES) {
        int index = slot_entry(ui, hover);
        if (index < 0)
            return BROWSER_UI_NONE;
        const struct file_browser_entry *entry = &ui->listing.entries[index];
        if (entry->kind == FILE_BROWSER_MEDIA) {
            ui->selected_page = ui->page;
            snprintf(ui->selected_title, sizeof(ui->selected_title), "%s",
                     entry->name);
            snprintf(path, path_size, "%s", entry->path);
            return BROWSER_UI_FILE_SELECTED;
        }
        request(ui, entry->path);
    } else if (hover == ID_BACK && ui->parent >= 0) {
        request(ui, ui->listing.entries[ui->parent].path);
    } else if (hover == ID_PREVIOUS) {
        if (ui->page > 0)
            set_page(ui, ui->page - 1);
    } else if (hover == ID_NEXT) {
        set_page(ui, ui->page + 1);
    } else if (hover >= ID_PAGE && hover < ID_PAGE + PAGE_BUTTONS) {
        size_t first, count;
        page_window(ui, &first, &count);
        if ((size_t)(hover - ID_PAGE) < count)
            set_page(ui, first + (size_t)(hover - ID_PAGE));
    } else if (hover == ID_RAIL + RAIL_START) {
        request(ui, "root:");
    } else if (hover == ID_RAIL + RAIL_JELLYFIN) {
        request(ui, "jf:");
    } else if (hover == ID_RAIL + RAIL_LOCAL) {
        const char *home = getenv("HOME");
        request(ui, home && home[0] ? home : "/");
    } else if (hover == ID_EXIT) {
        return BROWSER_UI_EXIT_REQUESTED;
    }
    return BROWSER_UI_NONE;
}

/* ---- model ------------------------------------------------------------- */

static void free_thumbnails(struct browser_ui *ui)
{
    if (ui->thumbs)
        for (size_t i = 0; i < ui->listing.entry_count; ++i)
            ui_image_free(ui->thumbs[i]);
    free(ui->thumbs);
    ui->thumbs = NULL;
    free(ui->thumb_requested);
    ui->thumb_requested = NULL;
}

static void install_listing(struct browser_ui *ui,
                            struct file_browser_listing *listing)
{
    free_thumbnails(ui);
    free(ui->grid);
    file_browser_listing_clear(&ui->listing);
    ui->listing = *listing;
    size_t slots = ui->listing.entry_count ? ui->listing.entry_count : 1;
    ui->grid = calloc(slots, sizeof(*ui->grid));
    ui->thumbs = calloc(slots, sizeof(*ui->thumbs));
    ui->thumb_requested = calloc(slots, sizeof(*ui->thumb_requested));
    ui->grid_count = 0;
    ui->parent = -1;
    for (size_t i = 0; i < ui->listing.entry_count; ++i) {
        if (ui->listing.entries[i].kind == FILE_BROWSER_PARENT) {
            if (ui->parent < 0)
                ui->parent = (int)i;
        } else if (ui->grid) {
            ui->grid[ui->grid_count++] = i;
        }
    }
    ui->page = ui->restore_page;
    ui->restore_page = 0;
    if (ui->page >= page_count(ui))
        ui->page = 0;
    ui->hover = -1;
    ui->detail = -1;
    ui->content_dirty = true;
    ui->dirty = true;
    request_thumbnails(ui);
}

void browser_ui_set_thumbnail(struct browser_ui *ui, size_t index,
                              struct ui_image *image)
{
    if (!ui->thumbs || index >= ui->listing.entry_count) {
        ui_image_free(image);
        return;
    }
    ui_image_free(ui->thumbs[index]);
    ui->thumbs[index] = image;
    /* Only the tile showing it (if it is on this page) needs a redraw. */
    for (int slot = 0; slot < (int)PAGE_ENTRIES; ++slot)
        if (slot_entry(ui, slot) == (int)index) {
            ui->stale_tiles |= 1U << slot;
            ui->dirty = true;
        }
    if (ui->detail == (int)index)
        ui->dirty = true;
}

bool browser_ui_update(struct browser_ui *ui)
{
    bool changed = false;
    struct thumbnail_result thumbnail;
    while (thumbnail_loader_poll(ui->loader, &thumbnail)) {
        if (thumbnail.generation == ui->listing.generation &&
            thumbnail.image) {
            browser_ui_set_thumbnail(ui, thumbnail.index, thumbnail.image);
            changed = true;
        } else {
            ui_image_free(thumbnail.image);
        }
    }
    struct file_browser_listing updated = {0};
    int result = file_browser_poll(ui->browser, &updated);
    if (result <= 0)
        return changed;
    install_listing(ui, &updated);
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
    ui->detail = -1;
    ui->parent = -1;
    ui->dirty = true;
    ui->content_dirty = true;
    snprintf(ui->location, sizeof(ui->location), "%s", location);
    if (message)
        snprintf(ui->status_message, sizeof(ui->status_message), "%s",
                 message);
    ui->loader = thumbnail_loader_create();
    load_sort(ui);
    int opened = file_browser_open(&ui->browser, location, error, error_size);
    if (opened == 0)
        file_browser_set_sort(ui->browser, ui->sort, ui->sort_descending);
    return opened;
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
    ui->content_dirty = true;
    ui->dirty = true;
    request(ui, location);
}

void browser_ui_close(struct browser_ui *ui)
{
    thumbnail_loader_destroy(ui->loader);
    ui->loader = NULL;
    free_thumbnails(ui);
    free(ui->grid);
    ui->grid = NULL;
    file_browser_listing_clear(&ui->listing);
    file_browser_close(&ui->browser);
    free(ui->base);
    ui->base = NULL;
    ui_font_free(ui->font);
    ui_font_free(ui->bold);
    ui->font = ui->bold = NULL;
}
