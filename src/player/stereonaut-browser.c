/* Native OpenXR file browser for the Stereonaut MVP. */
#define _POSIX_C_SOURCE 200809L
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_PLATFORM_XLIB

#include "file-browser.h"
#include "player-ui-ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define UI_WIDTH 1024U
#define UI_HEIGHT 768U
#define UI_BYTES (UI_WIDTH * UI_HEIGHT * 4U)
#define MAX_SWAPCHAIN_IMAGES 8U
#define PAGE_ENTRIES 9U
#define PANEL_WIDTH_METERS 1.6f
#define PANEL_HEIGHT_METERS 1.2f
/* Analog select with hysteresis; see the matching values in the renderer. */
#define SELECT_PRESS_THRESHOLD 0.30f
#define SELECT_RELEASE_THRESHOLD 0.15f

struct browser_app {
    int ipc_fd;
    struct file_browser *browser;
    struct file_browser_listing listing;
    char status_message[256];
    size_t page;
    int hover;
    bool pointer_visible;
    int pointer_x;
    int pointer_y;
    bool texture_dirty;
    bool exit_requested;

    XrInstance xr;
    XrSystemId system;
    XrSession session;
    XrSpace local_space;
    XrSessionState session_state;
    bool running;
    XrActionSet action_set;
    XrAction aim_action;
    XrAction select_action;
    bool has_valve_frame_controller;
    bool has_generic_controller;
    bool select_down[2];
    XrPath hands[2];
    XrSpace aim_spaces[2];

    VkInstance vk_instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkCommandPool command_pool;
    VkCommandBuffer command;
    VkFence fence;
    VkBuffer staging_buffer;
    VkDeviceMemory staging_memory;
    uint32_t *pixels;
    bool bgra;

    XrSwapchain swapchain;
    VkFormat swapchain_format;
    uint32_t image_count;
    XrSwapchainImageVulkan2KHR images[MAX_SWAPCHAIN_IMAGES];
    bool image_initialized[MAX_SWAPCHAIN_IMAGES];
};

static void fail(const char *message)
{
    fprintf(stderr, "%s\n", message);
    exit(2);
}

static void need(bool condition, const char *message)
{
    if (!condition)
        fail(message);
}

static void need_xr(const char *operation, XrResult result)
{
    if (XR_FAILED(result)) {
        fprintf(stderr, "%s: XrResult=%d\n", operation, result);
        exit(2);
    }
}

static void need_vk(const char *operation, VkResult result)
{
    if (result != VK_SUCCESS) {
        fprintf(stderr, "%s: VkResult=%d\n", operation, result);
        exit(2);
    }
}

static unsigned long long parse_u64(const char *text)
{
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || !end || *end || value > INT32_MAX)
        fail("invalid IPC fd");
    return value;
}

static uint32_t memory_type(VkPhysicalDevice physical, uint32_t bits,
                            VkMemoryPropertyFlags wanted)
{
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((bits & (1U << i)) &&
            (properties.memoryTypes[i].propertyFlags & wanted) == wanted)
            return i;
    }
    fail("no compatible Vulkan memory type");
    return 0;
}

static XrPath xr_path(struct browser_app *app, const char *text)
{
    XrPath path = XR_NULL_PATH;
    need_xr("xrStringToPath", xrStringToPath(app->xr, text, &path));
    return path;
}

