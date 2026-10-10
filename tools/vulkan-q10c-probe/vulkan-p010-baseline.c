/*
 * Decoder-free P010 baseline with driver-selected QCOM layout.
 * copy: transfer roundtrip; sample-identity: joint YCbCr identity sampler into a storage buffer.
 * buffer/linear: controls isolating image/UBWC processing.
 * No explicit producer offsets or pitches are accepted.
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
#define P010_Y_SIZE UINT64_C(4177920)
#define P010_UV_SIZE UINT64_C(2088960)
#define P010_SIZE (P010_Y_SIZE + P010_UV_SIZE)
#define QCOM_COMPRESSED UINT64_C(0x0500000000000001)
#define MEMORY_PLANE_COUNT 2U

static int sample_mode;
static int buffer_only;
static uint64_t selected_modifier;
static uint32_t sampler_descriptors;
static VkImageUsageFlags test_usage;
static VkImageCreateFlags test_flags;

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
        if ((properties[i].queueFlags & (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_COMPUTE_BIT)) {
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
        if (modifiers[i].drmFormatModifier == selected_modifier) {
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
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
        (sample_mode ? VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT :
                       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT);
    printf("capability_modifier=0x%016" PRIx64
           " memory_planes=%u features=0x%x"
           " transfer_src=%u transfer_dst=%u\n",
           selected_modifier, memory_planes, features,
           !!(features & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT),
           !!(features & VK_FORMAT_FEATURE_TRANSFER_DST_BIT));
    if (memory_planes != MEMORY_PLANE_COUNT ||
        (features & required) != required)
        fail_message("QCOM modifier lacks required planes or transfer features");

    if (sample_mode && !(features & VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT))
        fail_message("midpoint chroma sampling unsupported");

    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info = {
        .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .drmFormatModifier = selected_modifier,
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
        .usage = test_usage,
        .flags = test_flags,
    };
    VkSamplerYcbcrConversionImageFormatProperties conversion_properties = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES,
    };
    VkImageFormatProperties2 image_properties = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        .pNext = &conversion_properties,
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
        fail_message("requested usage/flags unsupported");
    sampler_descriptors = conversion_properties.combinedImageSamplerDescriptorCount;
    if (sample_mode && !sampler_descriptors) fail_message("invalid YCbCr descriptor count");
    printf("sampler_descriptors=%u\n", sampler_descriptors);
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

static VKAPI_ATTR VkBool32 VKAPI_CALL validation_message(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT *data, void *user)
{
    (void)severity; (void)type; (void)user;
    fprintf(stderr, "validation_error=%s\n", data->pMessage);
    /* Do not submit additional GPU work after a validation error. */
    exit(2);
}

struct sample_resources {
    VkImageView view;
    VkSamplerYcbcrConversion conversion;
    VkSampler sampler;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    VkPipelineLayout pipeline_layout;
    VkShaderModule shader;
    VkPipeline pipeline;
};

