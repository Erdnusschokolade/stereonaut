#define _POSIX_C_SOURCE 200809L

#include "renderer.h"

static bool has_device_extension(VkPhysicalDevice physical, const char *name)
{
    uint32_t count = 0;
    need_vk("vkEnumerateDeviceExtensionProperties(count)",
            vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL));
    VkExtensionProperties *properties = calloc(count, sizeof(*properties));
    need(properties != NULL, "allocate extension properties");
    need_vk("vkEnumerateDeviceExtensionProperties(list)",
            vkEnumerateDeviceExtensionProperties(physical, NULL, &count, properties));
    bool found = false;
    for (uint32_t i = 0; i < count; ++i)
        if (strcmp(properties[i].extensionName, name) == 0) found = true;
    free(properties);
    return found;
}

void die_xr(const char *what, XrResult result)
{
    fprintf(stderr, "%s: XrResult=%d\n", what, result);
    exit(2);
}

void need_xr(const char *what, XrResult result)
{
    if (XR_FAILED(result)) die_xr(what, result);
}

void need_vk(const char *what, VkResult result)
{
    if (result != VK_SUCCESS) {
        fprintf(stderr, "%s: VkResult=%d\n", what, result);
        exit(2);
    }
}

void need(bool condition, const char *what)
{
    if (!condition) {
        fprintf(stderr, "%s\n", what);
        exit(2);
    }
}

uint32_t memory_type(VkPhysicalDevice physical, uint32_t bits,
                     VkMemoryPropertyFlags wanted)
{
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        if ((bits & (1U << i)) &&
            (properties.memoryTypes[i].propertyFlags & wanted) == wanted)
            return i;
    fprintf(stderr, "no compatible Vulkan memory type\n");
    exit(2);
}

/* Started from the desktop overlay inside the SteamVR dashboard, the app
 * becomes visible but keeps the dashboard in front (session state VISIBLE
 * instead of FOCUSED) until the viewer clicks "back to game". Hide it with
 * SteamVR's own tool, only during startup so a dashboard the viewer opens
 * later stays alone. */
void hide_dashboard_at_startup(const struct app *app)
{
    enum { WINDOW_S = 20, MAX_ATTEMPTS = 4 };
    static time_t first_call_s;
    static time_t last_attempt_s;
    static int attempts;
    time_t now = time(NULL);
    if (!first_call_s)
        first_call_s = now;
    /* This workaround is specific to the Steam Frame dashboard launch.
     * Desktop SteamVR decides dashboard focus itself; hiding it there makes
     * a direct PCVR launch unexpectedly lose its system UI. */
    if (!app->xr.has_valve_frame_controller ||
        app->xr.state != XR_SESSION_STATE_VISIBLE ||
        now - first_call_s > WINDOW_S || attempts >= MAX_ATTEMPTS ||
        (attempts && now - last_attempt_s < 1))
        return;
    ++attempts;
    last_attempt_s = now;
    const char *tool = getenv("STEREONAUT_VRCMD");
    if (!tool || !tool[0])
        tool = "/opt/steamvr/bin/linuxarm64/vrcmd";
    if (access(tool, X_OK) < 0)
        return;
    signal(SIGCHLD, SIG_IGN);
    pid_t child = fork();
    if (child == 0) {
        int null = open("/dev/null", O_WRONLY);
        if (null >= 0) {
            dup2(null, STDOUT_FILENO);
            dup2(null, STDERR_FILENO);
        }
        execl(tool, "vrcmd", "--hidedashboard", (char *)NULL);
        _exit(127);
    }
    printf("hide_dashboard attempt=%d pid=%ld\n", attempts, (long)child);
}

