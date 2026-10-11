#ifndef STEREONAUT_PLAYER_UI_H
#define STEREONAUT_PLAYER_UI_H

#include <stdbool.h>
#include <stdint.h>

/* Layout and CPU rasterizer of the playback panel: a control card (title,
 * seek bar, transport buttons) and, above it, the optional settings popup
 * with the tabs view (scrub preview, head motion, projection, stereo
 * layout, eye order), position (zoom, height, rotation, stereo offsets),
 * picture (levels, brightness, contrast, saturation, gamma), focus
 * (autofocus), controller buttons and information. The host owns all state and passes it in;
 * nothing here knows about OpenXR or Vulkan. */

/* The playback panel sits closer to the viewer than the media browser.
 * Keep roughly the same angular pixel density, otherwise text and vector
 * controls look visibly upscaled on high-resolution PCVR headsets. */
#define PLAYER_UI_WIDTH 2048U
#define PLAYER_UI_HEIGHT 870U
#define PLAYER_UI_PANEL_WIDTH_METERS 1.4f
#define PLAYER_UI_PANEL_HEIGHT_METERS 0.595f

/* Adjustable values of the video, shown as steppers in the popup. Most are
 * stored per title; tilt and head motion are posture/taste and global. */
enum player_adjust {
    PLAYER_ADJUST_ZOOM,       /* metres towards the picture */
    PLAYER_ADJUST_HEIGHT,     /* metres the picture moves up */
    PLAYER_ADJUST_SLIDE,      /* metres the picture moves right */
    PLAYER_ADJUST_TILT,       /* degrees the picture tilts up (global) */
    PLAYER_ADJUST_YAW,        /* degrees the picture turns right */
    PLAYER_ADJUST_ROLL,       /* degrees the picture rolls clockwise */
    PLAYER_ADJUST_STEREO_X,   /* degrees the eye pictures move apart */
    PLAYER_ADJUST_STEREO_Y,   /* degrees the right picture moves up */
    PLAYER_ADJUST_HEAD_MOTION, /* 6DoF: share of head translation (global) */
    PLAYER_ADJUST_AUTOFOCUS,  /* share of near disparity removed (global) */
    /* Autofocus only for subjects nearer than this (metres), if
     * player_ui_state.focus_threshold is set (global). */
    PLAYER_ADJUST_FOCUS_DISTANCE,
    /* Relative change of the subject distance that refocuses (global). */
    PLAYER_ADJUST_FOCUS_HYSTERESIS,
    PLAYER_ADJUST_FOCUS_FADE, /* seconds a refocus takes (global) */
    PLAYER_ADJUST_BRIGHTNESS,
    PLAYER_ADJUST_CONTRAST,
    PLAYER_ADJUST_SATURATION,
    PLAYER_ADJUST_GAMMA,
    PLAYER_ADJUST_COUNT,
};

struct player_adjust_info {
    const char *label;
    const char *key;      /* name in titles.json / settings.json */
    float minimum, maximum, step, standard;
    float display_scale;  /* shown value = value * display_scale */
    const char *format;   /* printf format of the shown value */
    bool global;
};

extern const struct player_adjust_info
    player_adjust_info[PLAYER_ADJUST_COUNT];

/* Popup tabs. */
enum player_ui_tab {
    PLAYER_UI_TAB_VIEW,
    PLAYER_UI_TAB_POSITION,
    PLAYER_UI_TAB_PICTURE,
    PLAYER_UI_TAB_FOCUS,
    PLAYER_UI_TAB_BUTTONS,  /* controller bindings (global) */
    PLAYER_UI_TAB_INFO,     /* description from Jellyfin, read only */
    PLAYER_UI_TAB_COUNT,
};