static void initialize_xr_vulkan(struct browser_app *app)
{
    /* Optional controller profiles allow binding the analog trigger value, so
     * a press registers well before the trigger's full click travel. */
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
        if (strcmp(available[i].extensionName, valve_extension) == 0)
            app->has_valve_frame_controller = true;
        if (strcmp(available[i].extensionName, generic_extension) == 0)
            app->has_generic_controller = true;
    }
    free(available);
    const char *extensions[3] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    uint32_t extension_count = 1;
    if (app->has_valve_frame_controller)
        extensions[extension_count++] = valve_extension;
    if (app->has_generic_controller)
        extensions[extension_count++] = generic_extension;
    XrInstanceCreateInfo create = {
        .type = XR_TYPE_INSTANCE_CREATE_INFO,
        .applicationInfo = {
            .applicationVersion = 1,
            .engineVersion = 1,
            .apiVersion = XR_MAKE_VERSION(1, 0, 0),
        },
        .enabledExtensionCount = extension_count,
        .enabledExtensionNames = extensions,
    };
    snprintf(create.applicationInfo.applicationName,
             sizeof(create.applicationInfo.applicationName),
             "Stereonaut Browser");
    snprintf(create.applicationInfo.engineName,
             sizeof(create.applicationInfo.engineName), "Stereonaut");
    need_xr("xrCreateInstance", xrCreateInstance(&create, &app->xr));
    XrSystemGetInfo system = {
        .type = XR_TYPE_SYSTEM_GET_INFO,
        .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
    };
    need_xr("xrGetSystem", xrGetSystem(app->xr, &system, &app->system));

    PFN_xrCreateVulkanInstanceKHR create_instance = NULL;
    PFN_xrGetVulkanGraphicsDevice2KHR get_device = NULL;
    PFN_xrCreateVulkanDeviceKHR create_device = NULL;
    PFN_xrGetVulkanGraphicsRequirements2KHR get_requirements = NULL;
    need_xr("xrGetInstanceProcAddr(create instance)",
            xrGetInstanceProcAddr(app->xr, "xrCreateVulkanInstanceKHR",
                                  (PFN_xrVoidFunction *)&create_instance));
    need_xr("xrGetInstanceProcAddr(get device)",
            xrGetInstanceProcAddr(app->xr, "xrGetVulkanGraphicsDevice2KHR",
                                  (PFN_xrVoidFunction *)&get_device));
    need_xr("xrGetInstanceProcAddr(create device)",
            xrGetInstanceProcAddr(app->xr, "xrCreateVulkanDeviceKHR",
                                  (PFN_xrVoidFunction *)&create_device));
    need_xr("xrGetInstanceProcAddr(requirements)",
            xrGetInstanceProcAddr(app->xr,
                                  "xrGetVulkanGraphicsRequirements2KHR",
                                  (PFN_xrVoidFunction *)&get_requirements));
    need(create_instance && get_device && create_device && get_requirements,
         "missing OpenXR Vulkan-enable2 entry point");
    XrGraphicsRequirementsVulkanKHR requirements = {
        .type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR,
    };
    need_xr("xrGetVulkanGraphicsRequirements2KHR",
            get_requirements(app->xr, app->system, &requirements));

    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Stereonaut Browser",
        .applicationVersion = 1,
        .pEngineName = "Stereonaut",
        .engineVersion = 1,
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
    };
    XrVulkanInstanceCreateInfoKHR xr_instance_info = {
        .type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR,
        .systemId = app->system,
        .pfnGetInstanceProcAddr = vkGetInstanceProcAddr,
        .vulkanCreateInfo = &instance_info,
    };
    VkResult vk_result = VK_SUCCESS;
    need_xr("xrCreateVulkanInstanceKHR",
            create_instance(app->xr, &xr_instance_info,
                            &app->vk_instance, &vk_result));
    need_vk("xrCreateVulkanInstanceKHR(Vulkan)", vk_result);
    XrVulkanGraphicsDeviceGetInfoKHR device_info = {
        .type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR,
        .systemId = app->system,
        .vulkanInstance = app->vk_instance,
    };
    need_xr("xrGetVulkanGraphicsDevice2KHR",
            get_device(app->xr, &device_info, &app->physical));
    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(app->physical,
                                             &family_count, NULL);
    VkQueueFamilyProperties *families =
        calloc(family_count, sizeof(*families));
    need(families != NULL, "allocate Vulkan queue families");
    vkGetPhysicalDeviceQueueFamilyProperties(app->physical,
                                             &family_count, families);
    bool found = false;
    for (uint32_t i = 0; i < family_count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            app->queue_family = i;
            found = true;
            break;
        }
    }
    free(families);
    need(found, "OpenXR Vulkan device has no graphics queue");
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = app->queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo device_create = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue,
    };
    XrVulkanDeviceCreateInfoKHR xr_device_create = {
        .type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR,
        .systemId = app->system,
        .pfnGetInstanceProcAddr = vkGetInstanceProcAddr,
        .vulkanPhysicalDevice = app->physical,
        .vulkanCreateInfo = &device_create,
    };
    need_xr("xrCreateVulkanDeviceKHR",
            create_device(app->xr, &xr_device_create,
                          &app->device, &vk_result));
    need_vk("xrCreateVulkanDeviceKHR(Vulkan)", vk_result);
    vkGetDeviceQueue(app->device, app->queue_family, 0, &app->queue);
}

static void create_action(XrActionSet set, XrActionType type,
                          const char *name, const char *localized,
                          XrPath hands[2], XrAction *action)
{
    XrActionCreateInfo info = {
        .type = XR_TYPE_ACTION_CREATE_INFO,
        .actionType = type,
        .countSubactionPaths = 2,
        .subactionPaths = hands,
    };
    snprintf(info.actionName, sizeof(info.actionName), "%s", name);
    snprintf(info.localizedActionName, sizeof(info.localizedActionName),
             "%s", localized);
    need_xr("xrCreateAction", xrCreateAction(set, &info, action));
}