#define CHECK(call) do { VkResult checked = (call); \
    if (checked != VK_SUCCESS) fail_vk(#call, checked); } while (0)

static struct sample_resources create_sampling(VkDevice device, VkImage image,
                                                VkBuffer output)
{
    struct sample_resources s = {0};
    VkSamplerYcbcrConversionCreateInfo conversion = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
        .format = VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,
        .ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY,
        .ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL,
        .xChromaOffset = VK_CHROMA_LOCATION_MIDPOINT,
        .yChromaOffset = VK_CHROMA_LOCATION_MIDPOINT,
        .chromaFilter = VK_FILTER_NEAREST,
    };
    CHECK(vkCreateSamplerYcbcrConversion(device, &conversion, NULL, &s.conversion));
    VkSamplerYcbcrConversionInfo conversion_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
        .conversion = s.conversion,
    };
    VkImageViewCreateInfo view = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = &conversion_info,
        .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = conversion.format,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .levelCount = 1, .layerCount = 1},
    };
    CHECK(vkCreateImageView(device, &view, NULL, &s.view));
    VkSamplerCreateInfo sampler = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = &conversion_info,
        .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 0,
    };
    CHECK(vkCreateSampler(device, &sampler, NULL, &s.sampler));
    VkDescriptorSetLayoutBinding bindings[2] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
         .pImmutableSamplers = &s.sampler},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
    };
    VkDescriptorSetLayoutCreateInfo layout = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = bindings,
    };
    CHECK(vkCreateDescriptorSetLayout(device, &layout, NULL, &s.set_layout));
    VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sampler_descriptors},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
    };
    VkDescriptorPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = sizes,
    };
    CHECK(vkCreateDescriptorPool(device, &pool, NULL, &s.pool));
    VkDescriptorSetAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = s.pool, .descriptorSetCount = 1, .pSetLayouts = &s.set_layout,
    };
    CHECK(vkAllocateDescriptorSets(device, &allocate, &s.set));
    VkDescriptorImageInfo sampled_image = {s.sampler, s.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo buffer = {output, 0, P010_SIZE};
    VkWriteDescriptorSet writes[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s.set,
         .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &sampled_image},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s.set,
         .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &buffer},
    };
    vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
    VkPipelineLayoutCreateInfo pipeline_layout = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &s.set_layout,
    };
    CHECK(vkCreatePipelineLayout(device, &pipeline_layout, NULL, &s.pipeline_layout));
    FILE *file = fopen("p010-identity.comp.spv", "rb");
    if (!file) fail_errno("open compute shader");
    if (fseek(file, 0, SEEK_END)) fail_errno("seek shader");
    long length = ftell(file);
    if (length <= 0 || length % 4 || length > 1048576) fail_message("invalid shader length");
    rewind(file);
    uint32_t *code = malloc((size_t)length);
    if (!code) fail_message("allocate shader");
    if (fread(code, 1, (size_t)length, file) != (size_t)length) fail_message("read shader");
    if (fclose(file)) fail_errno("close shader");
    VkShaderModuleCreateInfo shader = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = (size_t)length, .pCode = code,
    };
    CHECK(vkCreateShaderModule(device, &shader, NULL, &s.shader));
    free(code);
    VkComputePipelineCreateInfo pipeline = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = s.shader, .pName = "main"},
        .layout = s.pipeline_layout,
    };
    CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline, NULL, &s.pipeline));
    return s;
}