void poll_events(struct app *app)
{
    for (;;) {
        XrEventDataBuffer event = {.type = XR_TYPE_EVENT_DATA_BUFFER};
        XrResult result = xrPollEvent(app->xr.instance, &event);
        if (result == XR_EVENT_UNAVAILABLE) return;
        need_xr("xrPollEvent", result);
        if (event.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
            static const char *const hand_names[2] = {"left", "right"};
            for (unsigned hand = 0; hand < 2; ++hand) {
                XrInteractionProfileState profile = {
                    .type = XR_TYPE_INTERACTION_PROFILE_STATE,
                };
                char text[XR_MAX_PATH_LENGTH] = "none";
                if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(
                        app->xr.session, app->input.hands[hand], &profile)) &&
                    profile.interactionProfile != XR_NULL_PATH) {
                    uint32_t length = 0;
                    xrPathToString(app->xr.instance, profile.interactionProfile,
                                   sizeof(text), &length, text);
                }
                printf("interaction_profile hand=%s profile=%s\n",
                       hand_names[hand], text);
            }
            continue;
        }
        if (event.type == RENDER_MODELS_CHANGED_EVENT) {
            app->overlay.render_models_dirty = true;
            continue;
        }
        if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            const XrEventDataReferenceSpaceChangePending *change =
                (const XrEventDataReferenceSpaceChangePending *)&event;
            printf("reference_space_change type=%d time=%" PRId64 "\n",
                   change->referenceSpaceType, change->changeTime);
            if (change->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                app->home_pending = true;
                app->home_after = change->changeTime;
            }
            continue;
        }
        if (event.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) continue;
        XrEventDataSessionStateChanged *change =
            (XrEventDataSessionStateChanged *)&event;
        app->xr.state = change->state;
        ++app->session_changes;
        printf("openxr_session_state=%d time=%" PRId64 "\n", app->xr.state,
               change->time);
        if (app->xr.state == XR_SESSION_STATE_READY) {
            XrSessionBeginInfo begin = {
                .type = XR_TYPE_SESSION_BEGIN_INFO,
                .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            };
            need_xr("xrBeginSession", xrBeginSession(app->xr.session, &begin));
            app->xr.running = true;
        } else if (app->xr.state == XR_SESSION_STATE_STOPPING) {
            need_xr("xrEndSession", xrEndSession(app->xr.session));
            app->xr.running = false;
        } else if (app->xr.state == XR_SESSION_STATE_EXITING ||
                   app->xr.state == XR_SESSION_STATE_LOSS_PENDING) {
            app->xr.running = false;
            app->quit = true;
            app->xr_exit = true;
            return;
        }
    }
}

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (!file) { perror(path); exit(2); }
    if (fseek(file, 0, SEEK_END) || (*size = (size_t)ftell(file)) == 0 ||
        fseek(file, 0, SEEK_SET)) { perror("read shader size"); exit(2); }
    uint8_t *data = malloc(*size);
    if (!data || fread(data, 1, *size, file) != *size) {
        perror("read shader"); exit(2);
    }
    fclose(file);
    return data;
}

VkShaderModule shader(VkDevice device, const char *path)
{
    size_t size = 0;
    uint8_t *data = read_file(path, &size);
    need(size % 4 == 0, "SPIR-V size is not word aligned");
    VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size, .pCode = (const uint32_t *)data,
    };
    VkShaderModule result = VK_NULL_HANDLE;
    need_vk("vkCreateShaderModule", vkCreateShaderModule(device, &info, NULL, &result));
    free(data);
    return result;
}