static void initialize_actions(struct browser_app *app)
{
    app->hands[0] = xr_path(app, "/user/hand/left");
    app->hands[1] = xr_path(app, "/user/hand/right");
    XrActionSetCreateInfo set = {
        .type = XR_TYPE_ACTION_SET_CREATE_INFO,
        .priority = 0,
    };
    snprintf(set.actionSetName, sizeof(set.actionSetName), "browser");
    snprintf(set.localizedActionSetName, sizeof(set.localizedActionSetName),
             "Dateibrowser");
    need_xr("xrCreateActionSet",
            xrCreateActionSet(app->xr, &set, &app->action_set));
    create_action(app->action_set, XR_ACTION_TYPE_POSE_INPUT,
                  "aim", "Zeigen", app->hands, &app->aim_action);
    create_action(app->action_set, XR_ACTION_TYPE_FLOAT_INPUT,
                  "select", "Auswaehlen", app->hands,
                  &app->select_action);
    struct {
        const char *profile;
        const char *select_component;
        bool enabled;
    } profiles[] = {
        {"/interaction_profiles/valve/frame_controller", "trigger/value",
         app->has_valve_frame_controller},
        {"/interaction_profiles/khr/generic_controller", "trigger/value",
         app->has_generic_controller},
        {"/interaction_profiles/valve/index_controller", "trigger/value",
         true},
        {"/interaction_profiles/khr/simple_controller", "select/click", true},
    };
    static const char *const hand_names[2] = {"left", "right"};
    unsigned accepted = 0;
    for (size_t p = 0; p < ARRAY_COUNT(profiles); ++p) {
        if (!profiles[p].enabled)
            continue;
        XrActionSuggestedBinding bindings[4];
        char text[2][2][96];
        for (unsigned hand = 0; hand < 2; ++hand) {
            snprintf(text[0][hand], sizeof(text[0][hand]),
                     "/user/hand/%s/input/aim/pose", hand_names[hand]);
            snprintf(text[1][hand], sizeof(text[1][hand]),
                     "/user/hand/%s/input/%s", hand_names[hand],
                     profiles[p].select_component);
            bindings[hand * 2] = (XrActionSuggestedBinding){
                app->aim_action, xr_path(app, text[0][hand])};
            bindings[hand * 2 + 1] = (XrActionSuggestedBinding){
                app->select_action, xr_path(app, text[1][hand])};
        }
        XrInteractionProfileSuggestedBinding suggestion = {
            .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
            .interactionProfile = xr_path(app, profiles[p].profile),
            .countSuggestedBindings = ARRAY_COUNT(bindings),
            .suggestedBindings = bindings,
        };
        if (XR_SUCCEEDED(
                xrSuggestInteractionProfileBindings(app->xr, &suggestion)))
            ++accepted;
    }
    need(accepted > 0, "no controller interaction profile was accepted");
    for (unsigned hand = 0; hand < 2; ++hand) {
        XrActionSpaceCreateInfo space = {
            .type = XR_TYPE_ACTION_SPACE_CREATE_INFO,
            .action = app->aim_action,
            .subactionPath = app->hands[hand],
            .poseInActionSpace = {{0, 0, 0, 1}, {0, 0, 0}},
        };
        need_xr("xrCreateActionSpace",
                xrCreateActionSpace(app->session, &space,
                                    &app->aim_spaces[hand]));
    }
    XrSessionActionSetsAttachInfo attach = {
        .type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO,
        .countActionSets = 1,
        .actionSets = &app->action_set,
    };
    need_xr("xrAttachSessionActionSets",
            xrAttachSessionActionSets(app->session, &attach));
}