static void destroy_sampling(VkDevice device, struct sample_resources *s)
{
    vkDestroyPipeline(device, s->pipeline, NULL);
    vkDestroyShaderModule(device, s->shader, NULL);
    vkDestroyPipelineLayout(device, s->pipeline_layout, NULL);
    vkDestroyDescriptorPool(device, s->pool, NULL);
    vkDestroyDescriptorSetLayout(device, s->set_layout, NULL);
    vkDestroySampler(device, s->sampler, NULL);
    vkDestroyImageView(device, s->view, NULL);
    vkDestroySamplerYcbcrConversion(device, s->conversion, NULL);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc != 5 || (strcmp(argv[1], "copy") && strcmp(argv[1], "sample-identity") &&
                     strcmp(argv[1], "buffer") && strcmp(argv[1], "linear"))) {
        fprintf(stderr,
                "usage: %s copy|sample-identity|buffer|linear source-before.bin readback.bin source-after.bin\n",
                argv[0]);
        return 2;
    }

    const VkFormat format =
        VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
    sample_mode = strcmp(argv[1], "sample-identity") == 0;
    buffer_only = strcmp(argv[1], "buffer") == 0;
    selected_modifier = strcmp(argv[1], "linear") == 0 ? 0 : QCOM_COMPRESSED;
    test_usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 (sample_mode ? VK_IMAGE_USAGE_SAMPLED_BIT : VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    test_flags = 0;
    printf("test mode=%s extent=%ux%u modifier=0x%016" PRIx64
           " usage=0x%x flags=0x%x layout=driver-selected\n",
           argv[1], WIDTH, CODED_HEIGHT, selected_modifier, test_usage, test_flags);

    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "stereonaut-p010-baseline",
        .applicationVersion = 1,
        .apiVersion = VK_API_VERSION_1_1,
    };
    uint32_t layer_count = 0;
    if (vkEnumerateInstanceLayerProperties(&layer_count, NULL) != VK_SUCCESS)
        fail_message("enumerate instance layers");
    VkLayerProperties *layers = calloc(layer_count ? layer_count : 1, sizeof(*layers));
    if (!layers) fail_message("allocate layers");
    if (vkEnumerateInstanceLayerProperties(&layer_count, layers) != VK_SUCCESS)
        fail_message("enumerate layer list");
    const char *validation = "VK_LAYER_KHRONOS_validation";
    uint32_t enable_validation = 0;
    for (uint32_t i = 0; i < layer_count; ++i)
        if (!strcmp(layers[i].layerName, validation)) enable_validation = 1;
    free(layers);
    printf("validation_layer=%s\n", enable_validation ? "enabled" : "unavailable");
    const char *debug_extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkDebugUtilsMessengerCreateInfoEXT debug_info = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = validation_message,
    };
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
        .enabledLayerCount = enable_validation,
        .ppEnabledLayerNames = &validation,
        .enabledExtensionCount = enable_validation,
        .ppEnabledExtensionNames = &debug_extension,
        .pNext = enable_validation ? &debug_info : NULL,
    };
    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&instance_info, NULL, &instance);
    if (result != VK_SUCCESS)
        fail_vk("vkCreateInstance", result);

    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    if (enable_validation) {
        PFN_vkCreateDebugUtilsMessengerEXT create_debug =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
        if (!create_debug) fail_message("missing debug messenger entry point");
        result = create_debug(instance, &debug_info, NULL, &messenger);
        if (result != VK_SUCCESS) fail_vk("create debug messenger", result);
    }
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
    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
    };
    VkPhysicalDeviceFeatures2 features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &ycbcr,
    };
    vkGetPhysicalDeviceFeatures2(physical, &features);
    if (!ycbcr.samplerYcbcrConversion) fail_message("samplerYcbcrConversion unsupported");
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &ycbcr,
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

    const uint64_t modifier = selected_modifier;
    VkImageDrmFormatModifierListCreateInfoEXT modifier_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
        .drmFormatModifierCount = 1, .pDrmFormatModifiers = &modifier,
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
        .usage = test_usage,
        .flags = test_flags,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage image = VK_NULL_HANDLE;
    result = vkCreateImage(device, &image_info, NULL, &image);
    if (result != VK_SUCCESS)
        fail_vk("vkCreateImage(driver-selected layout)", result);

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
    if (chosen_modifier.drmFormatModifier != selected_modifier)
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
    if (!image_requirements.memoryRequirements.size ||
        image_requirements.memoryRequirements.size > UINT64_C(16777216))
        fail_message("unexpected image size above baseline safety bound");
    for (uint32_t plane = 0; plane < MEMORY_PLANE_COUNT; ++plane) {
        VkImageSubresource subresource = {
            .aspectMask = plane ? VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT :
                                  VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
        };
        VkSubresourceLayout layout;
        vkGetImageSubresourceLayout(device, image, &subresource, &layout);
        printf("driver_memory_plane=%u offset=%" PRIu64 " row_pitch=%" PRIu64
               " size=%" PRIu64 "\n", plane, layout.offset, layout.rowPitch, layout.size);
    }

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
        .allocationSize = image_requirements.memoryRequirements.size,
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
           image_type, image_flags, image_requirements.memoryRequirements.size);

    struct host_buffer staging = create_host_buffer(
        physical, device, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        "staging_buffer");
    struct host_buffer readback = create_host_buffer(
        physical, device, sample_mode ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        "readback_buffer");
    void *staging_map = NULL;
    result = vkMapMemory(device, staging.memory, 0, P010_SIZE, 0,
                         &staging_map);
    if (result != VK_SUCCESS)
        fail_vk("vkMapMemory(staging)", result);
    uint16_t *reference = malloc(P010_SIZE);
    if (!reference) fail_message("allocate immutable reference");
    fill_synthetic_p010(reference);
    memcpy(staging_map, reference, P010_SIZE);
    write_file(argv[2], reference, P010_SIZE);
    void *readback_map = NULL;
    result = vkMapMemory(device, readback.memory, 0, P010_SIZE, 0,
                         &readback_map);
    if (result != VK_SUCCESS)
        fail_vk("vkMapMemory(readback)", result);
    memset(readback_map, 0xA5, P010_SIZE);

    struct sample_resources sampling = {0};
    if (sample_mode)
        sampling = create_sampling(device, image, readback.buffer);
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

    if (buffer_only) {
        /* Keep allocation order identical, but never access the image on the GPU. */
        VkBufferCopy copy = {.size = P010_SIZE};
        vkCmdCopyBuffer(command_buffer, staging.buffer, readback.buffer, 1, &copy);
    } else {
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
            .dstAccessMask = sample_mode ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = prepare_write.subresourceRange,
        };
        vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             (sample_mode ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT),
                             0, 0, NULL, 0, NULL, 1, &write_to_read);
        if (sample_mode) {
            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, sampling.pipeline);
            vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    sampling.pipeline_layout, 0, 1, &sampling.set, 0, NULL);
            vkCmdDispatch(command_buffer, CHROMA_WIDTH / 16, CODED_HEIGHT / 8, 1);
        } else {
            vkCmdCopyImageToBuffer(command_buffer, image, VK_IMAGE_LAYOUT_GENERAL,
                                   readback.buffer, MEMORY_PLANE_COUNT, copies);
        }

    }

    VkBufferMemoryBarrier readback_to_host = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = sample_mode ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = readback.buffer,
        .offset = 0,
        .size = P010_SIZE,
    };
    vkCmdPipelineBarrier(command_buffer,
                         sample_mode ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
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

    const uint16_t *readback_words = readback_map;
    const uint16_t *source_after = staging_map;
    uint64_t different[3] = {0}, source_changes = 0, raw_different = 0;
    uint64_t first = UINT64_MAX;
    unsigned maximum = 0;
    for (uint64_t i = 0; i < P010_SIZE / 2; ++i) {
        source_changes += source_after[i] != reference[i];
        raw_different += readback_words[i] != reference[i];
        unsigned wanted = reference[i] >> 6, got = readback_words[i] >> 6;
        if (wanted != got) {
            unsigned plane = i < P010_Y_SIZE / 2 ? 0 : 1 + (i & 1);
            ++different[plane];
            if (first == UINT64_MAX) first = i;
            unsigned delta = wanted > got ? wanted - got : got - wanted;
            if (delta > maximum) maximum = delta;
        }
    }
    printf("comparison y_different=%" PRIu64 " u_different=%" PRIu64
           " v_different=%" PRIu64 " raw_word_different=%" PRIu64
           " source_changes=%" PRIu64 " max_10bit_difference=%u\n",
           different[0], different[1], different[2], raw_different, source_changes, maximum);
    if (first != UINT64_MAX)
        printf("first_mismatch word=%" PRIu64 " expected=%u got=%u\n",
               first, reference[first] >> 6, readback_words[first] >> 6);
    write_file(argv[3], readback_map, P010_SIZE);
    write_file(argv[4], staging_map, P010_SIZE);
    int mismatch = source_changes || different[0] || different[1] || different[2];
    printf("result=%s mode=%s\n", mismatch ? "mismatch" : "exact_10bit_match", argv[1]);
    free(reference);
    if (sample_mode) destroy_sampling(device, &sampling);

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
    if (messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
        if (!destroy_debug) fail_message("missing destroy debug messenger");
        destroy_debug(instance, messenger, NULL);
    }
    vkDestroyInstance(instance, NULL);

    return mismatch ? 1 : 0;
}
