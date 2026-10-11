#define _GNU_SOURCE /* memfd_create, F_ADD_SEALS */
#include "software-video.h"

#include "media-pipeline.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/dma-buf.h>
#include <linux/udmabuf.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavutil/error.h>

/* Row pitch and chroma offset aligned well beyond what Turnip requires for
 * linear images. */
#define STRIDE_ALIGNMENT 256U
#define PLANE_ALIGNMENT 4096U

enum buffer_state {
    BUFFER_FREE,    /* the worker may decode into it */
    BUFFER_READY,   /* decoded, waiting in the ready queue */
    BUFFER_TAKEN,   /* with the session or the renderer */
};

struct ready_frame {
    unsigned index;
    int64_t pts_us;
};

struct software_video {
    struct software_video_layout layout;
    struct media_pipeline *pipeline;
    AVCodecContext *codec;
    int memfds[SOFTWARE_VIDEO_BUFFERS];
    uint8_t *mappings[SOFTWARE_VIDEO_BUFFERS];
    int64_t start_us;

    pthread_t thread;
    bool thread_started;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    enum buffer_state states[SOFTWARE_VIDEO_BUFFERS];
    struct ready_frame ready[SOFTWARE_VIDEO_BUFFERS];
    unsigned ready_first, ready_count;
    bool stopping, finished, failed;
    char failure[512];
    uint64_t preroll_drops;
};

static void set_error(char *error, size_t size, const char *format, ...)
{
    if (!error || !size)
        return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, size, format, arguments);
    va_end(arguments);
}

static void set_av_error(char *error, size_t size, const char *operation,
                         int result)
{
    char detail[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(result, detail, sizeof(detail));
    set_error(error, size, "%s: %s", operation, detail);
}

static uint64_t align_up(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1U) / alignment * alignment;
}

/* One frame buffer: a sealed memfd wrapped as a DMA-BUF by udmabuf, mapped
 * for the worker's writes. */
static int create_buffer(struct software_video *video, int device,
                         unsigned index, char *error, size_t error_size)
{
    size_t size = (size_t)video->layout.size;
    int memfd = memfd_create("stereonaut-frame", MFD_CLOEXEC |
                                                     MFD_ALLOW_SEALING);
    if (memfd < 0) {
        set_error(error, error_size, "memfd_create: %s", strerror(errno));
        return -1;
    }
    video->memfds[index] = memfd;
    if (ftruncate(memfd, (off_t)size) < 0 ||
        fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK) < 0) {
        set_error(error, error_size, "prepare frame memory: %s",
                  strerror(errno));
        return -1;
    }
    struct udmabuf_create create = {
        .memfd = (uint32_t)memfd,
        .flags = UDMABUF_FLAGS_CLOEXEC,
        .offset = 0,
        .size = size,
    };
    int dma_buf = ioctl(device, UDMABUF_CREATE, &create);
    if (dma_buf < 0) {
        set_error(error, error_size, "UDMABUF_CREATE: %s", strerror(errno));
        return -1;
    }
    video->layout.fds[index] = dma_buf;
    void *mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                         memfd, 0);
    if (mapping == MAP_FAILED) {
        set_error(error, error_size, "mmap frame memory: %s",
                  strerror(errno));
        return -1;
    }
    video->mappings[index] = mapping;
    video->layout.mappings[index] = mapping;
    return 0;
}

