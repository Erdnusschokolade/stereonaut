#define _POSIX_C_SOURCE 200809L
#include "trickplay.h"

#include "jellyfin.h"
#include "nv12-playback-ipc.h"
#include "thumbnails.h"
#include "ui-draw.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    uint32_t want_layout;
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
    uint32_t done_layout;
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

static struct ui_image *make_thumbnail(struct item_state *state, int index,
                                       uint32_t layout)
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
    return cut_thumbnail(state, index, layout);
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
                    trickplay->want_layout != state.done_layout)
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
        pthread_mutex_unlock(&trickplay->mutex);
        struct ui_image *image = NULL;
        if (index < 0)
            load_info(&state);
        else
            image = make_thumbnail(&state, index, layout);
        pthread_mutex_lock(&trickplay->mutex);
        if (index >= 0) {
            state.done_index = index;
            state.done_layout = layout;
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
                       uint32_t stereo_layout)
{
    if (!trickplay)
        return;
    pthread_mutex_lock(&trickplay->mutex);
    if (!trickplay->want_valid || trickplay->want_us != position_us ||
        trickplay->want_layout != stereo_layout) {
        trickplay->want_valid = true;
        trickplay->want_us = position_us;
        trickplay->want_layout = stereo_layout;
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
