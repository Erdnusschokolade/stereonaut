#define _POSIX_C_SOURCE 200809L
#include "player-ui.h"

#include "controller-bindings.h"
#include "i18n.h"
#include "jellyfin.h"
#include "nv12-playback-ipc.h"
#include "ui-draw.h"
#include "ui-layout.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Control card. */
#define CARD_X 40
#define CARD_Y 360
#define CARD_W 1520
#define CARD_H 310

/* Seek bar. */
#define BAR_X 250
#define BAR_W 1100
#define BAR_Y (CARD_Y + 96)
#define BAR_H 12
#define BAR_HIT_TOP (CARD_Y + 68)
#define BAR_HIT_BOTTOM (CARD_Y + 136)

/* Transport row. */
#define ROW_Y (CARD_Y + 220)
#define BACK_X 116
#define SKIP_BACK_X 650
#define PLAY_X 800
#define SKIP_FORWARD_X 950
#define VIEW_X 1484
#define PASSTHROUGH_X 532
#define VOLUME_DOWN_X 1160
#define VOLUME_UP_X 1350
#define VOLUME_R 30
#define END_X 196
#define END_W 260
#define END_H 48
#define SMALL_R 38
#define SKIP_R 42
#define PLAY_R 54

/* Scrub preview above the card: a strip of trickplay pictures, the middle
 * one (the previewed position, centred on it) larger and framed. */
#define PREVIEW_IMAGE_H 196
#define PREVIEW_SIDE_H 140
#define PREVIEW_GAP 10
#define PREVIEW_PAD 6
#define PREVIEW_LABEL_H 40
#define PREVIEW_BOTTOM (CARD_Y - 12)

/* Settings popup above the card: a tab row and three columns of controls
 * in up to three rows. */
#define POPUP_X PLAYER_UI_POPUP_X
#define POPUP_Y PLAYER_UI_POPUP_Y
#define POPUP_W PLAYER_UI_POPUP_W
#define POPUP_H PLAYER_UI_POPUP_H
#define COLUMN_W 416
#define COLUMN_X(column) (POPUP_X + 22 + (column) * 460)
#define TAB_Y (POPUP_Y + 18)
#define TAB_W 200
#define ROW_LABEL_Y(row) (POPUP_Y + 86 + (row) * 82)
#define ROW_CHIP_Y(row) (ROW_LABEL_Y(row) + 32)
#define STEPPER_Y(row) (POPUP_Y + 98 + (row) * 70)
#define STEPPER_LABEL_W 170
#define STEPPER_BUTTON_W 56
#define CHIP_GAP 10
#define CHIP_H 44

const int player_ui_preview_fovs[PLAYER_UI_PREVIEW_FOV_COUNT] = {
    60, 80, 100, 120,
};

const int player_ui_autohide[PLAYER_UI_AUTOHIDE_COUNT] = {0, 5, 15};

const struct player_adjust_info player_adjust_info[PLAYER_ADJUST_COUNT] = {
    [PLAYER_ADJUST_ZOOM] = {N_("Zoom"), "zoom", -0.5f, 0.8f, 0.05f, 0.0f,
                            100.0f, "%+.0f cm", false},
    [PLAYER_ADJUST_HEIGHT] = {N_("Height"), "height", -0.5f, 0.5f, 0.05f, 0.0f,
                              100.0f, "%+.0f cm", false},
    [PLAYER_ADJUST_SLIDE] = {N_("Sideways"), "slide", -0.5f, 0.5f, 0.05f, 0.0f,
                             100.0f, "%+.0f cm", false},
    [PLAYER_ADJUST_TILT] = {N_("Tilt"), "tilt", -90.0f, 90.0f, 5.0f, 0.0f,
                            1.0f, "%+.0f°", true},
    [PLAYER_ADJUST_YAW] = {N_("Turn"), "yaw", -180.0f, 180.0f, 5.0f, 0.0f,
                           1.0f, "%+.0f°", false},
    [PLAYER_ADJUST_ROLL] = {N_("Roll"), "roll", -30.0f, 30.0f, 1.0f, 0.0f,
                            1.0f, "%+.0f°", false},
    [PLAYER_ADJUST_STEREO_X] = {N_("Stereo horiz."), "stereo_x", -3.0f, 3.0f,
                                0.1f, 0.0f, 1.0f, "%+.1f°", false},
    [PLAYER_ADJUST_STEREO_Y] = {N_("Stereo vert."), "stereo_y", -2.0f, 2.0f,
                                0.1f, 0.0f, 1.0f, "%+.1f°", false},
    [PLAYER_ADJUST_HEAD_MOTION] = {N_("Strength"), "head_motion", 0.0f, 1.5f,
                                   0.1f, 0.0f, 100.0f, "%.0f %%", true},
    [PLAYER_ADJUST_AUTOFOCUS] = {N_("Autofocus"), "autofocus", 0.0f, 1.0f,
                                 0.1f, 0.0f, 100.0f, "%.0f %%", true},
    [PLAYER_ADJUST_FOCUS_DISTANCE] = {N_("Closer than"), "focus_distance",
                                      0.2f, 1.0f, 0.05f, 0.6f, 100.0f,
                                      "%.0f cm", true},
    [PLAYER_ADJUST_FOCUS_HYSTERESIS] = {N_("Hysteresis"), "focus_hysteresis",
                                        0.0f, 0.5f, 0.05f, 0.15f, 100.0f,
                                        "%.0f %%", true},
    [PLAYER_ADJUST_FOCUS_FADE] = {N_("Transition"), "focus_fade", 0.0f, 3.0f,
                                  0.1f, 1.0f, 1.0f, "%.1f s", true},
    [PLAYER_ADJUST_BRIGHTNESS] = {N_("Brightness"), "brightness", -0.3f, 0.3f,
                                  0.02f, 0.0f, 100.0f, "%+.0f", false},
    [PLAYER_ADJUST_CONTRAST] = {N_("Contrast"), "contrast", 0.5f, 1.5f, 0.05f,
                                1.0f, 100.0f, "%.0f %%", false},
    [PLAYER_ADJUST_SATURATION] = {N_("Saturation"), "saturation", 0.0f, 2.0f,
                                  0.1f, 1.0f, 100.0f, "%.0f %%", false},
    [PLAYER_ADJUST_GAMMA] = {N_("Gamma"), "gamma", 0.5f, 2.0f, 0.05f, 1.0f,
                             1.0f, "%.2f", false},
    [PLAYER_ADJUST_ENVIRONMENT_DIM] = {N_("Dimming"), "environment_dim", 0.0f,
                                       1.0f, 0.1f, 0.0f, 100.0f, "%.0f %%",
                                       true},
    [PLAYER_ADJUST_KEY_COLOR] = {N_("Chroma key"), "key_color", 0.0f, 3.0f,
                                 1.0f, 0.0f, 1.0f, "%.0f", false},
    [PLAYER_ADJUST_KEY_THRESHOLD] = {N_("Threshold"), "key_threshold", 0.0f,
                                     0.4f, 0.002f, 0.12f, 100.0f, "%.1f",
                                     false},
    [PLAYER_ADJUST_KEY_SOFTNESS] = {N_("Softness"), "key_softness", 0.0f, 0.3f,
                                    0.005f, 0.06f, 100.0f, "%.1f", false},
    [PLAYER_ADJUST_KEY_SPILL] = {N_("Spill"), "key_spill", 0.0f, 1.0f, 0.1f,
                                 0.6f, 100.0f, "%.0f %%", false},
    [PLAYER_ADJUST_KEY_Y] = {"Key Y", "key_y", 0.0f, 255.0f, 1.0f, 128.0f,
                             1.0f, "%.0f", false},
    [PLAYER_ADJUST_KEY_CB] = {"Key Cb", "key_cb", 0.0f, 255.0f, 1.0f,
                              128.0f, 1.0f, "%.0f", false},
    [PLAYER_ADJUST_KEY_CR] = {"Key Cr", "key_cr", 0.0f, 255.0f, 1.0f,
                              128.0f, 1.0f, "%.0f", false},
};

_Static_assert(PLAYER_UI_ADJUST_END <= PLAYER_UI_BINDING,
               "adjust controls overlap the binding controls");
_Static_assert(PLAYER_UI_BINDING + BINDING_ACTION_COUNT <=
                   PLAYER_UI_BINDING_CLEAR,
               "binding controls overlap");
