/* Minimal OpenXR XR_KHR_vulkan_enable2 capability probe for Steam Frame. */
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_PLATFORM_XLIB

#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

static void fail_xr(const char *operation, XrResult result)
{
    fprintf(stderr, "%s: XrResult=%d\n", operation, result);
    exit(2);
}

static void require_xr(const char *operation, XrResult result)
{
    if (XR_FAILED(result))
        fail_xr(operation, result);
}

static void require_vk(const char *operation, VkResult result)
{
    if (result != VK_SUCCESS) {
        fprintf(stderr, "%s: VkResult=%d\n", operation, result);
        exit(2);
    }
}

int main(void)
{
    uint32_t extension_count = 0;
    require_xr("xrEnumerateInstanceExtensionProperties(count)",
               xrEnumerateInstanceExtensionProperties(NULL, 0,
                                                      &extension_count, NULL));
    XrExtensionProperties *extensions = calloc(extension_count,
                                                sizeof(*extensions));
    if (!extensions) {
        perror("calloc OpenXR extensions");
        return 2;
    }
    for (uint32_t index = 0; index < extension_count; index++)
        extensions[index].type = XR_TYPE_EXTENSION_PROPERTIES;
    require_xr("xrEnumerateInstanceExtensionProperties(list)",
               xrEnumerateInstanceExtensionProperties(NULL, extension_count,
                                                      &extension_count,
                                                      extensions));
    bool have_vulkan_enable2 = false;
    for (uint32_t index = 0; index < extension_count; index++) {
        if (strcmp(extensions[index].extensionName,
                   XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME) == 0) {
            have_vulkan_enable2 = true;
            printf("openxr_extension name=%s version=%u\n",
                   extensions[index].extensionName,
                   extensions[index].extensionVersion);
        }
    }
    free(extensions);
    if (!have_vulkan_enable2) {
        fprintf(stderr, "XR_KHR_vulkan_enable2 is unavailable\n");
        return 2;
    }

    XrInstanceCreateInfo create_info = {
        .type = XR_TYPE_INSTANCE_CREATE_INFO,
        .applicationInfo = {
            .applicationVersion = 1,
            .engineVersion = 1,
            .apiVersion = XR_CURRENT_API_VERSION,
        },
        .enabledExtensionCount = 1,
        .enabledExtensionNames =
            (const char *const[]){XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME},
    };
    snprintf(create_info.applicationInfo.applicationName,
             sizeof(create_info.applicationInfo.applicationName),
             "stereonaut-xr-vulkan-probe");
    snprintf(create_info.applicationInfo.engineName,
             sizeof(create_info.applicationInfo.engineName), "stereonaut");

    XrInstance instance = XR_NULL_HANDLE;
    require_xr("xrCreateInstance", xrCreateInstance(&create_info, &instance));

    XrInstanceProperties instance_properties = {
        .type = XR_TYPE_INSTANCE_PROPERTIES,
    };
    require_xr("xrGetInstanceProperties",
               xrGetInstanceProperties(instance, &instance_properties));
    printf("openxr_runtime name=%s version=%" PRIu64 "\n",
           instance_properties.runtimeName, instance_properties.runtimeVersion);

    XrSystemGetInfo system_info = {
        .type = XR_TYPE_SYSTEM_GET_INFO,
        .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
    };
    XrSystemId system_id = XR_NULL_SYSTEM_ID;
    require_xr("xrGetSystem", xrGetSystem(instance, &system_info, &system_id));

    XrSystemProperties system_properties = {
        .type = XR_TYPE_SYSTEM_PROPERTIES,
    };
    require_xr("xrGetSystemProperties",
               xrGetSystemProperties(instance, system_id, &system_properties));
    printf("openxr_system id=%" PRIu64 " name=%s max_swapchain=%ux%u\n",
           system_id, system_properties.systemName,
           system_properties.graphicsProperties.maxSwapchainImageWidth,
           system_properties.graphicsProperties.maxSwapchainImageHeight);

    PFN_xrGetVulkanGraphicsRequirements2KHR get_requirements = NULL;
    PFN_xrCreateVulkanInstanceKHR create_vulkan_instance = NULL;
    PFN_xrGetVulkanGraphicsDevice2KHR get_vulkan_device = NULL;
    require_xr("xrGetInstanceProcAddr(requirements2)",
               xrGetInstanceProcAddr(instance,
                   "xrGetVulkanGraphicsRequirements2KHR",
                   (PFN_xrVoidFunction *)&get_requirements));
    require_xr("xrGetInstanceProcAddr(createVulkanInstance)",
               xrGetInstanceProcAddr(instance, "xrCreateVulkanInstanceKHR",
                   (PFN_xrVoidFunction *)&create_vulkan_instance));
    require_xr("xrGetInstanceProcAddr(getVulkanDevice2)",
               xrGetInstanceProcAddr(instance,
                   "xrGetVulkanGraphicsDevice2KHR",
                   (PFN_xrVoidFunction *)&get_vulkan_device));
    if (!get_requirements || !create_vulkan_instance || !get_vulkan_device) {
        fprintf(stderr, "XR_KHR_vulkan_enable2 function is missing\n");
        return 2;
    }

    XrGraphicsRequirementsVulkanKHR requirements = {
        .type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR,
    };
    require_xr("xrGetVulkanGraphicsRequirements2KHR",
               get_requirements(instance, system_id, &requirements));
    printf("openxr_vulkan_requirements min_api=%u.%u.%u max_api=%u.%u.%u\n",
           VK_VERSION_MAJOR(requirements.minApiVersionSupported),
           VK_VERSION_MINOR(requirements.minApiVersionSupported),
           VK_VERSION_PATCH(requirements.minApiVersionSupported),
           VK_VERSION_MAJOR(requirements.maxApiVersionSupported),
           VK_VERSION_MINOR(requirements.maxApiVersionSupported),
           VK_VERSION_PATCH(requirements.maxApiVersionSupported));

    VkApplicationInfo vk_application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "stereonaut-xr-vulkan-probe",
        .applicationVersion = 1,
        .pEngineName = "stereonaut",
        .engineVersion = 1,
        .apiVersion = requirements.minApiVersionSupported,
    };
    VkInstanceCreateInfo vk_create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &vk_application,
    };
    XrVulkanInstanceCreateInfoKHR xr_vk_create_info = {
        .type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR,
        .systemId = system_id,
        .pfnGetInstanceProcAddr = vkGetInstanceProcAddr,
        .vulkanCreateInfo = &vk_create_info,
    };
    VkInstance vk_instance = VK_NULL_HANDLE;
    VkResult vk_result = VK_SUCCESS;
    require_xr("xrCreateVulkanInstanceKHR",
               create_vulkan_instance(instance, &xr_vk_create_info,
                                      &vk_instance, &vk_result));
    require_vk("xrCreateVulkanInstanceKHR Vulkan result", vk_result);

    XrVulkanGraphicsDeviceGetInfoKHR device_info = {
        .type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR,
        .systemId = system_id,
        .vulkanInstance = vk_instance,
    };
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    require_xr("xrGetVulkanGraphicsDevice2KHR",
               get_vulkan_device(instance, &device_info, &physical_device));
    VkPhysicalDeviceProperties device_properties;
    vkGetPhysicalDeviceProperties(physical_device, &device_properties);
    printf("openxr_vulkan_device name=%s vendor=0x%x device=0x%x api=%u.%u.%u\n",
           device_properties.deviceName, device_properties.vendorID,
           device_properties.deviceID, VK_VERSION_MAJOR(device_properties.apiVersion),
           VK_VERSION_MINOR(device_properties.apiVersion),
           VK_VERSION_PATCH(device_properties.apiVersion));

    vkDestroyInstance(vk_instance, NULL);
    require_xr("xrDestroyInstance", xrDestroyInstance(instance));
    return 0;
}
