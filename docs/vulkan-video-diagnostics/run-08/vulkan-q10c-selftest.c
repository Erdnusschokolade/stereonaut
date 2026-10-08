/*
 * One-shot, decoder-free Turnip QCOM-compressed P010 round-trip self-test.
 * No V4L2, DMA-BUF, external memory, or foreign queue ownership is used.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define WIDTH 1920U
#define CODED_HEIGHT 1088U
#define CHROMA_WIDTH 960U
#define CHROMA_HEIGHT 544U
#define Q10C_STRIDE 2560U
#define Q10C_SIZE UINT64_C(4210688)
#define P010_Y_SIZE UINT64_C(4177920)
#define P010_UV_SIZE UINT64_C(2088960)
#define P010_SIZE (P010_Y_SIZE + P010_UV_SIZE)
#define QCOM_COMPRESSED UINT64_C(0x0500000000000001)
#define MEMORY_PLANE_COUNT 2U

struct host_buffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceSize allocation_size;
};

static void fail_message(const char *what)
{
    fprintf(stderr, "%s\n", what);
    exit(2);
}

static void fail_errno(const char *what)
{
    fprintf(stderr, "%s: errno=%d (%s)\n", what, errno, strerror(errno));
    exit(2);
}

static void fail_vk(const char *what, VkResult result)
{
    fprintf(stderr, "%s: VkResult=%d\n", what, result);
    exit(2);
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
    if (!count)
        fail_message("no Vulkan queue families");
    VkQueueFamilyProperties *properties = calloc(count, sizeof(*properties));
    if (!properties)
        fail_errno("allocate queue family properties");
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

static VkFormatFeatureFlags find_modifier_features(
    VkPhysicalDevice physical, VkFormat format, uint32_t *memory_planes)
{
    VkDrmFormatModifierPropertiesListEXT modifier_list = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
    };
    VkFormatProperties2 format_properties = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &modifier_list,
    };
    vkGetPhysicalDeviceFormatProperties2(physical, format,
                                         &format_properties);
    if (!modifier_list.drmFormatModifierCount)
        fail_message("format exposes no DRM modifiers");
    uint32_t capacity = modifier_list.drmFormatModifierCount;
    VkDrmFormatModifierPropertiesEXT *modifiers =
        calloc(capacity, sizeof(*modifiers));
    if (!modifiers)
        fail_errno("allocate modifier properties");
    modifier_list.pDrmFormatModifierProperties = modifiers;
    vkGetPhysicalDeviceFormatProperties2(physical, format,
                                         &format_properties);
    if (modifier_list.drmFormatModifierCount > capacity)
        fail_message("DRM modifier list grew unexpectedly");

    VkFormatFeatureFlags features = 0;
    *memory_planes = 0;
    for (uint32_t i = 0; i < modifier_list.drmFormatModifierCount; i++) {
        if (modifiers[i].drmFormatModifier == QCOM_COMPRESSED) {
            features = modifiers[i].drmFormatModifierTilingFeatures;
            *memory_planes = modifiers[i].drmFormatModifierPlaneCount;
            break;
        }
    }
    free(modifiers);
    return features;
}

static void capability_preflight(VkPhysicalDevice physical, VkFormat format,
                                 uint32_t queue_family)
{
    if (!has_device_extension(physical,
                              VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME))
        fail_message("VK_EXT_image_drm_format_modifier is unavailable");

    uint32_t memory_planes = 0;
    VkFormatFeatureFlags features =
        find_modifier_features(physical, format, &memory_planes);
    const VkFormatFeatureFlags required =
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    printf("capability_modifier=0x%016" PRIx64
           " memory_planes=%u features=0x%x"
           " transfer_src=%u transfer_dst=%u\n",
           QCOM_COMPRESSED, memory_planes, features,
           !!(features & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT),
           !!(features & VK_FORMAT_FEATURE_TRANSFER_DST_BIT));
    if (memory_planes != MEMORY_PLANE_COUNT ||
        (features & required) != required)
        fail_message("QCOM modifier lacks required planes or transfer features");

    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info = {
        .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .drmFormatModifier = QCOM_COMPRESSED,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 1,
        .pQueueFamilyIndices = &queue_family,
    };
    VkPhysicalDeviceImageFormatInfo2 image_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &modifier_info,
        .format = format,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
    };
    VkImageFormatProperties2 image_properties = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
    };
    VkResult result = vkGetPhysicalDeviceImageFormatProperties2(
        physical, &image_info, &image_properties);
    printf("capability_combined_usage result=%d max_extent=%ux%u"
           " max_mips=%u max_layers=%u\n",
           result, image_properties.imageFormatProperties.maxExtent.width,
           image_properties.imageFormatProperties.maxExtent.height,
           image_properties.imageFormatProperties.maxMipLevels,
           image_properties.imageFormatProperties.maxArrayLayers);
    if (result != VK_SUCCESS ||
        image_properties.imageFormatProperties.maxExtent.width < WIDTH ||
        image_properties.imageFormatProperties.maxExtent.height <
            CODED_HEIGHT ||
        image_properties.imageFormatProperties.maxMipLevels < 1 ||
        image_properties.imageFormatProperties.maxArrayLayers < 1)
        fail_message("combined TRANSFER_DST|TRANSFER_SRC usage unsupported");
    printf("capability_preflight=supported gpu_work_not_started_yet\n");
}

static struct host_buffer create_host_buffer(VkPhysicalDevice physical,
                                             VkDevice device,
                                             VkBufferUsageFlags usage,
                                             const char *label)
{
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = P010_SIZE,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    struct host_buffer result = {VK_NULL_HANDLE, VK_NULL_HANDLE, 0};
    VkResult vk_result =
        vkCreateBuffer(device, &buffer_info, NULL, &result.buffer);
    if (vk_result != VK_SUCCESS)
        fail_vk(label, vk_result);

    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, result.buffer, &requirements);
    VkMemoryPropertyFlags flags = 0;
    uint32_t memory_type = choose_memory_type(
        physical, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &flags);
    VkMemoryAllocateInfo allocation_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type,
    };
    vk_result = vkAllocateMemory(device, &allocation_info, NULL,
                                 &result.memory);
    if (vk_result != VK_SUCCESS)
        fail_vk("vkAllocateMemory(host buffer)", vk_result);
    vk_result = vkBindBufferMemory(device, result.buffer, result.memory, 0);
    if (vk_result != VK_SUCCESS)
        fail_vk("vkBindBufferMemory(host buffer)", vk_result);
    result.allocation_size = requirements.size;
    printf("%s size=%" PRIu64 " memory_type=%u flags=0x%x\n", label,
           requirements.size, memory_type, flags);
    return result;
}

static void fill_synthetic_p010(uint16_t *words)
{
    for (uint32_t y = 0; y < CODED_HEIGHT; y++) {
        for (uint32_t x = 0; x < WIDTH; x++) {
            uint16_t value =
                (uint16_t)((x * 13U + y * 29U + (x ^ y) * 3U) & 1023U);
            words[(size_t)y * WIDTH + x] = (uint16_t)(value << 6);
        }
    }
    uint16_t *uv = words + (P010_Y_SIZE / sizeof(uint16_t));
    for (uint32_t y = 0; y < CHROMA_HEIGHT; y++) {
        for (uint32_t x = 0; x < CHROMA_WIDTH; x++) {
            uint16_t u =
                (uint16_t)((x * 17U + y * 7U + 113U) & 1023U);
            uint16_t v =
                (uint16_t)((x * 5U + y * 19U + 509U) & 1023U);
            size_t index = ((size_t)y * CHROMA_WIDTH + x) * 2;
            uv[index] = (uint16_t)(u << 6);
            uv[index + 1] = (uint16_t)(v << 6);
        }
    }
}

static void write_file(const char *path, const void *data, size_t size)
{
    FILE *file = fopen(path, "wb");
    if (!file)
        fail_errno("open output file");
    if (fwrite(data, 1, size, file) != size)
        fail_errno("write output file");
    if (fclose(file) != 0)
        fail_errno("close output file");
}

static void write_y_difference_pgm(const char *path, const uint16_t *source,
                                   const uint16_t *readback)
{
    FILE *file = fopen(path, "wb");
    if (!file)
        fail_errno("open difference PGM");
    if (fprintf(file, "P5\n%u %u\n255\n", WIDTH, CODED_HEIGHT) < 0)
        fail_errno("write difference PGM header");
    for (uint64_t i = 0; i < P010_Y_SIZE / sizeof(uint16_t); i++) {
        int source_value = source[i] >> 6;
        int readback_value = readback[i] >> 6;
        unsigned difference = (unsigned)abs(readback_value - source_value);
        uint8_t pixel = (uint8_t)((difference * 255U + 511U) / 1023U);
        if (fwrite(&pixel, 1, 1, file) != 1)
            fail_errno("write difference PGM pixels");
    }
    if (fclose(file) != 0)
        fail_errno("close difference PGM");
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc != 4) {
        fprintf(stderr,
                "usage: %s p010-source.bin p010-readback.bin y-difference.pgm\n",
                argv[0]);
        return 2;
    }

    const VkFormat format =
        VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
    const VkSubresourceLayout producer_layouts[MEMORY_PLANE_COUNT] = {
        {.offset = 0, .size = 0, .rowPitch = Q10C_STRIDE},
        {.offset = 2805760, .size = 0, .rowPitch = Q10C_STRIDE},
    };
    printf("test extent=%ux%u format=%d modifier=0x%016" PRIx64
           " usage=TRANSFER_DST|TRANSFER_SRC\n",
           WIDTH, CODED_HEIGHT, format, QCOM_COMPRESSED);
    for (uint32_t i = 0; i < MEMORY_PLANE_COUNT; i++) {
        printf("memory_plane=%u offset=%" PRIu64 " row_pitch=%" PRIu64
               "\n",
               i, producer_layouts[i].offset,
               producer_layouts[i].rowPitch);
    }

    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "stereonaut-q10c-vulkan-selftest",
        .applicationVersion = 1,
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
    };
    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&instance_info, NULL, &instance);
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
        fail_message("unexpected Vulkan vendor; refusing non-Adreno path");

    uint32_t queue_family = find_transfer_queue(physical);
    capability_preflight(physical, format, queue_family);

    float priority = 0.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const char *extensions[] = {
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = extensions,
    };
    VkDevice device = VK_NULL_HANDLE;
    result = vkCreateDevice(physical, &device_info, NULL, &device);
    if (result != VK_SUCCESS)
        fail_vk("vkCreateDevice", result);
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, queue_family, 0, &queue);
    if (queue == VK_NULL_HANDLE)
        fail_message("vkGetDeviceQueue returned null");

    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_info = {
        .sType =
            VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier = QCOM_COMPRESSED,
        .drmFormatModifierPlaneCount = MEMORY_PLANE_COUNT,
        .pPlaneLayouts = producer_layouts,
    };
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &modifier_info,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {WIDTH, CODED_HEIGHT, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage image = VK_NULL_HANDLE;
    result = vkCreateImage(device, &image_info, NULL, &image);
    if (result != VK_SUCCESS)
        fail_vk("vkCreateImage(explicit QCOM layout)", result);

    PFN_vkGetImageDrmFormatModifierPropertiesEXT get_modifier =
        (PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkGetDeviceProcAddr(
            device, "vkGetImageDrmFormatModifierPropertiesEXT");
    if (!get_modifier)
        fail_message("vkGetImageDrmFormatModifierPropertiesEXT unavailable");
    VkImageDrmFormatModifierPropertiesEXT chosen_modifier = {
        .sType =
            VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
    };
    result = get_modifier(device, image, &chosen_modifier);
    if (result != VK_SUCCESS)
        fail_vk("vkGetImageDrmFormatModifierPropertiesEXT", result);
    if (chosen_modifier.drmFormatModifier != QCOM_COMPRESSED)
        fail_message("Vulkan returned a different DRM modifier");

    VkMemoryDedicatedRequirements dedicated_requirements = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
    };
    VkMemoryRequirements2 image_requirements = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        .pNext = &dedicated_requirements,
    };
    VkImageMemoryRequirementsInfo2 requirements_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = image,
    };
    vkGetImageMemoryRequirements2(device, &requirements_info,
                                  &image_requirements);
    printf("image_memory size=%" PRIu64 " alignment=%" PRIu64
           " type_bits=0x%x preferred_dedicated=%u required_dedicated=%u\n",
           image_requirements.memoryRequirements.size,
           image_requirements.memoryRequirements.alignment,
           image_requirements.memoryRequirements.memoryTypeBits,
           dedicated_requirements.prefersDedicatedAllocation,
           dedicated_requirements.requiresDedicatedAllocation);
    if (image_requirements.memoryRequirements.size != Q10C_SIZE ||
        image_requirements.memoryRequirements.alignment != 4096)
        fail_message("image memory requirements differ from confirmed layout");

    VkMemoryPropertyFlags image_flags = 0;
    uint32_t image_type = choose_memory_type(
        physical, image_requirements.memoryRequirements.memoryTypeBits, 0,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &image_flags);
    VkMemoryDedicatedAllocateInfo dedicated_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = image,
    };
    printf("dedicated_allocation_selected=1 driver_required=%u\n",
           dedicated_requirements.requiresDedicatedAllocation);
    VkMemoryAllocateInfo image_allocation_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated_info,
        .allocationSize = Q10C_SIZE,
        .memoryTypeIndex = image_type,
    };
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    result = vkAllocateMemory(device, &image_allocation_info, NULL,
                              &image_memory);
    if (result != VK_SUCCESS)
        fail_vk("vkAllocateMemory(image)", result);
    result = vkBindImageMemory(device, image, image_memory, 0);
    if (result != VK_SUCCESS)
        fail_vk("vkBindImageMemory", result);
    printf("image_memory_bound type=%u flags=0x%x bytes=%" PRIu64 "\n",
           image_type, image_flags, Q10C_SIZE);

    struct host_buffer staging = create_host_buffer(
        physical, device, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        "staging_buffer");
    struct host_buffer readback = create_host_buffer(
        physical, device, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        "readback_buffer");
    void *staging_map = NULL;
    result = vkMapMemory(device, staging.memory, 0, P010_SIZE, 0,
                         &staging_map);
    if (result != VK_SUCCESS)
        fail_vk("vkMapMemory(staging)", result);
    fill_synthetic_p010(staging_map);
    void *readback_map = NULL;
    result = vkMapMemory(device, readback.memory, 0, P010_SIZE, 0,
                         &readback_map);
    if (result != VK_SUCCESS)
        fail_vk("vkMapMemory(readback)", result);
    memset(readback_map, 0xA5, P010_SIZE);

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = queue_family,
    };
    VkCommandPool command_pool = VK_NULL_HANDLE;
    result = vkCreateCommandPool(device, &pool_info, NULL, &command_pool);
    if (result != VK_SUCCESS)
        fail_vk("vkCreateCommandPool", result);
    VkCommandBufferAllocateInfo command_allocation_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    result = vkAllocateCommandBuffers(device, &command_allocation_info,
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

    VkImageMemoryBarrier prepare_write = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         1, &prepare_write);

    const VkBufferImageCopy copies[MEMORY_PLANE_COUNT] = {
        {
            .bufferOffset = 0,
            .imageSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .imageExtent = {WIDTH, CODED_HEIGHT, 1},
        },
        {
            .bufferOffset = P010_Y_SIZE,
            .imageSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .imageExtent = {CHROMA_WIDTH, CHROMA_HEIGHT, 1},
        },
    };
    vkCmdCopyBufferToImage(command_buffer, staging.buffer, image,
                           VK_IMAGE_LAYOUT_GENERAL, MEMORY_PLANE_COUNT,
                           copies);

    VkImageMemoryBarrier write_to_read = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = prepare_write.subresourceRange,
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         1, &write_to_read);
    vkCmdCopyImageToBuffer(command_buffer, image, VK_IMAGE_LAYOUT_GENERAL,
                           readback.buffer, MEMORY_PLANE_COUNT, copies);

    VkBufferMemoryBarrier readback_to_host = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = readback.buffer,
        .offset = 0,
        .size = P010_SIZE,
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1,
                         &readback_to_host, 0, NULL);
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
    printf("gpu_fence=signaled submissions=1\n");

    const uint16_t *source_words = staging_map;
    const uint16_t *readback_words = readback_map;
    uint64_t different_words = 0;
    uint64_t different_y = 0;
    uint64_t different_uv = 0;
    uint16_t maximum_raw_difference = 0;
    for (uint64_t i = 0; i < P010_SIZE / sizeof(uint16_t); i++) {
        if (source_words[i] != readback_words[i]) {
            different_words++;
            if (i < P010_Y_SIZE / sizeof(uint16_t))
                different_y++;
            else
                different_uv++;
            unsigned difference = (unsigned)abs(
                (int)source_words[i] - (int)readback_words[i]);
            if (difference > maximum_raw_difference)
                maximum_raw_difference = (uint16_t)difference;
        }
    }
    printf("comparison words=%" PRIu64 " different=%" PRIu64
           " y_different=%" PRIu64 " uv_component_different=%" PRIu64
           " max_raw_difference=%u\n",
           P010_SIZE / sizeof(uint16_t), different_words, different_y,
           different_uv, maximum_raw_difference);
    write_file(argv[1], staging_map, P010_SIZE);
    write_file(argv[2], readback_map, P010_SIZE);
    if (different_words)
        write_y_difference_pgm(argv[3], source_words, readback_words);
    printf("source_written=%s readback_written=%s bytes=%" PRIu64 "\n",
           argv[1], argv[2], P010_SIZE);
    if (different_words)
        printf("difference_image_written=%s\n", argv[3]);

    vkUnmapMemory(device, readback.memory);
    vkUnmapMemory(device, staging.memory);
    vkDestroyFence(device, fence, NULL);
    vkDestroyCommandPool(device, command_pool, NULL);
    vkDestroyBuffer(device, readback.buffer, NULL);
    vkFreeMemory(device, readback.memory, NULL);
    vkDestroyBuffer(device, staging.buffer, NULL);
    vkFreeMemory(device, staging.memory, NULL);
    vkDestroyImage(device, image, NULL);
    vkFreeMemory(device, image_memory, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);

    printf("result=%s\n", different_words ? "mismatch" : "exact_match");
    return different_words ? 1 : 0;
}