static void poll_events(struct browser_app *app)
{
    for (;;) {
        XrEventDataBuffer event = {.type = XR_TYPE_EVENT_DATA_BUFFER};
        XrResult result = xrPollEvent(app->xr, &event);
        if (result == XR_EVENT_UNAVAILABLE)
            return;
        need_xr("xrPollEvent", result);
        if (event.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
            continue;
        XrEventDataSessionStateChanged *changed =
            (XrEventDataSessionStateChanged *)&event;
        app->session_state = changed->state;
        printf("openxr_session_state=%d time=%" PRId64 "\n",
               app->session_state, changed->time);
        if (app->session_state == XR_SESSION_STATE_READY) {
            XrSessionBeginInfo begin = {
                .type = XR_TYPE_SESSION_BEGIN_INFO,
                .primaryViewConfigurationType =
                    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            };
            need_xr("xrBeginSession", xrBeginSession(app->session, &begin));
            app->running = true;
        } else if (app->session_state == XR_SESSION_STATE_STOPPING) {
            need_xr("xrEndSession", xrEndSession(app->session));
            app->running = false;
        } else if (app->session_state == XR_SESSION_STATE_EXITING ||
                   app->session_state == XR_SESSION_STATE_LOSS_PENDING) {
            app->running = false;
            app->exit_requested = true;
        }
    }
}

static void create_swapchain(struct browser_app *app)
{
    uint32_t format_count = 0;
    need_xr("xrEnumerateSwapchainFormats(count)",
            xrEnumerateSwapchainFormats(app->session, 0,
                                        &format_count, NULL));
    int64_t *formats = calloc(format_count, sizeof(*formats));
    need(formats != NULL, "allocate swapchain formats");
    need_xr("xrEnumerateSwapchainFormats(list)",
            xrEnumerateSwapchainFormats(app->session, format_count,
                                        &format_count, formats));
    VkFormat preferred[] = {
        VK_FORMAT_R8G8B8A8_SRGB,
        VK_FORMAT_B8G8R8A8_SRGB,
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8A8_UNORM,
    };
    for (size_t candidate = 0; candidate < ARRAY_COUNT(preferred); ++candidate)
        for (uint32_t i = 0; i < format_count; ++i)
            if (formats[i] == preferred[candidate] && !app->swapchain_format)
                app->swapchain_format = preferred[candidate];
    free(formats);
    need(app->swapchain_format != VK_FORMAT_UNDEFINED,
         "no supported RGBA UI swapchain format");
    app->bgra = app->swapchain_format == VK_FORMAT_B8G8R8A8_SRGB ||
                app->swapchain_format == VK_FORMAT_B8G8R8A8_UNORM;
    XrSwapchainCreateInfo info = {
        .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
        .usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                      XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
        .format = app->swapchain_format,
        .sampleCount = 1,
        .width = UI_WIDTH,
        .height = UI_HEIGHT,
        .faceCount = 1,
        .arraySize = 1,
        .mipCount = 1,
    };
    need_xr("xrCreateSwapchain",
            xrCreateSwapchain(app->session, &info, &app->swapchain));
    need_xr("xrEnumerateSwapchainImages(count)",
            xrEnumerateSwapchainImages(app->swapchain, 0,
                                       &app->image_count, NULL));
    need(app->image_count > 0 && app->image_count <= MAX_SWAPCHAIN_IMAGES,
         "unexpected UI swapchain image count");
    for (uint32_t i = 0; i < app->image_count; ++i)
        app->images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
    need_xr("xrEnumerateSwapchainImages(list)",
            xrEnumerateSwapchainImages(
                app->swapchain, app->image_count, &app->image_count,
                (XrSwapchainImageBaseHeader *)app->images));
    printf("browser_swapchain format=%d dimensions=%ux%u images=%u\n",
           app->swapchain_format, UI_WIDTH, UI_HEIGHT, app->image_count);
}

static void create_transfer_resources(struct browser_app *app)
{
    VkCommandPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = app->queue_family,
    };
    need_vk("vkCreateCommandPool",
            vkCreateCommandPool(app->device, &pool, NULL,
                                &app->command_pool));
    VkCommandBufferAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = app->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    need_vk("vkAllocateCommandBuffers",
            vkAllocateCommandBuffers(app->device, &allocate, &app->command));
    VkFenceCreateInfo fence = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    need_vk("vkCreateFence",
            vkCreateFence(app->device, &fence, NULL, &app->fence));
    VkBufferCreateInfo buffer = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = UI_BYTES,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    need_vk("vkCreateBuffer(staging)",
            vkCreateBuffer(app->device, &buffer, NULL,
                           &app->staging_buffer));
    VkMemoryRequirements memory;
    vkGetBufferMemoryRequirements(app->device, app->staging_buffer, &memory);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memory.size,
        .memoryTypeIndex = memory_type(
            app->physical, memory.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    need_vk("vkAllocateMemory(staging)",
            vkAllocateMemory(app->device, &allocation, NULL,
                             &app->staging_memory));
    need_vk("vkBindBufferMemory(staging)",
            vkBindBufferMemory(app->device, app->staging_buffer,
                               app->staging_memory, 0));
    need_vk("vkMapMemory(staging)",
            vkMapMemory(app->device, app->staging_memory, 0,
                        UI_BYTES, 0, (void **)&app->pixels));
}

static uint32_t rgba(struct browser_app *app, uint8_t r, uint8_t g,
                     uint8_t b, uint8_t a)
{
    if (app->bgra) {
        uint8_t temporary = r;
        r = b;
        b = temporary;
    }
    return (uint32_t)r | ((uint32_t)g << 8U) |
           ((uint32_t)b << 16U) | ((uint32_t)a << 24U);
}

static void rectangle(struct browser_app *app, int x, int y, int width,
                      int height, uint32_t color)
{
    int left = x < 0 ? 0 : x;
    int top = y < 0 ? 0 : y;
    int right = x + width > (int)UI_WIDTH ? (int)UI_WIDTH : x + width;
    int bottom = y + height > (int)UI_HEIGHT ? (int)UI_HEIGHT : y + height;
    for (int row = top; row < bottom; ++row)
        for (int column = left; column < right; ++column)
            app->pixels[(size_t)row * UI_WIDTH + (size_t)column] = color;
}

static const uint8_t letter_rows[36][5] = {
    {2,5,7,5,5},{6,5,6,5,6},{3,4,4,4,3},{6,5,5,5,6},
    {7,4,6,4,7},{7,4,6,4,4},{3,4,5,5,3},{5,5,7,5,5},
    {7,2,2,2,7},{1,1,1,5,2},{5,5,6,5,5},{4,4,4,4,7},
    {5,7,7,5,5},{5,7,7,7,5},{2,5,5,5,2},{6,5,6,4,4},
    {2,5,5,3,1},{6,5,6,5,5},{3,4,2,1,6},{7,2,2,2,2},
    {5,5,5,5,7},{5,5,5,5,2},{5,5,7,7,5},{5,5,2,5,5},
    {5,5,2,2,2},{7,1,2,4,7},
    {2,5,5,5,2},{2,6,2,2,7},{6,1,2,4,7},{6,1,2,1,6},
    {5,5,7,1,1},{7,4,6,1,6},{3,4,6,5,2},{7,1,2,2,2},
    {2,5,2,5,2},{2,5,3,1,6},
};

static uint8_t glyph_row(unsigned char character, unsigned row)
{
    if (character >= 'a' && character <= 'z')
        character = (unsigned char)(character - 'a' + 'A');
    if (character >= 'A' && character <= 'Z')
        return letter_rows[character - 'A'][row];
    if (character >= '0' && character <= '9')
        return letter_rows[26U + character - '0'][row];
    if (character == '.')
        return row == 4 ? 2 : 0;
    if (character == '-')
        return row == 2 ? 7 : 0;
    if (character == '_')
        return row == 4 ? 7 : 0;
    if (character == '/')
        return (uint8_t)(1U << (row > 2 ? 0 : 2U - row));
    if (character == ':')
        return row == 1 || row == 3 ? 2 : 0;
    if (character == '>')
        return (uint8_t[]){4, 2, 1, 2, 4}[row];
    if (character == '<')
        return (uint8_t[]){1, 2, 4, 2, 1}[row];
    if (character == ' ')
        return 0;
    return (uint8_t[]){7, 5, 5, 5, 7}[row];
}

static void draw_text(struct browser_app *app, int x, int y, int scale,
                      const char *text, uint32_t color, size_t limit)
{
    size_t count = 0;
    for (const unsigned char *cursor = (const unsigned char *)text;
         *cursor && count < limit; ++cursor, ++count) {
        for (unsigned row = 0; row < 5; ++row) {
            uint8_t bits = glyph_row(*cursor, row);
            for (unsigned column = 0; column < 3; ++column)
                if (bits & (1U << (2U - column)))
                    rectangle(app, x + (int)column * scale,
                              y + (int)row * scale, scale, scale, color);
        }
        x += 4 * scale;
    }
}

static size_t page_count(const struct browser_app *app)
{
    return app->listing.entry_count
               ? (app->listing.entry_count + PAGE_ENTRIES - 1U) / PAGE_ENTRIES
               : 1U;
}

static void render_browser_pixels(struct browser_app *app)
{
    uint32_t background = rgba(app, 8, 13, 24, 255);
    uint32_t panel = rgba(app, 18, 29, 48, 255);
    uint32_t normal = rgba(app, 35, 54, 82, 255);
    uint32_t selected = rgba(app, 23, 128, 190, 255);
    uint32_t text = rgba(app, 238, 245, 255, 255);
    rectangle(app, 0, 0, UI_WIDTH, UI_HEIGHT, background);
    rectangle(app, 32, 24, 960, 720, panel);
    draw_text(app, 64, 48, 5, "STEREONAUT", text, 32);
    draw_text(app, 64, 92, 3,
              app->listing.path ? app->listing.path : "LADE VERZEICHNIS",
              text, 72);
    if (app->listing.error[0])
        draw_text(app, 64, 130, 3, app->listing.error, text, 72);
    else if (app->status_message[0])
        draw_text(app, 64, 130, 3, app->status_message, text, 72);
    size_t first = app->page * PAGE_ENTRIES;
    for (size_t row = 0; row < PAGE_ENTRIES; ++row) {
        size_t index = first + row;
        if (index >= app->listing.entry_count)
            break;
        int y = 165 + (int)row * 54;
        rectangle(app, 58, y, 908, 46,
                  app->hover == (int)row ? selected : normal);
        const struct file_browser_entry *entry = &app->listing.entries[index];
        const char *prefix = entry->kind == FILE_BROWSER_MEDIA ? "> " : "/ ";
        draw_text(app, 76, y + 11, 4, prefix, text, 2);
        draw_text(app, 112, y + 11, 4, entry->name, text, 52);
    }
    const char *previous = "< VORHERIGE";
    const char *next = "NAECHSTE >";
    rectangle(app, 58, 666, 260, 48,
              app->hover == 9 ? selected : normal);
    rectangle(app, 330, 666, 260, 48,
              app->hover == 10 ? selected : normal);
    rectangle(app, 602, 666, 364, 48,
              app->hover == 11 ? selected : normal);
    draw_text(app, 76, 678, 4, previous, text, 20);
    draw_text(app, 352, 678, 4, next, text, 20);
    char page_text[48];
    snprintf(page_text, sizeof(page_text), "SEITE %zu/%zu   BEENDEN",
             app->page + 1U, page_count(app));
    draw_text(app, 624, 678, 4, page_text, text, 28);
    if (app->pointer_visible) {
        uint32_t pointer = rgba(app, 255, 244, 96, 255);
        uint32_t pointer_center = rgba(app, 255, 255, 255, 255);
        rectangle(app, app->pointer_x - 7, app->pointer_y - 2,
                  15, 5, pointer);
        rectangle(app, app->pointer_x - 2, app->pointer_y - 7,
                  5, 15, pointer);
        rectangle(app, app->pointer_x - 1, app->pointer_y - 1,
                  3, 3, pointer_center);
    }
}

static void upload_ui(struct browser_app *app)
{
    render_browser_pixels(app);
    XrSwapchainImageAcquireInfo acquire = {
        .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO,
    };
    uint32_t index = 0;
    need_xr("xrAcquireSwapchainImage",
            xrAcquireSwapchainImage(app->swapchain, &acquire, &index));
    XrSwapchainImageWaitInfo wait = {
        .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
        .timeout = XR_INFINITE_DURATION,
    };
    need_xr("xrWaitSwapchainImage",
            xrWaitSwapchainImage(app->swapchain, &wait));
    need(index < app->image_count, "invalid UI swapchain index");
    need_vk("vkWaitForFences(UI)",
            vkWaitForFences(app->device, 1, &app->fence,
                            VK_TRUE, UINT64_C(5000000000)));
    need_vk("vkResetFences(UI)",
            vkResetFences(app->device, 1, &app->fence));
    need_vk("vkResetCommandPool(UI)",
            vkResetCommandPool(app->device, app->command_pool, 0));
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    need_vk("vkBeginCommandBuffer(UI)",
            vkBeginCommandBuffer(app->command, &begin));
    VkImageMemoryBarrier to_transfer = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = app->image_initialized[index]
                             ? VK_ACCESS_SHADER_READ_BIT : 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = app->image_initialized[index]
                         ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                         : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .image = app->images[index].image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    vkCmdPipelineBarrier(app->command,
                         app->image_initialized[index]
                             ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                             : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 1, &to_transfer);
    VkBufferImageCopy copy = {
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .layerCount = 1,
        },
        .imageExtent = {UI_WIDTH, UI_HEIGHT, 1},
    };
    vkCmdCopyBufferToImage(app->command, app->staging_buffer,
                           app->images[index].image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &copy);
    VkImageMemoryBarrier to_sample = to_transfer;
    to_sample.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_sample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_sample.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_sample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(app->command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, NULL, 0, NULL, 1, &to_sample);
    need_vk("vkEndCommandBuffer(UI)", vkEndCommandBuffer(app->command));
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &app->command,
    };
    need_vk("vkQueueSubmit(UI)",
            vkQueueSubmit(app->queue, 1, &submit, app->fence));
    need_vk("vkWaitForFences(UI completion)",
            vkWaitForFences(app->device, 1, &app->fence,
                            VK_TRUE, UINT64_C(5000000000)));
    app->image_initialized[index] = true;
    XrSwapchainImageReleaseInfo release = {
        .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO,
    };
    need_xr("xrReleaseSwapchainImage",
            xrReleaseSwapchainImage(app->swapchain, &release));
    app->texture_dirty = false;
}