_Static_assert(PLAYER_UI_TAB + PLAYER_UI_TAB_COUNT <= PLAYER_UI_TAB_END,
               "tab controls overlap");

static const struct ui_color color_card = UI_RGBA(17, 18, 22, 225);
static const struct ui_color color_border = UI_RGBA(255, 255, 255, 18);
static const struct ui_color color_button = UI_RGBA(42, 45, 54, 255);
static const struct ui_color color_button_hover = UI_RGBA(62, 67, 80, 255);
static const struct ui_color color_text = UI_RGBA(236, 239, 244, 255);
static const struct ui_color color_dim = UI_RGBA(150, 157, 170, 255);
static const struct ui_color color_accent = UI_RGBA(56, 160, 255, 255);
static const struct ui_color color_track = UI_RGBA(255, 255, 255, 50);
static const struct ui_color color_shadow = UI_RGBA(0, 0, 0, 100);

static void format_time(char text[32], int64_t microseconds)
{
    if (microseconds < 0)
        microseconds = 0;
    int64_t seconds = microseconds / 1000000;
    if (seconds >= 3600)
        snprintf(text, 32, "%d:%02d:%02d", (int)(seconds / 3600),
                 (int)(seconds / 60 % 60), (int)(seconds % 60));
    else
        snprintf(text, 32, "%d:%02d", (int)(seconds / 60),
                 (int)(seconds % 60));
}

static float time_ratio(const struct player_ui_state *state, int64_t position)
{
    if (state->duration_us <= 0 || position <= 0)
        return 0.0f;
    if (position >= state->duration_us)
        return 1.0f;
    return (float)((double)position / (double)state->duration_us);
}

static float shown_ratio(const struct player_ui_state *state)
{
    return time_ratio(state, state->scrubbing ? state->scrub_us
                                              : state->position_us);
}

static bool in_circle(float x, float y, float cx, float cy, float r)
{
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}

static bool in_rect(float x, float y, float rx, float ry, float rw, float rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

/* ---- popup layout -------------------------------------------------------
 * The popup is described once as a list of widgets; drawing and hit
 * testing both walk it. */

/* A paragraph is a hint wrapped across a column into up to `lines` lines,
 * so translations need not match the English line breaks. */
enum widget_kind {
    WIDGET_CHIP,
    WIDGET_TEXT,
    WIDGET_HINT,
    WIDGET_PARAGRAPH,
    WIDGET_VALUE,
};

struct widget {
    enum widget_kind kind;
    int control; /* chips only */
    int x, y, width;
    bool selected;
    bool small; /* chips: smaller text */
    int lines;  /* paragraphs */
    char text[256];
};

#define MAX_WIDGETS 64

struct popup_layout {
    struct widget items[MAX_WIDGETS];
    int count;
};

static struct widget *add_widget(struct popup_layout *layout,
                                 enum widget_kind kind, int x, int y,
                                 int width, const char *text)
{
    if (layout->count >= MAX_WIDGETS)
        return &layout->items[MAX_WIDGETS - 1];
    struct widget *widget = &layout->items[layout->count++];
    *widget = (struct widget){.kind = kind, .x = x, .y = y, .width = width};
    snprintf(widget->text, sizeof(widget->text), "%s", text);
    return widget;
}

static void add_paragraph(struct popup_layout *layout, int x, int y,
                          int lines, const char *text)
{
    add_widget(layout, WIDGET_PARAGRAPH, x, y, COLUMN_W, text)->lines = lines;
}

static void add_chip(struct popup_layout *layout, int control, int x, int y,
                     int width, const char *text, bool selected)
{
    struct widget *chip = add_widget(layout, WIDGET_CHIP, x, y, width, text);
    chip->control = control;
    chip->selected = selected;
}

/* `count` equal chips across a column. */
static void add_chip_row(struct popup_layout *layout, int column, int row,
                         int count, int first_control,
                         const char *const *labels, int selected)
{
    int width = (COLUMN_W - (count - 1) * CHIP_GAP) / count;
    for (int i = 0; i < count; ++i)
        add_chip(layout, first_control + i,
                 COLUMN_X(column) + i * (width + CHIP_GAP), ROW_CHIP_Y(row),
                 width, tr(labels[i]), i == selected);
}

static void format_adjust(char text[32], enum player_adjust adjust,
                          float value)
{
    const struct player_adjust_info *info = &player_adjust_info[adjust];
    if ((adjust == PLAYER_ADJUST_HEAD_MOTION ||
         adjust == PLAYER_ADJUST_AUTOFOCUS ||
         adjust == PLAYER_ADJUST_FOCUS_HYSTERESIS) && value <= 0.0f) {
        snprintf(text, 32, "%s", tr("Off"));
        return;
    }
    if (adjust == PLAYER_ADJUST_FOCUS_FADE && value <= 0.0f) {
        snprintf(text, 32, "%s", tr("Instant"));
        return;
    }
    float shown = value * info->display_scale;
    if (shown > -0.05f && shown < 0.05f)
        shown = 0.0f; /* no "-0" */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
    snprintf(text, 32, info->format, shown);
#pragma GCC diagnostic pop
    i18n_localize_number(text);
}

/* Label, [-], value, [+] on one line. */
static void add_stepper(struct popup_layout *layout,
                        const struct player_ui_state *state, int column,
                        int y, enum player_adjust adjust, const char *label)
{
    int x = COLUMN_X(column);
    add_widget(layout, WIDGET_TEXT, x, y + 10, STEPPER_LABEL_W,
               tr(label ? label : player_adjust_info[adjust].label));
    int minus_x = x + STEPPER_LABEL_W + 6;
    int plus_x = x + COLUMN_W - STEPPER_BUTTON_W;
    add_chip(layout, PLAYER_UI_ADJUST + 2 * (int)adjust, minus_x, y,
             STEPPER_BUTTON_W, "−", false);
    add_chip(layout, PLAYER_UI_ADJUST + 2 * (int)adjust + 1, plus_x, y,
             STEPPER_BUTTON_W, "+", false);
    char value[32];
    format_adjust(value, adjust, state->adjust ? state->adjust[adjust]
                                               : player_adjust_info[adjust]
                                                     .standard);
    int value_x = minus_x + STEPPER_BUTTON_W;
    add_widget(layout, WIDGET_VALUE, value_x, y + 8, plus_x - value_x, value);
}

/* ---- popup tabs laid out with Clay (ui-layout.h) -------------------------
 * The tab's content fills the popup below the tab row, mostly as three
 * columns. A column is a stack on a grid of rows: a hint above a row (a
 * chip, a chip row, a stepper or a line of text), and paragraphs. A
 * column's last chip may sit at its bottom after a spacer. Columns of
 * steppers alone use the tighter stepper grid. */

#define CONTENT_X COLUMN_X(0)
#define CONTENT_Y ROW_LABEL_Y(0)
#define CONTENT_W (3 * COLUMN_W + 2 * COLUMN_GAP)
#define CONTENT_H (ROW_CHIP_Y(2) + CHIP_H - CONTENT_Y)
#define COLUMN_GAP (COLUMN_X(1) - COLUMN_X(0) - COLUMN_W)
/* Between a hint and its row, and between one row and the next hint. */
#define STACK_GAP (ROW_CHIP_Y(0) - ROW_LABEL_Y(0) - HINT_H)
#define HINT_H 26 /* hint text (22 px) and its box */
/* A hint and its row. */
#define BAND_H (ROW_LABEL_Y(1) - ROW_LABEL_Y(0) - STACK_GAP)
#define STEPPER_GAP (STEPPER_Y(1) - STEPPER_Y(0) - CHIP_H)

#define COLUMNS                                                            \
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, \
                     .childGap = COLUMN_GAP}})

#define COLUMN                                                             \
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, \
                     .childGap = STACK_GAP,                                \
                     .layoutDirection = CLAY_TOP_TO_BOTTOM}})

#define STEPPER_COLUMN                                                     \
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, \
                     .padding = {.top = STEPPER_Y(0) - CONTENT_Y},         \
                     .childGap = STEPPER_GAP,                              \
                     .layoutDirection = CLAY_TOP_TO_BOTTOM}})

/* One band of the row grid (a hint and its row) for other content. */
#define BAND                                                               \
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0),                       \
                                CLAY_SIZING_FIXED(BAND_H)},                \
                     .layoutDirection = CLAY_TOP_TO_BOTTOM}})