static int open_decoder(struct software_video *video, char *error,
                        size_t error_size)
{
    /* FFmpeg's own "av1" decoder needs a hardware accelerator. */
    const AVCodec *decoder = avcodec_find_decoder_by_name("libdav1d");
    if (!decoder) {
        set_error(error, error_size, "FFmpeg was built without libdav1d");
        return -1;
    }
    AVCodecParameters *parameters = avcodec_parameters_alloc();
    if (!parameters ||
        media_pipeline_copy_video_parameters(video->pipeline,
                                             parameters) < 0) {
        avcodec_parameters_free(&parameters);
        set_error(error, error_size, "copy video parameters failed");
        return -1;
    }
    video->codec = avcodec_alloc_context3(decoder);
    int result = video->codec
                     ? avcodec_parameters_to_context(video->codec, parameters)
                     : AVERROR(ENOMEM);
    avcodec_parameters_free(&parameters);
    if (result < 0) {
        set_av_error(error, error_size, "prepare dav1d", result);
        return -1;
    }
    /* Packets carry microseconds (see decode_worker). */
    video->codec->pkt_timebase = (AVRational){1, 1000000};
    video->codec->thread_count = 0; /* one thread per core */
    /* More frames in flight spread single-tile streams over the cores:
     * 1.32x -> 1.83x real time for a 3840x1920 NVENC clip on the Frame. */
    AVDictionary *options = NULL;
    av_dict_set(&options, "max_frame_delay", "8", 0);
    result = avcodec_open2(video->codec, decoder, &options);
    av_dict_free(&options);
    if (result < 0) {
        set_av_error(error, error_size, "open dav1d", result);
        return -1;
    }
    return 0;
}

int software_video_open(struct software_video **result,
                        struct media_pipeline *pipeline, int64_t start_us,
                        char *error, size_t error_size)
{
    *result = NULL;
    const struct media_stream_info *info = media_pipeline_info(pipeline);
    if (!info || info->width <= 0 || info->height <= 0 ||
        info->width > SOFTWARE_VIDEO_MAX_WIDTH ||
        info->height > SOFTWARE_VIDEO_MAX_HEIGHT || (info->width & 1) ||
        (info->height & 1)) {
        set_error(error, error_size, "AV1 size outside the software limit");
        return -1;
    }
    struct software_video *video = calloc(1, sizeof(*video));
    if (!video) {
        set_error(error, error_size, "allocate software decoder");
        return -1;
    }
    for (unsigned i = 0; i < SOFTWARE_VIDEO_BUFFERS; ++i) {
        video->memfds[i] = -1;
        video->layout.fds[i] = -1;
    }
    pthread_mutex_init(&video->mutex, NULL);
    pthread_cond_init(&video->changed, NULL);
    video->pipeline = pipeline;
    video->start_us = start_us;
    struct software_video_layout *layout = &video->layout;
    layout->width = (uint32_t)info->width;
    layout->height = (uint32_t)info->height;
    layout->stride = (uint32_t)align_up(layout->width, STRIDE_ALIGNMENT);
    layout->uv_offset = align_up((uint64_t)layout->stride * layout->height,
                                 PLANE_ALIGNMENT);
    layout->size = align_up(layout->uv_offset + (uint64_t)layout->stride *
                                                    layout->height / 2U,
                            (uint64_t)sysconf(_SC_PAGESIZE));
    layout->count = SOFTWARE_VIDEO_BUFFERS;

    int device = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (device < 0) {
        set_error(error, error_size, "open /dev/udmabuf: %s",
                  strerror(errno));
        software_video_close(&video);
        return -1;
    }
    for (unsigned i = 0; i < layout->count; ++i) {
        if (create_buffer(video, device, i, error, error_size) < 0) {
            close(device);
            software_video_close(&video);
            return -1;
        }
    }
    close(device);
    if (open_decoder(video, error, error_size) < 0) {
        software_video_close(&video);
        return -1;
    }
    *result = video;
    return 0;
}

static void fail(struct software_video *video, const char *detail)
{
    pthread_mutex_lock(&video->mutex);
    if (!video->failed) {
        video->failed = true;
        snprintf(video->failure, sizeof(video->failure), "%s", detail);
    }
    pthread_cond_broadcast(&video->changed);
    pthread_mutex_unlock(&video->mutex);
}

/* yuv420p -> NV12. The DMA-BUF sync makes the CPU writes visible to the
 * GPU. */