static XrVector3f rotate_vector(XrQuaternionf q, XrVector3f vector)
{
    XrVector3f u = {q.x, q.y, q.z};
    float dot_uv = u.x * vector.x + u.y * vector.y + u.z * vector.z;
    float dot_uu = u.x * u.x + u.y * u.y + u.z * u.z;
    XrVector3f cross = {
        u.y * vector.z - u.z * vector.y,
        u.z * vector.x - u.x * vector.z,
        u.x * vector.y - u.y * vector.x,
    };
    return (XrVector3f){
        2.0f * dot_uv * u.x + (q.w * q.w - dot_uu) * vector.x +
            2.0f * q.w * cross.x,
        2.0f * dot_uv * u.y + (q.w * q.w - dot_uu) * vector.y +
            2.0f * q.w * cross.y,
        2.0f * dot_uv * u.z + (q.w * q.w - dot_uu) * vector.z +
            2.0f * q.w * cross.z,
    };
}

static int hit_test(float u, float v)
{
    int pixel_x = (int)(u * UI_WIDTH);
    int pixel_y = (int)(v * UI_HEIGHT);
    if (pixel_x >= 58 && pixel_x < 966) {
        for (int row = 0; row < (int)PAGE_ENTRIES; ++row) {
            int top = 165 + row * 54;
            if (pixel_y >= top && pixel_y < top + 46)
                return row;
        }
    }
    if (pixel_y >= 666 && pixel_y < 714) {
        if (pixel_x >= 58 && pixel_x < 318)
            return 9;
        if (pixel_x >= 330 && pixel_x < 590)
            return 10;
        if (pixel_x >= 602 && pixel_x < 966)
            return 11;
    }
    return -1;
}

