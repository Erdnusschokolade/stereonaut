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
#include <time.h>

#define PI 3.14159265358979323846
/* Finished thumbnails kept by the worker: moving the strip by a step only
 * makes the new end. */
#define CACHE_SIZE 16
/* Decoded tiles kept (3200x1600 RGBA, 20 MB each): a strip across a tile
 * border does not decode the two tiles in turn. */
#define DECODED_TILES 3
/* All tiles are fetched in the background once the item is described:
 * starting PREFETCH_DELAY_MS later (the video stream starts first) with
 * PREFETCH_PACE_MS between tiles; requested pictures go first. */
#define PREFETCH_DELAY_MS 3000
#define PREFETCH_PACE_MS 150
/* A strip is handed over whole (each hand-over redraws the panel); only
 * if that takes longer, its middle comes first. */
#define STRIP_WAIT_MS 20.0

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
    double want_at_ms; /* when the request last changed */
    int64_t want_us, want_spacing_us;
    uint32_t want_layout, want_projection;
    int want_fov;
    /* Newest finished strip, taken by trickplay_poll. */
    struct ui_image *result[TRICKPLAY_STRIP];
    bool result_ready;
    /* Description of the current item, taken by trickplay_details. */
    struct jf_details details;
    bool details_ready;
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
    struct {
        int tile; /* -1: free */
        struct ui_image *image;
        uint64_t used;
    } decoded[DECODED_TILES];
    uint64_t decode_clock;
    int prefetch_next;     /* next tile to fetch in the background */
    double prefetch_at_ms; /* monotonic time of that fetch */
    /* Thumbnails made for these view settings; image NULL if it failed. */
    uint32_t cache_layout, cache_projection;
    int cache_fov;
    struct {
        int index; /* -1: free */
        struct ui_image *image;
        uint64_t used;
    } cache[CACHE_SIZE];
    uint64_t cache_clock;
    int published[TRICKPLAY_STRIP]; /* indices of the last strip */
    int published_count;            /* of them, how many were made */
};

static void clear_cache(struct item_state *state)
{
    for (int i = 0; i < CACHE_SIZE; ++i) {
        ui_image_free(state->cache[i].image);
        state->cache[i].image = NULL;
        state->cache[i].index = -1;
    }
    state->published_count = -1;
}

static void reset_item(struct item_state *state)
{
    for (int i = 0; i < state->tile_count; ++i)
        free(state->tiles[i]);
    free(state->tiles);
    free(state->tile_sizes);
    for (int i = 0; i < DECODED_TILES; ++i)
        ui_image_free(state->decoded[i].image);
    for (int i = 0; i < CACHE_SIZE; ++i)
        ui_image_free(state->cache[i].image);
    memset(state, 0, sizeof(*state));
    for (int i = 0; i < DECODED_TILES; ++i)
        state->decoded[i].tile = -1;
    for (int i = 0; i < CACHE_SIZE; ++i)
        state->cache[i].index = -1;
    state->published_count = -1;
}

static double monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1e3 + now.tv_nsec / 1e6;
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

/* Thumbnail indices of the strip around the request, -1 outside the
 * title. */
static void strip_indices(const struct item_state *state, int64_t want_us,
                          int64_t spacing_us, int indices[TRICKPLAY_STRIP])
{
    int64_t interval_us = (int64_t)state->info.interval_ms * 1000;
    bool single = spacing_us <= 0;
    if (spacing_us < interval_us)
        spacing_us = interval_us;
    for (int slot = 0; slot < TRICKPLAY_STRIP; ++slot) {
        if (single && slot != TRICKPLAY_STRIP / 2) {
            indices[slot] = -1;
            continue;
        }
        int64_t position = want_us + (slot - TRICKPLAY_STRIP / 2) * spacing_us;
        int64_t index = position / interval_us;
        indices[slot] = position < 0 || index >= state->info.count
                            ? -1
                            : thumbnail_index(state, position);
    }
}