static void copy_frame(struct software_video *video, unsigned index,
                       const AVFrame *frame)
{
    const struct software_video_layout *layout = &video->layout;
    uint8_t *base = video->mappings[index];
    struct dma_buf_sync sync = {
        .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE,
    };
    ioctl(layout->fds[index], DMA_BUF_IOCTL_SYNC, &sync);
    for (uint32_t y = 0; y < layout->height; ++y)
        memcpy(base + (size_t)y * layout->stride,
               frame->data[0] + (size_t)y * (size_t)frame->linesize[0],
               layout->width);
    uint8_t *chroma = base + layout->uv_offset;
    for (uint32_t y = 0; y < layout->height / 2U; ++y) {
        const uint8_t *u = frame->data[1] + (size_t)y * (size_t)frame->linesize[1];
        const uint8_t *v = frame->data[2] + (size_t)y * (size_t)frame->linesize[2];
        /* Little endian: Cb in the low byte comes first. The row start is
         * 256-byte aligned. */
        uint16_t *row = (uint16_t *)(void *)(chroma + (size_t)y * layout->stride);
        for (uint32_t x = 0; x < layout->width / 2U; ++x)
            row[x] = (uint16_t)(u[x] | (uint16_t)(v[x] << 8));
    }
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
    ioctl(layout->fds[index], DMA_BUF_IOCTL_SYNC, &sync);
}

static int free_buffer(const struct software_video *video)
{
    for (unsigned i = 0; i < video->layout.count; ++i)
        if (video->states[i] == BUFFER_FREE)
            return (int)i;
    return -1;
}

/* Decodes while a buffer is free: packets from the pipeline in, NV12
 * frames out to the ready queue. */
static void *decode_worker(void *argument)
{
    struct software_video *video = argument;
    AVFrame *frame = av_frame_alloc();
    char error[512] = {0};
    if (!frame) {
        fail(video, "allocate decoder frame");
        return NULL;
    }
    bool draining = false;
    for (;;) {
        pthread_mutex_lock(&video->mutex);
        int index = -1;
        while (!video->stopping && (index = free_buffer(video)) < 0)
            pthread_cond_wait(&video->changed, &video->mutex);
        bool stopping = video->stopping;
        pthread_mutex_unlock(&video->mutex);
        if (stopping)
            break;

        int result = avcodec_receive_frame(video->codec, frame);
        if (result == AVERROR(EAGAIN) && !draining) {
            struct media_packet packet;
            error[0] = '\0';
            int popped = media_pipeline_pop_video(video->pipeline, &packet,
                                                  50, error, sizeof(error));
            if (popped == 0)
                continue;
            if (popped < 0) {
                if (error[0]) {
                    fail(video, error);
                    break;
                }
                draining = true;
                result = avcodec_send_packet(video->codec, NULL);
            } else {
                packet.packet->pts = packet.pts_us;
                packet.packet->dts = packet.dts_us;
                packet.packet->duration = packet.duration_us;
                result = avcodec_send_packet(video->codec, packet.packet);
                media_packet_unref(&packet);
            }
            if (result < 0 && result != AVERROR_EOF) {
                set_av_error(error, sizeof(error), "dav1d packet", result);
                fail(video, error);
                break;
            }
            continue;
        }
        if (result == AVERROR_EOF)
            break;
        if (result < 0) {
            set_av_error(error, sizeof(error), "dav1d frame", result);
            fail(video, error);
            break;
        }
        int64_t pts_us = frame->best_effort_timestamp != AV_NOPTS_VALUE
                             ? frame->best_effort_timestamp
                             : frame->pts;
        if (video->start_us > 0 && pts_us != AV_NOPTS_VALUE &&
            pts_us < video->start_us) {
            pthread_mutex_lock(&video->mutex);
            video->preroll_drops++;
            pthread_mutex_unlock(&video->mutex);
            av_frame_unref(frame);
            continue;
        }
        if ((frame->format != AV_PIX_FMT_YUV420P &&
             frame->format != AV_PIX_FMT_YUVJ420P) ||
            (uint32_t)frame->width != video->layout.width ||
            (uint32_t)frame->height != video->layout.height) {
            snprintf(error, sizeof(error),
                     "AV1 frame %dx%d format %d differs from the stream",
                     frame->width, frame->height, frame->format);
            fail(video, error);
            break;
        }
        copy_frame(video, (unsigned)index, frame);
        av_frame_unref(frame);
        pthread_mutex_lock(&video->mutex);
        video->states[index] = BUFFER_READY;
        unsigned slot = (video->ready_first + video->ready_count) %
                        SOFTWARE_VIDEO_BUFFERS;
        video->ready[slot] = (struct ready_frame){(unsigned)index,
                                                  pts_us == AV_NOPTS_VALUE
                                                      ? 0 : pts_us};
        video->ready_count++;
        pthread_cond_broadcast(&video->changed);
        pthread_mutex_unlock(&video->mutex);
    }
    av_frame_free(&frame);
    pthread_mutex_lock(&video->mutex);
    video->finished = true;
    pthread_cond_broadcast(&video->changed);
    pthread_mutex_unlock(&video->mutex);
    return NULL;
}

