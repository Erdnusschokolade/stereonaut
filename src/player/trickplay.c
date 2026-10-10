#define _POSIX_C_SOURCE 200809L
#include "trickplay.h"

#include "jellyfin.h"
#include "nv12-playback-ipc.h"
#include "thumbnails.h"
#include "ui-draw.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Horizontal field of view of the straightened 180/360 degree preview. */
#define FORWARD_FOV_DEGREES 100.0
#define PI 3.14159265358979323846

struct trickplay {
    pthread_t thread;
    bool thread_started;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    bool stopping;
    /* Request from the host. */
    uint64_t generation;
    char item[JF_ID_SIZE];
    bool want_valid;
    int64_t want_us;
    uint32_t want_layout, want_projection;
    /* Newest finished picture, taken by trickplay_poll. */
    struct ui_image *result;
};

/* Worker-only state of the current item. */
struct item_state {
    uint64_t generation;
    char item[JF_ID_SIZE];
    int info_state; /* 0 not loaded, 1 loaded, -1 unavailable */
    struct jf_config config;
    struct jf_trickplay info;
    int tile_count;
    char **tiles; /* compressed tile images, NULL until fetched */
    size_t *tile_sizes;
    int decoded_index;
    struct ui_image *decoded;
    int done_index;
    uint32_t done_layout, done_projection;
};

static void reset_item(struct item_state *state)
{
    for (int i = 0; i < state->tile_count; ++i)
        free(state->tiles[i]);
    free(state->tiles);
    free(state->tile_sizes);
    ui_image_free(state->decoded);
    memset(state, 0, sizeof(*state));
    state->decoded_index = -1;
    state->done_index = -1;
}

static int thumbnail_index(const struct item_state *state, int64_t position_us)
{
    int64_t index = position_us / 1000 / state->info.interval_ms;
    if (index < 0)
        index = 0;
    if (index >= state->info.count)
        index = state->info.count - 1;
    return (int)index;
}

static void load_info(struct item_state *state)
{
    char error[256];
    state->info_state = -1;
    if (jf_config_load(&state->config, error, sizeof(error)) < 0 ||
        !jf_config_has_token(&state->config) ||
        jf_trickplay_info(&state->config, state->item, &state->info) < 0) {
        printf("trickplay item=%s available=0\n", state->item);
        return;
    }
    int per_tile = state->info.columns * state->info.rows;
    state->tile_count = (state->info.count + per_tile - 1) / per_tile;
    state->tiles = calloc((size_t)state->tile_count, sizeof(*state->tiles));
    state->tile_sizes = calloc((size_t)state->tile_count,
                               sizeof(*state->tile_sizes));
    if (!state->tiles || !state->tile_sizes)
        return;
    state->info_state = 1;
    printf("trickplay item=%s available=1 width=%d height=%d tiles=%d"
           " interval_ms=%d count=%d\n",
           state->item, state->info.width, state->info.height,
           state->tile_count, state->info.interval_ms, state->info.count);
}

/* Cuts thumbnail `index` out of the decoded tile, reduced to the left
 * (SBS) or upper (TB) eye. */
static struct ui_image *cut_thumbnail(const struct item_state *state,
                                      int index, uint32_t layout)
{
    const struct jf_trickplay *info = &state->info;
    int slot = index % (info->columns * info->rows);
    int x0 = slot % info->columns * info->width;
    int y0 = slot / info->columns * info->height;
    int width = info->width, height = info->height;
    if (x0 + width > state->decoded->width)
        width = state->decoded->width - x0;
    if (y0 + height > state->decoded->height)
        height = state->decoded->height - y0;
    if (layout == NV12_STEREO_SBS)
        width /= 2;
    else if (layout == NV12_STEREO_TB)
        height /= 2;
    if (width <= 0 || height <= 0)
        return NULL;
    struct ui_image *image = ui_image_create(width, height);
    if (!image)
        return NULL;
    for (int y = 0; y < height; ++y)
        memcpy(image->rgba + (size_t)y * (size_t)width * 4U,
               state->decoded->rgba +
                   ((size_t)(y0 + y) * (size_t)state->decoded->width +
                    (size_t)x0) * 4U,
               (size_t)width * 4U);
    return image;
}