void initialize_xr_vulkan(struct app *app)
{
    /* Controller profiles with a squeeze/grip button are optional extensions;
     * khr/simple_controller has none. */
    const char *valve_extension = "XR_VALVE_frame_controller_interaction";
    const char *generic_extension = "XR_KHR_generic_controller";
    uint32_t available_count = 0;
    need_xr("xrEnumerateInstanceExtensionProperties(count)",
            xrEnumerateInstanceExtensionProperties(NULL, 0, &available_count,
                                                   NULL));
    XrExtensionProperties *available =
        calloc(available_count ? available_count : 1U, sizeof(*available));
    need(available != NULL, "allocate OpenXR extension list");
    for (uint32_t i = 0; i < available_count; ++i)
        available[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    need_xr("xrEnumerateInstanceExtensionProperties(list)",
            xrEnumerateInstanceExtensionProperties(NULL, available_count,
                                                   &available_count,
                                                   available));
    for (uint32_t i = 0; i < available_count; ++i) {
        if (strcmp(available[i].extensionName,
                   XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME) == 0)
            app->xr.has_eye_gaze_extension = true;
        if (strcmp(available[i].extensionName, valve_extension) == 0)
            app->xr.has_valve_frame_controller = true;
        if (strcmp(available[i].extensionName, generic_extension) == 0)
            app->xr.has_generic_controller = true;
    }
    /* No extension is required for the standardized PC interaction
     * profiles. Unsupported profiles are harmlessly rejected by
     * xrSuggestInteractionProfileBindings. */
    app->xr.has_pcvr_controller = true;
    const char *extensions[8] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    uint32_t extension_count = 1;
    if (app->xr.has_eye_gaze_extension)
        extensions[extension_count++] =
            XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME;
    if (app->xr.has_valve_frame_controller)
        extensions[extension_count++] = valve_extension;
    if (app->xr.has_generic_controller)
        extensions[extension_count++] = generic_extension;
    render_models_request_extensions(&app->overlay.render_models, available,
                                     available_count, extensions,
                                     &extension_count);
    free(available);
    XrInstanceCreateInfo info = {
        .type = XR_TYPE_INSTANCE_CREATE_INFO,
        .applicationInfo = {.applicationVersion = 1, .engineVersion = 1,
                            .apiVersion = XR_MAKE_VERSION(1, 0, 0)},
        .enabledExtensionCount = extension_count,
        .enabledExtensionNames = extensions,
    };
    snprintf(info.applicationInfo.applicationName,
             sizeof(info.applicationInfo.applicationName), "stereonaut-xr-quad");
    snprintf(info.applicationInfo.engineName, sizeof(info.applicationInfo.engineName), "stereonaut");
    need_xr("xrCreateInstance", xrCreateInstance(&info, &app->xr.instance));
    XrSystemGetInfo system_info = {.type = XR_TYPE_SYSTEM_GET_INFO,
                                   .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    need_xr("xrGetSystem", xrGetSystem(app->xr.instance, &system_info, &app->xr.system));
    if (app->xr.has_eye_gaze_extension) {
        XrSystemEyeGazeInteractionPropertiesEXT eye_gaze = {
            .type = XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT,
        };
        XrSystemProperties properties = {
            .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &eye_gaze,
        };
        if (XR_SUCCEEDED(xrGetSystemProperties(app->xr.instance, app->xr.system,
                                               &properties)))
            app->xr.has_eye_gaze = eye_gaze.supportsEyeGazeInteraction;
    }
    printf("eye_gaze extension=%d supported=%d\n",
           app->xr.has_eye_gaze_extension, app->xr.has_eye_gaze);
    XrEnvironmentBlendMode blend_modes[8];
    uint32_t blend_mode_count = 0;
    if (XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(
            app->xr.instance, app->xr.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            8, &blend_mode_count, blend_modes)))
        for (uint32_t i = 0; i < blend_mode_count; ++i)
            if (blend_modes[i] == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND)
                app->xr.alpha_blend_supported = true;
    printf("environment_blend_modes count=%u alpha_blend=%d\n",
           blend_mode_count, app->xr.alpha_blend_supported);

    PFN_xrCreateVulkanInstanceKHR create_instance = NULL;
    PFN_xrGetVulkanGraphicsDevice2KHR get_device = NULL;
    PFN_xrCreateVulkanDeviceKHR create_device = NULL;
    PFN_xrGetVulkanGraphicsRequirements2KHR get_requirements = NULL;
    need_xr("xrGetInstanceProcAddr(create instance)", xrGetInstanceProcAddr(app->xr.instance, "xrCreateVulkanInstanceKHR", (PFN_xrVoidFunction *)&create_instance));
    need_xr("xrGetInstanceProcAddr(get device)", xrGetInstanceProcAddr(app->xr.instance, "xrGetVulkanGraphicsDevice2KHR", (PFN_xrVoidFunction *)&get_device));
    need_xr("xrGetInstanceProcAddr(create device)", xrGetInstanceProcAddr(app->xr.instance, "xrCreateVulkanDeviceKHR", (PFN_xrVoidFunction *)&create_device));
    need_xr("xrGetInstanceProcAddr(requirements)", xrGetInstanceProcAddr(app->xr.instance, "xrGetVulkanGraphicsRequirements2KHR", (PFN_xrVoidFunction *)&get_requirements));
    need(create_instance && get_device && create_device && get_requirements, "missing Vulkan-enable2 entry point");
    XrGraphicsRequirementsVulkanKHR requirements = {
        .type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR,
    };
    need_xr("xrGetVulkanGraphicsRequirements2KHR", get_requirements(app->xr.instance, app->xr.system, &requirements));
    printf("openxr_vulkan_requirements min=%" PRIu64 " max=%" PRIu64 "\n",
           requirements.minApiVersionSupported, requirements.maxApiVersionSupported);

    VkApplicationInfo vk_app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "stereonaut-xr-quad", .applicationVersion = 1,
        .pEngineName = "stereonaut", .engineVersion = 1, .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo vk_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &vk_app};
    XrVulkanInstanceCreateInfoKHR xr_vk_info = {
        .type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR, .systemId = app->xr.system,
        .pfnGetInstanceProcAddr = vkGetInstanceProcAddr, .vulkanCreateInfo = &vk_info};
    VkResult vk_result = VK_SUCCESS;
    need_xr("xrCreateVulkanInstanceKHR", create_instance(app->xr.instance, &xr_vk_info, &app->gpu.instance, &vk_result));
    need_vk("xrCreateVulkanInstanceKHR Vulkan", vk_result);
    XrVulkanGraphicsDeviceGetInfoKHR device_info = {
        .type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR,
        .systemId = app->xr.system, .vulkanInstance = app->gpu.instance};
    need_xr("xrGetVulkanGraphicsDevice2KHR", get_device(app->xr.instance, &device_info, &app->gpu.physical));

    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(app->gpu.physical, &count, NULL);
    VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
    need(families != NULL, "allocate queue families");
    vkGetPhysicalDeviceQueueFamilyProperties(app->gpu.physical, &count, families);
    bool found = false;
    for (uint32_t i = 0; i < count; ++i)
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            app->gpu.queue_family = i; found = true; break;
        }
    free(families);
    need(found, "OpenXR Vulkan device has no graphics queue");
    const char *device_extensions[] = {
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
    };
    for (size_t i = 0; i < ARRAY_COUNT(device_extensions); ++i)
        need(has_device_extension(app->gpu.physical, device_extensions[i]),
             "required NV12 DMA-BUF Vulkan extension is missing");
    /* PC decoder buffers arrive as memfds (no udmabuf size limit). */
    const char *enabled_extensions[ARRAY_COUNT(device_extensions) + 1];
    memcpy(enabled_extensions, device_extensions, sizeof(device_extensions));
    uint32_t enabled_count = ARRAY_COUNT(device_extensions);
    bool host_import = has_device_extension(
        app->gpu.physical, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    if (host_import)
        enabled_extensions[enabled_count++] =
            VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    /* Panels drawn into the eye images are sampled anisotropically. */
    VkPhysicalDeviceFeatures supported, features = {0};
    vkGetPhysicalDeviceFeatures(app->gpu.physical, &supported);
    features.samplerAnisotropy = supported.samplerAnisotropy;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = app->gpu.queue_family, .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo vk_device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = enabled_count,
        .ppEnabledExtensionNames = enabled_extensions,
        .pEnabledFeatures = &features};
    XrVulkanDeviceCreateInfoKHR xr_device_info = {
        .type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR, .systemId = app->xr.system,
        .pfnGetInstanceProcAddr = vkGetInstanceProcAddr, .vulkanPhysicalDevice = app->gpu.physical,
        .vulkanCreateInfo = &vk_device_info};
    need_xr("xrCreateVulkanDeviceKHR", create_device(app->xr.instance, &xr_device_info, &app->gpu.device, &vk_result));
    need_vk("xrCreateVulkanDeviceKHR Vulkan", vk_result);
    vkGetDeviceQueue(app->gpu.device, app->gpu.queue_family, 0, &app->gpu.queue);
    need(app->gpu.queue != VK_NULL_HANDLE, "vkGetDeviceQueue returned null");
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(app->gpu.physical, &properties);
    printf("openxr_vulkan_device=%s queue_family=%u\n", properties.deviceName, app->gpu.queue_family);
    app->gpu.panel_anisotropy = 1.0f;
    if (features.samplerAnisotropy)
        app->gpu.panel_anisotropy =
            fminf(8.0f, properties.limits.maxSamplerAnisotropy);
    app->gpu.import_semaphore_fd = (PFN_vkImportSemaphoreFdKHR)
        vkGetDeviceProcAddr(app->gpu.device, "vkImportSemaphoreFdKHR");
    app->gpu.get_memory_fd_properties = (PFN_vkGetMemoryFdPropertiesKHR)
        vkGetDeviceProcAddr(app->gpu.device, "vkGetMemoryFdPropertiesKHR");
    app->gpu.get_modifier_properties = (PFN_vkGetImageDrmFormatModifierPropertiesEXT)
        vkGetDeviceProcAddr(app->gpu.device,
                            "vkGetImageDrmFormatModifierPropertiesEXT");
    need(app->gpu.import_semaphore_fd && app->gpu.get_memory_fd_properties &&
             app->gpu.get_modifier_properties,
         "required external-memory entry point is missing");
    if (host_import)
        app->gpu.get_host_pointer_properties =
            (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(
                app->gpu.device, "vkGetMemoryHostPointerPropertiesEXT");
}

