#define _POSIX_C_SOURCE 200809L
#include "player-ui.h"

#include "nv12-playback-ipc.h"
#include "ui-draw.h"

#include <stdio.h>
#include <stdlib.h>

/* Control card. */
#define CARD_X 40
#define CARD_Y 270
#define CARD_W 1520
#define CARD_H 310

/* Seek bar. */
#define BAR_X 250
#define BAR_W 1100
#define BAR_Y 366
#define BAR_H 12
#define BAR_HIT_TOP 338
#define BAR_HIT_BOTTOM 406

/* Transport row. */
#define ROW_Y 490
#define BACK_X 116
#define SKIP_BACK_X 650
#define PLAY_X 800
#define SKIP_FORWARD_X 950
#define VIEW_X 1484
#define SMALL_R 38
#define SKIP_R 42
#define PLAY_R 54

/* Scrub preview above the card, centred on the previewed position. */
#define PREVIEW_IMAGE_H 196
#define PREVIEW_PAD 6
#define PREVIEW_LABEL_H 40
#define PREVIEW_BOTTOM (CARD_Y - 12)

/* View popup above the card, right-aligned, in three columns: scrub
 * preview settings, picture (levels) and the view of the video. */
#define POPUP_X 180
#define POPUP_Y 10
#define POPUP_W 1380
#define POPUP_H 252
#define COLUMN_W (3 * CHIP_W + 2 * CHIP_GAP)
#define PREVIEW_CHIP_X (POPUP_X + 22)
#define LEVELS_CHIP_X (POPUP_X + 482)
#define CHIP_X (POPUP_X + 942)
#define CHIP_W 132
#define CHIP_GAP 10
#define CHIP_H 44
#define CHIPS1_Y (POPUP_Y + 50)
#define CHIPS2_Y (POPUP_Y + 132)
#define SWAP_Y (POPUP_Y + 194)

const int player_ui_preview_fovs[PLAYER_UI_PREVIEW_FOV_COUNT] = {
    60, 80, 100, 120,
};

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

static void chip_rect(int row_y, int index, int *x, int *width)
{
    *x = CHIP_X + index * (CHIP_W + CHIP_GAP);
    *width = CHIP_W;
    (void)row_y;
}

/* `count` equal chips across the column starting at `column_x`. */
static void column_chip_rect(int column_x, int count, int index, int *x,
                             int *width)
{
    *width = (COLUMN_W - (count - 1) * CHIP_GAP) / count;
    *x = column_x + index * (*width + CHIP_GAP);
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
                      bool hover)
{
    ui_fill_round_rect(canvas, x, y, width, CHIP_H, CHIP_H / 2.0f,
                       selected ? color_accent
                       : hover ? color_button_hover
                               : color_button);
    ui_draw_text(canvas, selected ? ui->bold : ui->font, 24, x + width / 2,
                 y + 8, width - 16, UI_ALIGN_CENTER, color_text, label);
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
    static const char *const projections[3] = {"Flach", "180°", "360°"};
    static const char *const layouts[3] = {"Mono", "SBS", "TB"};
    ui_draw_text(canvas, ui->font, 22, CHIP_X, POPUP_Y + 18, 0, UI_ALIGN_LEFT,
                 color_dim, "Projektion");
    ui_draw_text(canvas, ui->font, 22, CHIP_X, POPUP_Y + 100, 0,
                 UI_ALIGN_LEFT, color_dim, "Stereo");
    for (int i = 0; i < 3; ++i) {
        int x, width;
        chip_rect(CHIPS1_Y, i, &x, &width);
        draw_chip(ui, canvas, x, CHIPS1_Y, width, projections[i],
                  state->projection_mode == (uint32_t)i,
                  state->hover == PLAYER_UI_PROJECTION + i);
        draw_chip(ui, canvas, x, CHIPS2_Y, width, layouts[i],
                  state->stereo_layout == (uint32_t)i,
                  state->hover == PLAYER_UI_STEREO + i);
    }
    int swap_width = 3 * CHIP_W + 2 * CHIP_GAP;
    draw_chip(ui, canvas, CHIP_X, SWAP_Y, swap_width,
              state->swap_eyes ? "Augen getauscht" : "Augen tauschen",
              state->swap_eyes, state->hover == PLAYER_UI_SWAP_EYES);

    ui_fill_rect(canvas, LEVELS_CHIP_X - 22, POPUP_Y + 20, 2, POPUP_H - 40,
                 color_border);
    ui_fill_rect(canvas, CHIP_X - 22, POPUP_Y + 20, 2, POPUP_H - 40,
                 color_border);
    static const char *const levels[3] = {"Norm", "Tief", "Voll"};
    ui_draw_text(canvas, ui->font, 22, LEVELS_CHIP_X, POPUP_Y + 18, 0,
                 UI_ALIGN_LEFT, color_dim, "Schwarzwert");
    for (int i = 0; i < 3; ++i) {
        int x, width;
        column_chip_rect(LEVELS_CHIP_X, 3, i, &x, &width);
        draw_chip(ui, canvas, x, CHIPS1_Y, width, levels[i],
                  state->video_levels == (uint32_t)i,
                  state->hover == PLAYER_UI_LEVELS + i);
    }
    ui_draw_text(canvas, ui->font, 20, LEVELS_CHIP_X, POPUP_Y + 108, 0,
                 UI_ALIGN_LEFT, color_dim, "Norm: Schwarz 16, Weiß 235");
    ui_draw_text(canvas, ui->font, 20, LEVELS_CHIP_X, POPUP_Y + 138, 0,
                 UI_ALIGN_LEFT, color_dim, "Tief: Zeichnung unter Schwarz");
    ui_draw_text(canvas, ui->font, 20, LEVELS_CHIP_X, POPUP_Y + 168, 0,
                 UI_ALIGN_LEFT, color_dim, "Voll: Full Range 0–255");
    static const char *const modes[2] = {"Original", "Perspektive"};
    ui_draw_text(canvas, ui->font, 22, PREVIEW_CHIP_X, POPUP_Y + 18, 0,
                 UI_ALIGN_LEFT, color_dim, "Vorschau beim Spulen");
    ui_draw_text(canvas, ui->font, 22, PREVIEW_CHIP_X, POPUP_Y + 100, 0,
                 UI_ALIGN_LEFT, color_dim, "Sichtfeld (Perspektive)");
    for (int i = 0; i < 2; ++i) {
        int x, width;
        column_chip_rect(PREVIEW_CHIP_X, 2, i, &x, &width);
        draw_chip(ui, canvas, x, CHIPS1_Y, width, modes[i],
                  state->preview_perspective == (i == 1),
                  state->hover == PLAYER_UI_PREVIEW_MODE + i);
    }
    for (int i = 0; i < PLAYER_UI_PREVIEW_FOV_COUNT; ++i) {
        int x, width;
        column_chip_rect(PREVIEW_CHIP_X, PLAYER_UI_PREVIEW_FOV_COUNT, i, &x,
                         &width);
        char label[16];
        snprintf(label, sizeof(label), "%d°", player_ui_preview_fovs[i]);
        draw_chip(ui, canvas, x, CHIPS2_Y, width, label,
                  state->preview_fov == player_ui_preview_fovs[i],
                  state->hover == PLAYER_UI_PREVIEW_FOV + i);
    }
}