static int cache_find(const struct item_state *state, int index)
{
    for (int i = 0; i < CACHE_SIZE; ++i)
        if (state->cache[i].index == index)
            return i;
    return -1;
}

/* Keeps `image` for `index`, replacing the least recently used entry that
 * the current strip does not show. */
static void cache_store(struct item_state *state, int index,
                        struct ui_image *image,
                        const int shown[TRICKPLAY_STRIP])
{
    int victim = -1;
    for (int i = 0; i < CACHE_SIZE; ++i) {
        bool in_strip = false;
        for (int slot = 0; slot < TRICKPLAY_STRIP; ++slot)
            in_strip |= state->cache[i].index >= 0 &&
                        state->cache[i].index == shown[slot];
        if (in_strip)
            continue;
        if (victim < 0 || state->cache[i].index < 0 ||
            (state->cache[victim].index >= 0 &&
             state->cache[i].used < state->cache[victim].used))
            victim = i;
        if (state->cache[i].index < 0)
            break;
    }
    if (victim < 0) {
        ui_image_free(image);
        return;
    }
    ui_image_free(state->cache[victim].image);
    state->cache[victim].index = index;
    state->cache[victim].image = image;
    state->cache[victim].used = ++state->cache_clock;
}

static struct ui_image *copy_image(const struct ui_image *image)
{
    if (!image)
        return NULL;
    struct ui_image *copy = ui_image_create(image->width, image->height);
    if (copy)
        memcpy(copy->rgba, image->rgba,
               (size_t)image->width * (size_t)image->height * 4U);
    return copy;
}

