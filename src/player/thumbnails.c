#define _POSIX_C_SOURCE 200809L
#include "thumbnails.h"

#include "jellyfin.h"
#include "ui-draw.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <jpeglib.h> /* needs stdio.h first */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define WORKERS 2
#define DOWNLOAD_WIDTH 480

struct job {
    uint64_t generation;
    size_t index;
    char *source;
};

struct thumbnail_loader {
    pthread_t workers[WORKERS];
    int worker_count;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    bool stopping;
    uint64_t generation;
    struct job *jobs;
    size_t job_count, job_capacity;
    struct thumbnail_result *results;
    size_t result_count, result_capacity;
    pthread_mutex_t config_mutex; /* guards config and config_loaded */
    struct jf_config config;
    bool config_loaded;
};

/* ---- cache ------------------------------------------------------------- */

static bool cache_path(const char *item, const char *image, char *path,
                       size_t size)
{
    const char *base = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    char directory[PATH_MAX];
    if (base && base[0])
        snprintf(directory, sizeof(directory), "%s/stereonaut/thumbs", base);
    else if (home && home[0])
        snprintf(directory, sizeof(directory), "%s/.cache/stereonaut/thumbs",
                 home);
    else
        return false;
    const char *tag = strstr(image, "tag=");
    char clean[2][64];
    const char *parts[2] = {item, tag ? tag + 4 : "none"};
    for (int p = 0; p < 2; ++p) {
        size_t used = 0;
        for (const char *c = parts[p]; *c && used + 1 < sizeof(clean[p]); ++c)
            if ((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') ||
                (*c >= 'A' && *c <= 'Z'))
                clean[p][used++] = *c;
        clean[p][used] = '\0';
    }
    int written = snprintf(path, size, "%s/%s-%c-%s.jpg", directory, clean[0],
                           strncmp(image, "Backdrop", 8) == 0 ? 'b' : 'p',
                           clean[1]);
    return written > 0 && (size_t)written < size;
}

static void make_directories(const char *file)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s", file);
    for (char *slash = strchr(path + 1, '/'); slash;
         slash = strchr(slash + 1, '/')) {
        *slash = '\0';
        mkdir(path, 0700);
        *slash = '/';
    }
}

static char *read_file(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return NULL;
    char *data = NULL;
    if (fseek(file, 0, SEEK_END) == 0) {
        long length = ftell(file);
        if (length > 0 && length < 16L * 1024 * 1024 &&
            fseek(file, 0, SEEK_SET) == 0 && (data = malloc((size_t)length)) &&
            fread(data, 1, (size_t)length, file) == (size_t)length) {
            *size = (size_t)length;
        } else {
            free(data);
            data = NULL;
        }
    }
    fclose(file);
    return data;
}

static void write_file(const char *path, const char *data, size_t size)
{
    make_directories(path);
    char temporary[PATH_MAX + 16];
    snprintf(temporary, sizeof(temporary), "%s.%ld.tmp", path, (long)getpid());
    FILE *file = fopen(temporary, "wb");
    if (!file)
        return;
    bool ok = fwrite(data, 1, size, file) == size;
    ok = fclose(file) == 0 && ok;
    if (!ok || rename(temporary, path) < 0)
        unlink(temporary);
}

/* ---- decoding ---------------------------------------------------------- */

struct jpeg_failure {
    struct jpeg_error_mgr manager;
    jmp_buf jump;
};

static void jpeg_failed(j_common_ptr info)
{
    longjmp(((struct jpeg_failure *)info->err)->jump, 1);
}

