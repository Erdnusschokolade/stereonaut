/* Renders the browser panel with sample data into a PPM file, composited
 * over a dark backdrop, so the layout can be checked without a headset:
 *
 *   ui-preview OUTPUT.ppm [HOVER_ID]
 *
 * Optional sample thumbnails: STEREONAUT_PREVIEW_THUMBS=dir with 1.ppm ...
 * (binary P6). Without them synthetic gradients are used. */
#define _POSIX_C_SOURCE 200809L
#include "browser-ui.h"
#include "player-ui.h"
#include "ui-draw.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The preview never talks to the listing worker. */
int file_browser_open(struct file_browser **result, const char *initial_path,
                      char *error, size_t error_size)
{
    (void)initial_path; (void)error; (void)error_size;
    *result = NULL;
    return 0;
}
int file_browser_request(struct file_browser *browser, const char *path,
                         char *error, size_t error_size)
{
    (void)browser; (void)path; (void)error; (void)error_size;
    return 0;
}
int file_browser_poll(struct file_browser *browser,
                      struct file_browser_listing *listing)
{
    (void)browser; (void)listing;
    return 0;
}
void file_browser_set_sort(struct file_browser *browser, int sort,
                           bool descending)
{
    (void)browser; (void)sort; (void)descending;
}
void file_browser_listing_clear(struct file_browser_listing *listing)
{
    for (size_t i = 0; i < listing->entry_count; ++i) {
        free(listing->entries[i].name);
        free(listing->entries[i].path);
        free(listing->entries[i].image);
    }
    free(listing->entries);
    free(listing->path);
    memset(listing, 0, sizeof(*listing));
}
void file_browser_close(struct file_browser **browser) { *browser = NULL; }

static struct ui_image *read_ppm(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return NULL;
    int width, height, maximum;
    struct ui_image *image = NULL;
    if (fscanf(file, "P6 %d %d %d", &width, &height, &maximum) == 3 &&
        fgetc(file) != EOF && (image = ui_image_create(width, height))) {
        for (int i = 0; i < width * height; ++i) {
            unsigned char rgb[3];
            if (fread(rgb, 1, 3, file) != 3)
                break;
            memcpy(image->rgba + i * 4, rgb, 3);
            image->rgba[i * 4 + 3] = 255;
        }
    }
    fclose(file);
    return image;
}

static struct ui_image *synthetic(int seed)
{
    struct ui_image *image = ui_image_create(320, 200);
    for (int y = 0; y < 200; ++y)
        for (int x = 0; x < 320; ++x) {
            uint8_t *p = image->rgba + (y * 320 + x) * 4;
            float t = (float)y / 200.0f;
            float wave = 0.5f + 0.5f * sinf(x * 0.03f + seed * 1.7f);
            p[0] = (uint8_t)(40 + 150 * wave * (1 - t) + seed * 13 % 60);
            p[1] = (uint8_t)(70 + 120 * t + seed * 29 % 50);
            p[2] = (uint8_t)(90 + 140 * (1 - wave) * t);
            p[3] = 255;
        }
    return image;
}

static void add(struct file_browser_listing *listing,
                enum file_browser_entry_kind kind, const char *name,
                const char *path)
{
    listing->entries = realloc(listing->entries,
                               (listing->entry_count + 1) *
                                   sizeof(*listing->entries));
    struct file_browser_entry *entry =
        &listing->entries[listing->entry_count++];
    *entry = (struct file_browser_entry){
        .kind = kind, .name = strdup(name), .path = strdup(path)};
    if (kind == FILE_BROWSER_MEDIA) {
        entry->duration_us = (int64_t)(1234 + listing->entry_count * 517) * 1000000;
        entry->year = 2023;
        snprintf(entry->codec, sizeof(entry->codec), "HEVC Main 10");
        entry->width = 7680;
        entry->height = 3840;
    }
}

/* Writes RGBA canvas pixels composited over a backdrop as binary PPM. */
static int write_ppm(const char *path, const uint8_t *bytes, unsigned width,
                     unsigned height, bool video_backdrop)
{
    FILE *out = fopen(path, "wb");
    if (!out)
        return 1;
    fprintf(out, "P6 %u %u 255\n", width, height);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            const uint8_t *p = bytes + ((size_t)y * width + x) * 4;
            float t = (float)y / height, s = (float)x / width;
            float back[3];
            if (video_backdrop) { /* bright, busy scene behind the panel */
                back[0] = 90 + 80 * sinf(s * 7.0f) * (1 - t);
                back[1] = 140 + 60 * t + 30 * sinf(t * 9.0f + s * 3.0f);
                back[2] = 170 - 90 * t;
            } else {
                back[0] = 28 + 30 * t;
                back[1] = 30 + 26 * t;
                back[2] = 40 + 30 * t;
            }
            float keep = 1.0f - p[3] / 255.0f;
            unsigned char rgb[3];
            for (int c = 0; c < 3; ++c)
                rgb[c] = (unsigned char)fminf(255.0f, p[c] + back[c] * keep);
            fwrite(rgb, 1, 3, out);
        }
    fclose(out);
    return 0;
}

