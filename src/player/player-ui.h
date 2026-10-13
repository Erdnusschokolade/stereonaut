#ifndef STEREONAUT_PLAYER_UI_H
#define STEREONAUT_PLAYER_UI_H

#include <stdbool.h>
#include <stdint.h>

/* Layout and CPU rasterizer of the playback panel: a control card (title,
 * seek bar, transport buttons) and, above it, the optional popup with what
 * concerns the title: view (projection, stereo layout, eye order, head
 * motion), position (zoom, height, rotation, stereo offsets), picture
 * (levels, brightness, contrast, saturation, gamma), focus (autofocus
 * strength), passthrough (environment, chroma key) and information.
 * The global settings (playback, autofocus details, controller buttons,
 * language, updates)
 * use the same popup as a page of their own, which the host shows over the
 * browser (player_ui_render_settings). The host owns all state and passes
 * it in; nothing here knows about OpenXR or Vulkan. */

#define PLAYER_UI_WIDTH 1600U
#define PLAYER_UI_HEIGHT 680U
#define PLAYER_UI_PANEL_WIDTH_METERS 1.4f
#define PLAYER_UI_PANEL_HEIGHT_METERS 0.595f

/* The popup within the panel (and the settings page drawn alone). */
#define PLAYER_UI_POPUP_X 180
#define PLAYER_UI_POPUP_Y 10
#define PLAYER_UI_POPUP_W 1380
#define PLAYER_UI_POPUP_H 334

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
    /* Passthrough: share of the real environment darkened (global). */
    PLAYER_ADJUST_ENVIRONMENT_DIM,
    /* Chroma key of the title: 0 off, 1 green, 2 blue, 3 the colour picked
     * from the video (chips, no stepper), how far a colour may lie from
     * the key and still turn transparent, the soft edge above that, and
     * how much of the key's colour is removed from what stays (spill). */
    PLAYER_ADJUST_KEY_COLOR,
    PLAYER_ADJUST_KEY_THRESHOLD,
    PLAYER_ADJUST_KEY_SOFTNESS,
    PLAYER_ADJUST_KEY_SPILL,
    /* The picked colour (8-bit code values, not shown). */
    PLAYER_ADJUST_KEY_Y,
    PLAYER_ADJUST_KEY_CB,
    PLAYER_ADJUST_KEY_CR,
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

/* Popup tabs: those of the player, then those of the settings page. */
enum player_ui_tab {
    PLAYER_UI_TAB_VIEW,
    PLAYER_UI_TAB_POSITION,
    PLAYER_UI_TAB_PICTURE,
    PLAYER_UI_TAB_FOCUS,
    PLAYER_UI_TAB_PASSTHROUGH,
    PLAYER_UI_TAB_INFO,     /* description from Jellyfin, read only */
    PLAYER_UI_TAB_PLAYBACK, /* settings: preview, panel, AV1, passthrough */
    PLAYER_UI_TAB_AUTOFOCUS, /* settings: autofocus details */
    PLAYER_UI_TAB_BUTTONS,  /* settings: controller bindings */
    PLAYER_UI_TAB_LANGUAGE, /* settings: language of the interface */
    PLAYER_UI_TAB_UPDATES,  /* settings: updates from the release page */
    PLAYER_UI_TAB_COUNT,
};

#define PLAYER_UI_FIRST_SETTINGS_TAB PLAYER_UI_TAB_PLAYBACK

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
    PLAYER_UI_BINDING = 150,
    PLAYER_UI_BINDING_CLEAR = 170,    /* unbind the action being assigned */
    PLAYER_UI_BINDING_CANCEL = 171,
    PLAYER_UI_BINDING_DEFAULTS = 172, /* all bindings back to standard */
    PLAYER_UI_PASSTHROUGH = 180,      /* + 0 off, + 1 on (global) */
    PLAYER_UI_KEY_COLOR = 185,        /* + PLAYER_ADJUST_KEY_COLOR value */
    PLAYER_UI_KEY_PICK = 189,         /* pick the key colour in the video */
    PLAYER_UI_SETTINGS_CLOSE = 200,   /* leaves the settings page */
    PLAYER_UI_PASSTHROUGH_TOGGLE = 201, /* card button */
    /* + 0 automatic, + 1 + index into i18n_languages. */
    PLAYER_UI_LANGUAGE = 210,
    PLAYER_UI_LANGUAGE_END = 230,
    PLAYER_UI_UPDATE_CHECK = 231,
    PLAYER_UI_UPDATE_INSTALL = 232,
    PLAYER_UI_UPDATE_AUTO = 233,      /* + 0 off, + 1 on */
    PLAYER_UI_TAB = 240,              /* + enum player_ui_tab */
    PLAYER_UI_TAB_END = 270,
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
    /* The popup is the settings page (player_ui_render_settings). */
    bool settings_page;
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
    bool passthrough;     /* show the room where nothing is drawn */
    bool passthrough_supported; /* the runtime offers it */
    bool key_picking;     /* waiting for the trigger aimed at the video */
    /* Language chosen in the settings (index into i18n_languages or
     * I18N_AUTO) and the one "automatic" finds. */
    int language;
    int system_language;
    /* Updates tab: installed version ("" = built from the sources), the
     * newest release ("" = not known), a status line and what the buttons
     * may do. */
    const char *update_installed, *update_latest, *update_status;
    bool update_can_check, update_can_install, update_auto;
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
/* Only the popup as the settings page (state->settings_page), on a
 * transparent PLAYER_UI_WIDTH x PLAYER_UI_HEIGHT canvas; the host copies
 * the popup rectangle where it wants it. Hit testing works as for the
 * panel. */
void player_ui_render_settings(struct player_ui *ui,
                               const struct player_ui_state *state,
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