/* Picture (if any) with the time below it; without a picture just the
 * time. */
static void draw_preview(struct player_ui *ui, struct ui_canvas *canvas,
                         const struct player_ui_state *state)
{
    const struct ui_image *image = state->preview;
    int image_w = 0, image_h = 0;
    if (image && image->width > 0 && image->height > 0) {
        image_h = PREVIEW_IMAGE_H;
        image_w = image->width * PREVIEW_IMAGE_H / image->height;
        if (image_w > 2 * PREVIEW_IMAGE_H)
            image_w = 2 * PREVIEW_IMAGE_H;
    }
    int width = image_w ? image_w + 2 * PREVIEW_PAD : 140;
    int height = (image_h ? image_h + PREVIEW_PAD : 0) + PREVIEW_LABEL_H;
    float center = BAR_X + BAR_W * time_ratio(state, state->preview_us);
    float x = center - width / 2.0f;
    if (x < CARD_X)
        x = CARD_X;
    if (x > CARD_X + CARD_W - width)
        x = CARD_X + CARD_W - width;
    float y = PREVIEW_BOTTOM - height;
    ui_shadow_round_rect(canvas, x, y + 6, width, height, 18, 18,
                         color_shadow);
    ui_fill_round_rect(canvas, x, y, width, height, 18,
                       UI_RGBA(17, 18, 22, 240));
    ui_stroke_round_rect(canvas, x, y, width, height, 18, 2, color_border);
    if (image_w)
        ui_draw_image(canvas, image, x + PREVIEW_PAD, y + PREVIEW_PAD,
                      image_w, image_h, 12, 0.0f);
    char text[32];
    format_time(text, state->preview_us);
    ui_draw_text(canvas, ui->bold, 26, (int)(x + width / 2.0f),
                 (int)(y + height - PREVIEW_LABEL_H + 5), width - 12,
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
                     UI_ALIGN_LEFT, color_accent, "Lädt …");
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

    round_button(&canvas, VIEW_X, SMALL_R, hover == PLAYER_UI_VIEW_POPUP,
                 state->popup_open);
    ui_draw_icon(&canvas, UI_ICON_CUBE, VIEW_X, ROW_Y, 38, color_text);

    if (state->preview_visible && state->duration_us > 0)
        draw_preview(ui, &canvas, state);
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
        for (int i = 0; i < 3; ++i) {
            int cx, width;
            chip_rect(CHIPS1_Y, i, &cx, &width);
            if (in_rect(x, y, cx, CHIPS1_Y, width, CHIP_H))
                return PLAYER_UI_PROJECTION + i;
            if (in_rect(x, y, cx, CHIPS2_Y, width, CHIP_H))
                return PLAYER_UI_STEREO + i;
        }
        if (in_rect(x, y, CHIP_X, SWAP_Y, 3 * CHIP_W + 2 * CHIP_GAP, CHIP_H))
            return PLAYER_UI_SWAP_EYES;
        for (int i = 0; i < 2; ++i) {
            int cx, width;
            column_chip_rect(PREVIEW_CHIP_X, 2, i, &cx, &width);
            if (in_rect(x, y, cx, CHIPS1_Y, width, CHIP_H))
                return PLAYER_UI_PREVIEW_MODE + i;
        }
        for (int i = 0; i < PLAYER_UI_PREVIEW_FOV_COUNT; ++i) {
            int cx, width;
            column_chip_rect(PREVIEW_CHIP_X, PLAYER_UI_PREVIEW_FOV_COUNT, i,
                             &cx, &width);
            if (in_rect(x, y, cx, CHIPS2_Y, width, CHIP_H))
                return PLAYER_UI_PREVIEW_FOV + i;
        }
        for (int i = 0; i < 3; ++i) {
            int cx, width;
            column_chip_rect(LEVELS_CHIP_X, 3, i, &cx, &width);
            if (in_rect(x, y, cx, CHIPS1_Y, width, CHIP_H))
                return PLAYER_UI_LEVELS + i;
        }
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