/* Pushes what follows to the bottom of the column. */
#define SPACER                                                             \
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}}) {}

/* A row left empty, keeping the grid. */
#define EMPTY_ROW                                                          \
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0),                       \
                                CLAY_SIZING_FIXED(CHIP_H)}}}) {}
#define EMPTY_HINT                                                         \
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0),                       \
                                CLAY_SIZING_FIXED(HINT_H)}}}) {}

static void hint(struct ui_layout *layout, const char *text)
{
    ui_layout_text(layout, text, UI_LAYOUT_REGULAR, 22, color_dim);
}

static void paragraph(struct ui_layout *layout, int lines, const char *text)
{
    ui_layout_paragraph(layout, text, UI_LAYOUT_REGULAR, 22, color_dim, 30,
                        lines);
}

/* A paragraph remarking on the row above it, set off a little more. */
static void note(struct ui_layout *layout, int lines, const char *text)
{
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                     .padding = {.top = STACK_GAP}}}) {
        paragraph(layout, lines, text);
    }
}

/* A line of text in a row of its own (shortened to the column). */
static void text_row(struct ui_layout *layout, const char *text)
{
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0),
                                CLAY_SIZING_FIXED(CHIP_H)}}}) {
        ui_layout_label(layout, text, UI_LAYOUT_REGULAR, 24, color_text,
                        UI_ALIGN_LEFT, CLAY_SIZING_GROW(0));
    }
}

/* A chip `width` wide; `small` for longer texts (button names). */
static void chip_sized(struct ui_layout *layout,
                       const struct player_ui_state *state, int control,
                       const char *text, bool selected, Clay_SizingAxis width,
                       bool small)
{
    struct ui_color background = selected ? color_accent
                                 : state->hover == control
                                     ? color_button_hover
                                     : color_button;
    CLAY({.id = ui_layout_control(layout, control),
          .layout = {.sizing = {width, CLAY_SIZING_FIXED(CHIP_H)},
                     .padding = {8, 8, 0, 0},
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = ui_layout_color(background),
          .cornerRadius = CLAY_CORNER_RADIUS(CHIP_H / 2.0f)}) {
        ui_layout_label(layout, text,
                        selected ? UI_LAYOUT_BOLD : UI_LAYOUT_REGULAR,
                        small ? 20 : 24, color_text, UI_ALIGN_CENTER,
                        CLAY_SIZING_GROW(0));
    }
}

/* A chip sharing the row with its siblings. */
static void chip(struct ui_layout *layout,
                 const struct player_ui_state *state, int control,
                 const char *text, bool selected)
{
    chip_sized(layout, state, control, text, selected, CLAY_SIZING_GROW(0),
               false);
}

#define CHIP_ROW                                                           \
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0),                       \
                                CLAY_SIZING_FIXED(CHIP_H)},                \
                     .childGap = CHIP_GAP}})

/* `count` equal chips across the column. */
static void chip_row(struct ui_layout *layout,
                     const struct player_ui_state *state, int count,
                     int first_control, const char *const *labels,
                     int selected)
{
    CHIP_ROW {
        for (int i = 0; i < count; ++i)
            chip(layout, state, first_control + i, tr(labels[i]),
                 i == selected);
    }
}

/* Label, [-], value, [+] in one row. */
static void stepper(struct ui_layout *layout,
                    const struct player_ui_state *state,
                    enum player_adjust adjust, const char *label)
{
    char value[32];
    format_adjust(value, adjust, state->adjust ? state->adjust[adjust]
                                               : player_adjust_info[adjust]
                                                     .standard);
    int control = PLAYER_UI_ADJUST + 2 * (int)adjust;
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0),
                                CLAY_SIZING_FIXED(CHIP_H)},
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
        CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(STEPPER_LABEL_W + 6),
                                    CLAY_SIZING_FIT(0)},
                         .padding = {.right = 6}}}) {
            ui_layout_label(layout,
                            tr(label ? label
                                     : player_adjust_info[adjust].label),
                            UI_LAYOUT_REGULAR, 24, color_text, UI_ALIGN_LEFT,
                            CLAY_SIZING_GROW(0));
        }
        chip_sized(layout, state, control, "−", false,
                   CLAY_SIZING_FIXED(STEPPER_BUTTON_W), false);
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                         .padding = {4, 4, 0, 0}}}) {
            ui_layout_label(layout, value, UI_LAYOUT_BOLD, 24, color_text,
                            UI_ALIGN_CENTER, CLAY_SIZING_GROW(0));
        }
        chip_sized(layout, state, control + 1, "+", false,
                   CLAY_SIZING_FIXED(STEPPER_BUTTON_W), false);
    }
}

/* Focus tab: the autofocus strength and the gaze display (first column),
 * what it does (second). */
static void layout_focus(const struct player_ui_state *state,
                         struct ui_layout *layout)
{
    COLUMNS {
        COLUMN {
            hint(layout, tr("Autofocus (180°/360° with SBS/TB only)"));
            stepper(layout, state, PLAYER_ADJUST_AUTOFOCUS, N_("Strength"));
            hint(layout, tr("Check the measurement"));
            chip(layout, state, PLAYER_UI_GAZE_DEBUG, tr("Show gaze"),
                 state->gaze_debug);
        }
        COLUMN {
            paragraph(layout, 5,
                      tr("Near subjects are pushed back to about 1 m. "
                         "Applies to all titles; threshold, hysteresis and "
                         "transition under Settings."));
        }
        COLUMN {}
    }
}

/* Playback settings: the scrub preview and the panel (first column), AV1
 * (second), passthrough (third). */
static void layout_playback(const struct player_ui_state *state,
                            struct ui_layout *layout)
{
    static const char *const modes[2] = {N_("Original"), N_("Perspective")};
    const char *fovs[PLAYER_UI_PREVIEW_FOV_COUNT];
    char fov_text[PLAYER_UI_PREVIEW_FOV_COUNT][16];
    int fov_selected = -1;
    for (int i = 0; i < PLAYER_UI_PREVIEW_FOV_COUNT; ++i) {
        snprintf(fov_text[i], sizeof(fov_text[i]), "%d°",
                 player_ui_preview_fovs[i]);
        fovs[i] = fov_text[i];
        if (state->preview_fov == player_ui_preview_fovs[i])
            fov_selected = i;
    }
    static const char *const autohide[PLAYER_UI_AUTOHIDE_COUNT] = {
        N_("Never"), N_("after 5 s"), N_("after 15 s"),
    };
    int autohide_selected = 0;
    for (int i = 0; i < PLAYER_UI_AUTOHIDE_COUNT; ++i)
        if (player_ui_autohide[i] == state->autohide)
            autohide_selected = i;
    static const char *const off_on[2] = {N_("Off"), N_("On")};
    COLUMNS {
        COLUMN {
            hint(layout, tr("Scrub preview"));
            chip_row(layout, state, 2, PLAYER_UI_PREVIEW_MODE, modes,
                     state->preview_perspective ? 1 : 0);
            hint(layout, tr("Field of view (perspective)"));
            chip_row(layout, state, PLAYER_UI_PREVIEW_FOV_COUNT,
                     PLAYER_UI_PREVIEW_FOV, fovs, fov_selected);
            hint(layout, tr("Hide the panel (playback)"));
            chip_row(layout, state, PLAYER_UI_AUTOHIDE_COUNT,
                     PLAYER_UI_AUTOHIDE, autohide, autohide_selected);
        }
        COLUMN {
            hint(layout, tr("AV1 up to 4K on the CPU"));
            chip_row(layout, state, 2, PLAYER_UI_AV1_SOFTWARE, off_on,
                     state->av1_software ? 1 : 0);
            note(layout, 3,
                 tr("Iris has no AV1; dav1d plays 8 bit up to 4096×2160. "
                    "Applies from the next title."));
        }
        COLUMN {
            hint(layout, tr("Passthrough: the room instead of black"));
            chip_row(layout, state, 2, PLAYER_UI_PASSTHROUGH, off_on,
                     state->passthrough ? 1 : 0);
            hint(layout, tr("Dim the room"));
            stepper(layout, state, PLAYER_ADJUST_ENVIRONMENT_DIM,
                    N_("Strength"));
            if (!state->passthrough_supported)
                note(layout, 2, tr("SteamVR offers no passthrough here."));
        }
    }
}

