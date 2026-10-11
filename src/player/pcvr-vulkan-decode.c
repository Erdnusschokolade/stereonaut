#include "pcvr-vulkan-decode.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/version.h>

struct pcvr_vulkan_decoder {
    AVBufferRef *device;
    AVCodecContext *codec;
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

static enum AVPixelFormat select_vulkan_format(AVCodecContext *context,
                                                const enum AVPixelFormat *formats)
{
    (void)context;
    for (const enum AVPixelFormat *format = formats;
         *format != AV_PIX_FMT_NONE; ++format)
        if (*format == AV_PIX_FMT_VULKAN)
            return *format;
    return AV_PIX_FMT_NONE;
}

int pcvr_vulkan_decoder_open(struct pcvr_vulkan_decoder **result,
                             const struct pcvr_vulkan_decode_device *source,
                             enum AVCodecID codec_id, int width, int height,
                             char *error, size_t error_size)
{
    *result = NULL;
    if (!source || !source->instance || !source->physical ||
        !source->device || width <= 0 || height <= 0 ||
        (codec_id != AV_CODEC_ID_H264 && codec_id != AV_CODEC_ID_HEVC &&
         codec_id != AV_CODEC_ID_AV1)) {
        set_error(error, error_size, "invalid Vulkan Video decoder setup");
        return -1;
    }
    const AVCodec *codec = avcodec_find_decoder(codec_id);
    if (!codec) {
        set_error(error, error_size, "FFmpeg has no requested video decoder");
        return -1;
    }
    struct pcvr_vulkan_decoder *decoder = calloc(1, sizeof(*decoder));
    if (!decoder) {
        set_error(error, error_size, "allocate Vulkan Video decoder");
        return -1;
    }
    decoder->device = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
    if (!decoder->device) {
        set_error(error, error_size, "allocate FFmpeg Vulkan device");
        pcvr_vulkan_decoder_close(&decoder);
        return -1;
    }
    AVHWDeviceContext *context = (AVHWDeviceContext *)decoder->device->data;
    AVVulkanDeviceContext *vulkan = context->hwctx;
    vulkan->get_proc_addr = vkGetInstanceProcAddr;
    vulkan->inst = source->instance;
    vulkan->phys_dev = source->physical;
    vulkan->act_dev = source->device;
    vulkan->enabled_dev_extensions = source->enabled_device_extensions;
    vulkan->nb_enabled_dev_extensions =
        (int)source->enabled_device_extension_count;
#if LIBAVUTIL_VERSION_MAJOR >= 61
    vulkan->nb_qf = 1;
    vulkan->qf[0] = (AVVulkanDeviceQueueFamily){
        .idx = (int)source->graphics_queue_family,
        .num = 1,
        .flags = VK_QUEUE_GRAPHICS_BIT,
    };
    if (source->video_queue_family != source->graphics_queue_family) {
        vulkan->qf[vulkan->nb_qf++] = (AVVulkanDeviceQueueFamily){
            .idx = (int)source->video_queue_family,
            .num = 1,
            .flags = VK_QUEUE_VIDEO_DECODE_BIT_KHR,
            .video_caps = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR |
                          VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR,
        };
    } else {
        vulkan->qf[0].flags |= VK_QUEUE_VIDEO_DECODE_BIT_KHR;
        vulkan->qf[0].video_caps =
            VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR |
            VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
    }
#else
    vulkan->queue_family_index = (int)source->graphics_queue_family;
    vulkan->nb_graphics_queues = 1;
    vulkan->queue_family_tx_index = (int)source->graphics_queue_family;
    vulkan->nb_tx_queues = 1;
    vulkan->queue_family_comp_index = (int)source->graphics_queue_family;
    vulkan->nb_comp_queues = 1;
    vulkan->queue_family_decode_index = (int)source->video_queue_family;
    vulkan->nb_decode_queues = 1;
    vulkan->queue_family_encode_index = -1;
#endif
    int av_result = av_hwdevice_ctx_init(decoder->device);
    if (av_result < 0) {
        set_av_error(error, error_size, "initialize FFmpeg Vulkan device",
                     av_result);
        pcvr_vulkan_decoder_close(&decoder);
        return -1;
    }
    decoder->codec = avcodec_alloc_context3(codec);
    av_result = decoder->codec ? 0 : AVERROR(ENOMEM);
    if (av_result >= 0) {
        decoder->codec->width = width;
        decoder->codec->height = height;
        decoder->codec->pkt_timebase = (AVRational){1, 1000000};
        decoder->codec->get_format = select_vulkan_format;
        decoder->codec->hw_device_ctx = av_buffer_ref(decoder->device);
        av_result = decoder->codec->hw_device_ctx ? 0 : AVERROR(ENOMEM);
    }
    if (av_result >= 0)
        av_result = avcodec_open2(decoder->codec, codec, NULL);
    if (av_result < 0) {
        set_av_error(error, error_size, "open FFmpeg Vulkan decoder",
                     av_result);
        pcvr_vulkan_decoder_close(&decoder);
        return -1;
    }
    *result = decoder;
    return 0;
}

int pcvr_vulkan_decoder_send(struct pcvr_vulkan_decoder *decoder,
                             AVPacket *packet, char *error,
                             size_t error_size)
{
    if (!decoder || !decoder->codec) {
        set_error(error, error_size, "Vulkan Video decoder is not open");
        return -1;
    }
    int result = avcodec_send_packet(decoder->codec, packet);
    if (result == AVERROR(EAGAIN))
        return 0;
    if (result < 0 && result != AVERROR_EOF) {
        set_av_error(error, error_size, "send Vulkan Video packet", result);
        return -1;
    }
    return 1;
}

int pcvr_vulkan_decoder_receive(struct pcvr_vulkan_decoder *decoder,
                                AVFrame *frame, char *error,
                                size_t error_size)
{
    if (!decoder || !decoder->codec || !frame) {
        set_error(error, error_size, "Vulkan Video decoder is not open");
        return -1;
    }
    int result = avcodec_receive_frame(decoder->codec, frame);
    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
        return 0;
    if (result < 0) {
        set_av_error(error, error_size, "receive Vulkan Video frame", result);
        return -1;
    }
    if (frame->format != AV_PIX_FMT_VULKAN) {
        set_error(error, error_size, "FFmpeg returned non-Vulkan frame %d",
                  frame->format);
        av_frame_unref(frame);
        return -1;
    }
    return 1;
}

AVBufferRef *pcvr_vulkan_decoder_frames_ref(
    const struct pcvr_vulkan_decoder *decoder)
{
    if (!decoder || !decoder->codec || !decoder->codec->hw_frames_ctx)
        return NULL;
    return av_buffer_ref(decoder->codec->hw_frames_ctx);
}

void pcvr_vulkan_decoder_flush(struct pcvr_vulkan_decoder *decoder)
{
    if (decoder && decoder->codec)
        avcodec_flush_buffers(decoder->codec);
}

void pcvr_vulkan_decoder_close(struct pcvr_vulkan_decoder **pointer)
{
    if (!pointer || !*pointer)
        return;
    struct pcvr_vulkan_decoder *decoder = *pointer;
    avcodec_free_context(&decoder->codec);
    /* FFmpeg's Vulkan context destructor owns and destroys act_dev even when
     * the caller supplied it.  The OpenXR host owns this device, so its thin
     * FFmpeg wrapper deliberately lives until process exit.  It is tiny and
     * title changes retain it rather than risking destruction of the active
     * SteamVR/OpenXR device. */
    free(decoder);
    *pointer = NULL;
}
