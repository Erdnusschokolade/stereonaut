/* Enumerate DRM modifiers and DMA-BUF import properties for relevant YUV formats. */
#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define QCOM_COMPRESSED UINT64_C(0x0500000000000001)

struct format_desc {
    VkFormat format;
    const char *name;
    unsigned bits;
    unsigned format_planes;
};

static const struct format_desc formats[] = {
    {VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
     "VK_FORMAT_G8_B8R8_2PLANE_420_UNORM", 8, 2},
    {VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM,
     "VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM", 8, 3},
    {VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,
     "VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16", 10, 2},
    {VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16,
     "VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16", 10, 3},
};

static void fail(const char *what, VkResult result)
{
    fprintf(stderr, "%s: VkResult=%d\n", what, result);
    exit(2);
}

static bool has_extension(VkPhysicalDevice physical, const char *name)
{
    uint32_t count = 0;
    VkResult result = vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL);
    if (result != VK_SUCCESS)
        fail("vkEnumerateDeviceExtensionProperties(count)", result);
    VkExtensionProperties *props = calloc(count, sizeof(*props));
    if (!props) {
        perror("calloc extensions");
        exit(2);
    }
    result = vkEnumerateDeviceExtensionProperties(physical, NULL, &count, props);
    if (result != VK_SUCCESS)
        fail("vkEnumerateDeviceExtensionProperties(list)", result);
    bool found = false;
    for (uint32_t i = 0; i < count; i++)
        if (!strcmp(props[i].extensionName, name))
            found = true;
    free(props);
    return found;
}

static void print_feature(const char *name, VkFormatFeatureFlags value,
                          VkFormatFeatureFlagBits bit)
{
    printf(" %s=%s", name, (value & bit) ? "yes" : "no");
}

static void print_modifier_name(uint64_t modifier)
{
    if (modifier == 0)
        printf(" modifier_name=DRM_FORMAT_MOD_LINEAR");
    else if (modifier == QCOM_COMPRESSED)
        printf(" modifier_name=DRM_FORMAT_MOD_QCOM_COMPRESSED");
    else
        printf(" modifier_name=unknown");
}

static void query_external(VkPhysicalDevice physical, const struct format_desc *desc,
                           uint64_t modifier, VkImageUsageFlags usage,
                           const char *usage_name)
{
    VkPhysicalDeviceExternalImageFormatInfo external = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT drm = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .pNext = &external,
        .drmFormatModifier = modifier,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkPhysicalDeviceImageFormatInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &drm,
        .format = desc->format,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = usage,
    };
    VkExternalImageFormatProperties external_props = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
    };
    VkImageFormatProperties2 props = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        .pNext = &external_props,
    };
    VkResult result = vkGetPhysicalDeviceImageFormatProperties2(physical, &info, &props);
    printf("  dma_buf_query usage=%s result=%d", usage_name, result);
    if (result == VK_SUCCESS) {
        VkExternalMemoryProperties p = external_props.externalMemoryProperties;
        printf(" max_extent=%ux%u max_resource=%" PRIu64,
               props.imageFormatProperties.maxExtent.width,
               props.imageFormatProperties.maxExtent.height,
               props.imageFormatProperties.maxResourceSize);
        printf(" external_features=0x%x compatible_handles=0x%x export_from_imported=0x%x",
               p.externalMemoryFeatures, p.compatibleHandleTypes,
               p.exportFromImportedHandleTypes);
        printf(" importable=%s exportable=%s dedicated_only=%s",
               (p.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ? "yes" : "no",
               (p.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) ? "yes" : "no",
               (p.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) ? "yes" : "no");
    }
    putchar('\n');
}