/* Autofocus settings: timing (first column), when it acts (second). */
static void layout_autofocus(const struct player_ui_state *state,
                             struct ui_layout *layout)
{
    static const char *const thresholds[2] = {N_("Always"),
                                              N_("Near only")};
    COLUMNS {
        COLUMN {
            hint(layout, tr("Duration of a focus change"));
            stepper(layout, state, PLAYER_ADJUST_FOCUS_FADE, NULL);
            hint(layout, tr("Refocus from a distance change of"));
            stepper(layout, state, PLAYER_ADJUST_FOCUS_HYSTERESIS, NULL);
        }
        COLUMN {
            hint(layout, tr("Activation"));
            chip_row(layout, state, 2, PLAYER_UI_FOCUS_THRESHOLD, thresholds,
                     state->focus_threshold ? 1 : 0);
            hint(layout, state->focus_threshold
                             ? tr("Farther away: video unchanged")
                             : tr("Limit (with “Near only”)"));
            stepper(layout, state, PLAYER_ADJUST_FOCUS_DISTANCE, NULL);
        }
        COLUMN {
            paragraph(layout, 3,
                      tr("The strength of the autofocus is in the player "
                         "under “Focus”."));
        }
    }
}

/* Controller tab: pairs of actions in two columns, help or the action
 * being assigned in the third. */
static void layout_buttons(const struct player_ui_state *state,
                           struct ui_layout *layout)
{
    static const struct {
        const char *label;
        int first, second; /* enum binding_action, -1 = none */
    } rows[6] = {
        {N_("Seek (−10 s | +10 s)"), BINDING_SEEK_BACK,
         BINDING_SEEK_FORWARD},
        {N_("Volume (− | +)"), BINDING_VOLUME_DOWN, BINDING_VOLUME_UP},
        {N_("Video (previous | next)"), BINDING_PREVIOUS, BINDING_NEXT},
        {N_("Tilt (down | up)"), BINDING_TILT_DOWN, BINDING_TILT_UP},
        {N_("Zoom (out | in)"), BINDING_ZOOM_OUT, BINDING_ZOOM_IN},
        {N_("Play / pause"), BINDING_PLAY_PAUSE, -1},
    };
    bool learning = state->learn_action >= 0 &&
                    state->learn_action < BINDING_ACTION_COUNT;
    COLUMNS {
        for (int column = 0; column < 2; ++column)
            COLUMN {
                for (int r = 3 * column; r < 3 * column + 3; ++r) {
                    hint(layout, tr(rows[r].label));
                    CHIP_ROW {
                        int count = rows[r].second >= 0 ? 2 : 1;
                        for (int i = 0; i < count; ++i) {
                            int action = i ? rows[r].second : rows[r].first;
                            char text[64];
                            if (state->learn_action == action)
                                snprintf(text, sizeof(text), "%s",
                                         tr("press …"));
                            else
                                binding_describe(
                                    state->bindings
                                        ? state->bindings[action]
                                        : binding_action_info[action]
                                              .standard,
                                    text, sizeof(text));
                            chip_sized(layout, state,
                                       PLAYER_UI_BINDING + action, text,
                                       state->learn_action == action,
                                       CLAY_SIZING_GROW(0), true);
                        }
                    }
                }
            }
        COLUMN {
            if (learning) {
                char label[96];
                snprintf(label, sizeof(label), tr("Assign: %s"),
                         tr(binding_action_info[state->learn_action].label));
                ui_layout_text(layout, label, UI_LAYOUT_REGULAR, 24,
                               color_text);
                note(layout, 3,
                     tr("Press a button, or for a combination hold the "
                        "first and press the second."));
                SPACER;
                CHIP_ROW {
                    chip(layout, state, PLAYER_UI_BINDING_CLEAR, tr("Remove"),
                         false);
                    chip(layout, state, PLAYER_UI_BINDING_CANCEL,
                         tr("Cancel"), false);
                }
            } else {
                paragraph(layout, 5,
                          tr("Choose a field, then press the button or "
                             "combination on the controller. The grip "
                             "works only in combinations. Applies to all "
                             "titles."));
                SPACER;
                chip(layout, state, PLAYER_UI_BINDING_DEFAULTS,
                     tr("Default buttons"), false);
            }
        }
    }
}

/* View tab: head motion and centring (first column), projection, stereo
 * layout and eye order (second), what applies where (third). */
static void layout_view(const struct player_ui_state *state,
                        struct ui_layout *layout)
{
    static const char *const projections[4] = {N_("Flat"), "180°", "360°",
                                               N_("Fisheye")};
    static const char *const layouts[3] = {"Mono", "SBS", "TB"};
    static const char *const projection_labels[] = {
        [NV12_FORMAT_SOURCE_DEFAULT] = N_("Projection (default)"),
        [NV12_FORMAT_SOURCE_NAME] = N_("Projection (from the file name)"),
        [NV12_FORMAT_SOURCE_IMAGE] = N_("Projection (found in the picture)"),
        [NV12_FORMAT_SOURCE_USER] = N_("Projection"),
    };
    char projection_label[96];
    snprintf(projection_label, sizeof(projection_label), "%s",
             tr(projection_labels[state->format_source <=
                                          NV12_FORMAT_SOURCE_USER
                                      ? state->format_source
                                      : NV12_FORMAT_SOURCE_USER]));
    if (state->projection_mode == NV12_PROJECTION_FISHEYE) {
        size_t used = strlen(projection_label);
        snprintf(projection_label + used, sizeof(projection_label) - used,
                 " · %u°", state->fisheye_fov);
    }
    COLUMNS {
        COLUMN {
            hint(layout, tr("Head motion (6DoF, all titles)"));
            stepper(layout, state, PLAYER_ADJUST_HEAD_MOTION, NULL);
            hint(layout, tr("Orientation"));
            chip(layout, state, PLAYER_UI_RECENTER, tr("Centre"), false);
        }
        COLUMN {
            hint(layout, projection_label);
            CHIP_ROW {
                /* "Fisheye" needs more room than the angles. */
                for (int i = 0; i < 4; ++i)
                    chip_sized(layout, state, PLAYER_UI_PROJECTION + i,
                               tr(projections[i]),
                               i == (int)state->projection_mode,
                               i == 3 ? CLAY_SIZING_FIXED(128)
                                      : CLAY_SIZING_GROW(0),
                               false);
            }
            hint(layout, "Stereo");
            chip_row(layout, state, 3, PLAYER_UI_STEREO, layouts,
                     (int)state->stereo_layout);
            SPACER;
            chip(layout, state, PLAYER_UI_SWAP_EYES,
                 state->swap_eyes ? tr("Eyes swapped") : tr("Swap eyes"),
                 state->swap_eyes);
        }
        COLUMN {
            BAND {
                paragraph(layout, 2,
                          tr("Projection, stereo and placement apply to "
                             "this title."));
            }
            note(layout, 3,
                 tr("Scrub preview, panel, buttons: Settings on the start "
                    "page."));
        }
    }
}

/* Position tab: steppers in three columns, a remark below them. */
static void layout_position(const struct player_ui_state *state,
                            struct ui_layout *layout)
{
    static const enum player_adjust grid[3][3] = {
        {PLAYER_ADJUST_ZOOM, PLAYER_ADJUST_HEIGHT, PLAYER_ADJUST_SLIDE},
        {PLAYER_ADJUST_TILT, PLAYER_ADJUST_YAW, PLAYER_ADJUST_ROLL},
        {PLAYER_ADJUST_STEREO_X, PLAYER_ADJUST_STEREO_Y,
         PLAYER_ADJUST_COUNT},
    };
    COLUMNS {
        for (int column = 0; column < 3; ++column)
            STEPPER_COLUMN {
                for (int row = 0; row < 3; ++row)
                    if (grid[column][row] != PLAYER_ADJUST_COUNT)
                        stepper(layout, state, grid[column][row], NULL);
                if (column == 2)
                    chip(layout, state, PLAYER_UI_RESET_POSITION,
                         tr("Reset"), false);
            }
    }
    hint(layout,
         tr("Tilt applies to all titles, everything else per title."));
}

/* Picture tab: the black level (first column), the picture's steppers
 * (second and third). */
