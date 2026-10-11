#define _GNU_SOURCE /* memfd_create, F_ADD_SEALS */
#include "decoded-video.h"

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
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>

/* Row pitch, chroma offset and buffer size aligned beyond what Turnip
 * requires for linear images; NVIDIA rounds planes and size to 64 KiB. */
#define STRIDE_ALIGNMENT 256U
#define PLANE_ALIGNMENT 65536U

enum buffer_state {
    BUFFER_FREE,    /* the worker may decode into it */
    BUFFER_READY,   /* decoded, waiting in the ready queue */
    BUFFER_TAKEN,   /* with the session or the renderer */
};

struct ready_frame {
    unsigned index;
    int64_t pts_us;
};

struct decoded_video {
    struct decoded_video_layout layout;
    struct media_pipeline *pipeline;
    enum decoded_video_decoder decoder;
    const char *decoder_name;
    AVCodecContext *codec;
    /* Hardware decoding: the FFmpeg device, its frame format and the
     * frame that describes a buffer as download target. */
    AVBufferRef *hardware_device;
    enum AVPixelFormat hardware_format;
    AVFrame *download;
    int memfds[DECODED_VIDEO_BUFFERS];
    uint8_t *mappings[DECODED_VIDEO_BUFFERS];
    int64_t start_us;