static int preview_player(const char *path, int hover, bool popup)
{
    struct player_ui ui = {0};
    struct player_ui_state state = {
        .title = "Über den Wolken – Alpenflug 8K",
        .position_us = INT64_C(754000000),
        .duration_us = INT64_C(3165000000),
        .paused = hover == 99,
        .popup_open = popup,
        .projection_mode = 1,
        .stereo_layout = 1,
        .hover = hover == 99 ? 0 : hover,
        .preview_perspective = true,
        .preview_fov = 100,
        .video_levels = 1,
    };
    /* Hovering the seek bar shows the scrub preview (a stand-in picture of
     * one SBS eye). */
    struct ui_image *picture = NULL;
    if (hover == PLAYER_UI_SEEK_BAR) {
        picture = ui_image_create(160, 180);
        for (int i = 0; picture && i < 160 * 180; ++i) {
            uint8_t *p = picture->rgba + i * 4;
            p[0] = (uint8_t)(40 + i % 160);
            p[1] = (uint8_t)(60 + i / 160 / 2);
            p[2] = 140;
            p[3] = 255;
        }
        state.preview_visible = true;
        state.preview_us = INT64_C(2900000000);
        state.preview = picture;
    }
    uint32_t *pixels = malloc((size_t)PLAYER_UI_WIDTH * PLAYER_UI_HEIGHT * 4);
    player_ui_render(&ui, &state, pixels, false);
    ui_image_free(picture);
    int result = write_ppm(path, (const uint8_t *)pixels, PLAYER_UI_WIDTH,
                           PLAYER_UI_HEIGHT, true);
    player_ui_close(&ui);
    free(pixels);
    return result;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "--player") == 0)
        return preview_player(argv[2], argc > 3 ? atoi(argv[3]) : 0,
                              argc > 4 && atoi(argv[4]) != 0);
    if (argc < 2) {
        fprintf(stderr,
                "usage: %s OUTPUT.ppm [HOVER_ID]\n"
                "       %s --player OUTPUT.ppm [HOVER_ID] [POPUP]\n",
                argv[0], argv[0]);
        return 2;
    }
    struct browser_ui ui;
    char error[64];
    browser_ui_open(&ui, "jf:view:x", NULL, error, sizeof(error));
    struct file_browser_listing listing = {.generation = 1};
    listing.path = strdup("Jellyfin / VR-Filme");
    add(&listing, FILE_BROWSER_PARENT, "..", "jf:");
    const char *names[] = {
        "Über den Wolken – Alpenflug 8K", "Herbstwald im Nebel",
        "Tokyo Night Walk 東京", "Ocean Dive Great Barrier Reef",
        "Mondsee bei Nacht", "Konzert im Stadtpark (Live)",
        "Sehr langer Titel einer Dokumentation über Vulkane auf Island",
        "Wüstenfahrt bei Sonnenuntergang", "Kirschblüten in Kyoto", "Gletscherwanderung",
        "Wildlife Safari 180°", "Nordlichter",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        char path[64];
        snprintf(path, sizeof(path), i == 2 ? "/media/tokyo.mkv"
                                            : "jf:item:%zu:x", i);
        add(&listing, i == 4 ? FILE_BROWSER_DIRECTORY : FILE_BROWSER_MEDIA,
            names[i], path);
    }
    for (int i = 0; i < 30; ++i)
        add(&listing, FILE_BROWSER_MEDIA, "Weitere Seite", "jf:item:y:x");
    struct file_browser_listing copy = listing;
    ui.listing = copy;
    ui.grid = calloc(copy.entry_count, sizeof(*ui.grid));
    ui.thumbs = calloc(copy.entry_count, sizeof(*ui.thumbs));
    ui.parent = 0;
    for (size_t i = 1; i < copy.entry_count; ++i)
        ui.grid[ui.grid_count++] = i;
    const char *thumb_dir = getenv("STEREONAUT_PREVIEW_THUMBS");
    for (size_t i = 1; i < copy.entry_count; ++i) {
        if (i == 5 || i == 8)
            continue; /* folder / not yet loaded */
        struct ui_image *image = NULL;
        if (thumb_dir) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%zu.ppm", thumb_dir, i % 7 + 1);
            image = read_ppm(path);
        }
        ui.thumbs[i] = image ? image : synthetic((int)i);
    }
    ui.content_dirty = true;
    int hover = argc > 2 ? atoi(argv[2]) : 5;
    ui.hover = -1;
    browser_ui_set_pointer(&ui, true, 0, 0, -1);
    int px = 452 + (hover % 4) * 294 + 200, py = 196 + (hover / 4) * 220 + 90;
    if (hover >= 60) { /* sort menu open, item hover - 60 */
        ui.sort_menu_open = true;
        ui.sort = 4;
        ui.sort_descending = true;
        ui.content_dirty = true;
    } else if (hover == 50) {
        ui.sort = 4;
        ui.sort_descending = true;
    }
    browser_ui_set_pointer(&ui, true, px, py, hover);
    if (hover >= 60)
        ui.hover = hover;

    uint32_t *pixels = malloc((size_t)BROWSER_UI_WIDTH * BROWSER_UI_HEIGHT * 4);
    browser_ui_render(&ui, pixels, false);

    return write_ppm(argv[1], (const uint8_t *)pixels, BROWSER_UI_WIDTH,
                     BROWSER_UI_HEIGHT, false);
}