static void layout_picture(const struct player_ui_state *state,
                           struct ui_layout *layout)
{
    static const char *const levels[3] = {N_("Normal"), N_("Deep"),
                                          N_("Full")};
    COLUMNS {
        COLUMN {
            hint(layout, tr("Black level"));
            chip_row(layout, state, 3, PLAYER_UI_LEVELS, levels,
                     (int)state->video_levels);
            /* What the levels mean, one line each. */
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0),
                                        CLAY_SIZING_FIT(0)},
                             .padding = {.top = STACK_GAP},
                             .childGap = 30 - HINT_H,
                             .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
                hint(layout, tr("Normal: black 16, white 235"));
                hint(layout, tr("Deep: detail below black"));
                hint(layout, tr("Full: full range 0–255"));
            }
        }
        STEPPER_COLUMN {
            stepper(layout, state, PLAYER_ADJUST_BRIGHTNESS, NULL);
            stepper(layout, state, PLAYER_ADJUST_CONTRAST, NULL);
            stepper(layout, state, PLAYER_ADJUST_SATURATION, NULL);
        }
        STEPPER_COLUMN {
            stepper(layout, state, PLAYER_ADJUST_GAMMA, NULL);
            EMPTY_ROW;
            chip(layout, state, PLAYER_UI_RESET_PICTURE, tr("Reset"), false);
        }
    }
}

/* Updates tab: versions and the check (first column), the status and the
 * update (second), the check at start (third). */
static void layout_updates(const struct player_ui_state *state,
                           struct ui_layout *layout)
{
    COLUMNS {
        COLUMN {
            hint(layout, tr("Installed version"));
            text_row(layout,
                     state->update_installed && state->update_installed[0]
                         ? state->update_installed
                         : tr("built from the sources"));
            hint(layout, tr("Newest release"));
            text_row(layout, state->update_latest && state->update_latest[0]
                                 ? state->update_latest
                                 : tr("not known yet"));
            SPACER;
            if (state->update_can_check)
                chip(layout, state, PLAYER_UI_UPDATE_CHECK, tr("Check now"),
                     false);
        }
        COLUMN {
            hint(layout, tr("Status"));
            if (state->update_status && state->update_status[0])
                paragraph(layout, 4, state->update_status);
            SPACER;
            if (state->update_can_install)
                chip(layout, state, PLAYER_UI_UPDATE_INSTALL,
                     tr("Update now"), true);
        }
        COLUMN {
            static const char *const off_on[2] = {N_("Off"), N_("On")};
            hint(layout, tr("Check at start"));
            chip_row(layout, state, 2, PLAYER_UI_UPDATE_AUTO, off_on,
                     state->update_auto ? 1 : 0);
            note(layout, 4,
                 tr("Updates come from the release page of Stereonaut and "
                    "must carry its signature. Stereonaut restarts "
                    "afterwards."));
        }
    }
}

/* Language tab: "automatic" (with the language it finds) and the
 * languages, two per row: below "automatic" in the first column, then in
 * the second. */
static void language_pair(struct ui_layout *layout,
                          const struct player_ui_state *state, int first,
                          int count)
{
    EMPTY_HINT;
    if (first >= count)
        return;
    CHIP_ROW {
        for (int i = first; i < first + 2; ++i) {
            if (i < count)
                chip(layout, state, PLAYER_UI_LANGUAGE + 1 + i,
                     i18n_languages[i]->name, state->language == i);
            else /* keeps the half width */
                CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0)}}}) {}
        }
    }
}

static void layout_language(const struct player_ui_state *state,
                            struct ui_layout *layout)
{
    char automatic[96];
    int system = state->system_language >= 0 &&
                         state->system_language < i18n_language_count
                     ? state->system_language
                     : 0;
    snprintf(automatic, sizeof(automatic), tr("Automatic (%s)"),
             i18n_languages[system]->name);
    /* Two rows of pairs in the first column, three in the second. */
    int count = i18n_language_count;
    if (count > 10)
        count = 10;
    if (count > PLAYER_UI_LANGUAGE_END - PLAYER_UI_LANGUAGE - 1)
        count = PLAYER_UI_LANGUAGE_END - PLAYER_UI_LANGUAGE - 1;
    COLUMNS {
        COLUMN {
            hint(layout, tr("Language"));
            chip(layout, state, PLAYER_UI_LANGUAGE, automatic,
                 state->language == I18N_AUTO);
            for (int pair = 0; pair < 2; ++pair)
                language_pair(layout, state, 2 * pair, count);
        }
        COLUMN {
            for (int pair = 2; pair < 5; ++pair)
                language_pair(layout, state, 2 * pair, count);
        }
        COLUMN {
            paragraph(layout, 5,
                      tr("Automatic follows the language of Steam or of the "
                         "system and shows English where no translation "
                         "exists."));
        }
    }
}

/* Lays out the content of the tabs that use Clay; false for the others
 * (still in struct popup_layout). */
static bool layout_tab(const struct player_ui_state *state,
                       struct ui_layout *layout)
{
    void (*describe)(const struct player_ui_state *, struct ui_layout *) =
        NULL;
    switch (state->popup_tab) {
    case PLAYER_UI_TAB_FOCUS:
        describe = layout_focus;
        break;
    case PLAYER_UI_TAB_PLAYBACK:
        describe = layout_playback;
        break;
    case PLAYER_UI_TAB_AUTOFOCUS:
        describe = layout_autofocus;
        break;
    case PLAYER_UI_TAB_BUTTONS:
        describe = layout_buttons;
        break;
    case PLAYER_UI_TAB_VIEW:
        describe = layout_view;
        break;
    case PLAYER_UI_TAB_POSITION:
        describe = layout_position;
        break;
    case PLAYER_UI_TAB_PICTURE:
        describe = layout_picture;
        break;
    case PLAYER_UI_TAB_LANGUAGE:
        describe = layout_language;
        break;
    case PLAYER_UI_TAB_UPDATES:
        describe = layout_updates;
        break;
    }
    if (!describe)
        return false;
    ui_layout_begin(layout, PLAYER_UI_WIDTH, PLAYER_UI_HEIGHT);
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                     .padding = {.left = CONTENT_X, .top = CONTENT_Y}}}) {
        CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(CONTENT_W),
                                    CLAY_SIZING_FIXED(CONTENT_H)},
                         .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
            describe(state, layout);
        }
    }
    ui_layout_end(layout);
    return true;
}

static void layout_popup(const struct player_ui_state *state,
                         struct popup_layout *layout)
{
    layout->count = 0;
    static const char *const tabs[PLAYER_UI_TAB_COUNT] = {
        N_("View"),     N_("Position"),  N_("Picture"),
        N_("Focus"),    N_("Passthrough"), N_("Info"),
        N_("Playback"), N_("Autofocus"), N_("Buttons"),
        N_("Language"), N_("Updates"),
    };
    int first = state->settings_page ? PLAYER_UI_FIRST_SETTINGS_TAB : 0;
    int end = state->settings_page ? PLAYER_UI_TAB_COUNT
                                   : PLAYER_UI_FIRST_SETTINGS_TAB;
    for (int i = first; i < end; ++i)
        add_chip(layout, PLAYER_UI_TAB + i,
                 COLUMN_X(0) + (i - first) * (TAB_W + CHIP_GAP), TAB_Y,
                 TAB_W, tr(tabs[i]), state->popup_tab == i);
    if (state->settings_page)
        add_chip(layout, PLAYER_UI_SETTINGS_CLOSE,
                 POPUP_X + POPUP_W - 22 - TAB_W, TAB_Y, TAB_W, tr("Close"),
                 false);