/* Bilinear sample of `image` at pixel coordinates (x, y), clamped. */
static void sample(const struct ui_image *image, double x, double y,
                   uint8_t out[4])
{
    x = fmin(fmax(x - 0.5, 0.0), image->width - 1.0);
    y = fmin(fmax(y - 0.5, 0.0), image->height - 1.0);
    int x0 = (int)x, y0 = (int)y;
    int x1 = x0 + 1 < image->width ? x0 + 1 : x0;
    int y1 = y0 + 1 < image->height ? y0 + 1 : y0;
    double fx = x - x0, fy = y - y0;
    const uint8_t *p00 = image->rgba + ((size_t)y0 * image->width + x0) * 4U;
    const uint8_t *p01 = image->rgba + ((size_t)y0 * image->width + x1) * 4U;
    const uint8_t *p10 = image->rgba + ((size_t)y1 * image->width + x0) * 4U;
    const uint8_t *p11 = image->rgba + ((size_t)y1 * image->width + x1) * 4U;
    for (int c = 0; c < 4; ++c) {
        double top = p00[c] + (p01[c] - p00[c]) * fx;
        double bottom = p10[c] + (p11[c] - p10[c]) * fx;
        out[c] = (uint8_t)lround(top + (bottom - top) * fy);
    }
}

/* Perspective view straight ahead (4:3) out of an equirectangular eye,
 * with the same mapping as the projection shader. */
static struct ui_image *straighten(const struct ui_image *eye,
                                   uint32_t projection)
{
    int height = eye->height, width = eye->height * 4 / 3;
    struct ui_image *view = ui_image_create(width, height);
    if (!view)
        return NULL;
    double half_x = tan(FORWARD_FOV_DEGREES * PI / 360.0);
    double half_y = half_x * height / width;
    double span = projection == NV12_PROJECTION_EQUIRECT_360 ? 2.0 * PI : PI;
    for (int y = 0; y < height; ++y) {
        double dy = (1.0 - (y + 0.5) * 2.0 / height) * half_y;
        for (int x = 0; x < width; ++x) {
            double dx = ((x + 0.5) * 2.0 / width - 1.0) * half_x;
            double longitude = atan2(dx, 1.0);
            double latitude = atan2(dy, sqrt(dx * dx + 1.0));
            double u = longitude / span + 0.5;
            double v = 0.5 - latitude / PI;
            sample(eye, u * eye->width, v * eye->height,
                   view->rgba + ((size_t)y * width + x) * 4U);
        }
    }
    return view;
}

static struct ui_image *make_thumbnail(struct item_state *state, int index,
                                       uint32_t layout, uint32_t projection)
{
    int tile = index / (state->info.columns * state->info.rows);
    if (tile >= state->tile_count)
        return NULL;
    if (!state->tiles[tile] &&
        jf_fetch_trickplay_tile(&state->config, state->item, &state->info,
                                tile, &state->tiles[tile],
                                &state->tile_sizes[tile]) < 0) {
        printf("trickplay tile=%d fetch=failed\n", tile);
        return NULL;
    }
    if (state->decoded_index != tile) {
        ui_image_free(state->decoded);
        state->decoded = thumbnail_decode_jpeg(state->tiles[tile],
                                               state->tile_sizes[tile]);
        state->decoded_index = state->decoded ? tile : -1;
        if (!state->decoded)
            return NULL;
    }
    struct ui_image *eye = cut_thumbnail(state, index, layout);
    if (!eye || (projection != NV12_PROJECTION_EQUIRECT_180 &&
                 projection != NV12_PROJECTION_EQUIRECT_360))
        return eye;
    struct ui_image *view = straighten(eye, projection);
    ui_image_free(eye);
    return view;
}