static struct ui_image *decode_jpeg(const char *data, size_t size)
{
    struct jpeg_decompress_struct info;
    struct jpeg_failure failure;
    struct ui_image *volatile image = NULL;
    info.err = jpeg_std_error(&failure.manager);
    failure.manager.error_exit = jpeg_failed;
    if (setjmp(failure.jump)) {
        jpeg_destroy_decompress(&info);
        ui_image_free(image);
        return NULL;
    }
    jpeg_create_decompress(&info);
    jpeg_mem_src(&info, (const unsigned char *)data, (unsigned long)size);
    jpeg_read_header(&info, TRUE);
    info.out_color_space = JCS_RGB;
    jpeg_start_decompress(&info);
    image = ui_image_create((int)info.output_width, (int)info.output_height);
    unsigned char *row = malloc((size_t)info.output_width * 3U);
    if (!image || !row) {
        free(row);
        jpeg_destroy_decompress(&info);
        ui_image_free(image);
        return NULL;
    }
    while (info.output_scanline < info.output_height) {
        unsigned y = info.output_scanline;
        JSAMPROW rows[1] = {row};
        jpeg_read_scanlines(&info, rows, 1);
        uint8_t *target = image->rgba + (size_t)y * info.output_width * 4U;
        for (unsigned x = 0; x < info.output_width; ++x) {
            memcpy(target + x * 4U, row + x * 3U, 3);
            target[x * 4U + 3U] = 255;
        }
    }
    free(row);
    jpeg_finish_decompress(&info);
    jpeg_destroy_decompress(&info);
    return image;
}

/* Side-by-side frames (about 2:1 and wider) show both eyes; keep the left. */
static struct ui_image *prepare(struct ui_image *decoded)
{
    struct ui_image *source = decoded;
    struct ui_image *eye = NULL;
    if (decoded->width >= decoded->height * 195 / 100) {
        eye = ui_image_create(decoded->width / 2, decoded->height);
        if (eye) {
            for (int y = 0; y < decoded->height; ++y)
                memcpy(eye->rgba + (size_t)y * (size_t)eye->width * 4U,
                       decoded->rgba + (size_t)y * (size_t)decoded->width * 4U,
                       (size_t)eye->width * 4U);
            source = eye;
        }
    }
    struct ui_image *result = source;
    if (source->width > THUMBNAIL_WIDTH) {
        int height = source->height * THUMBNAIL_WIDTH / source->width;
        result = ui_image_scaled(source, THUMBNAIL_WIDTH,
                                 height > 0 ? height : 1);
    }
    if (result != decoded)
        ui_image_free(decoded);
    if (eye && result != eye)
        ui_image_free(eye);
    return result;
}

/* `config` is NULL if Jellyfin is not configured; *stale is set when a
 * download failed and the configuration should be reloaded. */
static struct ui_image *load(const struct jf_config *config,
                             const char *source, bool *stale)
{
    const char *separator = strchr(source, '|');
    if (!separator)
        return NULL;
    char item[JF_ID_SIZE];
    size_t length = (size_t)(separator - source);
    if (length >= sizeof(item))
        return NULL;
    memcpy(item, source, length);
    item[length] = '\0';
    const char *image = separator + 1;

    char path[PATH_MAX];
    bool cacheable = cache_path(item, image, path, sizeof(path));
    size_t size = 0;
    char *data = cacheable ? read_file(path, &size) : NULL;
    if (!data) {
        if (!config)
            return NULL;
        if (jf_fetch_image(config, item, image, DOWNLOAD_WIDTH, &data,
                           &size) < 0) {
            /* The token may have changed (new sign-in): reload next time. */
            *stale = true;
            return NULL;
        }
        if (cacheable)
            write_file(path, data, size);
    }
    struct ui_image *decoded = decode_jpeg(data, size);
    free(data);
    return decoded ? prepare(decoded) : NULL;
}

/* ---- queue ------------------------------------------------------------- */