    if (state->popup_tab == PLAYER_UI_TAB_PASSTHROUGH) {
        static const char *const off_on[2] = {N_("Off"), N_("On")};
        add_widget(layout, WIDGET_HINT, COLUMN_X(0), ROW_LABEL_Y(0), 0,
                   tr("The room instead of black (all titles)"));
        add_chip_row(layout, 0, 0, 2, PLAYER_UI_PASSTHROUGH, off_on,
                     state->passthrough ? 1 : 0);
        add_widget(layout, WIDGET_HINT, COLUMN_X(0), ROW_LABEL_Y(1), 0,
                   tr("Dim the room"));
        add_stepper(layout, state, 0, ROW_CHIP_Y(1),
                    PLAYER_ADJUST_ENVIRONMENT_DIM, N_("Strength"));
        if (!state->passthrough_supported)
            add_paragraph(layout, COLUMN_X(0), ROW_LABEL_Y(2) + 6, 2,
                          tr("SteamVR offers no passthrough here."));

        static const char *const keys[4] = {N_("Off"), N_("Green"),
                                            N_("Blue"), N_("Picked")};
        add_widget(layout, WIDGET_HINT, COLUMN_X(1), ROW_LABEL_Y(0), 0,
                   tr("Chroma key (per title)"));
        int key = state->adjust
                      ? (int)(state->adjust[PLAYER_ADJUST_KEY_COLOR] + 0.5f)
                      : 0;
        add_chip_row(layout, 1, 0, 4, PLAYER_UI_KEY_COLOR, keys, key);
        add_stepper(layout, state, 1, STEPPER_Y(1) + 12,
                    PLAYER_ADJUST_KEY_THRESHOLD, NULL);
        add_stepper(layout, state, 1, STEPPER_Y(2) + 12,
                    PLAYER_ADJUST_KEY_SOFTNESS, NULL);

        add_paragraph(layout, COLUMN_X(2), ROW_LABEL_Y(0), 3,
                      state->key_picking
                          ? tr("Aim the controller at the background in "
                               "the video and pull the trigger.")
                          : tr("The key colour turns transparent; with "
                               "passthrough the room shows there."));
        add_chip(layout, PLAYER_UI_KEY_PICK, COLUMN_X(2),
                 STEPPER_Y(1) + 12, COLUMN_W,
                 state->key_picking ? tr("Cancel") : tr("Pick the colour"),
                 state->key_picking);
        add_stepper(layout, state, 2, STEPPER_Y(2) + 12,
                    PLAYER_ADJUST_KEY_SPILL, NULL);
    }
}

float player_adjust_step(enum player_adjust adjust, float value,
                         int direction)
{
    const struct player_adjust_info *info = &player_adjust_info[adjust];
    /* Stay on the step grid so repeated steps do not drift. */
    float steps = (value - info->standard) / info->step;
    long index = (long)(steps + (steps < 0.0f ? -0.5f : 0.5f)) + direction;
    float result = info->standard + (float)index * info->step;
    if (result < info->minimum)
        result = info->minimum;
    if (result > info->maximum)
        result = info->maximum;
    return result;
}

/* ---- drawing ----------------------------------------------------------- */

static void round_button(struct ui_canvas *canvas, float cx, float r,
                         bool hover, bool active)
{
    ui_fill_circle(canvas, cx, ROW_Y, r,
                   active ? color_accent
                   : hover ? color_button_hover
                           : color_button);
}

static void draw_chip(struct player_ui *ui, struct ui_canvas *canvas, int x,
                      int y, int width, const char *label, bool selected,
                      bool hover, bool small)
{
    ui_fill_round_rect(canvas, x, y, width, CHIP_H, CHIP_H / 2.0f,
                       selected ? color_accent
                       : hover ? color_button_hover
                               : color_button);
    ui_draw_text(canvas, selected ? ui->bold : ui->font, small ? 20 : 24,
                 x + width / 2, y + (small ? 10 : 8), width - 16,
                 UI_ALIGN_CENTER, color_text, label);
}

/* Information tab: the description across two columns, facts in the
 * third. */
static void draw_info(struct player_ui *ui, struct ui_canvas *canvas,
                      const struct jf_details *details)
{
    const int left = COLUMN_X(0), top = ROW_LABEL_Y(0);
    const int text_w = COLUMN_X(2) - 44 - left;
    if (!details) {
        ui_draw_text_wrapped(canvas, ui->font, 24, left, top, text_w, 32, 3,
                             UI_ALIGN_LEFT, color_dim,
                             tr("No information – only Jellyfin titles have "
                                "a description."));
        return;
    }
    ui_draw_text(canvas, ui->font, 22, left, top, 0, UI_ALIGN_LEFT,
                 color_dim, tr("Description"));
    ui_draw_text_wrapped(canvas, ui->font, 23, left, top + 34, text_w, 31, 6,
                         UI_ALIGN_LEFT,
                         details->overview[0] ? color_text : color_dim,
                         details->overview[0] ? details->overview
                                              : tr("No description."));
    char year[16] = "", rating[16] = "";
    if (details->year > 0)
        snprintf(year, sizeof(year), "%d", details->year);
    if (details->community_rating > 0.0f)
        snprintf(rating, sizeof(rating), "%.1f", details->community_rating);
    const struct {
        const char *label, *value;
    } rows[] = {
        {tr("Year"), year},
        {tr("Rating"), rating},
        {tr("Studio"), details->studios},
        {tr("People"), details->people},
        {tr("Genres"), details->genres},
        {tr("Tags"), details->tags},
    };
    const int x = COLUMN_X(2), label_w = 150, bottom = POPUP_Y + POPUP_H - 20;
    int y = top;
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        if (!rows[i].value[0])
            continue;
        if (y + 30 > bottom)
            break;
        ui_draw_text(canvas, ui->font, 22, x, y + 2, label_w, UI_ALIGN_LEFT,
                     color_dim, rows[i].label);
        int lines = (bottom - y) / 30 >= 2 ? 2 : 1;
        y += 30 * ui_draw_text_wrapped(canvas, ui->font, 23, x + label_w, y,
                                       COLUMN_W - label_w, 30, lines,
                                       UI_ALIGN_LEFT, color_text,
                                       rows[i].value) +
             8;
    }
}

static void draw_popup(struct player_ui *ui, struct ui_canvas *canvas,
                       const struct player_ui_state *state)
{
    ui_shadow_round_rect(canvas, POPUP_X, POPUP_Y + 6, POPUP_W, POPUP_H, 26,
                         22, color_shadow);
    ui_fill_round_rect(canvas, POPUP_X, POPUP_Y, POPUP_W, POPUP_H, 26,
                       UI_RGBA(17, 18, 22, 240));
    ui_stroke_round_rect(canvas, POPUP_X, POPUP_Y, POPUP_W, POPUP_H, 26, 2,
                         color_border);
    ui_fill_rect(canvas, POPUP_X + 20, TAB_Y + CHIP_H + 10, POPUP_W - 40, 2,
                 color_border);
    for (int column = state->popup_tab == PLAYER_UI_TAB_INFO ? 2 : 1;
         column < 3; ++column)
        ui_fill_rect(canvas, COLUMN_X(column) - 22, TAB_Y + CHIP_H + 26, 2,
                     POPUP_H - CHIP_H - 64, color_border);
    if (state->popup_tab == PLAYER_UI_TAB_INFO)
        draw_info(ui, canvas, state->details);
    struct popup_layout layout;
    layout_popup(state, &layout);
    for (int i = 0; i < layout.count; ++i) {
        const struct widget *widget = &layout.items[i];
        switch (widget->kind) {
        case WIDGET_CHIP:
            draw_chip(ui, canvas, widget->x, widget->y, widget->width,
                      widget->text, widget->selected,
                      state->hover == widget->control, widget->small);
            break;
        case WIDGET_TEXT:
            ui_draw_text(canvas, ui->font, 24, widget->x, widget->y,
                         widget->width, UI_ALIGN_LEFT, color_text,
                         widget->text);
            break;
        case WIDGET_HINT:
            ui_draw_text(canvas, ui->font, 22, widget->x, widget->y, 0,
                         UI_ALIGN_LEFT, color_dim, widget->text);
            break;
        case WIDGET_PARAGRAPH:
            ui_draw_text_wrapped(canvas, ui->font, 22, widget->x, widget->y,
                                 widget->width, 30, widget->lines,
                                 UI_ALIGN_LEFT, color_dim, widget->text);
            break;
        case WIDGET_VALUE:
            ui_draw_text(canvas, ui->bold, 24, widget->x + widget->width / 2,
                         widget->y, widget->width - 8, UI_ALIGN_CENTER,
                         color_text, widget->text);
            break;
        }
    }
    struct ui_layout tab;
    if (layout_tab(state, &tab)) {
        struct ui_font *const fonts[UI_LAYOUT_FONT_COUNT] = {ui->font,
                                                             ui->bold};
        ui_layout_draw(&tab, canvas, fonts);
    }
}

/* Name of the chapter `position_us` lies in, or NULL. */
static const char *chapter_at(const struct player_ui_state *state,
                              int64_t position_us)
{
    const struct jf_details *details = state->details;
    const char *name = NULL;
    for (int i = 0; details && i < details->chapter_count; ++i)
        if (details->chapter_us[i] <= position_us &&
            details->chapter_names[i][0])
            name = details->chapter_names[i];
    return name;
}

