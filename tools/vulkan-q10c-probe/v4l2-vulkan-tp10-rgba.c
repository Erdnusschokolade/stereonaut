/*
 * One-shot Iris Q10C -> DMA-BUF -> Turnip readback that lets the GPU undo
 * only the UBWC compression: each TP10 plane is imported as an RGBA8 UBWC
 * image with the same byte geometry (three 10-bit samples per
 * 32-bit word, so 1920 px = 640 words; tile 48x4 px = 64 B x 4 rows, the
 * same as a 16x4 RGBA8 tile). The copy writes the raw TP10 words linearly;
 * unpacking and the comparison happen offline (analyze-tp10-rgba.py).
 * Fixed to 1920x1088 Q10C from the 60-AU Main10 fixtures and /dev/video-dec0.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define WIDTH 1920U
#define CODED_HEIGHT 1088U
#define VISIBLE_HEIGHT 1080U
#define CHROMA_WIDTH 960U
#define CHROMA_HEIGHT 544U
#define Q10C_STRIDE 2560U
#define Q10C_SIZE UINT64_C(4210688)
/* TP10 planes as 32-bpp images: width 1920 / 3 words. */
#define WORDS 640U
#define Y_DATA_SIZE UINT64_C(2785280)  /* 2560 x 1088 */
#define UV_DATA_SIZE UINT64_C(1392640) /* 2560 x 544 */
#define READBACK_SIZE (Y_DATA_SIZE + UV_DATA_SIZE)
#define UV_PLANE_OFFSET UINT64_C(2805760) /* Y metadata + Y data */
#define QCOM_COMPRESSED UINT64_C(0x0500000000000001)
#define Q10C_FOURCC v4l2_fourcc('Q', '1', '0', 'C')
#define MAX_OUTPUT_BUFFERS 8U
#define MAX_CAPTURE_BUFFERS 32U
#define MAX_PACKETS 60U

struct output_buffer {
    void *mapping;
    size_t length;
};

struct capture_buffer {
    size_t length;
};

struct packet {
    size_t offset;
    size_t size;
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

static double monotonic_seconds(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) < 0)
        fail_errno("clock_gettime");
    return value.tv_sec + value.tv_nsec / 1000000000.0;
}

static unsigned split_access_units(const uint8_t *data, size_t size,
                                   struct packet packets[MAX_PACKETS])
{
    size_t starts[MAX_PACKETS];
    unsigned count = 0;

    for (size_t i = 0; i + 5 < size;) {
        unsigned prefix = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            prefix = 3;
        else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 &&
                 data[i + 3] == 1)
            prefix = 4;
        if (prefix) {
            if (((data[i + prefix] >> 1) & 63) == 35) {
                if (count == MAX_PACKETS)
                    fail_message("fixture has more than 60 access units");
                starts[count++] = i;
            }
            i += prefix + 2;
        } else {
            i++;
        }
    }
    if (!count)
        fail_message("fixture has no AUD-delimited access units");
    starts[0] = 0;
    for (unsigned i = 0; i < count; i++) {
        packets[i].offset = starts[i];
        packets[i].size = (i + 1 < count ? starts[i + 1] : size) - starts[i];
    }
    return count;
}

static uint8_t *read_fixture(const char *path, size_t *size_out,
                             struct packet packets[MAX_PACKETS],
                             unsigned *packet_count_out)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        fail_errno("open input fixture");
    if (fseek(file, 0, SEEK_END) != 0)
        fail_errno("seek input fixture");
    long length = ftell(file);
    if (length <= 0 || length > 16 * 1024 * 1024)
        fail_message("fixture size outside bound");
    rewind(file);

    uint8_t *data = malloc((size_t)length);
    if (!data)
        fail_errno("allocate fixture");
    if (fread(data, 1, (size_t)length, file) != (size_t)length)
        fail_errno("read fixture");
    if (fclose(file) != 0)
        fail_errno("close fixture");

    *packet_count_out = split_access_units(data, (size_t)length, packets);
    if (*packet_count_out != MAX_PACKETS)
        fail_message("fixture must contain exactly 60 access units");
    *size_out = (size_t)length;
    return data;
}