    pthread_t thread;
    bool thread_started;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    enum buffer_state states[DECODED_VIDEO_BUFFERS];
    struct ready_frame ready[DECODED_VIDEO_BUFFERS];
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

/* One frame buffer: a sealed memfd, on the Frame wrapped as a DMA-BUF by
 * udmabuf (limited to 64 MiB per buffer by default, too small for 8K
 * P010), mapped for the worker's writes. */
static int create_buffer(struct decoded_video *video, int device,
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
    if (video->layout.host_memory) {
        video->layout.fds[index] = memfd;
    } else {
        struct udmabuf_create create = {
            .memfd = (uint32_t)memfd,
            .flags = UDMABUF_FLAGS_CLOEXEC,
            .offset = 0,
            .size = size,
        };
        int dma_buf = ioctl(device, UDMABUF_CREATE, &create);
        if (dma_buf < 0) {
            set_error(error, error_size, "UDMABUF_CREATE: %s",
                      strerror(errno));
            return -1;
        }
        video->layout.fds[index] = dma_buf;
    }
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

static int open_dav1d(struct decoded_video *video, char *error,
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
    video->decoder_name = "dav1d";
    return 0;
}

/* No software fallback: a profile the GPU decoder lacks is an error. */
static enum AVPixelFormat pick_hardware_format(
    AVCodecContext *codec, const enum AVPixelFormat *formats)
{
    const struct decoded_video *video = codec->opaque;
    for (; *formats != AV_PIX_FMT_NONE; ++formats)
        if (*formats == video->hardware_format)
            return *formats;
    return AV_PIX_FMT_NONE;
}

/* CUDA (NVDEC) where it exists, i.e. on NVIDIA: both it and Vulkan Video
 * decode 8K 10-bit HEVC at about 145 fps on an RTX 4080, but FFmpeg
 * downloads CUDA frames faster (87 against 60 fps for 100 MB P010 frames,
 * too slow for 8K60 in the player). Vulkan Video covers AMD and Intel with
 * one API; VA-API remains for drivers without it. */
static const struct {
    enum AVHWDeviceType type;
    const char *name;
} hardware_types[] = {
    {AV_HWDEVICE_TYPE_CUDA, "cuda"},
    {AV_HWDEVICE_TYPE_VULKAN, "vulkan"},
    {AV_HWDEVICE_TYPE_VAAPI, "vaapi"},
};

static int open_hardware(struct decoded_video *video, char *error,
                         size_t error_size)
{
    AVCodecParameters *parameters = avcodec_parameters_alloc();
    if (!parameters ||
        media_pipeline_copy_video_parameters(video->pipeline,
                                             parameters) < 0) {
        avcodec_parameters_free(&parameters);
        set_error(error, error_size, "copy video parameters failed");
        return -1;
    }
    /* FFmpeg's native decoders carry the hardware accelerators; for AV1
     * avcodec_find_decoder may return libdav1d instead. */
    const AVCodec *decoder = parameters->codec_id == AV_CODEC_ID_AV1
                                 ? avcodec_find_decoder_by_name("av1")
                                 : avcodec_find_decoder(parameters->codec_id);
    const char *wanted = getenv("STEREONAUT_HWDEC");
    char tried[256] = "";
    for (size_t t = 0; decoder && t < sizeof(hardware_types) /
                                          sizeof(hardware_types[0]); ++t) {
        if (wanted && strcmp(wanted, hardware_types[t].name) != 0)
            continue;
        enum AVPixelFormat format = AV_PIX_FMT_NONE;
        for (int i = 0;; ++i) {
            const AVCodecHWConfig *config = avcodec_get_hw_config(decoder, i);
            if (!config)
                break;
            if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
                config->device_type == hardware_types[t].type) {
                format = config->pix_fmt;
                break;
            }
        }
        if (format == AV_PIX_FMT_NONE)
            continue;
        AVBufferRef *device = NULL;
        int result = av_hwdevice_ctx_create(&device, hardware_types[t].type,
                                            NULL, NULL, 0);
        if (result >= 0) {
            video->codec = avcodec_alloc_context3(decoder);
            result = video->codec ? avcodec_parameters_to_context(
                                        video->codec, parameters)
                                  : AVERROR(ENOMEM);
        }
        if (result >= 0) {
            video->hardware_format = format;
            video->codec->opaque = video;
            video->codec->get_format = pick_hardware_format;
            video->codec->pkt_timebase = (AVRational){1, 1000000};
            video->codec->hw_device_ctx = av_buffer_ref(device);
            result = video->codec->hw_device_ctx
                         ? avcodec_open2(video->codec, decoder, NULL)
                         : AVERROR(ENOMEM);
        }
        if (result >= 0) {
            video->hardware_device = device;
            video->decoder_name = hardware_types[t].name;
            avcodec_parameters_free(&parameters);
            return 0;
        }
        avcodec_free_context(&video->codec);
        av_buffer_unref(&device);
        char detail[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(result, detail, sizeof(detail));
        size_t used = strlen(tried);
        snprintf(tried + used, sizeof(tried) - used, "%s%s: %s",
                 used ? ", " : "", hardware_types[t].name, detail);
    }
    set_error(error, error_size, "no hardware decoder for %s (%s)",
              avcodec_get_name(parameters->codec_id),
              tried[0] ? tried : "none offered");
    avcodec_parameters_free(&parameters);
    return -1;
}

int decoded_video_open(struct decoded_video **result,
                       struct media_pipeline *pipeline, int64_t start_us,
                       enum decoded_video_decoder decoder, char *error,
                       size_t error_size)
{
    *result = NULL;
    const struct media_stream_info *info = media_pipeline_info(pipeline);
    if (!info || info->width <= 0 || info->height <= 0 ||
        (info->width & 1) || (info->height & 1) ||
        (decoder == DECODED_VIDEO_DAV1D &&
         (info->width > AV1_SOFTWARE_MAX_WIDTH ||
          info->height > AV1_SOFTWARE_MAX_HEIGHT))) {
        set_error(error, error_size, "video size outside the decoder limit");
        return -1;
    }
    struct decoded_video *video = calloc(1, sizeof(*video));
    if (!video) {
        set_error(error, error_size, "allocate video decoder");
        return -1;
    }
    for (unsigned i = 0; i < DECODED_VIDEO_BUFFERS; ++i) {
        video->memfds[i] = -1;
        video->layout.fds[i] = -1;
    }
    pthread_mutex_init(&video->mutex, NULL);
    pthread_cond_init(&video->changed, NULL);
    video->pipeline = pipeline;
    video->start_us = start_us;
    video->decoder = decoder;
    struct decoded_video_layout *layout = &video->layout;
    layout->width = (uint32_t)info->width;
    layout->height = (uint32_t)info->height;
    layout->p010 = decoder == DECODED_VIDEO_HARDWARE &&
                   info->pixel_format == AV_PIX_FMT_YUV420P10LE;
    layout->stride = (uint32_t)align_up(
        (uint64_t)layout->width * (layout->p010 ? 2U : 1U), STRIDE_ALIGNMENT);
    layout->uv_offset = align_up((uint64_t)layout->stride * layout->height,
                                 PLANE_ALIGNMENT);
    layout->size = align_up(layout->uv_offset + (uint64_t)layout->stride *
                                                    layout->height / 2U,
                            PLANE_ALIGNMENT);
    layout->count = DECODED_VIDEO_BUFFERS;
    layout->host_memory = decoder == DECODED_VIDEO_HARDWARE;

    int device = layout->host_memory
                     ? -1 : open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (device < 0 && !layout->host_memory) {
        set_error(error, error_size, "open /dev/udmabuf: %s",
                  strerror(errno));
        decoded_video_close(&video);
        return -1;
    }
    for (unsigned i = 0; i < layout->count; ++i) {
        if (create_buffer(video, device, i, error, error_size) < 0) {
            if (device >= 0)
                close(device);
            decoded_video_close(&video);
            return -1;
        }
    }
    if (device >= 0)
        close(device);
    video->download = av_frame_alloc();
    if (!video->download ||
        (decoder == DECODED_VIDEO_DAV1D
             ? open_dav1d(video, error, error_size)
             : open_hardware(video, error, error_size)) < 0) {
        decoded_video_close(&video);
        return -1;
    }
    *result = video;
    return 0;
}

static void fail(struct decoded_video *video, const char *detail)
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
static void copy_frame(struct decoded_video *video, unsigned index,
                       const AVFrame *frame)
{
    const struct decoded_video_layout *layout = &video->layout;
    uint8_t *base = video->mappings[index];
    struct dma_buf_sync sync = {
        .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE,
    };
    if (!layout->host_memory)
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
    if (!layout->host_memory)
        ioctl(layout->fds[index], DMA_BUF_IOCTL_SYNC, &sync);
}

static void keep_buffer(void *opaque, uint8_t *data)
{
    (void)opaque;
    (void)data;
}

/* A hardware frame downloaded straight into the buffer (one copy from GPU
 * to system memory); the renderer samples it from there. */
static int download_frame(struct decoded_video *video, unsigned index,
                          const AVFrame *frame, char *error,
                          size_t error_size)
{
    const struct decoded_video_layout *layout = &video->layout;
    enum AVPixelFormat format = layout->p010 ? AV_PIX_FMT_P010LE
                                             : AV_PIX_FMT_NV12;
    const AVHWFramesContext *frames =
        frame->hw_frames_ctx
            ? (const AVHWFramesContext *)frame->hw_frames_ctx->data
            : NULL;
    if (!frames || frames->sw_format != format) {
        set_error(error, error_size, "decoder frames are %s, expected %s",
                  frames ? av_get_pix_fmt_name(frames->sw_format) : "none",
                  av_get_pix_fmt_name(format));
        return -1;
    }
    uint8_t *base = video->mappings[index];
    AVFrame *target = video->download;
    target->format = format;
    target->width = frame->width;
    target->height = frame->height;
    target->data[0] = base;
    target->data[1] = base + layout->uv_offset;
    target->linesize[0] = target->linesize[1] = (int)layout->stride;
    target->buf[0] = av_buffer_create(base, (size_t)layout->size,
                                      keep_buffer, NULL, 0);
    if (!target->buf[0]) {
        set_error(error, error_size, "wrap frame buffer");
        return -1;
    }
    struct dma_buf_sync sync = {
        .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE,
    };
    if (!layout->host_memory)
        ioctl(layout->fds[index], DMA_BUF_IOCTL_SYNC, &sync);
    int result = av_hwframe_transfer_data(target, frame, 0);
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
    if (!layout->host_memory)
        ioctl(layout->fds[index], DMA_BUF_IOCTL_SYNC, &sync);
    av_frame_unref(target);
    if (result < 0) {
        set_av_error(error, error_size, "download decoded frame", result);
        return -1;
    }
    return 0;
}

static int free_buffer(const struct decoded_video *video)
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
    struct decoded_video *video = argument;
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
                set_av_error(error, sizeof(error), "decode packet", result);
                fail(video, error);
                break;
            }
            continue;
        }
        if (result == AVERROR_EOF)
            break;
        if (result < 0) {
            set_av_error(error, sizeof(error), "decode frame", result);
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
        bool hardware = video->decoder == DECODED_VIDEO_HARDWARE;
        if ((hardware ? frame->format != video->hardware_format
                      : frame->format != AV_PIX_FMT_YUV420P &&
                            frame->format != AV_PIX_FMT_YUVJ420P) ||
            (uint32_t)frame->width != video->layout.width ||
            (uint32_t)frame->height != video->layout.height) {
            snprintf(error, sizeof(error),
                     "frame %dx%d format %d differs from the stream",
                     frame->width, frame->height, frame->format);
            fail(video, error);
            break;
        }
        if (hardware) {
            if (download_frame(video, (unsigned)index, frame, error,
                               sizeof(error)) < 0) {
                fail(video, error);
                break;
            }
        } else {
            copy_frame(video, (unsigned)index, frame);
        }
        av_frame_unref(frame);
        pthread_mutex_lock(&video->mutex);
        video->states[index] = BUFFER_READY;
        unsigned slot = (video->ready_first + video->ready_count) %
                        DECODED_VIDEO_BUFFERS;
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

const char *decoded_video_decoder_name(const struct decoded_video *video)
{
    return video->decoder_name;
}

const struct decoded_video_layout *decoded_video_layout(
    const struct decoded_video *video)
{
    return &video->layout;
}

int decoded_video_next(struct decoded_video *video, unsigned *index,
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
                             DECODED_VIDEO_BUFFERS;
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

void decoded_video_release(struct decoded_video *video, unsigned index)
{
    if (index >= video->layout.count)
        return;
    pthread_mutex_lock(&video->mutex);
    video->states[index] = BUFFER_FREE;
    pthread_cond_broadcast(&video->changed);
    pthread_mutex_unlock(&video->mutex);
}

void decoded_video_stop(struct decoded_video *video)
{
    pthread_mutex_lock(&video->mutex);
    video->stopping = true;
    pthread_cond_broadcast(&video->changed);
    pthread_mutex_unlock(&video->mutex);
    if (video->thread_started)
        pthread_join(video->thread, NULL);
    video->thread_started = false;
    avcodec_flush_buffers(video->codec);
    pthread_mutex_lock(&video->mutex);
    for (unsigned i = 0; i < video->layout.count; ++i)
        if (video->states[i] == BUFFER_READY)
            video->states[i] = BUFFER_FREE;
    video->ready_first = video->ready_count = 0;
    pthread_mutex_unlock(&video->mutex);
}

void decoded_video_restart(struct decoded_video *video, int64_t start_us)
{
    pthread_mutex_lock(&video->mutex);
    video->start_us = start_us;
    video->stopping = video->finished = video->failed = false;
    video->failure[0] = '\0';
    pthread_mutex_unlock(&video->mutex);
}

uint64_t decoded_video_preroll_drops(struct decoded_video *video)
{
    pthread_mutex_lock(&video->mutex);
    uint64_t drops = video->preroll_drops;
    pthread_mutex_unlock(&video->mutex);
    return drops;
}

void decoded_video_close(struct decoded_video **pointer)
{
    if (!pointer || !*pointer)
        return;
    struct decoded_video *video = *pointer;
    if (video->thread_started) {
        pthread_mutex_lock(&video->mutex);
        video->stopping = true;
        pthread_cond_broadcast(&video->changed);
        pthread_mutex_unlock(&video->mutex);
        pthread_join(video->thread, NULL);
    }
    avcodec_free_context(&video->codec);
    av_buffer_unref(&video->hardware_device);
    av_frame_free(&video->download);
    for (unsigned i = 0; i < DECODED_VIDEO_BUFFERS; ++i) {
        if (video->mappings[i])
            munmap(video->mappings[i], (size_t)video->layout.size);
        if (video->layout.fds[i] >= 0 &&
            video->layout.fds[i] != video->memfds[i])
            close(video->layout.fds[i]);
        if (video->memfds[i] >= 0)
            close(video->memfds[i]);
    }
    pthread_cond_destroy(&video->changed);
    pthread_mutex_destroy(&video->mutex);
    free(video);
    *pointer = NULL;
}