const struct software_video_layout *software_video_layout(
    const struct software_video *video)
{
    return &video->layout;
}

int software_video_next(struct software_video *video, unsigned *index,
                        int64_t *pts_us, int timeout_ms, char *error,
                        size_t error_size)
{
    pthread_mutex_lock(&video->mutex);
    if (!video->thread_started) {
        /* Started on first use, after the renderer imported the buffers. */
        if (pthread_create(&video->thread, NULL, decode_worker, video) != 0) {
            pthread_mutex_unlock(&video->mutex);
            set_error(error, error_size, "start decoder thread failed");
            return -1;
        }
        video->thread_started = true;
    }
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += (long)timeout_ms * 1000000L;
    deadline.tv_sec += deadline.tv_nsec / 1000000000L;
    deadline.tv_nsec %= 1000000000L;
    while (!video->ready_count && !video->finished && !video->failed) {
        if (pthread_cond_timedwait(&video->changed, &video->mutex,
                                   &deadline) == ETIMEDOUT)
            break;
    }
    int result;
    if (video->ready_count) {
        struct ready_frame ready = video->ready[video->ready_first];
        video->ready_first = (video->ready_first + 1U) %
                             SOFTWARE_VIDEO_BUFFERS;
        video->ready_count--;
        video->states[ready.index] = BUFFER_TAKEN;
        *index = ready.index;
        *pts_us = ready.pts_us;
        result = 1;
    } else if (video->failed) {
        set_error(error, error_size, "%s", video->failure);
        result = -1;
    } else if (video->finished) {
        set_error(error, error_size, "%s", "");
        result = -1;
    } else {
        result = 0;
    }
    pthread_mutex_unlock(&video->mutex);
    return result;
}

void software_video_release(struct software_video *video, unsigned index)
{
    if (index >= video->layout.count)
        return;
    pthread_mutex_lock(&video->mutex);
    video->states[index] = BUFFER_FREE;
    pthread_cond_broadcast(&video->changed);
    pthread_mutex_unlock(&video->mutex);
}

uint64_t software_video_preroll_drops(struct software_video *video)
{
    pthread_mutex_lock(&video->mutex);
    uint64_t drops = video->preroll_drops;
    pthread_mutex_unlock(&video->mutex);
    return drops;
}

void software_video_close(struct software_video **pointer)
{
    if (!pointer || !*pointer)
        return;
    struct software_video *video = *pointer;
    if (video->thread_started) {
        pthread_mutex_lock(&video->mutex);
        video->stopping = true;
        pthread_cond_broadcast(&video->changed);
        pthread_mutex_unlock(&video->mutex);
        pthread_join(video->thread, NULL);
    }
    avcodec_free_context(&video->codec);
    for (unsigned i = 0; i < SOFTWARE_VIDEO_BUFFERS; ++i) {
        if (video->mappings[i])
            munmap(video->mappings[i], (size_t)video->layout.size);
        if (video->layout.fds[i] >= 0)
            close(video->layout.fds[i]);
        if (video->memfds[i] >= 0)
            close(video->memfds[i]);
    }
    pthread_cond_destroy(&video->changed);
    pthread_mutex_destroy(&video->mutex);
    free(video);
    *pointer = NULL;
}
