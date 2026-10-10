#ifndef STEREONAUT_PLAYER_UI_H
#define STEREONAUT_PLAYER_UI_H

#include <stdbool.h>
#include <stdint.h>

/* Layout and CPU rasterizer of the playback panel: a control card (title,
 * seek bar, transport buttons) and, above it, the optional view popup
 * (projection, stereo layout, eye order). The host owns all state and
 * passes it in; nothing here knows about OpenXR or Vulkan. */

#define PLAYER_UI_WIDTH 1600U
#define PLAYER_UI_HEIGHT 600U
#define PLAYER_UI_PANEL_WIDTH_METERS 1.4f
#define PLAYER_UI_PANEL_HEIGHT_METERS 0.525f

/* Control ids. The transport ids equal the NV12_ACTION_* values. */
enum player_ui_control {
    PLAYER_UI_NONE = 0,
    PLAYER_UI_TOGGLE_PAUSE = 1,
    PLAYER_UI_SEEK_BACK = 2,
    PLAYER_UI_SEEK_FORWARD = 3,
    PLAYER_UI_BACK = 4,
    PLAYER_UI_SEEK_BAR = 5,
    PLAYER_UI_VIEW_POPUP = 10,
    PLAYER_UI_PROJECTION = 20, /* + enum nv12_projection_mode */
    PLAYER_UI_STEREO = 30,     /* + enum nv12_stereo_layout */
    PLAYER_UI_SWAP_EYES = 40,
};

struct player_ui_state {
    const char *title;
    int64_t position_us, duration_us;
    int64_t scrub_us;  /* shown instead of the position while scrubbing */
    bool scrubbing;
    bool loading;      /* no frame yet (start or seek) */
    bool paused;
    bool grabbed;
    bool popup_open;
    uint32_t projection_mode, stereo_layout;
    bool swap_eyes;
    int hover;         /* enum player_ui_control */
    /* Scrub preview above the seek bar (while hovered or dragged): the
     * position and, if the server has trickplay tiles, its picture. */
    bool preview_visible;
    int64_t preview_us;
    const struct ui_image *preview;
};

struct ui_font;
struct ui_image;

struct player_ui {
    struct ui_font *font, *bold;
};

void player_ui_render(struct player_ui *ui, const struct player_ui_state *state,
                      uint32_t *pixels, bool bgra);
void player_ui_close(struct player_ui *ui);
/* Control under panel texture coordinates u/v, or PLAYER_UI_NONE. */
int player_ui_hit_test(const struct player_ui_state *state, float u, float v);
/* True if u/v lies on a visible part of the panel (card or open popup);
 * elsewhere the panel is transparent and the grip shows/hides it. */
bool player_ui_on_panel(const struct player_ui_state *state, float u, float v);
/* Seek bar position (0..1) under u. */
float player_ui_seek_ratio(float u);

#endif
