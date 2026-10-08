/*
 * Create one unbound Vulkan image with the exact Iris Q10C producer layout.
 * This probe never allocates or binds memory and never submits GPU work.
 */
#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>

#define QCOM_COMPRESSED UINT64_C(0x0500000000000001)
#define IRIS_BUFFER_SIZE UINT64_C(4210688)
#define IRIS_PLANE_COUNT 2U

static void fail(const char *what, VkResult result)
{
    fprintf(stderr, "%s: VkResult=%d\n", what, result);
    exit(2);
}

static uint32_t find_queue_family(VkPhysicalDevice physical)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    VkQueueFamilyProperties *props = calloc(count, sizeof(*props));
    if (!props) {
        perror("calloc queue families");
        exit(2);
    }
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, props);
    for (uint32_t i = 0; i < count; i++) {
        if (props[i].queueFlags &
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT |
             VK_QUEUE_TRANSFER_BIT)) {
            free(props);
            return i;
        }
    }
    free(props);
    fprintf(stderr, "no usable Vulkan queue family\n");
    exit(2);
}

static uint32_t modifier_plane_count(VkPhysicalDevice physical, VkFormat format)
{
    VkDrmFormatModifierPropertiesListEXT list = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
    };
    VkFormatProperties2 props = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &list,
    };
    vkGetPhysicalDeviceFormatProperties2(physical, format, &props);
    VkDrmFormatModifierPropertiesEXT *mods =
        calloc(list.drmFormatModifierCount, sizeof(*mods));
    if (!mods) {
        perror("calloc modifiers");
        exit(2);
    }
    list.pDrmFormatModifierProperties = mods;
    vkGetPhysicalDeviceFormatProperties2(physical, format, &props);

    uint32_t planes = 0;
    for (uint32_t i = 0; i < list.drmFormatModifierCount; i++) {
        if (mods[i].drmFormatModifier == QCOM_COMPRESSED) {
            planes = mods[i].drmFormatModifierPlaneCount;
            printf("enumerated_modifier=0x%016" PRIx64
                   " memory_planes=%u features=0x%x\n",
                   mods[i].drmFormatModifier, planes,
                   mods[i].drmFormatModifierTilingFeatures);
        }
    }
    free(mods);
    return planes;
}

int main(void)
{
    const VkFormat format =
        VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
    const VkSubresourceLayout producer_layouts[IRIS_PLANE_COUNT] = {
        {
            .offset = 0,
            .size = 0,
            .rowPitch = 2560,
            .arrayPitch = 0,
            .depthPitch = 0,
        },
        {
            .offset = 2805760,
            .size = 0,
            .rowPitch = 2560,
            .arrayPitch = 0,
            .depthPitch = 0,
        },
    };

    printf("requested_modifier=0x%016" PRIx64
           " extent=1920x1088 format=%d usage=SAMPLED|TRANSFER_SRC\n",
           QCOM_COMPRESSED, format);
    for (uint32_t i = 0; i < IRIS_PLANE_COUNT; i++) {
        printf("requested_memory_plane=%u offset=%" PRIu64
               " row_pitch=%" PRIu64 "\n",
               i, producer_layouts[i].offset,
               producer_layouts[i].rowPitch);
    }

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "stereonaut-vulkan-explicit-layout-probe",
        .applicationVersion = 1,
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&instance_info, NULL, &instance);
    if (result != VK_SUCCESS)
        fail("vkCreateInstance", result);

    uint32_t physical_count = 0;
    result = vkEnumeratePhysicalDevices(instance, &physical_count, NULL);
    if (result != VK_SUCCESS || physical_count == 0)
        fail("vkEnumeratePhysicalDevices(count)", result);
    VkPhysicalDevice *physicals = calloc(physical_count, sizeof(*physicals));
    if (!physicals) {
        perror("calloc physical devices");
        return 2;
    }
    result = vkEnumeratePhysicalDevices(instance, &physical_count, physicals);
    if (result != VK_SUCCESS)
        fail("vkEnumeratePhysicalDevices(list)", result);
    VkPhysicalDevice physical = physicals[0];
    free(physicals);

    const uint32_t planes = modifier_plane_count(physical, format);
    if (planes != IRIS_PLANE_COUNT) {
        fprintf(stderr,
                "QCOM compressed modifier has %u memory planes; expected %u\n",
                planes, IRIS_PLANE_COUNT);
        return 1;
    }

    const uint32_t queue_family = find_queue_family(physical);
    const float priority = 0.0f;
    const VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const char *extensions[] = {
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    };
    const VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = sizeof(extensions) / sizeof(extensions[0]),
        .ppEnabledExtensionNames = extensions,
    };
    VkDevice device = VK_NULL_HANDLE;
    result = vkCreateDevice(physical, &device_info, NULL, &device);
    if (result != VK_SUCCESS)
        fail("vkCreateDevice", result);

    const VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_info = {
        .sType =
            VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier = QCOM_COMPRESSED,
        .drmFormatModifierPlaneCount = IRIS_PLANE_COUNT,
        .pPlaneLayouts = producer_layouts,
    };
    const VkExternalMemoryImageCreateInfo external_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &modifier_info,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    const VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external_info,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {1920, 1088, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage image = VK_NULL_HANDLE;
    result = vkCreateImage(device, &image_info, NULL, &image);
    printf("create_image result=%d\n", result);
    if (result != VK_SUCCESS)
        fail("vkCreateImage", result);

    PFN_vkGetImageDrmFormatModifierPropertiesEXT get_modifier =
        (PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkGetDeviceProcAddr(
            device, "vkGetImageDrmFormatModifierPropertiesEXT");
    if (!get_modifier) {
        fprintf(stderr,
                "vkGetImageDrmFormatModifierPropertiesEXT unavailable\n");
        return 2;
    }
    VkImageDrmFormatModifierPropertiesEXT chosen = {
        .sType =
            VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
    };
    result = get_modifier(device, image, &chosen);
    if (result != VK_SUCCESS)
        fail("vkGetImageDrmFormatModifierPropertiesEXT", result);
    printf("chosen_modifier=0x%016" PRIx64 "\n", chosen.drmFormatModifier);

    VkMemoryDedicatedRequirements dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
    };
    VkMemoryRequirements2 requirements = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        .pNext = &dedicated,
    };
    const VkImageMemoryRequirementsInfo2 requirements_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = image,
    };
    vkGetImageMemoryRequirements2(device, &requirements_info, &requirements);
    printf("memory size=%" PRIu64 " alignment=%" PRIu64
           " memory_type_bits=0x%x prefers_dedicated=%u"
           " requires_dedicated=%u iris_buffer_size=%" PRIu64 "\n",
           requirements.memoryRequirements.size,
           requirements.memoryRequirements.alignment,
           requirements.memoryRequirements.memoryTypeBits,
           dedicated.prefersDedicatedAllocation,
           dedicated.requiresDedicatedAllocation, IRIS_BUFFER_SIZE);

    int exit_status = 0;
    if (chosen.drmFormatModifier != QCOM_COMPRESSED) {
        fprintf(stderr, "returned modifier does not match QCOM compressed\n");
        exit_status = 1;
    }
    if (requirements.memoryRequirements.size > IRIS_BUFFER_SIZE) {
        fprintf(stderr,
                "memory requirement exceeds the Iris buffer by %" PRIu64
                " bytes\n",
                requirements.memoryRequirements.size - IRIS_BUFFER_SIZE);
        exit_status = 1;
    }

    vkDestroyImage(device, image, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    return exit_status;
}
