/*
 * One-frame Iris HEVC Main -> NV12 DMA-BUF producer for the OpenXR probe.
 *
 * Input must be an AUD-delimited Annex-B HEVC Main 8-bit elementary stream.
 * The probe paces access units at the supplied constant frame rate, imports
 * every Iris CAPTURE allocation once, executes one two-plane Vulkan transfer
 * for every decoded frame, and requeues CAPTURE only after the Vulkan fence.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/dma-buf.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define MAX_OUTPUT_BUFFERS 16U
#define MAX_CAPTURE_BUFFERS 32U
#define DRM_FORMAT_MOD_LINEAR UINT64_C(0)

struct access_unit {
    size_t offset;
    size_t size;
};

struct input_stream {
    uint8_t *data;
    size_t size;
    struct access_unit *units;
    size_t unit_count;
    size_t largest_unit;
};

struct output_buffer {
    void *mapping;
    size_t length;
    bool free;
};

struct capture_buffer {
    size_t length;
    int dma_buf_fd;
    VkImage image;
    VkDeviceMemory memory;
    VkCommandBuffer command;
};

struct timing_stats {
    uint64_t count;
    double sum_ms;
    double maximum_ms;
};

struct vulkan_context {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    uint32_t timestamp_valid_bits;
    float timestamp_period;
    VkCommandPool command_pool;
    VkSemaphore producer_semaphore;
    VkFence fence;
    VkQueryPool query_pool;
    VkBuffer transfer_buffer;
    VkDeviceMemory transfer_memory;
    VkMemoryPropertyFlags transfer_memory_flags;
    VkDeviceSize transfer_size;
    PFN_vkImportSemaphoreFdKHR import_semaphore_fd;
    PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties;
    PFN_vkGetImageDrmFormatModifierPropertiesEXT get_modifier_properties;
};

struct video_layout {
    uint32_t width;
    uint32_t coded_height;
    uint32_t visible_width;
    uint32_t visible_height;
    uint32_t stride;
    uint32_t sizeimage;
    uint64_t uv_offset;
};

struct capture_contract {
    uint32_t pixel_format;
    uint32_t coded_width;
    uint32_t coded_height;
    uint32_t memory_planes;
    uint32_t bytes_per_line;
    uint32_t sizeimage;
    struct v4l2_rect compose;
};

struct capture_minimum {
    bool available;
    unsigned value;
};

static int video_fd = -1;

static void fail_errno(const char *what)
{
    fprintf(stderr, "%s: errno=%d (%s)\n", what, errno, strerror(errno));
    exit(2);
}

static void fail_message(const char *what)
{
    fprintf(stderr, "%s\n", what);
    exit(2);
}

static void fail_vk(const char *what, VkResult result)
{
    fprintf(stderr, "%s: VkResult=%d\n", what, result);
    exit(2);
}

static int xioctl(unsigned long request, void *argument)
{
    int result;
    do {
        result = ioctl(video_fd, request, argument);
    } while (result < 0 && errno == EINTR);
    return result;
}

static void must_ioctl(unsigned long request, void *argument, const char *what)
{
    if (xioctl(request, argument) < 0)
        fail_errno(what);
}

static int fd_ioctl(int fd, unsigned long request, void *argument)
{
    int result;
    do {
        result = ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);
    return result;
}

static double monotonic_seconds(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) < 0)
        fail_errno("clock_gettime");
    return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
}

static void show_frame_in_openxr(const struct capture_buffer *capture,
                                 const struct video_layout *layout)
{
    int inherited_fd = fcntl(capture->dma_buf_fd, F_DUPFD, 3);
    if (inherited_fd < 0)
        fail_errno("duplicate CAPTURE DMA-BUF for renderer");
    char fd_text[32], length_text[32], width_text[32], height_text[32];
    char stride_text[32], uv_offset_text[32];
    snprintf(fd_text, sizeof(fd_text), "%d", inherited_fd);
    snprintf(length_text, sizeof(length_text), "%zu", capture->length);
    snprintf(width_text, sizeof(width_text), "%u", layout->width);
    snprintf(height_text, sizeof(height_text), "%u", layout->coded_height);
    snprintf(stride_text, sizeof(stride_text), "%u", layout->stride);
    snprintf(uv_offset_text, sizeof(uv_offset_text), "%" PRIu64,
             layout->uv_offset);
    pid_t child = fork();
    if (child < 0)
        fail_errno("fork OpenXR renderer");
    if (child == 0) {
        execl("./xr-nv12-renderer", "xr-nv12-renderer", fd_text,
              length_text, width_text, height_text, stride_text,
              uv_offset_text, (char *)NULL);
        perror("exec xr-nv12-renderer");
        _exit(127);
    }
    if (close(inherited_fd) < 0)
        fail_errno("close parent renderer DMA-BUF duplicate");
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            fail_errno("waitpid OpenXR renderer");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        fail_message("OpenXR renderer did not complete successfully");
    printf("single_frame_renderer=success capture_held_until_renderer_exit=1\n");
}

static void update_stats(struct timing_stats *stats, double milliseconds)
{
    stats->count++;
    stats->sum_ms += milliseconds;
    if (milliseconds > stats->maximum_ms)
        stats->maximum_ms = milliseconds;
}

static uint32_t parse_u32(const char *text, const char *name)
{
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || !end || *end || value == 0 || value > UINT32_MAX) {
        fprintf(stderr, "invalid %s: %s\n", name, text);
        exit(2);
    }
    return (uint32_t)value;
}

static struct input_stream read_input(const char *path)
{
    struct stat status;
    if (stat(path, &status) < 0)
        fail_errno("stat input");
    if (status.st_size <= 0)
        fail_message("input is empty");
    if ((uintmax_t)status.st_size > SIZE_MAX)
        fail_message("input exceeds addressable size");

    FILE *file = fopen(path, "rb");
    if (!file)
        fail_errno("open input");
    struct input_stream stream = {
        .size = (size_t)status.st_size,
    };
    stream.data = malloc(stream.size);
    if (!stream.data)
        fail_errno("allocate input");
    if (fread(stream.data, 1, stream.size, file) != stream.size)
        fail_errno("read input");
    if (fclose(file) != 0)
        fail_errno("close input");

    size_t capacity = 16384;
    size_t *starts = malloc(capacity * sizeof(*starts));
    if (!starts)
        fail_errno("allocate access-unit starts");
    for (size_t i = 0; i + 5 < stream.size;) {
        unsigned prefix = 0;
        if (stream.data[i] == 0 && stream.data[i + 1] == 0 &&
            stream.data[i + 2] == 1) {
            prefix = 3;
        } else if (stream.data[i] == 0 && stream.data[i + 1] == 0 &&
                   stream.data[i + 2] == 0 && stream.data[i + 3] == 1) {
            prefix = 4;
        }
        if (!prefix) {
            i++;
            continue;
        }
        uint8_t nal_type = (stream.data[i + prefix] >> 1) & 63U;
        if (nal_type == 35U) {
            if (stream.unit_count == capacity) {
                if (capacity > SIZE_MAX / 2 / sizeof(*starts))
                    fail_message("too many access units");
                capacity *= 2;
                size_t *grown = realloc(starts, capacity * sizeof(*starts));
                if (!grown)
                    fail_errno("grow access-unit starts");
                starts = grown;
            }
            starts[stream.unit_count++] = i;
        }
        i += prefix + 2;
    }
    if (!stream.unit_count)
        fail_message("input has no AUD-delimited access units");
    starts[0] = 0;
    stream.units = calloc(stream.unit_count, sizeof(*stream.units));
    if (!stream.units)
        fail_errno("allocate access units");
    for (size_t i = 0; i < stream.unit_count; i++) {
        size_t end = i + 1 < stream.unit_count ? starts[i + 1] : stream.size;
        if (end <= starts[i])
            fail_message("invalid access-unit boundaries");
        stream.units[i].offset = starts[i];
        stream.units[i].size = end - starts[i];
        if (stream.units[i].size > stream.largest_unit)
            stream.largest_unit = stream.units[i].size;
    }
    free(starts);
    printf("input_bytes=%zu access_units=%zu largest_access_unit=%zu\n",
           stream.size, stream.unit_count, stream.largest_unit);
    return stream;
}

static bool has_device_extension(VkPhysicalDevice physical, const char *name)
{
    uint32_t count = 0;
    VkResult result = vkEnumerateDeviceExtensionProperties(
        physical, NULL, &count, NULL);
    if (result != VK_SUCCESS)
        fail_vk("vkEnumerateDeviceExtensionProperties(count)", result);
    VkExtensionProperties *properties = calloc(count, sizeof(*properties));
    if (!properties)
        fail_errno("allocate extension properties");
    result = vkEnumerateDeviceExtensionProperties(physical, NULL, &count,
                                                   properties);
    if (result != VK_SUCCESS)
        fail_vk("vkEnumerateDeviceExtensionProperties(list)", result);
    bool found = false;
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(properties[i].extensionName, name) == 0) {
            found = true;
            break;
        }
    }
    free(properties);
    return found;
}

static uint32_t choose_memory_type(VkPhysicalDevice physical,
                                   uint32_t allowed_bits,
                                   VkMemoryPropertyFlags required,
                                   VkMemoryPropertyFlags preferred,
                                   VkMemoryPropertyFlags *chosen_flags)
{
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (unsigned pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < properties.memoryTypeCount; i++) {
            VkMemoryPropertyFlags flags =
                properties.memoryTypes[i].propertyFlags;
            if (!(allowed_bits & (1U << i)) ||
                (flags & required) != required)
                continue;
            if (pass == 0 && (flags & preferred) != preferred)
                continue;
            *chosen_flags = flags;
            return i;
        }
    }
    fail_message("no compatible Vulkan memory type");
    return 0;
}

static void check_nv12_modifier_capability(struct vulkan_context *vk)
{
    VkDrmFormatModifierPropertiesListEXT list = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
    };
    VkFormatProperties2 properties = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &list,
    };
    vkGetPhysicalDeviceFormatProperties2(
        vk->physical, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, &properties);
    if (!list.drmFormatModifierCount)
        fail_message("Turnip reports no NV12 DRM modifiers");
    list.pDrmFormatModifierProperties =
        calloc(list.drmFormatModifierCount,
               sizeof(*list.pDrmFormatModifierProperties));
    if (!list.pDrmFormatModifierProperties)
        fail_errno("allocate modifier properties");
    uint32_t capacity = list.drmFormatModifierCount;
    vkGetPhysicalDeviceFormatProperties2(
        vk->physical, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, &properties);
    if (list.drmFormatModifierCount > capacity)
        fail_message("NV12 modifier list grew unexpectedly");
    bool linear = false;
    for (uint32_t i = 0; i < list.drmFormatModifierCount; i++) {
        VkDrmFormatModifierPropertiesEXT *item =
            &list.pDrmFormatModifierProperties[i];
        if (item->drmFormatModifier == DRM_FORMAT_MOD_LINEAR &&
            item->drmFormatModifierPlaneCount == 2 &&
            (item->drmFormatModifierTilingFeatures &
             (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
              VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)) ==
                (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                 VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)) {
            linear = true;
        }
    }
    free(list.pDrmFormatModifierProperties);
    if (!linear)
        fail_message("linear NV12 lacks two memory planes or required usage");

    VkPhysicalDeviceExternalImageFormatInfo external = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier = {
        .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .pNext = &external,
        .drmFormatModifier = DRM_FORMAT_MOD_LINEAR,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkPhysicalDeviceImageFormatInfo2 format = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &modifier,
        .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
    };
    VkExternalImageFormatProperties external_properties = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
    };
    VkImageFormatProperties2 image_properties = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        .pNext = &external_properties,
    };
    VkResult result = vkGetPhysicalDeviceImageFormatProperties2(
        vk->physical, &format, &image_properties);
    if (result != VK_SUCCESS)
        fail_vk("NV12 linear DMA-BUF capability query", result);
    VkExternalMemoryFeatureFlags features =
        external_properties.externalMemoryProperties.externalMemoryFeatures;
    if (!(features & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
        fail_message("linear NV12 DMA-BUF is not importable");
    printf("vulkan_preflight nv12_linear=1 memory_planes=2"
           " sampled=1 transfer_src=1 external_features=0x%x\n",
           features);
}

static void initialize_vulkan(struct vulkan_context *vk)
{
    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "stereonaut-realvideo-001",
        .applicationVersion = 1,
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
    };
    VkResult result = vkCreateInstance(&instance_info, NULL, &vk->instance);
    if (result != VK_SUCCESS)
        fail_vk("vkCreateInstance", result);

    uint32_t physical_count = 0;
    result = vkEnumeratePhysicalDevices(vk->instance, &physical_count, NULL);
    if (result != VK_SUCCESS || physical_count == 0)
        fail_vk("vkEnumeratePhysicalDevices(count)", result);
    VkPhysicalDevice *physicals = calloc(physical_count, sizeof(*physicals));
    if (!physicals)
        fail_errno("allocate physical devices");
    result = vkEnumeratePhysicalDevices(vk->instance, &physical_count,
                                         physicals);
    if (result != VK_SUCCESS)
        fail_vk("vkEnumeratePhysicalDevices(list)", result);
    vk->physical = physicals[0];
    free(physicals);

    VkPhysicalDeviceProperties physical_properties;
    vkGetPhysicalDeviceProperties(vk->physical, &physical_properties);
    if (physical_properties.vendorID != 0x5143)
        fail_message("unexpected Vulkan vendor; refusing non-Turnip path");
    vk->timestamp_period = physical_properties.limits.timestampPeriod;
    printf("vulkan_device=%s vendor=0x%x device=0x%x timestamp_period_ns=%f\n",
           physical_properties.deviceName, physical_properties.vendorID,
           physical_properties.deviceID, vk->timestamp_period);

    const char *extensions[] = {
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
    };
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
        if (!has_device_extension(vk->physical, extensions[i])) {
            fprintf(stderr, "required Vulkan extension missing: %s\n",
                    extensions[i]);
            exit(2);
        }
    }

    check_nv12_modifier_capability(vk);

    VkPhysicalDeviceExternalSemaphoreInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    VkExternalSemaphoreProperties semaphore_properties = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,
    };
    vkGetPhysicalDeviceExternalSemaphoreProperties(
        vk->physical, &semaphore_info, &semaphore_properties);
    if (!(semaphore_properties.externalSemaphoreFeatures &
          VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) ||
        !(semaphore_properties.compatibleHandleTypes &
          VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT))
        fail_message("SYNC_FD semaphore import is unsupported");
    printf("sync_fd_preflight importable=1 features=0x%x\n",
           semaphore_properties.externalSemaphoreFeatures);

    uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vk->physical, &queue_count, NULL);
    VkQueueFamilyProperties *queues = calloc(queue_count, sizeof(*queues));
    if (!queues)
        fail_errno("allocate queue properties");
    vkGetPhysicalDeviceQueueFamilyProperties(vk->physical, &queue_count,
                                              queues);
    bool queue_found = false;
    for (uint32_t i = 0; i < queue_count; i++) {
        if ((queues[i].queueFlags & VK_QUEUE_TRANSFER_BIT) &&
            queues[i].timestampValidBits) {
            vk->queue_family = i;
            vk->timestamp_valid_bits = queues[i].timestampValidBits;
            queue_found = true;
            break;
        }
    }
    free(queues);
    if (!queue_found)
        fail_message("no transfer queue with timestamp support");

    float priority = 0.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = vk->queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount =
            (uint32_t)(sizeof(extensions) / sizeof(extensions[0])),
        .ppEnabledExtensionNames = extensions,
    };
    result = vkCreateDevice(vk->physical, &device_info, NULL, &vk->device);
    if (result != VK_SUCCESS)
        fail_vk("vkCreateDevice", result);
    vkGetDeviceQueue(vk->device, vk->queue_family, 0, &vk->queue);
    if (vk->queue == VK_NULL_HANDLE)
        fail_message("vkGetDeviceQueue returned null");

    vk->import_semaphore_fd =
        (PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(
            vk->device, "vkImportSemaphoreFdKHR");
    vk->get_memory_fd_properties =
        (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(
            vk->device, "vkGetMemoryFdPropertiesKHR");
    vk->get_modifier_properties =
        (PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkGetDeviceProcAddr(
            vk->device, "vkGetImageDrmFormatModifierPropertiesEXT");
    if (!vk->import_semaphore_fd || !vk->get_memory_fd_properties ||
        !vk->get_modifier_properties)
        fail_message("required Vulkan external-memory entry point missing");

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = vk->queue_family,
    };
    result = vkCreateCommandPool(vk->device, &pool_info, NULL,
                                 &vk->command_pool);
    if (result != VK_SUCCESS)
        fail_vk("vkCreateCommandPool", result);
    VkSemaphoreCreateInfo create_semaphore = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    result = vkCreateSemaphore(vk->device, &create_semaphore, NULL,
                               &vk->producer_semaphore);
    if (result != VK_SUCCESS)
        fail_vk("vkCreateSemaphore", result);
    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    result = vkCreateFence(vk->device, &fence_info, NULL, &vk->fence);
    if (result != VK_SUCCESS)
        fail_vk("vkCreateFence", result);
    VkQueryPoolCreateInfo query_info = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = 2,
    };
    result = vkCreateQueryPool(vk->device, &query_info, NULL, &vk->query_pool);
    if (result != VK_SUCCESS)
        fail_vk("vkCreateQueryPool", result);
    printf("vulkan_preflight=success queue_family=%u timestamp_bits=%u"
           " decoder_stream_not_started_yet\n",
           vk->queue_family, vk->timestamp_valid_bits);
}

static void create_transfer_buffer(struct vulkan_context *vk,
                                   const struct video_layout *layout)
{
    vk->transfer_size = (VkDeviceSize)layout->width * layout->coded_height *
                        3U / 2U;
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = vk->transfer_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkResult result = vkCreateBuffer(vk->device, &buffer_info, NULL,
                                     &vk->transfer_buffer);
    if (result != VK_SUCCESS)
        fail_vk("vkCreateBuffer(transfer)", result);
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(vk->device, vk->transfer_buffer,
                                  &requirements);
    uint32_t memory_type = choose_memory_type(
        vk->physical, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &vk->transfer_memory_flags);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type,
    };
    result = vkAllocateMemory(vk->device, &allocation, NULL,
                              &vk->transfer_memory);
    if (result != VK_SUCCESS)
        fail_vk("vkAllocateMemory(transfer)", result);
    result = vkBindBufferMemory(vk->device, vk->transfer_buffer,
                                vk->transfer_memory, 0);
    if (result != VK_SUCCESS)
        fail_vk("vkBindBufferMemory(transfer)", result);
    printf("transfer_buffer bytes=%" PRIu64 " memory_type=%u flags=0x%x\n",
           (uint64_t)vk->transfer_size, memory_type,
           vk->transfer_memory_flags);
}

static void record_transfer_command(struct vulkan_context *vk,
                                    struct capture_buffer *capture,
                                    const struct video_layout *layout)
{
    VkCommandBufferAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = vk->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkResult result = vkAllocateCommandBuffers(vk->device, &allocation,
                                                &capture->command);
    if (result != VK_SUCCESS)
        fail_vk("vkAllocateCommandBuffers", result);
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT,
    };
    result = vkBeginCommandBuffer(capture->command, &begin);
    if (result != VK_SUCCESS)
        fail_vk("vkBeginCommandBuffer", result);
    vkCmdResetQueryPool(capture->command, vk->query_pool, 0, 2);
    vkCmdWriteTimestamp(capture->command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        vk->query_pool, 0);

    VkImageMemoryBarrier acquire = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
        .dstQueueFamilyIndex = vk->queue_family,
        .image = capture->image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    vkCmdPipelineBarrier(capture->command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         1, &acquire);
    const VkBufferImageCopy copies[2] = {
        {
            .bufferOffset = 0,
            .imageSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .imageExtent = {layout->width, layout->coded_height, 1},
        },
        {
            .bufferOffset =
                (VkDeviceSize)layout->width * layout->coded_height,
            .imageSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .imageExtent = {layout->width / 2, layout->coded_height / 2, 1},
        },
    };
    vkCmdCopyImageToBuffer(capture->command, capture->image,
                           VK_IMAGE_LAYOUT_GENERAL, vk->transfer_buffer, 2,
                           copies);
    VkImageMemoryBarrier release = acquire;
    release.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    release.dstAccessMask = 0;
    release.srcQueueFamilyIndex = vk->queue_family;
    release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    vkCmdPipelineBarrier(capture->command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0,
                         NULL, 1, &release);
    vkCmdWriteTimestamp(capture->command,
                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, vk->query_pool,
                        1);
    result = vkEndCommandBuffer(capture->command);
    if (result != VK_SUCCESS)
        fail_vk("vkEndCommandBuffer", result);
}

static void import_capture_image(struct vulkan_context *vk,
                                 struct capture_buffer *capture,
                                 const struct video_layout *layout,
                                 unsigned index)
{
    const VkSubresourceLayout plane_layouts[2] = {
        {
            .offset = 0,
            .size = (VkDeviceSize)layout->stride * layout->coded_height,
            .rowPitch = layout->stride,
        },
        {
            .offset = layout->uv_offset,
            .size = (VkDeviceSize)layout->stride * layout->coded_height / 2U,
            .rowPitch = layout->stride,
        },
    };
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_info = {
        .sType =
            VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier = DRM_FORMAT_MOD_LINEAR,
        .drmFormatModifierPlaneCount = 2,
        .pPlaneLayouts = plane_layouts,
    };
    VkExternalMemoryImageCreateInfo external_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &modifier_info,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external_info,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        .extent = {layout->width, layout->coded_height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult result = vkCreateImage(vk->device, &image_info, NULL,
                                    &capture->image);
    if (result != VK_SUCCESS)
        fail_vk("vkCreateImage(NV12 DMA-BUF)", result);

    VkImageDrmFormatModifierPropertiesEXT chosen_modifier = {
        .sType =
            VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
    };
    result = vk->get_modifier_properties(vk->device, capture->image,
                                          &chosen_modifier);
    if (result != VK_SUCCESS)
        fail_vk("vkGetImageDrmFormatModifierPropertiesEXT", result);
    if (chosen_modifier.drmFormatModifier != DRM_FORMAT_MOD_LINEAR)
        fail_message("Vulkan returned a non-linear NV12 modifier");

    for (uint32_t plane = 0; plane < 2; plane++) {
        VkImageSubresource subresource = {
            .aspectMask = plane == 0 ? VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT
                                     : VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
        };
        VkSubresourceLayout returned;
        vkGetImageSubresourceLayout(vk->device, capture->image, &subresource,
                                    &returned);
        printf("vulkan_layout index=%u memory_plane=%u offset=%" PRIu64
               " row_pitch=%" PRIu64 " size=%" PRIu64 "\n",
               index, plane, (uint64_t)returned.offset,
               (uint64_t)returned.rowPitch, (uint64_t)returned.size);
        if (returned.offset != plane_layouts[plane].offset ||
            returned.rowPitch != plane_layouts[plane].rowPitch)
            fail_message("Turnip returned a different explicit NV12 layout");
    }

    VkMemoryDedicatedRequirements dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
    };
    VkMemoryRequirements2 requirements = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        .pNext = &dedicated,
    };
    VkImageMemoryRequirementsInfo2 requirements_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = capture->image,
    };
    vkGetImageMemoryRequirements2(vk->device, &requirements_info,
                                  &requirements);
    printf("image_memory index=%u size=%" PRIu64 " alignment=%" PRIu64
           " type_bits=0x%x dedicated_required=%u\n",
           index, (uint64_t)requirements.memoryRequirements.size,
           (uint64_t)requirements.memoryRequirements.alignment,
           requirements.memoryRequirements.memoryTypeBits,
           dedicated.requiresDedicatedAllocation);
    if (requirements.memoryRequirements.size > capture->length)
        fail_message("Vulkan NV12 image requirement exceeds Iris DMA-BUF");

    VkMemoryFdPropertiesKHR fd_properties = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
    };
    result = vk->get_memory_fd_properties(
        vk->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        capture->dma_buf_fd, &fd_properties);
    if (result != VK_SUCCESS)
        fail_vk("vkGetMemoryFdPropertiesKHR", result);
    uint32_t compatible = fd_properties.memoryTypeBits &
                          requirements.memoryRequirements.memoryTypeBits;
    if (!compatible)
        fail_message("DMA-BUF/image memory-type intersection is empty");
    VkMemoryPropertyFlags chosen_flags = 0;
    uint32_t memory_type = choose_memory_type(
        vk->physical, compatible, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        &chosen_flags);

    int imported_fd = dup(capture->dma_buf_fd);
    if (imported_fd < 0)
        fail_errno("dup DMA-BUF for Vulkan import");
    VkImportMemoryFdInfoKHR import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = imported_fd,
    };
    VkMemoryDedicatedAllocateInfo dedicated_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &import_info,
        .image = capture->image,
    };
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated_info,
        .allocationSize = capture->length,
        .memoryTypeIndex = memory_type,
    };
    result = vkAllocateMemory(vk->device, &allocation, NULL,
                              &capture->memory);
    if (result != VK_SUCCESS) {
        close(imported_fd);
        fail_vk("vkAllocateMemory(import NV12 DMA-BUF)", result);
    }
    result = vkBindImageMemory(vk->device, capture->image, capture->memory, 0);
    if (result != VK_SUCCESS)
        fail_vk("vkBindImageMemory(NV12 DMA-BUF)", result);
    printf("dmabuf_import index=%u bytes=%zu fd_type_bits=0x%x"
           " intersection=0x%x memory_type=%u flags=0x%x dedicated=1\n",
           index, capture->length, fd_properties.memoryTypeBits, compatible,
           memory_type, chosen_flags);
    record_transfer_command(vk, capture, layout);
}

static void write_selected_frame(struct vulkan_context *vk,
                                 const char *prefix, uint64_t frame_index)
{
    void *mapped = NULL;
    VkResult result = vkMapMemory(vk->device, vk->transfer_memory, 0,
                                  vk->transfer_size, 0, &mapped);
    if (result != VK_SUCCESS)
        fail_vk("vkMapMemory(selected readback)", result);
    if (!(vk->transfer_memory_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        VkMappedMemoryRange range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = vk->transfer_memory,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
        };
        result = vkInvalidateMappedMemoryRanges(vk->device, 1, &range);
        if (result != VK_SUCCESS)
            fail_vk("vkInvalidateMappedMemoryRanges(selected)", result);
    }
    char path[4096];
    int length = snprintf(path, sizeof(path), "%s-frame-%08" PRIu64 ".nv12",
                          prefix, frame_index);
    if (length < 0 || (size_t)length >= sizeof(path))
        fail_message("selected-frame output path is too long");
    FILE *file = fopen(path, "wb");
    if (!file)
        fail_errno("open selected-frame output");
    if (fwrite(mapped, 1, (size_t)vk->transfer_size, file) !=
        (size_t)vk->transfer_size)
        fail_errno("write selected-frame output");
    if (fclose(file) != 0)
        fail_errno("close selected-frame output");
    vkUnmapMemory(vk->device, vk->transfer_memory);
    printf("selected_readback frame=%" PRIu64 " path=%s bytes=%" PRIu64
           "\n",
           frame_index, path, (uint64_t)vk->transfer_size);
}

static double execute_transfer(struct vulkan_context *vk,
                               struct capture_buffer *capture,
                               bool selected, const char *output_prefix,
                               uint64_t frame_index, double *wall_ms)
{
    struct dma_buf_export_sync_file sync_export = {
        .flags = DMA_BUF_SYNC_READ,
        .fd = -1,
    };
    if (fd_ioctl(capture->dma_buf_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE,
                 &sync_export) < 0)
        fail_errno("DMA_BUF_IOCTL_EXPORT_SYNC_FILE READ");
    VkImportSemaphoreFdInfoKHR semaphore_import = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
        .semaphore = vk->producer_semaphore,
        .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
        .fd = sync_export.fd,
    };
    VkResult result = vk->import_semaphore_fd(vk->device, &semaphore_import);
    if (result != VK_SUCCESS) {
        if (sync_export.fd >= 0)
            close(sync_export.fd);
        fail_vk("vkImportSemaphoreFdKHR(producer SYNC_FD)", result);
    }
    result = vkResetFences(vk->device, 1, &vk->fence);
    if (result != VK_SUCCESS)
        fail_vk("vkResetFences", result);
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &vk->producer_semaphore,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &capture->command,
    };
    double started = monotonic_seconds();
    result = vkQueueSubmit(vk->queue, 1, &submit, vk->fence);
    if (result != VK_SUCCESS)
        fail_vk("vkQueueSubmit(frame transfer)", result);
    result = vkWaitForFences(vk->device, 1, &vk->fence, VK_TRUE,
                             UINT64_C(5000000000));
    double finished = monotonic_seconds();
    if (result != VK_SUCCESS)
        fail_vk("vkWaitForFences(frame transfer)", result);
    *wall_ms = (finished - started) * 1000.0;

    uint64_t timestamps[2] = {0, 0};
    result = vkGetQueryPoolResults(
        vk->device, vk->query_pool, 0, 2, sizeof(timestamps), timestamps,
        sizeof(timestamps[0]), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    if (result != VK_SUCCESS)
        fail_vk("vkGetQueryPoolResults", result);
    uint64_t mask = vk->timestamp_valid_bits == 64
                        ? UINT64_MAX
                        : (UINT64_C(1) << vk->timestamp_valid_bits) - 1U;
    uint64_t ticks = (timestamps[1] - timestamps[0]) & mask;
    double gpu_ms = (double)ticks * vk->timestamp_period / 1000000.0;
    if (selected)
        write_selected_frame(vk, output_prefix, frame_index);
    return gpu_ms;
}

static unsigned allocate_output_buffers(
    struct output_buffer buffers[MAX_OUTPUT_BUFFERS])
{
    struct v4l2_requestbuffers request = {
        .count = 6,
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
        .memory = V4L2_MEMORY_MMAP,
    };
    must_ioctl(VIDIOC_REQBUFS, &request, "VIDIOC_REQBUFS OUTPUT");
    if (!request.count || request.count > MAX_OUTPUT_BUFFERS)
        fail_message("unexpected OUTPUT buffer count");
    for (unsigned i = 0; i < request.count; i++) {
        struct v4l2_plane plane = {0};
        struct v4l2_buffer buffer = {
            .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
            .length = 1,
            .m.planes = &plane,
        };
        must_ioctl(VIDIOC_QUERYBUF, &buffer, "VIDIOC_QUERYBUF OUTPUT");
        if (buffer.length != 1)
            fail_message("OUTPUT is not one memory plane");
        buffers[i].length = plane.length;
        buffers[i].mapping = mmap(NULL, plane.length, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, video_fd, plane.m.mem_offset);
        if (buffers[i].mapping == MAP_FAILED)
            fail_errno("mmap OUTPUT");
        buffers[i].free = true;
    }
    printf("output_buffers=%u\n", request.count);
    return request.count;
}

static void queue_output(unsigned index, struct output_buffer *buffer,
                         const struct input_stream *input, size_t unit_index,
                         uint32_t fps_numerator, uint32_t fps_denominator,
                         double *queued_at)
{
    const struct access_unit *unit = &input->units[unit_index];
    if (unit->size > buffer->length)
        fail_message("access unit exceeds OUTPUT allocation");
    memcpy(buffer->mapping, input->data + unit->offset, unit->size);
    struct v4l2_plane plane = {
        .bytesused = (uint32_t)unit->size,
        .length = (uint32_t)buffer->length,
    };
    struct v4l2_buffer queued = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
        .memory = V4L2_MEMORY_MMAP,
        .index = index,
        .length = 1,
        .m.planes = &plane,
    };
    uint64_t timestamp_us =
        (uint64_t)unit_index * fps_denominator * UINT64_C(1000000) /
        fps_numerator;
    queued.timestamp.tv_sec = (time_t)(timestamp_us / UINT64_C(1000000));
    queued.timestamp.tv_usec =
        (suseconds_t)(timestamp_us % UINT64_C(1000000));
    must_ioctl(VIDIOC_QBUF, &queued, "VIDIOC_QBUF OUTPUT");
    buffer->free = false;
    queued_at[unit_index] = monotonic_seconds();
}

static int export_capture(unsigned index, size_t expected_size)
{
    struct v4l2_exportbuffer exported = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        .index = index,
        .plane = 0,
        .flags = O_CLOEXEC | O_RDONLY,
    };
    must_ioctl(VIDIOC_EXPBUF, &exported, "VIDIOC_EXPBUF CAPTURE");
    struct stat status;
    if (fstat(exported.fd, &status) < 0)
        fail_errno("fstat CAPTURE DMA-BUF");
    if (status.st_size < 0 || (uintmax_t)status.st_size != expected_size)
        fail_message("DMA-BUF size differs from V4L2 sizeimage");
    printf("dmabuf_export index=%u fd=%d size=%jd\n", index, exported.fd,
           (intmax_t)status.st_size);
    return exported.fd;
}

static void queue_capture(unsigned index, size_t length)
{
    struct v4l2_plane plane = {
        .length = (uint32_t)length,
    };
    struct v4l2_buffer buffer = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        .memory = V4L2_MEMORY_MMAP,
        .index = index,
        .length = 1,
        .m.planes = &plane,
    };
    must_ioctl(VIDIOC_QBUF, &buffer, "VIDIOC_QBUF CAPTURE");
}

static void query_capture_contract(struct capture_contract *contract)
{
    struct v4l2_format format = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
    };
    must_ioctl(VIDIOC_G_FMT, &format, "VIDIOC_G_FMT after SOURCE_CHANGE");
    char fourcc[5] = {
        (char)(format.fmt.pix_mp.pixelformat & 0xff),
        (char)((format.fmt.pix_mp.pixelformat >> 8) & 0xff),
        (char)((format.fmt.pix_mp.pixelformat >> 16) & 0xff),
        (char)((format.fmt.pix_mp.pixelformat >> 24) & 0xff),
        0,
    };
    printf("capture_format fourcc=%s width=%u height=%u planes=%u"
           " stride=%u sizeimage=%u colorspace=%u xfer=%u ycbcr=%u quant=%u\n",
           fourcc, format.fmt.pix_mp.width, format.fmt.pix_mp.height,
           format.fmt.pix_mp.num_planes,
           format.fmt.pix_mp.plane_fmt[0].bytesperline,
           format.fmt.pix_mp.plane_fmt[0].sizeimage,
           format.fmt.pix_mp.colorspace, format.fmt.pix_mp.xfer_func,
           format.fmt.pix_mp.ycbcr_enc, format.fmt.pix_mp.quantization);
    struct v4l2_selection selection = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .target = V4L2_SEL_TGT_COMPOSE,
    };
    must_ioctl(VIDIOC_G_SELECTION, &selection, "VIDIOC_G_SELECTION COMPOSE");
    printf("visible_rect left=%d top=%d width=%u height=%u\n",
           selection.r.left, selection.r.top, selection.r.width,
           selection.r.height);
    contract->pixel_format = format.fmt.pix_mp.pixelformat;
    contract->coded_width = format.fmt.pix_mp.width;
    contract->coded_height = format.fmt.pix_mp.height;
    contract->memory_planes = format.fmt.pix_mp.num_planes;
    contract->bytes_per_line = format.fmt.pix_mp.plane_fmt[0].bytesperline;
    contract->sizeimage = format.fmt.pix_mp.plane_fmt[0].sizeimage;
    contract->compose = selection.r;
}

static void validate_capture_contract(const struct capture_contract *contract,
                                      struct video_layout *layout,
                                      uint32_t expected_width,
                                      uint32_t expected_visible_height)
{
    if (contract->pixel_format != V4L2_PIX_FMT_NV12 ||
        contract->memory_planes != 1)
        fail_message("Iris did not negotiate one-memory-plane NV12");
    if (contract->compose.left != 0 || contract->compose.top != 0 ||
        contract->compose.width != expected_width ||
        contract->compose.height != expected_visible_height)
        fail_message("Iris visible rectangle differs from input metadata");

    layout->width = contract->coded_width;
    layout->coded_height = contract->coded_height;
    layout->visible_width = contract->compose.width;
    layout->visible_height = contract->compose.height;
    layout->stride = contract->bytes_per_line;
    layout->sizeimage = contract->sizeimage;
    if (layout->width != expected_width || (layout->width & 1U) ||
        (layout->coded_height & 1U) || layout->stride < layout->width)
        fail_message("unsupported NV12 dimensions or stride");
    layout->uv_offset = (uint64_t)layout->stride * layout->coded_height;
    uint64_t required = layout->uv_offset +
                        (uint64_t)layout->stride * layout->coded_height / 2U;
    if (required != layout->sizeimage)
        fail_message("NV12 sizeimage does not match contiguous linear layout");
    printf("nv12_contract modifier=DRM_FORMAT_MOD_LINEAR memory_planes=2"
           " v4l2_memory_planes=1 y_offset=0 uv_offset=%" PRIu64
           " row_pitch=%u required_bytes=%" PRIu64 "\n",
           layout->uv_offset, layout->stride, required);
}

static struct capture_minimum query_capture_minimum(void)
{
    struct v4l2_control minimum = {
        .id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE,
    };
    if (xioctl(VIDIOC_G_CTRL, &minimum) < 0) {
        int control_errno = errno;
        if (control_errno == EINVAL || control_errno == ENOTTY) {
            printf("min_buffers_control=unavailable errno=%d\n",
                   control_errno);
            return (struct capture_minimum){0};
        }
        fail_errno("VIDIOC_G_CTRL MIN_BUFFERS_FOR_CAPTURE");
    }
    if (minimum.value < 0)
        fail_message("invalid V4L2_CID_MIN_BUFFERS_FOR_CAPTURE value");
    printf("min_buffers_control=available value=%d\n", minimum.value);
    return (struct capture_minimum){
        .available = true,
        .value = (unsigned)minimum.value,
    };
}

static unsigned setup_capture(
    struct vulkan_context *vk,
    struct capture_buffer buffers[MAX_CAPTURE_BUFFERS],
    struct capture_contract *contract, struct video_layout *layout,
    uint32_t expected_width, uint32_t expected_visible_height)
{
    query_capture_contract(contract);
    validate_capture_contract(contract, layout, expected_width,
                              expected_visible_height);

    create_transfer_buffer(vk, layout);
    struct capture_minimum minimum = query_capture_minimum();
    unsigned wanted = 12;
    if (minimum.available) {
        if (minimum.value > MAX_CAPTURE_BUFFERS - 4U)
            fail_message("CAPTURE minimum buffer count exceeds bound");
        wanted = minimum.value + 4U;
    }
    if (wanted > MAX_CAPTURE_BUFFERS)
        fail_message("CAPTURE buffer request exceeds bound");
    struct v4l2_requestbuffers request = {
        .count = wanted,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        .memory = V4L2_MEMORY_MMAP,
    };
    must_ioctl(VIDIOC_REQBUFS, &request, "VIDIOC_REQBUFS CAPTURE");
    if (!request.count || request.count > MAX_CAPTURE_BUFFERS)
        fail_message("unexpected CAPTURE buffer count");
    for (unsigned i = 0; i < request.count; i++) {
        struct v4l2_plane plane = {0};
        struct v4l2_buffer buffer = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
            .length = 1,
            .m.planes = &plane,
        };
        must_ioctl(VIDIOC_QUERYBUF, &buffer, "VIDIOC_QUERYBUF CAPTURE");
        if (buffer.length != 1 || plane.length != layout->sizeimage)
            fail_message("CAPTURE allocation differs from sizeimage");
        buffers[i].length = plane.length;
        printf("capture_allocation index=%u length=%zu\n", i,
               buffers[i].length);
        buffers[i].dma_buf_fd = export_capture(i, plane.length);
        import_capture_image(vk, &buffers[i], layout, i);
        queue_capture(i, plane.length);
    }
    printf("capture_buffers=%u imported_images=%u cpu_mapped=0\n",
           request.count, request.count);
    return request.count;
}

static bool capture_memory_compatible(const struct capture_contract *old,
                                      const struct capture_contract *updated)
{
    return old->pixel_format == updated->pixel_format &&
           old->coded_width == updated->coded_width &&
           old->coded_height == updated->coded_height &&
           old->memory_planes == updated->memory_planes &&
           old->bytes_per_line == updated->bytes_per_line &&
           old->sizeimage == updated->sizeimage;
}

static bool same_compose(const struct v4l2_rect *old,
                         const struct v4l2_rect *updated)
{
    return old->left == updated->left && old->top == updated->top &&
           old->width == updated->width && old->height == updated->height;
}

static void verify_capture_allocations(
    struct capture_buffer buffers[MAX_CAPTURE_BUFFERS],
    unsigned capture_count, uint32_t expected_size)
{
    for (unsigned i = 0; i < capture_count; i++) {
        struct v4l2_plane plane = {0};
        struct v4l2_buffer buffer = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
            .length = 1,
            .m.planes = &plane,
        };
        must_ioctl(VIDIOC_QUERYBUF, &buffer,
                   "VIDIOC_QUERYBUF CAPTURE reuse");
        if (buffer.length != 1 || plane.length != expected_size ||
            buffers[i].length != plane.length)
            fail_message("existing CAPTURE allocation is incompatible");
        printf("capture_reuse_allocation index=%u length=%u\n", i,
               plane.length);
    }
}

static size_t timestamp_to_unit(const struct timeval *timestamp,
                                uint32_t fps_numerator,
                                uint32_t fps_denominator)
{
    uint64_t microseconds = (uint64_t)timestamp->tv_sec * UINT64_C(1000000) +
                            (uint64_t)timestamp->tv_usec;
    uint64_t denominator =
        (uint64_t)fps_denominator * UINT64_C(1000000);
    return (size_t)((microseconds * fps_numerator + denominator / 2U) /
                    denominator);
}

static void release_capture_generation(struct vulkan_context *vk,
                                       struct capture_buffer *captures,
                                       unsigned capture_count)
{
    VkResult result = vkDeviceWaitIdle(vk->device);
    if (result != VK_SUCCESS)
        fail_vk("vkDeviceWaitIdle(release CAPTURE generation)", result);
    for (unsigned i = 0; i < capture_count; i++) {
        if (captures[i].command != VK_NULL_HANDLE)
            vkFreeCommandBuffers(vk->device, vk->command_pool, 1,
                                 &captures[i].command);
        if (captures[i].image != VK_NULL_HANDLE)
            vkDestroyImage(vk->device, captures[i].image, NULL);
        if (captures[i].memory != VK_NULL_HANDLE)
            vkFreeMemory(vk->device, captures[i].memory, NULL);
        if (captures[i].dma_buf_fd >= 0 && close(captures[i].dma_buf_fd) < 0)
            fail_errno("close CAPTURE DMA-BUF");
        memset(&captures[i], 0, sizeof(captures[i]));
        captures[i].dma_buf_fd = -1;
    }
    if (vk->transfer_buffer != VK_NULL_HANDLE)
        vkDestroyBuffer(vk->device, vk->transfer_buffer, NULL);
    if (vk->transfer_memory != VK_NULL_HANDLE)
        vkFreeMemory(vk->device, vk->transfer_memory, NULL);
    vk->transfer_buffer = VK_NULL_HANDLE;
    vk->transfer_memory = VK_NULL_HANDLE;
    vk->transfer_size = 0;
    vk->transfer_memory_flags = 0;
}

static void cleanup_vulkan(struct vulkan_context *vk,
                           struct capture_buffer *captures,
                           unsigned capture_count)
{
    if (vk->device != VK_NULL_HANDLE)
        release_capture_generation(vk, captures, capture_count);
    if (vk->query_pool != VK_NULL_HANDLE)
        vkDestroyQueryPool(vk->device, vk->query_pool, NULL);
    if (vk->fence != VK_NULL_HANDLE)
        vkDestroyFence(vk->device, vk->fence, NULL);
    if (vk->producer_semaphore != VK_NULL_HANDLE)
        vkDestroySemaphore(vk->device, vk->producer_semaphore, NULL);
    if (vk->command_pool != VK_NULL_HANDLE)
        vkDestroyCommandPool(vk->device, vk->command_pool, NULL);
    if (vk->device != VK_NULL_HANDLE)
        vkDestroyDevice(vk->device, NULL);
    if (vk->instance != VK_NULL_HANDLE)
        vkDestroyInstance(vk->instance, NULL);
}

int main(int argc, char **argv)
{
    /* Retain the proven transfer implementation for the later continuous
     * path while this executable exercises only the frozen-frame renderer. */
    (void)execute_transfer;
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc != 6) {
        fprintf(stderr,
                "usage: %s INPUT.hevc WIDTH VISIBLE_HEIGHT FPS_NUM FPS_DEN\n",
                argv[0]);
        return 2;
    }
    uint32_t expected_width = parse_u32(argv[2], "width");
    uint32_t expected_height = parse_u32(argv[3], "visible height");
    uint32_t fps_numerator = parse_u32(argv[4], "fps numerator");
    uint32_t fps_denominator = parse_u32(argv[5], "fps denominator");
    if ((expected_width & 1U) || (expected_height & 1U))
        fail_message("NV12 requires even visible dimensions");

    struct input_stream input = read_input(argv[1]);
    if (input.largest_unit > UINT32_MAX)
        fail_message("largest access unit exceeds V4L2 bytesused range");
    double frame_interval =
        (double)fps_denominator / (double)fps_numerator;
    double source_duration = input.unit_count * frame_interval;
    printf("source width=%u visible_height=%u fps=%u/%u duration_s=%.6f\n",
           expected_width, expected_height, fps_numerator, fps_denominator,
           source_duration);

    double *queued_at = calloc(input.unit_count, sizeof(*queued_at));
    if (!queued_at)
        fail_errno("allocate queue timestamps");
    struct vulkan_context vk = {0};
    initialize_vulkan(&vk);

    video_fd = open("/dev/video-dec0", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (video_fd < 0)
        fail_errno("open /dev/video-dec0");
    struct v4l2_capability capability = {0};
    must_ioctl(VIDIOC_QUERYCAP, &capability, "VIDIOC_QUERYCAP");
    printf("v4l2_device=/dev/video-dec0 driver=%s card=%s bus=%s\n",
           capability.driver, capability.card, capability.bus_info);
    if (strcmp((char *)capability.driver, "iris_driver") != 0)
        fail_message("/dev/video-dec0 is not iris_driver");
    struct v4l2_event_subscription source_subscription = {
        .type = V4L2_EVENT_SOURCE_CHANGE,
    };
    must_ioctl(VIDIOC_SUBSCRIBE_EVENT, &source_subscription,
               "VIDIOC_SUBSCRIBE_EVENT SOURCE_CHANGE");
    struct v4l2_event_subscription eos_subscription = {
        .type = V4L2_EVENT_EOS,
    };
    must_ioctl(VIDIOC_SUBSCRIBE_EVENT, &eos_subscription,
               "VIDIOC_SUBSCRIBE_EVENT EOS");

    struct v4l2_format output_format = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
    };
    output_format.fmt.pix_mp.width = expected_width;
    output_format.fmt.pix_mp.height = expected_height;
    output_format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_HEVC;
    output_format.fmt.pix_mp.field = V4L2_FIELD_NONE;
    output_format.fmt.pix_mp.num_planes = 1;
    output_format.fmt.pix_mp.plane_fmt[0].sizeimage =
        (uint32_t)input.largest_unit;
    must_ioctl(VIDIOC_S_FMT, &output_format, "VIDIOC_S_FMT OUTPUT HEVC");
    if (output_format.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_HEVC ||
        output_format.fmt.pix_mp.num_planes != 1 ||
        output_format.fmt.pix_mp.plane_fmt[0].sizeimage < input.largest_unit)
        fail_message("Iris OUTPUT format cannot hold the largest access unit");
    printf("output_format fourcc=HEVC width=%u height=%u sizeimage=%u\n",
           output_format.fmt.pix_mp.width, output_format.fmt.pix_mp.height,
           output_format.fmt.pix_mp.plane_fmt[0].sizeimage);

    struct v4l2_format initial_capture = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
    };
    initial_capture.fmt.pix_mp.width = expected_width;
    initial_capture.fmt.pix_mp.height = expected_height;
    initial_capture.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    initial_capture.fmt.pix_mp.field = V4L2_FIELD_NONE;
    initial_capture.fmt.pix_mp.num_planes = 1;
    must_ioctl(VIDIOC_S_FMT, &initial_capture,
               "VIDIOC_S_FMT initial CAPTURE NV12");
    if (initial_capture.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12)
        fail_message("Iris substituted the requested NV12 format");

    struct output_buffer outputs[MAX_OUTPUT_BUFFERS] = {0};
    unsigned output_count = allocate_output_buffers(outputs);
    struct capture_buffer captures[MAX_CAPTURE_BUFFERS];
    memset(captures, 0, sizeof(captures));
    for (unsigned i = 0; i < MAX_CAPTURE_BUFFERS; i++)
        captures[i].dma_buf_fd = -1;
    struct video_layout layout = {0};
    struct capture_contract capture_contract = {0};
    unsigned capture_count = 0;

    double initial_start = monotonic_seconds();
    queue_output(0, &outputs[0], &input, 0, fps_numerator, fps_denominator,
                 queued_at);
    size_t next_unit = 1;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    must_ioctl(VIDIOC_STREAMON, &type, "VIDIOC_STREAMON OUTPUT");
    bool capture_started = false;
    bool source_change_pending = false;
    bool drain_sent = false;
    bool last_seen = false;
    bool eos_seen = false;
    double playback_start = 0.0;
    double deadline = initial_start + 10.0;
    double last_progress = initial_start;
    uint64_t frames = 0;
    uint64_t sequence_drops = 0;
    uint64_t v4l2_errors = 0;
    uint32_t previous_sequence = 0;
    bool have_sequence = false;
    uint64_t output_returned = 0;
    uint64_t source_change_events = 0;
    uint64_t capture_reconfigurations = 0;
    struct timing_stats decode_stats = {0};
    struct timing_stats gpu_stats = {0};
    struct timing_stats gpu_wall_stats = {0};
    double maximum_feed_lateness_ms = 0.0;
    bool single_frame_shown = false;

    while (!last_seen) {
        double now = monotonic_seconds();
        if (now > deadline)
            fail_message("probe deadline exceeded");
        if (capture_started && now - last_progress > 10.0)
            fail_message("no decoder/GPU progress for 10 seconds");
        int timeout_ms = 20;
        if (capture_started && next_unit < input.unit_count) {
            double due = playback_start + next_unit * frame_interval;
            if (due > now) {
                double wait_ms = (due - now) * 1000.0;
                if (wait_ms < timeout_ms)
                    timeout_ms = (int)wait_ms;
            } else {
                timeout_ms = 0;
            }
        }
        struct pollfd descriptor = {
            .fd = video_fd,
            .events = POLLIN | POLLOUT | POLLPRI,
        };
        int poll_result = poll(&descriptor, 1, timeout_ms);
        if (poll_result < 0 && errno != EINTR)
            fail_errno("poll Iris");

        for (;;) {
            struct v4l2_event event = {0};
            if (xioctl(VIDIOC_DQEVENT, &event) < 0) {
                if (errno != EAGAIN && errno != ENOENT)
                    fail_errno("VIDIOC_DQEVENT");
                break;
            }
            if (event.type == V4L2_EVENT_SOURCE_CHANGE) {
                if (!(event.u.src_change.changes &
                      V4L2_EVENT_SRC_CH_RESOLUTION))
                    fail_message("SOURCE_CHANGE lacks RESOLUTION flag");
                source_change_events++;
                if (!capture_started) {
                    capture_count = setup_capture(&vk, captures,
                                                  &capture_contract, &layout,
                                                  expected_width,
                                                  expected_height);
                    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
                    must_ioctl(VIDIOC_STREAMON, &type,
                               "VIDIOC_STREAMON CAPTURE");
                    capture_started = true;
                    playback_start = monotonic_seconds();
                    last_progress = playback_start;
                    deadline = playback_start + source_duration * 3.0 + 30.0;
                    printf("source_change event=%" PRIu64
                           " phase=initial changes=0x%x\n",
                           source_change_events,
                           event.u.src_change.changes);
                    printf("playback_started paced=1 maximum_runtime_factor=3"
                           " deadline_margin_s=30 stall_timeout_s=10\n");
                } else {
                    if (source_change_pending)
                        fail_message("overlapping SOURCE_CHANGE events");
                    source_change_pending = true;
                    last_progress = monotonic_seconds();
                    printf("source_change event=%" PRIu64
                           " phase=dynamic changes=0x%x frames=%" PRIu64
                           " action=drain-old-capture\n",
                           source_change_events,
                           event.u.src_change.changes, frames);
                }
            } else if (event.type == V4L2_EVENT_EOS) {
                eos_seen = true;
                last_progress = monotonic_seconds();
                printf("v4l2_event=EOS frames=%" PRIu64 "\n", frames);
            }
        }

        for (;;) {
            struct v4l2_plane plane = {0};
            struct v4l2_buffer dequeued = {
                .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                .memory = V4L2_MEMORY_MMAP,
                .length = 1,
                .m.planes = &plane,
            };
            if (xioctl(VIDIOC_DQBUF, &dequeued) < 0) {
                if (errno != EAGAIN)
                    fail_errno("VIDIOC_DQBUF OUTPUT");
                break;
            }
            if (dequeued.index >= output_count)
                fail_message("OUTPUT returned an invalid index");
            outputs[dequeued.index].free = true;
            output_returned++;
            last_progress = monotonic_seconds();
            if (dequeued.flags & V4L2_BUF_FLAG_ERROR) {
                v4l2_errors++;
                fail_message("Iris marked an OUTPUT buffer erroneous");
            }
        }

        if (capture_started) {
            now = monotonic_seconds();
            while (next_unit < input.unit_count) {
                double due = playback_start + next_unit * frame_interval;
                if (now < due)
                    break;
                unsigned free_index = output_count;
                for (unsigned i = 0; i < output_count; i++) {
                    if (outputs[i].free) {
                        free_index = i;
                        break;
                    }
                }
                if (free_index == output_count)
                    break;
                double lateness_ms = (now - due) * 1000.0;
                if (lateness_ms > maximum_feed_lateness_ms)
                    maximum_feed_lateness_ms = lateness_ms;
                queue_output(free_index, &outputs[free_index], &input,
                             next_unit, fps_numerator, fps_denominator,
                             queued_at);
                next_unit++;
                now = monotonic_seconds();
                last_progress = now;
            }
            if (next_unit == input.unit_count && !drain_sent) {
                struct v4l2_decoder_cmd command = {
                    .cmd = V4L2_DEC_CMD_STOP,
                };
                must_ioctl(VIDIOC_DECODER_CMD, &command,
                           "VIDIOC_DECODER_CMD STOP");
                drain_sent = true;
                printf("decoder_drain_sent queued_access_units=%zu\n",
                       next_unit);
            }
        }

        if (capture_started) {
            bool capture_generation_last = false;
            unsigned last_capture_index = capture_count;
            for (;;) {
                struct v4l2_plane plane = {0};
                struct v4l2_buffer dequeued = {
                    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                    .memory = V4L2_MEMORY_MMAP,
                    .length = 1,
                    .m.planes = &plane,
                };
                if (xioctl(VIDIOC_DQBUF, &dequeued) < 0) {
                    if (errno != EAGAIN)
                        fail_errno("VIDIOC_DQBUF CAPTURE");
                    break;
                }
                if (dequeued.index >= capture_count)
                    fail_message("CAPTURE returned an invalid index");
                if (dequeued.flags & V4L2_BUF_FLAG_ERROR) {
                    v4l2_errors++;
                    fail_message("Iris marked a CAPTURE buffer erroneous");
                }
                if (plane.bytesused) {
                    uint64_t frame_index = frames;
                    if (have_sequence &&
                        dequeued.sequence > previous_sequence + 1U)
                        sequence_drops +=
                            dequeued.sequence - previous_sequence - 1U;
                    previous_sequence = dequeued.sequence;
                    have_sequence = true;
                    size_t source_unit = timestamp_to_unit(
                        &dequeued.timestamp, fps_numerator, fps_denominator);
                    if (source_unit < input.unit_count &&
                        queued_at[source_unit] > 0.0) {
                        update_stats(&decode_stats,
                                     (monotonic_seconds() -
                                      queued_at[source_unit]) *
                                         1000.0);
                    }
                    show_frame_in_openxr(&captures[dequeued.index], &layout);
                    single_frame_shown = true;
                    frames++;
                    last_progress = monotonic_seconds();
                    {
                        printf("frame=%" PRIu64 " capture_index=%u"
                               " sequence=%u bytesused=%u timestamp=%ld.%06ld"
                               " frozen_openxr=1\n",
                               frame_index, dequeued.index,
                               dequeued.sequence, plane.bytesused,
                               (long)dequeued.timestamp.tv_sec,
                               (long)dequeued.timestamp.tv_usec);
                    }
                    last_seen = true;
                    break;
                }
                if (dequeued.flags & V4L2_BUF_FLAG_LAST) {
                    capture_generation_last = true;
                    last_capture_index = dequeued.index;
                    last_seen = !source_change_pending;
                    printf("capture_flag=LAST frames=%" PRIu64
                           " reason=%s bytesused=%u\n",
                           frames,
                           source_change_pending ? "source-change"
                                                 : "end-of-stream",
                           plane.bytesused);
                    break;
                }
                queue_capture(dequeued.index,
                              captures[dequeued.index].length);
            }
            if (capture_generation_last && source_change_pending) {
                if (last_capture_index >= capture_count)
                    fail_message("SOURCE_CHANGE LAST buffer has invalid index");
                VkResult idle = vkDeviceWaitIdle(vk.device);
                if (idle != VK_SUCCESS)
                    fail_vk("vkDeviceWaitIdle(CAPTURE reuse)", idle);
                struct capture_contract updated_contract = {0};
                struct video_layout updated_layout = {0};
                query_capture_contract(&updated_contract);
                bool memory_compatible = capture_memory_compatible(
                    &capture_contract, &updated_contract);
                bool compose_unchanged = same_compose(
                    &capture_contract.compose, &updated_contract.compose);
                printf("capture_reuse_check memory_compatible=%u"
                       " compose_unchanged=%u existing_buffers=%u\n",
                       memory_compatible, compose_unchanged, capture_count);
                if (!memory_compatible)
                    fail_message("SOURCE_CHANGE CAPTURE memory contract changed");
                if (!compose_unchanged)
                    fail_message("SOURCE_CHANGE visible compose region changed");
                if (capture_count != 12U)
                    fail_message("SOURCE_CHANGE CAPTURE count is not twelve");
                validate_capture_contract(&updated_contract, &updated_layout,
                                          expected_width, expected_height);
                struct capture_minimum minimum = query_capture_minimum();
                if (minimum.available && capture_count < minimum.value)
                    fail_message("existing CAPTURE count is below new minimum");
                verify_capture_allocations(captures, capture_count,
                                           updated_contract.sizeimage);
                queue_capture(last_capture_index,
                              captures[last_capture_index].length);
                struct v4l2_decoder_cmd command = {
                    .cmd = V4L2_DEC_CMD_START,
                };
                must_ioctl(VIDIOC_DECODER_CMD, &command,
                           "VIDIOC_DECODER_CMD START CAPTURE reuse");
                capture_contract = updated_contract;
                layout = updated_layout;
                source_change_pending = false;
                capture_reconfigurations++;
                last_progress = monotonic_seconds();
                printf("capture_reused generation=%" PRIu64
                       " frames=%" PRIu64
                       " output_stream_kept=1 capture_stream_kept=1"
                       " vulkan_imports_kept=1 last_index=%u\n",
                       capture_reconfigurations + 1U, frames,
                       last_capture_index);
            }
        }
    }

    double playback_end = monotonic_seconds();
    double elapsed = playback_end - playback_start;
    double achieved_fps = elapsed > 0.0 ? frames / elapsed : 0.0;
    double source_fps = (double)fps_numerator / fps_denominator;
    uint64_t count_difference =
        frames < input.unit_count ? input.unit_count - frames
                                 : frames - input.unit_count;
    printf("summary access_units=%zu output_returned=%" PRIu64
           " frames=%" PRIu64 " sequence_drops=%" PRIu64
           " count_difference=%" PRIu64 " errors=%" PRIu64
           " source_changes=%" PRIu64 " capture_reconfigurations=%" PRIu64
           " eos_event=%u last_buffer=%u\n",
           input.unit_count, output_returned, frames, sequence_drops,
           count_difference, v4l2_errors, source_change_events,
           capture_reconfigurations, eos_seen, last_seen);
    printf("timing source_fps=%.6f achieved_fps=%.6f elapsed_s=%.6f"
           " source_duration_s=%.6f realtime_ratio=%.6f"
           " maximum_feed_lateness_ms=%.6f\n",
           source_fps, achieved_fps, elapsed, source_duration,
           source_fps > 0.0 ? achieved_fps / source_fps : 0.0,
           maximum_feed_lateness_ms);
    printf("decode_timing samples=%" PRIu64 " mean_ms=%.6f max_ms=%.6f\n",
           decode_stats.count,
           decode_stats.count ? decode_stats.sum_ms / decode_stats.count : 0,
           decode_stats.maximum_ms);
    printf("gpu_timing samples=%" PRIu64 " mean_ms=%.6f max_ms=%.6f"
           " wall_mean_ms=%.6f wall_max_ms=%.6f\n",
           gpu_stats.count,
           gpu_stats.count ? gpu_stats.sum_ms / gpu_stats.count : 0,
           gpu_stats.maximum_ms,
           gpu_wall_stats.count
               ? gpu_wall_stats.sum_ms / gpu_wall_stats.count
               : 0,
           gpu_wall_stats.maximum_ms);

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    must_ioctl(VIDIOC_STREAMOFF, &type, "VIDIOC_STREAMOFF CAPTURE");
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    must_ioctl(VIDIOC_STREAMOFF, &type, "VIDIOC_STREAMOFF OUTPUT");
    for (unsigned i = 0; i < output_count; i++) {
        if (munmap(outputs[i].mapping, outputs[i].length) < 0)
            fail_errno("munmap OUTPUT");
    }
    if (close(video_fd) < 0)
        fail_errno("close /dev/video-dec0");
    video_fd = -1;
    cleanup_vulkan(&vk, captures, capture_count);
    free(queued_at);
    free(input.units);
    free(input.data);
    bool success = single_frame_shown && frames == 1 && v4l2_errors == 0;
    printf("result=%s mode=single-frozen-frame\n",
           success ? "success" : "failure");
    return success ? 0 : 1;
}