static void *worker(void *argument)
{
    struct trickplay *trickplay = argument;
    struct item_state state = {0};
    reset_item(&state);
    pthread_mutex_lock(&trickplay->mutex);
    for (;;) {
        int index = -1;
        while (!trickplay->stopping) {
            if (trickplay->generation != state.generation)
                break;
            if (state.info_state == 0 && state.item[0])
                break;
            if (state.info_state == 1 && trickplay->want_valid) {
                index = thumbnail_index(&state, trickplay->want_us);
                if (index != state.done_index ||
                    trickplay->want_layout != state.done_layout ||
                    trickplay->want_projection != state.done_projection)
                    break;
            }
            index = -1;
            pthread_cond_wait(&trickplay->changed, &trickplay->mutex);
        }
        if (trickplay->stopping)
            break;
        if (trickplay->generation != state.generation) {
            reset_item(&state);
            state.generation = trickplay->generation;
            snprintf(state.item, sizeof(state.item), "%s", trickplay->item);
            continue;
        }
        uint64_t generation = state.generation;
        uint32_t layout = trickplay->want_layout;
        uint32_t projection = trickplay->want_projection;
        pthread_mutex_unlock(&trickplay->mutex);
        struct ui_image *image = NULL;
        if (index < 0)
            load_info(&state);
        else
            image = make_thumbnail(&state, index, layout, projection);
        pthread_mutex_lock(&trickplay->mutex);
        if (index >= 0) {
            state.done_index = index;
            state.done_layout = layout;
            state.done_projection = projection;
        }
        if (image && generation == trickplay->generation) {
            ui_image_free(trickplay->result);
            trickplay->result = image;
        } else {
            ui_image_free(image);
        }
    }
    pthread_mutex_unlock(&trickplay->mutex);
    reset_item(&state);
    return NULL;
}

struct trickplay *trickplay_create(void)
{
    struct trickplay *trickplay = calloc(1, sizeof(*trickplay));
    if (!trickplay)
        return NULL;
    pthread_mutex_init(&trickplay->mutex, NULL);
    pthread_cond_init(&trickplay->changed, NULL);
    trickplay->thread_started =
        pthread_create(&trickplay->thread, NULL, worker, trickplay) == 0;
    if (!trickplay->thread_started) {
        trickplay_destroy(trickplay);
        return NULL;
    }
    return trickplay;
}

void trickplay_destroy(struct trickplay *trickplay)
{
    if (!trickplay)
        return;
    if (trickplay->thread_started) {
        pthread_mutex_lock(&trickplay->mutex);
        trickplay->stopping = true;
        pthread_cond_signal(&trickplay->changed);
        pthread_mutex_unlock(&trickplay->mutex);
        pthread_join(trickplay->thread, NULL);
    }
    ui_image_free(trickplay->result);
    pthread_cond_destroy(&trickplay->changed);
    pthread_mutex_destroy(&trickplay->mutex);
    free(trickplay);
}

void trickplay_open(struct trickplay *trickplay, const char *item_id)
{
    if (!trickplay)
        return;
    pthread_mutex_lock(&trickplay->mutex);
    trickplay->generation++;
    snprintf(trickplay->item, sizeof(trickplay->item), "%s",
             item_id ? item_id : "");
    trickplay->want_valid = false;
    ui_image_free(trickplay->result);
    trickplay->result = NULL;
    pthread_cond_signal(&trickplay->changed);
    pthread_mutex_unlock(&trickplay->mutex);
}

void trickplay_request(struct trickplay *trickplay, int64_t position_us,
                       uint32_t stereo_layout, uint32_t projection_mode)
{
    if (!trickplay)
        return;
    pthread_mutex_lock(&trickplay->mutex);
    if (!trickplay->want_valid || trickplay->want_us != position_us ||
        trickplay->want_layout != stereo_layout ||
        trickplay->want_projection != projection_mode) {
        trickplay->want_valid = true;
        trickplay->want_us = position_us;
        trickplay->want_layout = stereo_layout;
        trickplay->want_projection = projection_mode;
        pthread_cond_signal(&trickplay->changed);
    }
    pthread_mutex_unlock(&trickplay->mutex);
}

bool trickplay_poll(struct trickplay *trickplay, struct ui_image **image)
{
    if (!trickplay)
        return false;
    pthread_mutex_lock(&trickplay->mutex);
    *image = trickplay->result;
    trickplay->result = NULL;
    pthread_mutex_unlock(&trickplay->mutex);
    return *image != NULL;
}