/* What happens when a title has played to its end (global setting). */
enum player_end_action {
    PLAYER_END_STOP,    /* back to the browser */
    PLAYER_END_REPEAT,  /* the same title again */
    PLAYER_END_NEXT,    /* the next title of the folder */
    PLAYER_END_COUNT,
};

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
    PLAYER_UI_LEVELS = 45,       /* + enum nv12_video_levels */
    PLAYER_UI_PREVIEW_MODE = 50, /* + 0 original, + 1 perspective */
    PLAYER_UI_PREVIEW_FOV = 60,  /* + index into player_ui_preview_fovs */
    PLAYER_UI_TAB = 70,          /* + enum player_ui_tab */
    PLAYER_UI_RECENTER = 75,     /* centre the picture on the gaze */
    PLAYER_UI_RESET_POSITION = 76,
    PLAYER_UI_RESET_PICTURE = 77,
    PLAYER_UI_GAZE_DEBUG = 78,   /* show the gaze and autofocus values */
    PLAYER_UI_VOLUME_DOWN = 90,
    PLAYER_UI_VOLUME_UP = 91,
    PLAYER_UI_END_ACTION = 92,   /* cycles enum player_end_action */
    PLAYER_UI_AUTOHIDE = 93,     /* + index into player_ui_autohide */
    PLAYER_UI_FOCUS_THRESHOLD = 96, /* + 0 always, + 1 only near subjects */
    PLAYER_UI_AV1_SOFTWARE = 98,    /* + 0 off, + 1 on (experimental) */
    /* + 2 * enum player_adjust: decrease; + 1: increase. */
    PLAYER_UI_ADJUST = 100,
    PLAYER_UI_ADJUST_END = PLAYER_UI_ADJUST + 2 * PLAYER_ADJUST_COUNT,
    /* + enum binding_action: wait for the buttons to assign. */
    PLAYER_UI_BINDING = 140,
    PLAYER_UI_BINDING_CLEAR = 160,    /* unbind the action being assigned */
    PLAYER_UI_BINDING_CANCEL = 161,
    PLAYER_UI_BINDING_DEFAULTS = 162, /* all bindings back to standard */
};

/* Seconds without input after which the panel hides during playback
 * (0 = never). */
#define PLAYER_UI_AUTOHIDE_COUNT 3
extern const int player_ui_autohide[PLAYER_UI_AUTOHIDE_COUNT];

/* Field of view choices (degrees) of the perspective scrub preview. */
#define PLAYER_UI_PREVIEW_FOV_COUNT 4
extern const int player_ui_preview_fovs[PLAYER_UI_PREVIEW_FOV_COUNT];

struct player_ui_state {
    const char *title;
    int64_t position_us, duration_us;
    int64_t scrub_us;  /* shown instead of the position while scrubbing */
    bool scrubbing;
    bool loading;      /* no frame yet (start or seek) */
    bool paused;
    bool grabbed;
    bool popup_open;
    int popup_tab;     /* enum player_ui_tab */
    uint32_t projection_mode, stereo_layout;
    bool swap_eyes;
    uint32_t video_levels;
    uint32_t format_source; /* enum nv12_format_source */
    uint32_t fisheye_fov;   /* degrees, shown for fisheye titles */
    float volume;      /* linear output gain, 0 to 1.5 */
    int end_action;    /* enum player_end_action */
    int autohide;      /* seconds, see player_ui_autohide */
    /* Jellyfin description of the title (chapters, information) or NULL. */
    const struct jf_details *details;
    const float *adjust; /* PLAYER_ADJUST_COUNT values */
    bool gaze_debug;
    bool focus_threshold; /* autofocus only below FOCUS_DISTANCE */
    bool av1_software;    /* decode AV1 up to 4K on the CPU (next start) */
    /* Controller bindings (BINDING_ACTION_COUNT, NULL = standard) and the
     * action waiting for buttons, or -1. */
    const struct binding *bindings;
    int learn_action;
    const char *debug_text; /* line at the bottom of the card, or NULL */
    int hover;         /* enum player_ui_control */
    /* Scrub preview above the seek bar (while hovered or dragged): the
     * position and, if the server has trickplay tiles, its picture. */
    bool preview_visible;
    int64_t preview_us;
    const struct ui_image *preview;
    /* Preview settings shown in the view popup. */
    bool preview_perspective;
    int preview_fov; /* degrees */
};

struct ui_font;
struct ui_image;
struct jf_details;
struct binding;

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
/* Value one step from `value` (direction -1 or +1), clamped. */
float player_adjust_step(enum player_adjust adjust, float value,
                         int direction);

#endif