static void load_info(struct trickplay *trickplay, struct item_state *state)
{
    char error[256];
    state->info_state = -1;
    if (jf_config_load(&state->config, error, sizeof(error)) < 0 ||
        !jf_config_has_token(&state->config))
        return;
    struct jf_details *details = malloc(sizeof(*details));
    if (details &&
        jf_item_details(&state->config, state->item, details) == 0) {
        printf("item_details item=%s chapters=%d overview=%zu\n",
               state->item, details->chapter_count,
               strlen(details->overview));
        pthread_mutex_lock(&trickplay->mutex);
        if (state->generation == trickplay->generation) {
            trickplay->details = *details;
            trickplay->details_ready = true;
        }
        pthread_mutex_unlock(&trickplay->mutex);
    }
    free(details);
    if (jf_trickplay_info(&state->config, state->item, &state->info) < 0) {
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
    state->prefetch_at_ms = monotonic_ms() + PREFETCH_DELAY_MS;
    printf("trickplay item=%s available=1 width=%d height=%d tiles=%d"
           " interval_ms=%d count=%d\n",
           state->item, state->info.width, state->info.height,
           state->tile_count, state->info.interval_ms, state->info.count);
}

/* Cuts thumbnail `index` out of its decoded tile, reduced to the left
 * (SBS) or upper (TB) eye. */
static struct ui_image *cut_thumbnail(const struct item_state *state,
                                      const struct ui_image *decoded,
                                      int index, uint32_t layout)
{
    const struct jf_trickplay *info = &state->info;
    int slot = index % (info->columns * info->rows);
    int x0 = slot % info->columns * info->width;
    int y0 = slot / info->columns * info->height;
    int width = info->width, height = info->height;
    if (x0 + width > decoded->width)
        width = decoded->width - x0;
    if (y0 + height > decoded->height)
        height = decoded->height - y0;
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
               decoded->rgba +
                   ((size_t)(y0 + y) * (size_t)decoded->width +
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

/* Where each pixel of the straightened view samples the eye picture: it
 * depends only on the sizes, the projection and the field of view, so it
 * is computed once (per pixel two arctangents and a square root made a
 * strip of five pictures five times slower). */
struct view_map {
    int eye_width, eye_height;
    uint32_t projection;
    int fov;
    float *xy; /* source x, y per view pixel */
};

static const float *view_map(struct view_map *map, const struct ui_image *eye,
                             uint32_t projection, int fov_degrees)
{
    if (map->xy && map->eye_width == eye->width &&
        map->eye_height == eye->height && map->projection == projection &&
        map->fov == fov_degrees)
        return map->xy;
    int height = eye->height, width = eye->height * 4 / 3;
    free(map->xy);
    map->xy = malloc((size_t)width * (size_t)height * 2U * sizeof(float));
    if (!map->xy)
        return NULL;
    double half_x = tan(fov_degrees * PI / 360.0);
    double half_y = half_x * height / width;
    double span = projection == NV12_PROJECTION_EQUIRECT_360 ? 2.0 * PI : PI;
    for (int y = 0; y < height; ++y) {
        double dy = (1.0 - (y + 0.5) * 2.0 / height) * half_y;
        for (int x = 0; x < width; ++x) {
            double dx = ((x + 0.5) * 2.0 / width - 1.0) * half_x;
            double longitude = atan2(dx, 1.0);
            double latitude = atan2(dy, sqrt(dx * dx + 1.0));
            float *out = map->xy + ((size_t)y * width + x) * 2U;
            out[0] = (float)((longitude / span + 0.5) * eye->width);
            out[1] = (float)((0.5 - latitude / PI) * eye->height);
        }
    }
    map->eye_width = eye->width;
    map->eye_height = eye->height;
    map->projection = projection;
    map->fov = fov_degrees;
    return map->xy;
}

/* Perspective view straight ahead (4:3) out of an equirectangular eye,
 * with the same mapping as the projection shader. */
static struct ui_image *straighten(const struct ui_image *eye,
                                   struct view_map *map, uint32_t projection,
                                   int fov_degrees)
{
    const float *xy = view_map(map, eye, projection, fov_degrees);
    int height = eye->height, width = eye->height * 4 / 3;
    struct ui_image *view = xy ? ui_image_create(width, height) : NULL;
    if (!view)
        return NULL;
    for (size_t i = 0; i < (size_t)width * (size_t)height; ++i)
        sample(eye, xy[2 * i], xy[2 * i + 1], view->rgba + i * 4U);
    return view;
}

static bool fetch_tile(struct item_state *state, int tile)
{
    if (state->tiles[tile])
        return true;
    if (jf_fetch_trickplay_tile(&state->config, state->item, &state->info,
                                tile, &state->tiles[tile],
                                &state->tile_sizes[tile]) == 0)
        return true;
    printf("trickplay tile=%d fetch=failed\n", tile);
    return false;
}

/* The decoded tile, decoding it into the least recently used slot. */
static const struct ui_image *decoded_tile(struct item_state *state, int tile)
{
    int slot = 0;
    for (int i = 0; i < DECODED_TILES; ++i) {
        if (state->decoded[i].tile == tile) {
            state->decoded[i].used = ++state->decode_clock;
            return state->decoded[i].image;
        }
        if (state->decoded[i].used < state->decoded[slot].used)
            slot = i;
    }
    ui_image_free(state->decoded[slot].image);
    state->decoded[slot].image = thumbnail_decode_jpeg(
        state->tiles[tile], state->tile_sizes[tile]);
    state->decoded[slot].tile = state->decoded[slot].image ? tile : -1;
    state->decoded[slot].used = ++state->decode_clock;
    return state->decoded[slot].image;
}

static struct ui_image *make_thumbnail(struct item_state *state,
                                       struct view_map *map, int index,
                                       uint32_t layout, uint32_t projection,
                                       int fov)
{
    int tile = index / (state->info.columns * state->info.rows);
    if (tile >= state->tile_count || !fetch_tile(state, tile))
        return NULL;
    const struct ui_image *decoded = decoded_tile(state, tile);
    if (!decoded)
        return NULL;
    struct ui_image *eye = cut_thumbnail(state, decoded, index, layout);
    if (!eye || fov <= 0 || fov >= 180 ||
        (projection != NV12_PROJECTION_EQUIRECT_180 &&
         projection != NV12_PROJECTION_EQUIRECT_360))
        return eye;
    struct ui_image *view = straighten(eye, map, projection, fov);
    ui_image_free(eye);
    return view;
}

/* With the mutex held: publishes the strip when it is complete, or as far
 * as it is made once STRIP_WAIT_MS passed and its middle is (a moved strip
 * keeps its old pictures until then), and returns the next thumbnail to
 * make, -1 when the strip is complete. */
static int next_thumbnail(struct trickplay *trickplay,
                          struct item_state *state)
{
    if (trickplay->want_layout != state->cache_layout ||
        trickplay->want_projection != state->cache_projection ||
        trickplay->want_fov != state->cache_fov) {
        clear_cache(state);
        state->cache_layout = trickplay->want_layout;
        state->cache_projection = trickplay->want_projection;
        state->cache_fov = trickplay->want_fov;
    }
    int indices[TRICKPLAY_STRIP];
    strip_indices(state, trickplay->want_us, trickplay->want_spacing_us,
                  indices);
    /* The middle first, then outwards. */
    static const int order[TRICKPLAY_STRIP] = {2, 1, 3, 0, 4};
    int missing = -1, made = 0;
    for (int i = 0; i < TRICKPLAY_STRIP; ++i) {
        int index = indices[order[i]];
        if (index < 0)
            continue;
        int entry = cache_find(state, index);
        if (entry < 0) {
            if (missing < 0)
                missing = index;
            continue;
        }
        state->cache[entry].used = ++state->cache_clock;
        ++made;
    }
    int middle = indices[TRICKPLAY_STRIP / 2];
    bool middle_made = middle < 0 || cache_find(state, middle) >= 0;
    bool waited = monotonic_ms() - trickplay->want_at_ms > STRIP_WAIT_MS;
    if (middle_made && (missing < 0 || waited) &&
        (made != state->published_count ||
         memcmp(indices, state->published, sizeof(indices)) != 0)) {
        for (int slot = 0; slot < TRICKPLAY_STRIP; ++slot) {
            int entry = indices[slot] < 0 ? -1
                                          : cache_find(state, indices[slot]);
            ui_image_free(trickplay->result[slot]);
            trickplay->result[slot] =
                entry < 0 ? NULL : copy_image(state->cache[entry].image);
        }
        trickplay->result_ready = true;
        memcpy(state->published, indices, sizeof(indices));
        state->published_count = made;
    }
    return missing;
}

static void *worker(void *argument)
{
    struct trickplay *trickplay = argument;
    struct item_state state = {0};
    struct view_map map = {0};
    reset_item(&state);
    pthread_mutex_lock(&trickplay->mutex);
    for (;;) {
        int index = -1, prefetch = -1;
        while (!trickplay->stopping) {
            if (trickplay->generation != state.generation)
                break;
            if (state.info_state == 0 && state.item[0])
                break;
            if (state.info_state == 1 && trickplay->want_valid) {
                index = next_thumbnail(trickplay, &state);
                if (index >= 0)
                    break;
            }
            while (state.info_state == 1 &&
                   state.prefetch_next < state.tile_count &&
                   state.tiles[state.prefetch_next])
                ++state.prefetch_next;
            if (state.info_state != 1 ||
                state.prefetch_next >= state.tile_count) {
                pthread_cond_wait(&trickplay->changed, &trickplay->mutex);
                continue;
            }
            double wait_ms = state.prefetch_at_ms - monotonic_ms();
            if (wait_ms <= 0.0) {
                prefetch = state.prefetch_next;
                break;
            }
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            long long ns = until.tv_nsec + (long long)(wait_ms * 1e6);
            until.tv_sec += (time_t)(ns / 1000000000LL);
            until.tv_nsec = (long)(ns % 1000000000LL);
            pthread_cond_timedwait(&trickplay->changed, &trickplay->mutex,
                                   &until);
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
        uint32_t layout = state.cache_layout;
        uint32_t projection = state.cache_projection;
        int fov = state.cache_fov;
        pthread_mutex_unlock(&trickplay->mutex);
        struct ui_image *image = NULL;
        if (prefetch >= 0) {
            fetch_tile(&state, prefetch);
            ++state.prefetch_next; /* a failed tile is fetched on demand */
            state.prefetch_at_ms = monotonic_ms() + PREFETCH_PACE_MS;
            if (state.prefetch_next >= state.tile_count)
                printf("trickplay prefetch=done tiles=%d\n",
                       state.tile_count);
        } else if (index < 0) {
            load_info(trickplay, &state);
        } else {
            image = make_thumbnail(&state, &map, index, layout, projection,
                                   fov);
        }
        pthread_mutex_lock(&trickplay->mutex);
        if (index < 0)
            continue;
        /* A failed thumbnail is kept as NULL so it is not tried again. */
        if (generation == trickplay->generation &&
            layout == trickplay->want_layout &&
            projection == trickplay->want_projection &&
            fov == trickplay->want_fov) {
            int shown[TRICKPLAY_STRIP];
            strip_indices(&state, trickplay->want_us,
                          trickplay->want_spacing_us, shown);
            cache_store(&state, index, image, shown);
        } else {
            ui_image_free(image);
        }
    }
    pthread_mutex_unlock(&trickplay->mutex);
    reset_item(&state);
    free(map.xy);
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
    for (int slot = 0; slot < TRICKPLAY_STRIP; ++slot)
        ui_image_free(trickplay->result[slot]);
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
    trickplay->details_ready = false;
    for (int slot = 0; slot < TRICKPLAY_STRIP; ++slot) {
        ui_image_free(trickplay->result[slot]);
        trickplay->result[slot] = NULL;
    }
    trickplay->result_ready = false;
    pthread_cond_signal(&trickplay->changed);
    pthread_mutex_unlock(&trickplay->mutex);
}

void trickplay_request(struct trickplay *trickplay, int64_t position_us,
                       int64_t spacing_us, uint32_t stereo_layout,
                       uint32_t projection_mode, int fov_degrees)
{
    if (!trickplay)
        return;
    pthread_mutex_lock(&trickplay->mutex);
    if (!trickplay->want_valid || trickplay->want_us != position_us ||
        trickplay->want_spacing_us != spacing_us ||
        trickplay->want_layout != stereo_layout ||
        trickplay->want_projection != projection_mode ||
        trickplay->want_fov != fov_degrees) {
        trickplay->want_valid = true;
        trickplay->want_at_ms = monotonic_ms();
        trickplay->want_us = position_us;
        trickplay->want_spacing_us = spacing_us;
        trickplay->want_layout = stereo_layout;
        trickplay->want_projection = projection_mode;
        trickplay->want_fov = fov_degrees;
        pthread_cond_signal(&trickplay->changed);
    }
    pthread_mutex_unlock(&trickplay->mutex);
}

bool trickplay_details(struct trickplay *trickplay,
                       struct jf_details *details)
{
    if (!trickplay)
        return false;
    pthread_mutex_lock(&trickplay->mutex);
    bool ready = trickplay->details_ready;
    if (ready)
        *details = trickplay->details;
    trickplay->details_ready = false;
    pthread_mutex_unlock(&trickplay->mutex);
    return ready;
}

bool trickplay_poll(struct trickplay *trickplay,
                    struct ui_image *strip[TRICKPLAY_STRIP])
{
    if (!trickplay)
        return false;
    pthread_mutex_lock(&trickplay->mutex);
    bool ready = trickplay->result_ready;
    for (int slot = 0; slot < TRICKPLAY_STRIP; ++slot) {
        strip[slot] = ready ? trickplay->result[slot] : NULL;
        if (ready)
            trickplay->result[slot] = NULL;
    }
    trickplay->result_ready = false;
    pthread_mutex_unlock(&trickplay->mutex);
    return ready;
}