/* Picture (if any) with the time (and chapter) below it; without a
 * picture just the label. */
static int preview_width(const struct ui_image *image, int height)
{
    if (!image || image->width <= 0 || image->height <= 0)
        return 0;
    int width = image->width * height / image->height;
    return width > 2 * height ? 2 * height : width;
}

/* Width of the strip's slots first..last that have a picture. */
static int strip_width(const int widths[PLAYER_UI_PREVIEW_STRIP], int first,
                       int last)
{
    int total = 0;
    for (int slot = first; slot <= last; ++slot)
        if (widths[slot])
            total += (total ? PREVIEW_GAP : 0) + widths[slot];
    return total;
}

static void draw_preview(struct player_ui *ui, struct ui_canvas *canvas,
                         const struct player_ui_state *state)
{
    const int middle = PLAYER_UI_PREVIEW_STRIP / 2;
    int widths[PLAYER_UI_PREVIEW_STRIP] = {0};
    int first = middle, last = middle;
    widths[middle] = preview_width(state->preview[middle], PREVIEW_IMAGE_H);
    if (widths[middle]) {
        for (int slot = 0; slot < PLAYER_UI_PREVIEW_STRIP; ++slot)
            if (slot != middle)
                widths[slot] =
                    preview_width(state->preview[slot], PREVIEW_SIDE_H);
        first = 0;
        last = PLAYER_UI_PREVIEW_STRIP - 1;
        /* Outer pictures go pairwise until the strip fits the card. */
        while (first < middle &&
               strip_width(widths, first, last) + 2 * PREVIEW_PAD > CARD_W) {
            ++first;
            --last;
        }
    }
    int images_w = strip_width(widths, first, last);
    char text[96];
    format_time(text, state->preview_us);
    const char *chapter = chapter_at(state, state->preview_us);
    if (chapter) {
        size_t used = strlen(text);
        snprintf(text + used, sizeof(text) - used, " · %s", chapter);
    }
    int width = images_w ? images_w + 2 * PREVIEW_PAD : 140;
    if (chapter && width < 360)
        width = 360;
    int height = (images_w ? PREVIEW_IMAGE_H + PREVIEW_PAD : 0) +
                 PREVIEW_LABEL_H;
    /* Keep the middle picture over the previewed position. */
    float strip_x = (width - images_w) / 2.0f;
    int before = strip_width(widths, first, middle - 1);
    float middle_center = strip_x + before + (before ? PREVIEW_GAP : 0) +
                          widths[middle] / 2.0f;
    if (!images_w)
        middle_center = width / 2.0f;
    float center = BAR_X + BAR_W * time_ratio(state, state->preview_us);
    float x = center - middle_center;
    if (x < CARD_X)
        x = CARD_X;
    if (x > CARD_X + CARD_W - width)
        x = CARD_X + CARD_W - width;
    float y = PREVIEW_BOTTOM - height;
    /* No shadow: redrawn on every hovered position, it cost 2 ms. */
    ui_fill_round_rect(canvas, x, y, width, height, 18,
                       UI_RGBA(17, 18, 22, 240));
    ui_stroke_round_rect(canvas, x, y, width, height, 18, 2, color_border);
    float cursor = x + strip_x;
    for (int slot = first; slot <= last; ++slot) {
        if (!widths[slot])
            continue;
        bool current = slot == middle;
        int image_h = current ? PREVIEW_IMAGE_H : PREVIEW_SIDE_H;
        float image_y = y + PREVIEW_PAD + PREVIEW_IMAGE_H - image_h;
        ui_draw_image(canvas, state->preview[slot], cursor, image_y,
                      widths[slot], image_h, 12, current ? 0.0f : 0.35f);
        if (current)
            ui_stroke_round_rect(canvas, cursor - 2, image_y - 2,
                                 widths[slot] + 4, image_h + 4, 14, 3,
                                 color_accent);
        cursor += widths[slot] + PREVIEW_GAP;
    }
    float label_center = x + middle_center;
    float half = fminf(label_center - x, x + width - label_center) - 6.0f;
    if (half < 70.0f)
        half = 70.0f;
    ui_draw_text(canvas, ui->bold, 26, (int)label_center,
                 (int)(y + height - PREVIEW_LABEL_H + 5), (int)(2.0f * half),
                 UI_ALIGN_CENTER, color_text, text);
}

