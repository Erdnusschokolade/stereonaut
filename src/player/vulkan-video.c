#include "vulkan-video.h"

#include <stdlib.h>
#include <string.h>

/* Use literal names so the host still builds with Vulkan headers predating
 * Vulkan Video.  The queue flag itself is conditionally compiled below. */
#define VIDEO_QUEUE_EXTENSION "VK_KHR_video_queue"
#define VIDEO_DECODE_EXTENSION "VK_KHR_video_decode_queue"
#define VIDEO_H264_EXTENSION "VK_KHR_video_decode_h264"
#define VIDEO_HEVC_EXTENSION "VK_KHR_video_decode_h265"
#define VIDEO_AV1_EXTENSION "VK_KHR_video_decode_av1"

static bool has_extension(VkPhysicalDevice physical, const char *wanted)
{
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL) !=
        VK_SUCCESS)
        return false;
    VkExtensionProperties *properties = calloc(count ? count : 1U,
                                                sizeof(*properties));
    if (!properties)
        return false;
    bool found = false;
    if (vkEnumerateDeviceExtensionProperties(physical, NULL, &count,
                                             properties) == VK_SUCCESS)
        for (uint32_t i = 0; i < count; ++i)
            if (strcmp(properties[i].extensionName, wanted) == 0) {
                found = true;
                break;
            }
    free(properties);
    return found;
}

#if defined(VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME) || \
    defined(VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME)
/* The renderer samples NV12 directly.  An extension advertised by a driver
 * is not sufficient: the selected codec profile must also accept an optimal
 * tiled NV12 decode destination with sampled-image usage. */
static bool profile_has_sampled_nv12(VkPhysicalDevice physical,
                                     const VkVideoProfileInfoKHR *profile)
{
    VkVideoCapabilitiesKHR capabilities = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR,
    };
    if (vkGetPhysicalDeviceVideoCapabilitiesKHR(physical, profile,
                                                 &capabilities) != VK_SUCCESS ||
        capabilities.maxCodedExtent.width == 0 ||
        capabilities.maxCodedExtent.height == 0)
        return false;
    VkVideoProfileListInfoKHR profiles = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR,
        .profileCount = 1, .pProfiles = profile,
    };
    VkPhysicalDeviceVideoFormatInfoKHR query = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR,
        .pNext = &profiles,
        .imageUsage = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR |
                      VK_IMAGE_USAGE_SAMPLED_BIT,
    };
    uint32_t count = 0;
    if (vkGetPhysicalDeviceVideoFormatPropertiesKHR(physical, &query, &count,
                                                    NULL) != VK_SUCCESS ||
        count == 0)
        return false;
    VkVideoFormatPropertiesKHR *formats = calloc(count, sizeof(*formats));
    if (!formats)
        return false;
    for (uint32_t i = 0; i < count; ++i)
        formats[i].sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
    bool found = vkGetPhysicalDeviceVideoFormatPropertiesKHR(
                     physical, &query, &count, formats) == VK_SUCCESS;
    for (uint32_t i = 0; found && i < count; ++i)
        if (formats[i].format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM &&
            formats[i].imageTiling == VK_IMAGE_TILING_OPTIMAL &&
            (formats[i].imageUsageFlags & query.imageUsage) ==
                query.imageUsage) {
            free(formats);
            return true;
        }
    free(formats);
    return false;
}
#endif

#ifdef VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME
static bool h264_supported(VkPhysicalDevice physical)
{
    VkVideoDecodeH264ProfileInfoKHR codec = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR,
        .stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH,
        .pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR,
    };
    VkVideoProfileInfoKHR profile = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR,
        .pNext = &codec,
        .videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR,
        .chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR,
        .lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
        .chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
    };
    return profile_has_sampled_nv12(physical, &profile);
}
#endif

#ifdef VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME
static bool hevc_supported(VkPhysicalDevice physical)
{
    VkVideoDecodeH265ProfileInfoKHR codec = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR,
        .stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN,
    };
    VkVideoProfileInfoKHR profile = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR,
        .pNext = &codec,
        .videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR,
        .chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR,
        .lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
        .chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
    };
    return profile_has_sampled_nv12(physical, &profile);
}
#endif

struct vulkan_video_support vulkan_video_probe(VkPhysicalDevice physical)
{
    struct vulkan_video_support support = {
        .queue_family = UINT32_MAX,
    };
#ifdef VK_QUEUE_VIDEO_DECODE_BIT_KHR
    if (!has_extension(physical, VIDEO_QUEUE_EXTENSION) ||
        !has_extension(physical, VIDEO_DECODE_EXTENSION))
        return support;
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    VkQueueFamilyProperties *properties = calloc(count ? count : 1U,
                                                  sizeof(*properties));
    if (!properties)
        return support;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, properties);
    for (uint32_t i = 0; i < count; ++i)
        if (properties[i].queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) {
            support.queue = true;
            support.queue_family = i;
            break;
        }
    free(properties);
    if (!support.queue)
        return support;
    support.h264 = has_extension(physical, VIDEO_H264_EXTENSION);
    support.hevc = has_extension(physical, VIDEO_HEVC_EXTENSION);
#ifdef VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME
    if (support.h264)
        support.h264 = h264_supported(physical);
#else
    support.h264 = false;
#endif
#ifdef VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME
    if (support.hevc)
        support.hevc = hevc_supported(physical);
#else
    support.hevc = false;
#endif
    support.av1 = has_extension(physical, VIDEO_AV1_EXTENSION);
#else
    (void)physical;
#endif
    return support;
}

uint32_t vulkan_video_extensions(const struct vulkan_video_support *support,
                                 const char **names, uint32_t capacity)
{
    if (!support || !support->queue)
        return 0;
    const char *all[5] = {VIDEO_QUEUE_EXTENSION, VIDEO_DECODE_EXTENSION,
                          VIDEO_H264_EXTENSION, VIDEO_HEVC_EXTENSION,
                          VIDEO_AV1_EXTENSION};
    bool enabled[5] = {true, true, support->h264, support->hevc,
                       support->av1};
    uint32_t count = 0;
    for (uint32_t i = 0; i < 5; ++i) {
        if (!enabled[i])
            continue;
        if (names && count < capacity)
            names[count] = all[i];
        ++count;
    }
    return count;
}