static void send_result(struct browser_app *app, uint32_t type,
                        const char *path)
{
    struct player_ui_result result = {.type = type};
    if (path)
        snprintf(result.path, sizeof(result.path), "%s", path);
    ssize_t written;
    do {
        written = send(app->ipc_fd, &result, sizeof(result), MSG_NOSIGNAL);
    } while (written < 0 && errno == EINTR);
    need(written == (ssize_t)sizeof(result), "send browser result failed");
    app->exit_requested = true;
}

static void activate_hover(struct browser_app *app)
{
    if (app->hover >= 0 && app->hover < (int)PAGE_ENTRIES) {
        size_t index = app->page * PAGE_ENTRIES + (size_t)app->hover;
        if (index >= app->listing.entry_count)
            return;
        const struct file_browser_entry *entry = &app->listing.entries[index];
        if (entry->kind == FILE_BROWSER_MEDIA) {
            send_result(app, PLAYER_UI_FILE_SELECTED, entry->path);
        } else {
            char error[256] = {0};
            if (file_browser_request(app->browser, entry->path,
                                     error, sizeof(error)) < 0) {
                snprintf(app->listing.error, sizeof(app->listing.error),
                         "%s", error);
                app->texture_dirty = true;
            }
        }
    } else if (app->hover == 9) {
        if (app->page > 0)
            --app->page;
        app->texture_dirty = true;
    } else if (app->hover == 10) {
        if (app->page + 1U < page_count(app))
            ++app->page;
        app->texture_dirty = true;
    } else if (app->hover == 11) {
        send_result(app, PLAYER_UI_EXIT_REQUESTED, NULL);
    }
}