void player_ui_render(struct player_ui *ui, const struct player_ui_state *state,
                      uint32_t *pixels, bool bgra)
{
    if (!ui->font)
        ui->font = ui_font_load(false);
    if (!ui->bold)
        ui->bold = ui_font_load(true);
    struct ui_canvas canvas = {(uint8_t *)pixels, PLAYER_UI_WIDTH,
                               PLAYER_UI_HEIGHT, bgra};
    ui_clear(&canvas, UI_RGBA(0, 0, 0, 0));
    if (state->popup_open)
        draw_popup(ui, &canvas, state);

    ui_shadow_round_rect(&canvas, CARD_X, CARD_Y + 8, CARD_W, CARD_H, 36, 26,
                         color_shadow);
    ui_fill_round_rect(&canvas, CARD_X, CARD_Y, CARD_W, CARD_H, 36,
                       color_card);
    ui_stroke_round_rect(&canvas, CARD_X, CARD_Y, CARD_W, CARD_H, 36,
                         state->grabbed ? 4 : 2,
                         state->grabbed ? color_accent : color_border);

    ui_draw_text(&canvas, ui->bold, 32, PLAYER_UI_WIDTH / 2, CARD_Y + 20,
                 1100, UI_ALIGN_CENTER, color_text,
                 state->title && state->title[0] ? state->title : "Video");

    char position[32], duration[32];
    format_time(position, state->scrubbing ? state->scrub_us
                                           : state->position_us);
    format_time(duration, state->duration_us);
    if (state->loading && !state->scrubbing)
        ui_draw_text(&canvas, ui->bold, 26, CARD_X + 40, BAR_Y - 16, 0,
                     UI_ALIGN_LEFT, color_accent, tr("Loading …"));
    else
        ui_draw_text(&canvas, ui->font, 26, CARD_X + 40, BAR_Y - 16, 0,
                     UI_ALIGN_LEFT, color_text, position);
    ui_draw_text(&canvas, ui->font, 26, CARD_X + CARD_W - 40, BAR_Y - 16, 0,
                 UI_ALIGN_RIGHT, color_dim, duration);
    float ratio = shown_ratio(state);
    bool bar_hot = state->scrubbing || state->hover == PLAYER_UI_SEEK_BAR;
    ui_fill_round_rect(&canvas, BAR_X, BAR_Y, BAR_W, BAR_H, BAR_H / 2.0f,
                       color_track);
    if (ratio > 0.0f)
        ui_fill_round_rect(&canvas, BAR_X, BAR_Y, BAR_W * ratio, BAR_H,
                           BAR_H / 2.0f, color_accent);
    for (int i = 0; state->details && i < state->details->chapter_count;
         ++i) {
        float mark = time_ratio(state, state->details->chapter_us[i]);
        if (mark > 0.0f && mark < 1.0f)
            ui_fill_rect(&canvas, BAR_X + BAR_W * mark - 2, BAR_Y - 6, 4,
                         BAR_H + 12, UI_RGBA(255, 255, 255, 170));
    }
    float knob_x = BAR_X + BAR_W * ratio;
    ui_fill_circle(&canvas, knob_x, BAR_Y + BAR_H / 2.0f, bar_hot ? 20 : 15,
                   UI_RGBA(0, 0, 0, 80));
    ui_fill_circle(&canvas, knob_x, BAR_Y + BAR_H / 2.0f, bar_hot ? 17 : 12,
                   UI_RGBA(255, 255, 255, 255));

    int hover = state->hover;
    round_button(&canvas, BACK_X, SMALL_R, hover == PLAYER_UI_BACK, false);
    ui_draw_icon(&canvas, UI_ICON_EXIT, BACK_X, ROW_Y, 38, color_text);

    round_button(&canvas, SKIP_BACK_X, SKIP_R, hover == PLAYER_UI_SEEK_BACK,
                 false);
    ui_draw_text(&canvas, ui->bold, 26, SKIP_BACK_X, ROW_Y - 18, 0,
                 UI_ALIGN_CENTER, color_text, "−10");
    round_button(&canvas, SKIP_FORWARD_X, SKIP_R,
                 hover == PLAYER_UI_SEEK_FORWARD, false);
    ui_draw_text(&canvas, ui->bold, 26, SKIP_FORWARD_X, ROW_Y - 18, 0,
                 UI_ALIGN_CENTER, color_text, "+10");

    bool play_hover = hover == PLAYER_UI_TOGGLE_PAUSE;
    ui_fill_circle(&canvas, PLAY_X, ROW_Y, PLAY_R,
                   play_hover ? color_accent : UI_RGBA(240, 243, 248, 255));
    ui_draw_icon(&canvas, state->paused ? UI_ICON_PLAY : UI_ICON_PAUSE,
                 PLAY_X + (state->paused ? 4 : 0), ROW_Y, 46,
                 play_hover ? UI_RGBA(255, 255, 255, 255)
                            : UI_RGBA(20, 22, 28, 255));

    static const char *const end_labels[PLAYER_END_COUNT] = {
        N_("At end: stop"), N_("At end: repeat"), N_("At end: next"),
    };
    int end_action = state->end_action >= 0 &&
                             state->end_action < PLAYER_END_COUNT
                         ? state->end_action
                         : PLAYER_END_STOP;
    ui_fill_round_rect(&canvas, END_X, ROW_Y - END_H / 2.0f, END_W, END_H,
                       END_H / 2.0f,
                       hover == PLAYER_UI_END_ACTION ? color_button_hover
                                                     : color_button);
    ui_draw_text(&canvas, end_action == PLAYER_END_STOP ? ui->font : ui->bold,
                 22, END_X + END_W / 2, ROW_Y - 15, END_W - 24,
                 UI_ALIGN_CENTER,
                 end_action == PLAYER_END_STOP ? color_dim : color_accent,
                 tr(end_labels[end_action]));

    round_button(&canvas, VOLUME_DOWN_X, VOLUME_R,
                 hover == PLAYER_UI_VOLUME_DOWN, false);
    ui_draw_text(&canvas, ui->bold, 28, VOLUME_DOWN_X, ROW_Y - 20, 0,
                 UI_ALIGN_CENTER, color_text, "−");
    round_button(&canvas, VOLUME_UP_X, VOLUME_R, hover == PLAYER_UI_VOLUME_UP,
                 false);
    ui_draw_text(&canvas, ui->bold, 28, VOLUME_UP_X, ROW_Y - 20, 0,
                 UI_ALIGN_CENTER, color_text, "+");
    char volume[24];
    snprintf(volume, sizeof(volume), "%d %%",
             (int)(state->volume * 100.0f + 0.5f));
    ui_draw_text(&canvas, ui->font, 20, (VOLUME_DOWN_X + VOLUME_UP_X) / 2,
                 ROW_Y - 34, 0, UI_ALIGN_CENTER, color_dim, tr("Volume"));
    ui_draw_text(&canvas, ui->bold, 26, (VOLUME_DOWN_X + VOLUME_UP_X) / 2,
                 ROW_Y - 8, 0, UI_ALIGN_CENTER,
                 state->volume <= 0.0f ? color_dim : color_text,
                 state->volume <= 0.0f ? tr("Muted") : volume);

    round_button(&canvas, VIEW_X, SMALL_R, hover == PLAYER_UI_VIEW_POPUP,
                 state->popup_open);
    ui_draw_icon(&canvas, UI_ICON_CUBE, VIEW_X, ROW_Y, 38, color_text);
    if (state->passthrough_supported) {
        round_button(&canvas, PASSTHROUGH_X, SMALL_R,
                     hover == PLAYER_UI_PASSTHROUGH_TOGGLE,
                     state->passthrough);
        ui_draw_icon(&canvas, UI_ICON_EYE, PASSTHROUGH_X, ROW_Y, 54,
                     color_text);
    }

    if (state->debug_text && state->debug_text[0])
        ui_draw_text(&canvas, ui->font, 20, CARD_X + 40, CARD_Y + CARD_H - 34,
                     CARD_W - 80, UI_ALIGN_LEFT, color_dim, state->debug_text);

    if (state->preview_visible && state->duration_us > 0)
        draw_preview(ui, &canvas, state);
}

void player_ui_render_settings(struct player_ui *ui,
                               const struct player_ui_state *state,
                               uint32_t *pixels, bool bgra)
{
    if (!ui->font)
        ui->font = ui_font_load(false);
    if (!ui->bold)
        ui->bold = ui_font_load(true);
    struct ui_canvas canvas = {(uint8_t *)pixels, PLAYER_UI_WIDTH,
                               PLAYER_UI_HEIGHT, bgra};
    ui_clear(&canvas, UI_RGBA(0, 0, 0, 0));
    draw_popup(ui, &canvas, state);
}

void player_ui_close(struct player_ui *ui)
{
    ui_font_free(ui->font);
    ui_font_free(ui->bold);
    ui->font = ui->bold = NULL;
}

/* ---- input ------------------------------------------------------------- */

int player_ui_hit_test(const struct player_ui_state *state, float u, float v)
{
    float x = u * PLAYER_UI_WIDTH, y = v * PLAYER_UI_HEIGHT;
    if (state->popup_open && in_rect(x, y, POPUP_X, POPUP_Y, POPUP_W,
                                     POPUP_H)) {
        struct popup_layout layout;
        layout_popup(state, &layout);
        for (int i = 0; i < layout.count; ++i) {
            const struct widget *widget = &layout.items[i];
            if (widget->kind == WIDGET_CHIP &&
                in_rect(x, y, widget->x, widget->y, widget->width, CHIP_H))
                return widget->control;
        }
        struct ui_layout tab;
        if (layout_tab(state, &tab))
            return ui_layout_hit(&tab, x, y, PLAYER_UI_NONE);
        return PLAYER_UI_NONE;
    }
    if (state->duration_us > 0 && y >= BAR_HIT_TOP && y < BAR_HIT_BOTTOM &&
        x >= BAR_X - 16 && x < BAR_X + BAR_W + 16)
        return PLAYER_UI_SEEK_BAR;
    if (in_circle(x, y, BACK_X, ROW_Y, SMALL_R + 6))
        return PLAYER_UI_BACK;
    if (in_circle(x, y, SKIP_BACK_X, ROW_Y, SKIP_R + 6))
        return PLAYER_UI_SEEK_BACK;
    if (in_circle(x, y, PLAY_X, ROW_Y, PLAY_R + 6))
        return PLAYER_UI_TOGGLE_PAUSE;
    if (in_circle(x, y, SKIP_FORWARD_X, ROW_Y, SKIP_R + 6))
        return PLAYER_UI_SEEK_FORWARD;
    if (in_circle(x, y, VIEW_X, ROW_Y, SMALL_R + 6))
        return PLAYER_UI_VIEW_POPUP;
    if (state->passthrough_supported &&
        in_circle(x, y, PASSTHROUGH_X, ROW_Y, SMALL_R + 6))
        return PLAYER_UI_PASSTHROUGH_TOGGLE;
    if (in_rect(x, y, END_X, ROW_Y - END_H / 2.0f, END_W, END_H))
        return PLAYER_UI_END_ACTION;
    if (in_circle(x, y, VOLUME_DOWN_X, ROW_Y, VOLUME_R + 6))
        return PLAYER_UI_VOLUME_DOWN;
    if (in_circle(x, y, VOLUME_UP_X, ROW_Y, VOLUME_R + 6))
        return PLAYER_UI_VOLUME_UP;
    return PLAYER_UI_NONE;
}

bool player_ui_on_panel(const struct player_ui_state *state, float u, float v)
{
    float x = u * PLAYER_UI_WIDTH, y = v * PLAYER_UI_HEIGHT;
    return in_rect(x, y, CARD_X, CARD_Y, CARD_W, CARD_H) ||
           (state->popup_open &&
            in_rect(x, y, POPUP_X, POPUP_Y, POPUP_W, POPUP_H));
}

float player_ui_seek_ratio(float u)
{
    float ratio = (u * PLAYER_UI_WIDTH - BAR_X) / BAR_W;
    return ratio < 0.0f ? 0.0f : ratio > 1.0f ? 1.0f : ratio;
}