static void inspect_format(VkPhysicalDevice physical, const struct format_desc *desc)
{
    VkDrmFormatModifierPropertiesListEXT list = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
    };
    VkFormatProperties2 props = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &list,
    };
    vkGetPhysicalDeviceFormatProperties2(physical, desc->format, &props);
    printf("format=%s bits=%u logical_format_planes=%u modifiers=%u\n",
           desc->name, desc->bits, desc->format_planes, list.drmFormatModifierCount);
    printf(" optimal_features=0x%x", props.formatProperties.optimalTilingFeatures);
    print_feature("sampled", props.formatProperties.optimalTilingFeatures,
                  VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    print_feature("transfer_src", props.formatProperties.optimalTilingFeatures,
                  VK_FORMAT_FEATURE_TRANSFER_SRC_BIT);
    print_feature("transfer_dst", props.formatProperties.optimalTilingFeatures,
                  VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
    print_feature("linear_filter", props.formatProperties.optimalTilingFeatures,
                  VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    print_feature("ycbcr_linear_filter", props.formatProperties.optimalTilingFeatures,
                  VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT);
    putchar('\n');

    if (!list.drmFormatModifierCount)
        return;
    list.pDrmFormatModifierProperties = calloc(list.drmFormatModifierCount,
                                                sizeof(*list.pDrmFormatModifierProperties));
    if (!list.pDrmFormatModifierProperties) {
        perror("calloc modifiers");
        exit(2);
    }
    uint32_t capacity = list.drmFormatModifierCount;
    vkGetPhysicalDeviceFormatProperties2(physical, desc->format, &props);
    if (list.drmFormatModifierCount > capacity) {
        fprintf(stderr, "modifier list grew unexpectedly\n");
        exit(2);
    }
    for (uint32_t i = 0; i < list.drmFormatModifierCount; i++) {
        const VkDrmFormatModifierPropertiesEXT *p = &list.pDrmFormatModifierProperties[i];
        printf(" modifier=0x%016" PRIx64, p->drmFormatModifier);
        print_modifier_name(p->drmFormatModifier);
        printf(" memory_planes=%u features=0x%x", p->drmFormatModifierPlaneCount,
               p->drmFormatModifierTilingFeatures);
        print_feature("sampled", p->drmFormatModifierTilingFeatures,
                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
        print_feature("transfer_src", p->drmFormatModifierTilingFeatures,
                      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT);
        print_feature("transfer_dst", p->drmFormatModifierTilingFeatures,
                      VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
        putchar('\n');
        query_external(physical, desc, p->drmFormatModifier,
                       VK_IMAGE_USAGE_SAMPLED_BIT, "sampled");
        query_external(physical, desc, p->drmFormatModifier,
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "transfer_src");
        query_external(physical, desc, p->drmFormatModifier,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                       "sampled+transfer_src");
    }
    free(list.pDrmFormatModifierProperties);
}

int main(void)
{
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "stereonaut-vulkan-format-probe",
        .applicationVersion = 1,
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo create = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&create, NULL, &instance);
    if (result != VK_SUCCESS)
        fail("vkCreateInstance", result);

    uint32_t count = 0;
    result = vkEnumeratePhysicalDevices(instance, &count, NULL);
    if (result != VK_SUCCESS || !count)
        fail("vkEnumeratePhysicalDevices(count)", result);
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    if (!devices) {
        perror("calloc devices");
        return 2;
    }
    result = vkEnumeratePhysicalDevices(instance, &count, devices);
    if (result != VK_SUCCESS)
        fail("vkEnumeratePhysicalDevices(list)", result);

    VkPhysicalDevice physical = devices[0];
    VkPhysicalDeviceProperties device_props;
    vkGetPhysicalDeviceProperties(physical, &device_props);
    printf("device=%s vendor=0x%04x device=0x%04x api=%u.%u.%u driver=0x%x\n",
           device_props.deviceName, device_props.vendorID, device_props.deviceID,
           VK_API_VERSION_MAJOR(device_props.apiVersion),
           VK_API_VERSION_MINOR(device_props.apiVersion),
           VK_API_VERSION_PATCH(device_props.apiVersion), device_props.driverVersion);

    const char *required[] = {
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
    };
    bool all = true;
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        bool present = has_extension(physical, required[i]);
        printf("extension=%s present=%s\n", required[i], present ? "yes" : "no");
        all &= present;
    }
    if (!all) {
        fprintf(stderr, "required extension missing\n");
        return 1;
    }

    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++)
        inspect_format(physical, &formats[i]);

    free(devices);
    vkDestroyInstance(instance, NULL);
    return 0;
}