static unsigned allocate_output_buffers(struct output_buffer buffers[MAX_OUTPUT_BUFFERS])
{
    struct v4l2_requestbuffers request = {
        .count = 4,
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
            fail_message("OUTPUT is not single-planar memory");
        buffers[i].length = plane.length;
        buffers[i].mapping = mmap(NULL, plane.length, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, video_fd, plane.m.mem_offset);
        if (buffers[i].mapping == MAP_FAILED)
            fail_errno("mmap OUTPUT");
    }
    printf("output_buffers=%u\n", request.count);
    return request.count;
}

static void queue_output(unsigned index, const struct output_buffer *buffer,
                         const uint8_t *data, const struct packet *packet,
                         unsigned sequence)
{
    if (packet->size > buffer->length)
        fail_message("access unit exceeds OUTPUT buffer");
    memcpy(buffer->mapping, data + packet->offset, packet->size);

    struct v4l2_plane plane = {
        .bytesused = (uint32_t)packet->size,
        .length = (uint32_t)buffer->length,
    };
    struct v4l2_buffer queued = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
        .memory = V4L2_MEMORY_MMAP,
        .index = index,
        .length = 1,
        .m.planes = &plane,
    };
    queued.timestamp.tv_sec = sequence / 30;
    queued.timestamp.tv_usec = (sequence % 30) * 1000000 / 30;
    must_ioctl(VIDIOC_QBUF, &queued, "VIDIOC_QBUF OUTPUT");
}

static unsigned allocate_capture_buffers(
    struct capture_buffer buffers[MAX_CAPTURE_BUFFERS])
{
    struct v4l2_control minimum = {
        .id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE,
    };
    unsigned wanted = 12;
    if (xioctl(VIDIOC_G_CTRL, &minimum) == 0 && minimum.value > 0)
        wanted = (unsigned)minimum.value + 2;
    if (wanted > MAX_CAPTURE_BUFFERS)
        fail_message("CAPTURE buffer count exceeds bound");

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
        if (buffer.length != 1 || plane.length != Q10C_SIZE)
            fail_message("unexpected CAPTURE allocation layout");
        buffers[i].length = plane.length;
        must_ioctl(VIDIOC_QBUF, &buffer, "VIDIOC_QBUF CAPTURE");
    }
    printf("capture_buffers=%u cpu_mapped=no\n", request.count);
    return request.count;
}

static int export_dequeued_capture(unsigned index)
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
        fail_errno("fstat exported DMA-BUF");
    printf("dmabuf_export index=%u fd=%d size=%jd\n", index, exported.fd,
           (intmax_t)status.st_size);
    return exported.fd;
}

static int has_device_extension(VkPhysicalDevice physical, const char *name)
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
    int found = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(properties[i].extensionName, name) == 0) {
            found = 1;
            break;
        }
    }
    free(properties);
    return found;
}

static uint32_t find_transfer_queue(VkPhysicalDevice physical)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    VkQueueFamilyProperties *properties = calloc(count, sizeof(*properties));
    if (!properties)
        fail_errno("allocate queue properties");
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, properties);
    for (uint32_t i = 0; i < count; i++) {
        if (properties[i].queueFlags & VK_QUEUE_TRANSFER_BIT) {
            free(properties);
            return i;
        }
    }
    free(properties);
    fail_message("no Vulkan transfer queue family");
    return 0;
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
            VkMemoryPropertyFlags flags = properties.memoryTypes[i].propertyFlags;
            if (!(allowed_bits & (1U << i)) || (flags & required) != required)
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

static void write_readback(const char *path, const void *data, size_t size)
{
    FILE *file = fopen(path, "wb");
    if (!file)
        fail_errno("open readback output");
    if (fwrite(data, 1, size, file) != size)
        fail_errno("write readback output");
    if (fclose(file) != 0)
        fail_errno("close readback output");
}

struct imported_plane {
    VkImage image;
    VkDeviceMemory memory;
};

/* One TP10 plane (metadata + data) as an RGBA8 image with the QCOM UBWC
 * modifier, its memory imported from the same DMA-BUF. */