void initialize_swapchain(struct app *app)
{
    uint32_t format_count = 0;
    need_xr("xrEnumerateSwapchainFormats(count)", xrEnumerateSwapchainFormats(app->xr.session, 0, &format_count, NULL));
    int64_t *formats = calloc(format_count, sizeof(*formats));
    need(formats != NULL, "allocate swapchain formats");
    need_xr("xrEnumerateSwapchainFormats(list)", xrEnumerateSwapchainFormats(app->xr.session, format_count, &format_count, formats));
    app->xr.color_format = VK_FORMAT_B8G8R8A8_SRGB;
    bool supported = false;
    for (uint32_t i = 0; i < format_count; ++i) if (formats[i] == app->xr.color_format) supported = true;
    if (!supported) { app->xr.color_format = (VkFormat)formats[0]; supported = true; }
    free(formats);
    XrViewConfigurationView views[2] = {
        {.type = XR_TYPE_VIEW_CONFIGURATION_VIEW}, {.type = XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    uint32_t view_count = 0;
    need_xr("xrEnumerateViewConfigurationViews", xrEnumerateViewConfigurationViews(app->xr.instance, app->xr.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &view_count, views));
    need(view_count == 2, "Steam Frame did not report stereo views");
    app->xr.width = views[0].recommendedImageRectWidth;
    app->xr.height = views[0].recommendedImageRectHeight;
    need(views[1].recommendedImageRectWidth == app->xr.width && views[1].recommendedImageRectHeight == app->xr.height, "eyes have different recommended dimensions");
    /* On a PC the panels are drawn into the eye images, which at the
     * recommended size blur them: render larger. Above 1.3 streaming to
     * the Frame shows the eyes with mismatched perspectives (other players
     * too). STEREONAUT_RENDER_SCALE (1 to 2) overrides it. */
    const char *scale_text = getenv("STEREONAUT_RENDER_SCALE");
    float scale = scale_text ? strtof(scale_text, NULL)
                             : overlay_in_eye_images() ? PC_RENDER_SCALE
                                                       : 1.0f;
    if (scale > 1.0f && scale <= 2.0f) {
        app->xr.width = (uint32_t)(app->xr.width * scale);
        app->xr.height = (uint32_t)(app->xr.height * scale);
        if (app->xr.width > views[0].maxImageRectWidth)
            app->xr.width = views[0].maxImageRectWidth;
        if (app->xr.height > views[0].maxImageRectHeight)
            app->xr.height = views[0].maxImageRectHeight;
    }
    XrSwapchainCreateInfo info = {.type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
        .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT,
        .format = app->xr.color_format, .sampleCount = 1, .width = app->xr.width,
        .height = app->xr.height, .faceCount = 1, .arraySize = 2, .mipCount = 1};
    need_xr("xrCreateSwapchain", xrCreateSwapchain(app->xr.session, &info, &app->xr.swapchain));
    need_xr("xrEnumerateSwapchainImages(count)", xrEnumerateSwapchainImages(app->xr.swapchain, 0, &app->xr.image_count, NULL));
    need(app->xr.image_count > 0 && app->xr.image_count <= MAX_IMAGES, "unexpected OpenXR swapchain image count");
    for (uint32_t i = 0; i < app->xr.image_count; ++i) app->xr.images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
    need_xr("xrEnumerateSwapchainImages(list)", xrEnumerateSwapchainImages(app->xr.swapchain, app->xr.image_count, &app->xr.image_count, (XrSwapchainImageBaseHeader *)app->xr.images));
    printf("openxr_swapchain format=%d dimensions=%ux%u images=%u\n", app->xr.color_format, app->xr.width, app->xr.height, app->xr.image_count);
}

double monotonic_seconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}