static void sync_input(struct browser_app *app, XrTime time)
{
    XrActiveActionSet active = {.actionSet = app->action_set};
    XrActionsSyncInfo sync = {
        .type = XR_TYPE_ACTIONS_SYNC_INFO,
        .countActiveActionSets = 1,
        .activeActionSets = &active,
    };
    need_xr("xrSyncActions", xrSyncActions(app->session, &sync));
    int hover = -1;
    bool pressed = false;
    bool pointer_visible = false;
    int pointer_x = 0;
    int pointer_y = 0;
    for (unsigned hand = 0; hand < 2; ++hand) {
        XrActionStateGetInfo pose_info = {
            .type = XR_TYPE_ACTION_STATE_GET_INFO,
            .action = app->aim_action,
            .subactionPath = app->hands[hand],
        };
        XrActionStatePose pose_state = {.type = XR_TYPE_ACTION_STATE_POSE};
        need_xr("xrGetActionStatePose",
                xrGetActionStatePose(app->session, &pose_info, &pose_state));
        if (pose_state.isActive) {
            XrSpaceLocation location = {.type = XR_TYPE_SPACE_LOCATION};
            need_xr("xrLocateSpace(controller aim)",
                    xrLocateSpace(app->aim_spaces[hand], app->local_space,
                                  time, &location));
            XrSpaceLocationFlags valid =
                XR_SPACE_LOCATION_POSITION_VALID_BIT |
                XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            if ((location.locationFlags & valid) == valid) {
                XrVector3f direction = rotate_vector(
                    location.pose.orientation, (XrVector3f){0, 0, -1});
                if (direction.z < -0.0001f) {
                    float distance =
                        (-2.0f - location.pose.position.z) / direction.z;
                    if (distance > 0.0f) {
                        float x = location.pose.position.x +
                                  direction.x * distance;
                        float y = location.pose.position.y +
                                  direction.y * distance;
                        float u = x / PANEL_WIDTH_METERS + 0.5f;
                        float v = 0.5f - y / PANEL_HEIGHT_METERS;
                        if (u >= 0 && u < 1 && v >= 0 && v < 1) {
                            pointer_visible = true;
                            pointer_x = (int)(u * UI_WIDTH);
                            pointer_y = (int)(v * UI_HEIGHT);
                            hover = hit_test(u, v);
                        }
                    }
                }
            }
        }
        XrActionStateGetInfo select_info = {
            .type = XR_TYPE_ACTION_STATE_GET_INFO,
            .action = app->select_action,
            .subactionPath = app->hands[hand],
        };
        XrActionStateFloat select = {.type = XR_TYPE_ACTION_STATE_FLOAT};
        need_xr("xrGetActionStateFloat",
                xrGetActionStateFloat(app->session, &select_info, &select));
        bool down = app->select_down[hand];
        if (!select.isActive)
            down = false;
        else if (!down && select.currentState >= SELECT_PRESS_THRESHOLD)
            down = true;
        else if (down && select.currentState < SELECT_RELEASE_THRESHOLD)
            down = false;
        if (down && !app->select_down[hand])
            pressed = true;
        app->select_down[hand] = down;
    }
    if (hover != app->hover) {
        app->hover = hover;
        app->texture_dirty = true;
    }
    if (pointer_visible != app->pointer_visible ||
        (pointer_visible &&
         (pointer_x != app->pointer_x || pointer_y != app->pointer_y))) {
        app->pointer_visible = pointer_visible;
        app->pointer_x = pointer_x;
        app->pointer_y = pointer_y;
        app->texture_dirty = true;
    }
    if (pressed)
        activate_hover(app);
}