static struct imported_plane import_plane(
    VkPhysicalDevice physical, VkDevice device, int dma_buf_fd,
    uint64_t offset, uint32_t height, const char *name)
{
    VkResult result;
    const VkSubresourceLayout layout = {
        .offset = offset, .size = 0, .rowPitch = Q10C_STRIDE,
    };
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_info = {
        .sType =
            VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier = QCOM_COMPRESSED,
        .drmFormatModifierPlaneCount = 1,
        .pPlaneLayouts = &layout,
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
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {WORDS, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    struct imported_plane plane = {0};
    result = vkCreateImage(device, &image_info, NULL, &plane.image);
    if (result != VK_SUCCESS)
        fail_vk("vkCreateImage(RGBA8 UBWC plane)", result);

    PFN_vkGetImageDrmFormatModifierPropertiesEXT get_modifier =
        (PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkGetDeviceProcAddr(
            device, "vkGetImageDrmFormatModifierPropertiesEXT");
    PFN_vkGetMemoryFdPropertiesKHR get_fd_properties =
        (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(
            device, "vkGetMemoryFdPropertiesKHR");
    if (!get_modifier || !get_fd_properties)
        fail_message("required Vulkan FD/modifier entry point unavailable");
    VkImageDrmFormatModifierPropertiesEXT chosen_modifier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
    };
    result = get_modifier(device, plane.image, &chosen_modifier);
    if (result != VK_SUCCESS)
        fail_vk("vkGetImageDrmFormatModifierPropertiesEXT", result);
    if (chosen_modifier.drmFormatModifier != QCOM_COMPRESSED)
        fail_message("Vulkan returned a different DRM modifier");

    VkMemoryDedicatedRequirements dedicated_requirements = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
    };
    VkMemoryRequirements2 requirements = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        .pNext = &dedicated_requirements,
    };
    VkImageMemoryRequirementsInfo2 requirements_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = plane.image,
    };
    vkGetImageMemoryRequirements2(device, &requirements_info, &requirements);
    printf("plane=%s offset=%" PRIu64 " extent=%ux%u memory size=%" PRIu64
           " alignment=%" PRIu64 " dedicated_required=%u\n",
           name, offset, WORDS, height, requirements.memoryRequirements.size,
           requirements.memoryRequirements.alignment,
           dedicated_requirements.requiresDedicatedAllocation);
    /* Turnip's own RGBA8 UBWC layout must end exactly where the Iris plane
     * ends; anything else means the geometry assumption is wrong. */
    uint64_t expected_end = offset == 0 ? UV_PLANE_OFFSET : Q10C_SIZE;
    printf("plane=%s layout_end_matches=%u expected_end=%" PRIu64 "\n", name,
           requirements.memoryRequirements.size == expected_end,
           expected_end);
    if (requirements.memoryRequirements.size > Q10C_SIZE)
        fail_message("RGBA8 UBWC plane would exceed the Iris allocation");

    VkMemoryFdPropertiesKHR fd_properties = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
    };
    result = get_fd_properties(device,
                               VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                               dma_buf_fd, &fd_properties);
    if (result != VK_SUCCESS)
        fail_vk("vkGetMemoryFdPropertiesKHR", result);
    uint32_t compatible_bits = fd_properties.memoryTypeBits &
                               requirements.memoryRequirements.memoryTypeBits;
    if (!compatible_bits)
        fail_message("DMA-BUF and image memory-type intersection is empty");
    VkMemoryPropertyFlags flags = 0;
    uint32_t type = choose_memory_type(physical, compatible_bits, 0,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                       &flags);
    int imported_fd = dup(dma_buf_fd);
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
        .image = plane.image,
    };
    VkMemoryAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated_info,
        .allocationSize = Q10C_SIZE,
        .memoryTypeIndex = type,
    };
    result = vkAllocateMemory(device, &allocate_info, NULL, &plane.memory);
    if (result != VK_SUCCESS) {
        close(imported_fd);
        fail_vk("vkAllocateMemory(import DMA-BUF)", result);
    }
    result = vkBindImageMemory(device, plane.image, plane.memory, 0);
    if (result != VK_SUCCESS)
        fail_vk("vkBindImageMemory", result);
    return plane;
}

