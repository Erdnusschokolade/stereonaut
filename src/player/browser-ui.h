#ifndef STEREONAUT_BROWSER_UI_H
#define STEREONAUT_BROWSER_UI_H

#include "file-browser.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Model and CPU rasterizer of the media browser panel. It knows nothing about
 * OpenXR or Vulkan: the host feeds pointer hits in and uploads the pixels. */

#define BROWSER_UI_WIDTH 1024U
#define BROWSER_UI_HEIGHT 768U
#define BROWSER_UI_PANEL_WIDTH_METERS 1.6f
#define BROWSER_UI_PANEL_HEIGHT_METERS 1.2f

enum browser_ui_action {
    BROWSER_UI_NONE,
    BROWSER_UI_FILE_SELECTED,
    BROWSER_UI_EXIT_REQUESTED,
};

struct browser_ui {
    struct file_browser *browser;
    struct file_browser_listing listing;
    char status_message[256];
    size_t page;
    int hover;
    bool pointer_visible;
    int pointer_x;
    int pointer_y;
    bool dirty;
};

int browser_ui_open(struct browser_ui *ui, const char *location,
                    const char *message, char *error, size_t error_size);
void browser_ui_close(struct browser_ui *ui);
/* Shows another location (directory or Jellyfin path) with an optional
 * status line. */
void browser_ui_navigate(struct browser_ui *ui, const char *location,
                         const char *message);
/* Picks up a finished listing; true if the picture changed. */
bool browser_ui_update(struct browser_ui *ui);
void browser_ui_render(struct browser_ui *ui, uint32_t *pixels, bool bgra);
/* Maps panel texture coordinates to an item (0-8 entries, 9 previous,
 * 10 next, 11 exit) or -1. */
int browser_ui_hit_test(float u, float v);
void browser_ui_set_pointer(struct browser_ui *ui, bool visible, int x, int y,
                            int hover);
enum browser_ui_action browser_ui_activate(struct browser_ui *ui, char *path,
                                           size_t path_size);

#endif