static void render_frame(struct browser_app *app)
{
    XrFrameState state = {.type = XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo wait = {.type = XR_TYPE_FRAME_WAIT_INFO};
    need_xr("xrWaitFrame", xrWaitFrame(app->session, &wait, &state));
    XrFrameBeginInfo begin = {.type = XR_TYPE_FRAME_BEGIN_INFO};
    need_xr("xrBeginFrame", xrBeginFrame(app->session, &begin));
    sync_input(app, state.predictedDisplayTime);
    if (app->texture_dirty)
        upload_ui(app);
    XrCompositionLayerQuad quad = {
        .type = XR_TYPE_COMPOSITION_LAYER_QUAD,
        .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
        .space = app->local_space,
        .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
        .subImage = {
            .swapchain = app->swapchain,
            .imageRect = {.extent = {UI_WIDTH, UI_HEIGHT}},
        },
        .pose = {{0, 0, 0, 1}, {0, 0, -2.0f}},
        .size = {PANEL_WIDTH_METERS, PANEL_HEIGHT_METERS},
    };
    const XrCompositionLayerBaseHeader *layers[] = {
        (const XrCompositionLayerBaseHeader *)&quad,
    };
    XrFrameEndInfo end = {
        .type = XR_TYPE_FRAME_END_INFO,
        .displayTime = state.predictedDisplayTime,
        .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
        .layerCount = state.shouldRender ? 1U : 0U,
        .layers = state.shouldRender ? layers : NULL,
    };
    need_xr("xrEndFrame", xrEndFrame(app->session, &end));
}

static void update_listing(struct browser_app *app)
{
    struct file_browser_listing updated = {0};
    int result = file_browser_poll(app->browser, &updated);
    need(result >= 0, "poll file browser failed");
    if (!result)
        return;
    file_browser_listing_clear(&app->listing);
    app->listing = updated;
    app->page = 0;
    app->hover = -1;
    app->texture_dirty = true;
    printf("browser_listing generation=%" PRIu64 " path=%s entries=%zu"
           " error=%s\n",
           app->listing.generation,
           app->listing.path ? app->listing.path : "",
           app->listing.entry_count,
           app->listing.error[0] ? app->listing.error : "none");
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc != 3 && argc != 4) {
        fprintf(stderr,
                "usage: %s IPC_FD INITIAL_DIRECTORY [STATUS_MESSAGE]\n",
                argv[0]);
        return 2;
    }
    struct browser_app app = {
        .ipc_fd = (int)parse_u64(argv[1]),
        .hover = -1,
        .texture_dirty = true,
    };
    if (argc == 4)
        snprintf(app.status_message, sizeof(app.status_message),
                 "%s", argv[3]);
    char browser_error[256] = {0};
    if (file_browser_open(&app.browser, argv[2],
                          browser_error, sizeof(browser_error)) < 0) {
        fprintf(stderr, "browser_error=%s\n", browser_error);
        return 2;
    }
    initialize_xr_vulkan(&app);
    XrGraphicsBindingVulkan2KHR binding = {
        .type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR,
        .instance = app.vk_instance,
        .physicalDevice = app.physical,
        .device = app.device,
        .queueFamilyIndex = app.queue_family,
        .queueIndex = 0,
    };
    XrSessionCreateInfo session = {
        .type = XR_TYPE_SESSION_CREATE_INFO,
        .next = &binding,
        .systemId = app.system,
    };
    need_xr("xrCreateSession",
            xrCreateSession(app.xr, &session, &app.session));
    initialize_actions(&app);
    struct timespec delay = {.tv_nsec = 10000000};
    struct timespec started;
    clock_gettime(CLOCK_MONOTONIC, &started);
    while (!app.running && !app.exit_requested) {
        poll_events(&app);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        need(now.tv_sec - started.tv_sec < 10,
             "OpenXR browser session did not become ready");
        if (!app.running)
            nanosleep(&delay, NULL);
    }
    XrReferenceSpaceCreateInfo space = {
        .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
        .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL,
        .poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}},
    };
    need_xr("xrCreateReferenceSpace",
            xrCreateReferenceSpace(app.session, &space, &app.local_space));
    create_swapchain(&app);
    create_transfer_resources(&app);
    while (!app.exit_requested) {
        update_listing(&app);
        poll_events(&app);
        if (app.running)
            render_frame(&app);
        else
            nanosleep(&delay, NULL);
    }
    if (app.session_state != XR_SESSION_STATE_EXITING &&
        app.session_state != XR_SESSION_STATE_LOSS_PENDING &&
        app.ipc_fd >= 0) {
        /* A file/button action already sent its result. Runtime exit has not. */
    } else if (app.ipc_fd >= 0) {
        send_result(&app, PLAYER_UI_EXIT_REQUESTED, NULL);
    }
    file_browser_listing_clear(&app.listing);
    file_browser_close(&app.browser);
    return 0;
}