static void vulkan_readback(int dma_buf_fd, const char *output_path)
{
    VkResult result;
    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "stereonaut-tp10-rgba-probe",
        .applicationVersion = 1,
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
    };
    VkInstance instance = VK_NULL_HANDLE;
    result = vkCreateInstance(&instance_info, NULL, &instance);
    if (result != VK_SUCCESS)
        fail_vk("vkCreateInstance", result);

    uint32_t physical_count = 0;
    result = vkEnumeratePhysicalDevices(instance, &physical_count, NULL);
    if (result != VK_SUCCESS || physical_count == 0)
        fail_vk("vkEnumeratePhysicalDevices(count)", result);
    VkPhysicalDevice *physicals = calloc(physical_count, sizeof(*physicals));
    if (!physicals)
        fail_errno("allocate physical devices");
    result = vkEnumeratePhysicalDevices(instance, &physical_count, physicals);
    if (result != VK_SUCCESS)
        fail_vk("vkEnumeratePhysicalDevices(list)", result);
    VkPhysicalDevice physical = physicals[0];
    free(physicals);

    VkPhysicalDeviceProperties physical_properties;
    vkGetPhysicalDeviceProperties(physical, &physical_properties);
    printf("vulkan_device=%s vendor=0x%x device=0x%x\n",
           physical_properties.deviceName, physical_properties.vendorID,
           physical_properties.deviceID);
    if (physical_properties.vendorID != 0x5143)
        fail_message("unexpected Vulkan vendor; refusing software/non-Adreno path");

    const char *extensions[] = {
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
    };
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
        if (!has_device_extension(physical, extensions[i])) {
            fprintf(stderr, "required Vulkan extension missing: %s\n",
                    extensions[i]);
            exit(2);
        }
    }

    uint32_t queue_family = find_transfer_queue(physical);
    float priority = 0.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = sizeof(extensions) / sizeof(extensions[0]),
        .ppEnabledExtensionNames = extensions,
    };
    VkDevice device = VK_NULL_HANDLE;
    result = vkCreateDevice(physical, &device_info, NULL, &device);
    if (result != VK_SUCCESS)
        fail_vk("vkCreateDevice", result);
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, queue_family, 0, &queue);
    if (queue == VK_NULL_HANDLE)
        fail_message("vkGetDeviceQueue returned a null queue");

    struct imported_plane planes[2] = {
        import_plane(physical, device, dma_buf_fd, 0, CODED_HEIGHT, "y"),
        import_plane(physical, device, dma_buf_fd, UV_PLANE_OFFSET,
                     CHROMA_HEIGHT, "uv"),
    };

    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = READBACK_SIZE,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer readback_buffer = VK_NULL_HANDLE;
    result = vkCreateBuffer(device, &buffer_info, NULL, &readback_buffer);
    if (result != VK_SUCCESS)
        fail_vk("vkCreateBuffer(readback)", result);
    VkMemoryRequirements readback_requirements;
    vkGetBufferMemoryRequirements(device, readback_buffer,
                                  &readback_requirements);
    VkMemoryPropertyFlags readback_flags = 0;
    uint32_t readback_type = choose_memory_type(
        physical, readback_requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &readback_flags);
    VkMemoryAllocateInfo readback_allocate_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = readback_requirements.size,
        .memoryTypeIndex = readback_type,
    };
    VkDeviceMemory readback_memory = VK_NULL_HANDLE;
    result = vkAllocateMemory(device, &readback_allocate_info, NULL,
                              &readback_memory);
    if (result != VK_SUCCESS)
        fail_vk("vkAllocateMemory(readback)", result);
    result = vkBindBufferMemory(device, readback_buffer, readback_memory, 0);
    if (result != VK_SUCCESS)
        fail_vk("vkBindBufferMemory(readback)", result);

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = queue_family,
    };
    VkCommandPool command_pool = VK_NULL_HANDLE;
    result = vkCreateCommandPool(device, &pool_info, NULL, &command_pool);
    if (result != VK_SUCCESS)
        fail_vk("vkCreateCommandPool", result);
    VkCommandBufferAllocateInfo command_allocate_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    result = vkAllocateCommandBuffers(device, &command_allocate_info,
                                      &command_buffer);
    if (result != VK_SUCCESS)
        fail_vk("vkAllocateCommandBuffers", result);
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    result = vkBeginCommandBuffer(command_buffer, &begin_info);
    if (result != VK_SUCCESS)
        fail_vk("vkBeginCommandBuffer", result);

    VkImageMemoryBarrier barriers[2];
    for (unsigned i = 0; i < 2; i++)
        barriers[i] = (VkImageMemoryBarrier){
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
            .dstQueueFamilyIndex = queue_family,
            .image = planes[i].image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         2, barriers);
    for (unsigned i = 0; i < 2; i++) {
        VkBufferImageCopy copy = {
            .bufferOffset = i ? Y_DATA_SIZE : 0,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {WORDS, i ? CHROMA_HEIGHT : CODED_HEIGHT, 1},
        };
        vkCmdCopyImageToBuffer(command_buffer, planes[i].image,
                               VK_IMAGE_LAYOUT_GENERAL, readback_buffer, 1,
                               &copy);
    }
    for (unsigned i = 0; i < 2; i++) {
        barriers[i].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[i].dstAccessMask = 0;
        barriers[i].srcQueueFamilyIndex = queue_family;
        barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    }
    VkBufferMemoryBarrier host_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = readback_buffer,
        .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT |
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, NULL, 1, &host_barrier, 2, barriers);
    result = vkEndCommandBuffer(command_buffer);
    if (result != VK_SUCCESS)
        fail_vk("vkEndCommandBuffer", result);

    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    VkFence fence = VK_NULL_HANDLE;
    result = vkCreateFence(device, &fence_info, NULL, &fence);
    if (result != VK_SUCCESS)
        fail_vk("vkCreateFence", result);
    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer,
    };
    result = vkQueueSubmit(queue, 1, &submit_info, fence);
    if (result != VK_SUCCESS)
        fail_vk("vkQueueSubmit", result);
    result = vkWaitForFences(device, 1, &fence, VK_TRUE,
                             UINT64_C(5000000000));
    if (result != VK_SUCCESS)
        fail_vk("vkWaitForFences", result);
    printf("gpu_fence=signaled\n");

    void *mapped = NULL;
    result = vkMapMemory(device, readback_memory, 0, READBACK_SIZE, 0, &mapped);
    if (result != VK_SUCCESS)
        fail_vk("vkMapMemory(readback after fence)", result);
    if (!(readback_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        VkMappedMemoryRange range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = readback_memory,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
        };
        result = vkInvalidateMappedMemoryRanges(device, 1, &range);
        if (result != VK_SUCCESS)
            fail_vk("vkInvalidateMappedMemoryRanges", result);
    }
    write_readback(output_path, mapped, READBACK_SIZE);
    vkUnmapMemory(device, readback_memory);
    printf("readback_written=%s bytes=%" PRIu64 "\n", output_path,
           READBACK_SIZE);

    vkDestroyFence(device, fence, NULL);
    vkDestroyCommandPool(device, command_pool, NULL);
    vkDestroyBuffer(device, readback_buffer, NULL);
    vkFreeMemory(device, readback_memory, NULL);
    for (unsigned i = 0; i < 2; i++) {
        vkDestroyImage(device, planes[i].image, NULL);
        vkFreeMemory(device, planes[i].memory, NULL);
    }
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc != 3) {
        fprintf(stderr, "usage: %s MAIN10_FIXTURE.hevc tp10-readback.bin\n",
                argv[0]);
        return 2;
    }

    size_t fixture_size = 0;
    struct packet packets[MAX_PACKETS];
    unsigned packet_count = 0;
    uint8_t *fixture = read_fixture(argv[1], &fixture_size, packets,
                                    &packet_count);
    printf("fixture_bytes=%zu access_units=%u\n", fixture_size, packet_count);

    video_fd = open("/dev/video-dec0", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (video_fd < 0)
        fail_errno("open /dev/video-dec0");
    struct v4l2_capability capability = {0};
    must_ioctl(VIDIOC_QUERYCAP, &capability, "VIDIOC_QUERYCAP");
    printf("v4l2_driver=%s card=%s\n", capability.driver, capability.card);
    if (strcmp((char *)capability.driver, "iris_driver") != 0)
        fail_message("/dev/video-dec0 is not iris_driver");

    struct v4l2_event_subscription subscription = {
        .type = V4L2_EVENT_SOURCE_CHANGE,
    };
    must_ioctl(VIDIOC_SUBSCRIBE_EVENT, &subscription,
               "VIDIOC_SUBSCRIBE_EVENT SOURCE_CHANGE");

    struct v4l2_format output_format = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
    };
    output_format.fmt.pix_mp.width = WIDTH;
    output_format.fmt.pix_mp.height = VISIBLE_HEIGHT;
    output_format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_HEVC;
    output_format.fmt.pix_mp.field = V4L2_FIELD_NONE;
    output_format.fmt.pix_mp.num_planes = 1;
    output_format.fmt.pix_mp.plane_fmt[0].sizeimage = 1024 * 1024;
    must_ioctl(VIDIOC_S_FMT, &output_format, "VIDIOC_S_FMT OUTPUT HEVC");
    if (output_format.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_HEVC ||
        output_format.fmt.pix_mp.num_planes != 1)
        fail_message("Iris rejected the fixed HEVC OUTPUT format");

    struct v4l2_format capture_format = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
    };
    capture_format.fmt.pix_mp.width = WIDTH;
    capture_format.fmt.pix_mp.height = VISIBLE_HEIGHT;
    capture_format.fmt.pix_mp.pixelformat = Q10C_FOURCC;
    capture_format.fmt.pix_mp.field = V4L2_FIELD_NONE;
    capture_format.fmt.pix_mp.num_planes = 1;
    must_ioctl(VIDIOC_S_FMT, &capture_format,
               "VIDIOC_S_FMT initial CAPTURE Q10C");
    if (capture_format.fmt.pix_mp.pixelformat != Q10C_FOURCC)
        fail_message("Iris substituted the requested Q10C format");

    struct output_buffer output_buffers[MAX_OUTPUT_BUFFERS] = {0};
    struct capture_buffer capture_buffers[MAX_CAPTURE_BUFFERS] = {0};
    unsigned output_count = allocate_output_buffers(output_buffers);
    unsigned next_packet = 0;
    for (unsigned i = 0; i < output_count && next_packet < packet_count;
         i++, next_packet++) {
        queue_output(i, &output_buffers[i], fixture, &packets[next_packet],
                     next_packet);
    }
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    must_ioctl(VIDIOC_STREAMON, &type, "VIDIOC_STREAMON OUTPUT");

    int capture_started = 0;
    unsigned capture_count = 0;
    int completed = 0;
    const double deadline = monotonic_seconds() + 10.0;
    while (!completed && monotonic_seconds() < deadline) {
        struct pollfd descriptor = {
            .fd = video_fd,
            .events = POLLIN | POLLOUT | POLLPRI,
        };
        int poll_result = poll(&descriptor, 1, 100);
        if (poll_result < 0 && errno != EINTR)
            fail_errno("poll Iris");

        struct v4l2_event event = {0};
        int event_result = capture_started ? -1 : xioctl(VIDIOC_DQEVENT, &event);
        if (event_result == 0 && event.type == V4L2_EVENT_SOURCE_CHANGE) {
            capture_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            must_ioctl(VIDIOC_G_FMT, &capture_format,
                       "VIDIOC_G_FMT after SOURCE_CHANGE");
            printf("capture_format fourcc=%c%c%c%c width=%u height=%u"
                   " planes=%u stride=%u sizeimage=%u\n",
                   capture_format.fmt.pix_mp.pixelformat & 0xff,
                   (capture_format.fmt.pix_mp.pixelformat >> 8) & 0xff,
                   (capture_format.fmt.pix_mp.pixelformat >> 16) & 0xff,
                   (capture_format.fmt.pix_mp.pixelformat >> 24) & 0xff,
                   capture_format.fmt.pix_mp.width,
                   capture_format.fmt.pix_mp.height,
                   capture_format.fmt.pix_mp.num_planes,
                   capture_format.fmt.pix_mp.plane_fmt[0].bytesperline,
                   capture_format.fmt.pix_mp.plane_fmt[0].sizeimage);
            if (capture_format.fmt.pix_mp.pixelformat != Q10C_FOURCC ||
                capture_format.fmt.pix_mp.width != WIDTH ||
                capture_format.fmt.pix_mp.height != CODED_HEIGHT ||
                capture_format.fmt.pix_mp.num_planes != 1 ||
                capture_format.fmt.pix_mp.plane_fmt[0].bytesperline !=
                    Q10C_STRIDE ||
                capture_format.fmt.pix_mp.plane_fmt[0].sizeimage != Q10C_SIZE)
                fail_message("SOURCE_CHANGE format does not match approved Q10C layout");

            struct v4l2_selection selection = {
                .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                .target = V4L2_SEL_TGT_COMPOSE,
            };
            must_ioctl(VIDIOC_G_SELECTION, &selection,
                       "VIDIOC_G_SELECTION COMPOSE");
            printf("visible_rect left=%d top=%d width=%u height=%u\n",
                   selection.r.left, selection.r.top, selection.r.width,
                   selection.r.height);
            if (selection.r.left != 0 || selection.r.top != 0 ||
                selection.r.width != WIDTH ||
                selection.r.height != VISIBLE_HEIGHT)
                fail_message("unexpected visible rectangle");

            capture_count = allocate_capture_buffers(capture_buffers);
            type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            must_ioctl(VIDIOC_STREAMON, &type, "VIDIOC_STREAMON CAPTURE");
            capture_started = 1;
        }
        if (!capture_started && event_result < 0 &&
            errno != EAGAIN && errno != ENOENT)
            fail_errno("VIDIOC_DQEVENT");

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
            if (dequeued.flags & V4L2_BUF_FLAG_ERROR)
                fail_message("Iris marked an OUTPUT buffer erroneous");
            if (next_packet < packet_count) {
                queue_output(dequeued.index, &output_buffers[dequeued.index],
                             fixture, &packets[next_packet], next_packet);
                next_packet++;
            }
        }

        if (capture_started) {
            struct v4l2_plane plane = {0};
            struct v4l2_buffer dequeued = {
                .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                .memory = V4L2_MEMORY_MMAP,
                .length = 1,
                .m.planes = &plane,
            };
            if (xioctl(VIDIOC_DQBUF, &dequeued) == 0) {
                if (dequeued.index >= capture_count ||
                    capture_buffers[dequeued.index].length != Q10C_SIZE ||
                    (dequeued.flags & V4L2_BUF_FLAG_ERROR) ||
                    !plane.bytesused)
                    fail_message("invalid first Q10C CAPTURE buffer");
                printf("capture_dequeued index=%u sequence=%u bytesused=%u"
                       " flags=0x%x queued_access_units=%u\n",
                       dequeued.index, dequeued.sequence, plane.bytesused,
                       dequeued.flags, next_packet);
                int dma_buf_fd = export_dequeued_capture(dequeued.index);
                vulkan_readback(dma_buf_fd, argv[2]);
                if (close(dma_buf_fd) < 0)
                    fail_errno("close exported DMA-BUF");

                plane.length = (uint32_t)capture_buffers[dequeued.index].length;
                plane.bytesused = 0;
                must_ioctl(VIDIOC_QBUF, &dequeued,
                           "VIDIOC_QBUF CAPTURE after Vulkan fence");
                printf("capture_requeued_after_fence index=%u\n",
                       dequeued.index);
                completed = 1;
            } else if (errno != EAGAIN) {
                fail_errno("VIDIOC_DQBUF CAPTURE");
            }
        }
    }
    if (!completed)
        fail_message("timed out before one Q10C frame completed");

    if (capture_started) {
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        must_ioctl(VIDIOC_STREAMOFF, &type, "VIDIOC_STREAMOFF CAPTURE");
    }
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    must_ioctl(VIDIOC_STREAMOFF, &type, "VIDIOC_STREAMOFF OUTPUT");
    for (unsigned i = 0; i < output_count; i++) {
        if (munmap(output_buffers[i].mapping, output_buffers[i].length) < 0)
            fail_errno("munmap OUTPUT");
    }
    if (close(video_fd) < 0)
        fail_errno("close /dev/video-dec0");
    video_fd = -1;
    free(fixture);
    printf("result=success frames=1 dma_buf_exports=1 gpu_readbacks=1\n");
    return 0;
}
