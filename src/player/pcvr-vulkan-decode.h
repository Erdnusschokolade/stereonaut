#ifndef STEREONAUT_PCVR_VULKAN_DECODE_H
#define STEREONAUT_PCVR_VULKAN_DECODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>

/* FFmpeg's Vulkan hwaccel is used rather than a second hand-written H.264
 * or HEVC parser.  The caller supplies the OpenXR renderer's Vulkan handles;
 * decoded AV_PIX_FMT_VULKAN images consequently live on the device from
 * which the projection shader samples them. */
struct pcvr_vulkan_decode_device {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    uint32_t graphics_queue_family;
    uint32_t video_queue_family;
    const char *const *enabled_device_extensions;
    uint32_t enabled_device_extension_count;
};

struct pcvr_vulkan_decoder;

int pcvr_vulkan_decoder_open(struct pcvr_vulkan_decoder **result,
                             const struct pcvr_vulkan_decode_device *device,
                             enum AVCodecID codec, int width, int height,
                             char *error, size_t error_size);
int pcvr_vulkan_decoder_send(struct pcvr_vulkan_decoder *decoder,
                             AVPacket *packet, char *error,
                             size_t error_size);
/* 1: a decoded AV_PIX_FMT_VULKAN frame, 0: no frame ready, -1: failure. */
int pcvr_vulkan_decoder_receive(struct pcvr_vulkan_decoder *decoder,
                                AVFrame *frame, char *error,
                                size_t error_size);
AVBufferRef *pcvr_vulkan_decoder_frames_ref(
    const struct pcvr_vulkan_decoder *decoder);
void pcvr_vulkan_decoder_flush(struct pcvr_vulkan_decoder *decoder);
void pcvr_vulkan_decoder_close(struct pcvr_vulkan_decoder **decoder);

#endif
