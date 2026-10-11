#ifndef STEREONAUT_VULKAN_VIDEO_H
#define STEREONAUT_VULKAN_VIDEO_H

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

/* A decode candidate is deliberately stricter than a codec extension alone:
 * Vulkan Video needs the queue and decode extensions and an actual decode
 * queue family.  Codec/session creation additionally validates the stream's
 * profile and output format before it selects this backend. */
struct vulkan_video_support {
    bool queue;
    bool h264;
    bool hevc;
    bool av1;
    uint32_t queue_family;
};

struct vulkan_video_support vulkan_video_probe(VkPhysicalDevice physical);

/* Append the extensions for all advertised codecs. `names` may be NULL to
 * obtain the required number first; otherwise `capacity` must be sufficient. */
uint32_t vulkan_video_extensions(const struct vulkan_video_support *support,
                                 const char **names, uint32_t capacity);

#endif