static void *worker(void *argument)
{
    struct thumbnail_loader *loader = argument;
    pthread_mutex_lock(&loader->mutex);
    for (;;) {
        while (!loader->job_count && !loader->stopping)
            pthread_cond_wait(&loader->changed, &loader->mutex);
        if (loader->stopping)
            break;
        struct job job = loader->jobs[0];
        memmove(loader->jobs, loader->jobs + 1,
                (loader->job_count - 1) * sizeof(*loader->jobs));
        --loader->job_count;
        pthread_mutex_unlock(&loader->mutex);
        struct jf_config config;
        pthread_mutex_lock(&loader->config_mutex);
        if (!loader->config_loaded) {
            char error[256];
            if (jf_config_load(&loader->config, error, sizeof(error)) == 0)
                loader->config_loaded = true;
        }
        bool configured = loader->config_loaded;
        config = loader->config;
        pthread_mutex_unlock(&loader->config_mutex);
        bool stale = false;
        struct ui_image *image = load(configured ? &config : NULL,
                                      job.source, &stale);
        free(job.source);
        if (stale) {
            pthread_mutex_lock(&loader->config_mutex);
            loader->config_loaded = false;
            pthread_mutex_unlock(&loader->config_mutex);
        }
        pthread_mutex_lock(&loader->mutex);
        if (job.generation != loader->generation) {
            ui_image_free(image);
            continue;
        }
        if (loader->result_count == loader->result_capacity) {
            size_t capacity = loader->result_capacity ? loader->result_capacity * 2
                                                      : 32;
            struct thumbnail_result *grown =
                realloc(loader->results, capacity * sizeof(*grown));
            if (!grown) {
                ui_image_free(image);
                continue;
            }
            loader->results = grown;
            loader->result_capacity = capacity;
        }
        loader->results[loader->result_count++] = (struct thumbnail_result){
            job.generation, job.index, image};
    }
    pthread_mutex_unlock(&loader->mutex);
    return NULL;
}

struct thumbnail_loader *thumbnail_loader_create(void)
{
    struct thumbnail_loader *loader = calloc(1, sizeof(*loader));
    if (!loader)
        return NULL;
    pthread_mutex_init(&loader->mutex, NULL);
    pthread_mutex_init(&loader->config_mutex, NULL);
    pthread_cond_init(&loader->changed, NULL);
    for (int i = 0; i < WORKERS; ++i)
        if (pthread_create(&loader->workers[i], NULL, worker, loader) == 0)
            ++loader->worker_count;
    return loader;
}

void thumbnail_loader_destroy(struct thumbnail_loader *loader)
{
    if (!loader)
        return;
    pthread_mutex_lock(&loader->mutex);
    loader->stopping = true;
    pthread_cond_broadcast(&loader->changed);
    pthread_mutex_unlock(&loader->mutex);
    for (int i = 0; i < loader->worker_count; ++i)
        pthread_join(loader->workers[i], NULL);
    for (size_t i = 0; i < loader->job_count; ++i)
        free(loader->jobs[i].source);
    for (size_t i = 0; i < loader->result_count; ++i)
        ui_image_free(loader->results[i].image);
    free(loader->jobs);
    free(loader->results);
    pthread_cond_destroy(&loader->changed);
    pthread_mutex_destroy(&loader->config_mutex);
    pthread_mutex_destroy(&loader->mutex);
    free(loader);
}

void thumbnail_loader_request(struct thumbnail_loader *loader,
                              uint64_t generation, size_t index,
                              const char *source)
{
    if (!loader || !source)
        return;
    pthread_mutex_lock(&loader->mutex);
    if (generation != loader->generation) {
        for (size_t i = 0; i < loader->job_count; ++i)
            free(loader->jobs[i].source);
        loader->job_count = 0;
        for (size_t i = 0; i < loader->result_count; ++i)
            ui_image_free(loader->results[i].image);
        loader->result_count = 0;
        loader->generation = generation;
    }
    if (loader->job_count == loader->job_capacity) {
        size_t capacity = loader->job_capacity ? loader->job_capacity * 2 : 32;
        struct job *grown = realloc(loader->jobs, capacity * sizeof(*grown));
        if (!grown) {
            pthread_mutex_unlock(&loader->mutex);
            return;
        }
        loader->jobs = grown;
        loader->job_capacity = capacity;
    }
    char *copy = strdup(source);
    if (copy) {
        loader->jobs[loader->job_count++] = (struct job){generation, index,
                                                         copy};
        pthread_cond_signal(&loader->changed);
    }
    pthread_mutex_unlock(&loader->mutex);
}

bool thumbnail_loader_poll(struct thumbnail_loader *loader,
                           struct thumbnail_result *result)
{
    if (!loader)
        return false;
    pthread_mutex_lock(&loader->mutex);
    bool available = loader->result_count > 0;
    if (available) {
        *result = loader->results[0];
        memmove(loader->results, loader->results + 1,
                (loader->result_count - 1) * sizeof(*loader->results));
        --loader->result_count;
    }
    pthread_mutex_unlock(&loader->mutex);
    return available;
}
