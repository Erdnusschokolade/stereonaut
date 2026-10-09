/* Continuously render Iris NV12 DMA-BUF frames on the validated OpenXR quad. */
#define _POSIX_C_SOURCE 200809L
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_PLATFORM_XLIB

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/dma-buf.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "nv12-playback-ipc.h"

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define MAX_IMAGES 8U
#define UBO_STRIDE 256U
#define DRM_FORMAT_MOD_LINEAR UINT64_C(0)
#define UI_WIDTH 1024U
#define UI_HEIGHT 256U
#define UI_BYTES (UI_WIDTH * UI_HEIGHT * 4U)
#define UI_PANEL_WIDTH_METERS 1.4f
#define UI_PANEL_HEIGHT_METERS 0.35f

struct mat4 { float v[16]; };

struct view_uniform {
    struct mat4 mvp;
    struct mat4 camera_to_world;
    float fov_tangents[4];
    int32_t modes[4];
    float source_info[4];
};

_Static_assert(sizeof(struct view_uniform) <= UBO_STRIDE,
               "view uniform exceeds dynamic UBO stride");

struct video_buffer {
    int dma_buf_fd;
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkSemaphore producer_semaphore;
    VkDescriptorSet descriptor_set;
    bool producer_wait_pending;
};

struct app {
    XrInstance xr;
    XrSystemId system;
    XrSession session;
    XrSpace space;
    XrSpace view_space;
    XrSessionState state;
    bool running;
    bool xr_should_render;
    bool view_geometry_logged;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkCommandPool command_pool;
    VkCommandBuffer command;
    VkFence fence;
    VkRenderPass render_pass;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    VkDescriptorSetLayout descriptor_layout;
    VkDescriptorPool descriptor_pool;
    VkSamplerYcbcrConversion conversion;
    VkSampler video_sampler;
    struct video_buffer video_buffers[NV12_IPC_MAX_BUFFERS];
    uint32_t video_buffer_count;
    int current_video_index;
    bool current_video_presented;
    int ipc_fd;
    bool eos_received;
    PFN_vkImportSemaphoreFdKHR import_semaphore_fd;
    PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties;
    PFN_vkGetImageDrmFormatModifierPropertiesEXT get_modifier_properties;
    VkDeviceSize dma_buf_size;
    uint32_t video_width, video_height, video_stride;
    uint32_t projection_mode, stereo_layout;
    bool swap_eyes;
    XrActionSet action_set;
    XrAction aim_action;
    XrAction select_action;
    XrPath hands[2];
    XrSpace aim_spaces[2];
    int ui_hover;
    bool ui_dirty;
    uint32_t player_state;
    int64_t position_us;
    int64_t duration_us;
    VkDeviceSize uv_offset;
    VkBuffer uniform_buffer;
    VkDeviceMemory uniform_memory;
    void *uniform_mapping;
    VkFormat color_format;
    XrSwapchain swapchain;
    uint32_t width, height, image_count;
    XrSwapchainImageVulkan2KHR images[MAX_IMAGES];
    VkImageView image_views[MAX_IMAGES][2];
    VkFramebuffer framebuffers[MAX_IMAGES][2];
    XrSwapchain ui_swapchain;
    uint32_t ui_image_count;
    XrSwapchainImageVulkan2KHR ui_images[MAX_IMAGES];
    bool ui_image_initialized[MAX_IMAGES];
    VkFormat ui_format;
    bool ui_bgra;
    VkBuffer ui_staging_buffer;
    VkDeviceMemory ui_staging_memory;
    uint32_t *ui_pixels;
    VkCommandBuffer ui_command;
    VkFence ui_fence;
    uint64_t xr_frames, rendered_frames, session_changes;
    uint64_t received_frames, released_frames;
    uint64_t unique_video_frames, repeated_video_frames;
    uint64_t hidden_video_frames, pipeline_skipped_frames;
};

static void need_vk(const char *what, VkResult result);
static void need(bool condition, const char *what);

static uint64_t parse_u64(const char *text, const char *name)
{
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || !end || *end || value == 0) {
        fprintf(stderr, "invalid %s: %s\n", name, text);
        exit(2);
    }
    return (uint64_t)value;
}

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

static void die_xr(const char *what, XrResult result)
{
    fprintf(stderr, "%s: XrResult=%d\n", what, result);
    exit(2);
}

static void need_xr(const char *what, XrResult result)
{
    if (XR_FAILED(result)) die_xr(what, result);
}

static void need_vk(const char *what, VkResult result)
{
    if (result != VK_SUCCESS) {
        fprintf(stderr, "%s: VkResult=%d\n", what, result);
        exit(2);
    }
}

static void need(bool condition, const char *what)
{
    if (!condition) {
        fprintf(stderr, "%s\n", what);
        exit(2);
    }
}

static XrPath xr_path(struct app *app, const char *text)
{
    XrPath path = XR_NULL_PATH;
    need_xr("xrStringToPath", xrStringToPath(app->xr, text, &path));
    return path;
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

static void initialize_actions(struct app *app)
{
    app->hands[0] = xr_path(app, "/user/hand/left");
    app->hands[1] = xr_path(app, "/user/hand/right");
    XrActionSetCreateInfo set = {
        .type = XR_TYPE_ACTION_SET_CREATE_INFO,
    };
    snprintf(set.actionSetName, sizeof(set.actionSetName), "player");
    snprintf(set.localizedActionSetName, sizeof(set.localizedActionSetName),
             "Stereonaut Player");
    need_xr("xrCreateActionSet",
            xrCreateActionSet(app->xr, &set, &app->action_set));
    create_action(app->action_set, XR_ACTION_TYPE_POSE_INPUT,
                  "aim", "Zeigen", app->hands, &app->aim_action);
    create_action(app->action_set, XR_ACTION_TYPE_BOOLEAN_INPUT,
                  "select", "Auswaehlen", app->hands,
                  &app->select_action);
    XrPath profile = xr_path(app,
        "/interaction_profiles/khr/simple_controller");
    XrActionSuggestedBinding bindings[4] = {
        {app->aim_action,
         xr_path(app, "/user/hand/left/input/aim/pose")},
        {app->aim_action,
         xr_path(app, "/user/hand/right/input/aim/pose")},
        {app->select_action,
         xr_path(app, "/user/hand/left/input/select/click")},
        {app->select_action,
         xr_path(app, "/user/hand/right/input/select/click")},
    };
    XrInteractionProfileSuggestedBinding suggestion = {
        .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
        .interactionProfile = profile,
        .countSuggestedBindings = ARRAY_COUNT(bindings),
        .suggestedBindings = bindings,
    };
    need_xr("xrSuggestInteractionProfileBindings",
            xrSuggestInteractionProfileBindings(app->xr, &suggestion));
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

static void receive_initial_buffers(struct app *app)
{
    struct nv12_ipc_message message = {0};
    char control[CMSG_SPACE(sizeof(int) * NV12_IPC_MAX_BUFFERS)] = {0};
    struct iovec iov = {.iov_base = &message, .iov_len = sizeof(message)};
    struct msghdr received = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control, .msg_controllen = sizeof(control),
    };
    ssize_t bytes;
    do {
        bytes = recvmsg(app->ipc_fd, &received, 0);
    } while (bytes < 0 && errno == EINTR);
    if (bytes < 0) { perror("recvmsg NV12 init"); exit(2); }
    need((size_t)bytes == sizeof(message) && message.type == NV12_IPC_INIT,
         "invalid NV12 IPC init message");
    need(message.buffer_count == 12 &&
             message.buffer_count <= NV12_IPC_MAX_BUFFERS,
         "NV12 IPC buffer count is not twelve");
    struct cmsghdr *header = CMSG_FIRSTHDR(&received);
    need(header && header->cmsg_level == SOL_SOCKET &&
             header->cmsg_type == SCM_RIGHTS,
         "NV12 IPC init lacks DMA-BUF file descriptors");
    size_t fd_bytes = header->cmsg_len - CMSG_LEN(0);
    need(fd_bytes == message.buffer_count * sizeof(int),
         "NV12 IPC descriptor count differs from buffer count");
    int *fds = (int *)CMSG_DATA(header);
    app->video_buffer_count = message.buffer_count;
    for (uint32_t i = 0; i < app->video_buffer_count; ++i) {
        app->video_buffers[i].dma_buf_fd = fds[i];
        struct stat status;
        if (fstat(fds[i], &status) < 0) {
            perror("fstat IPC DMA-BUF");
            exit(2);
        }
        need((uint64_t)status.st_size == message.size,
             "IPC DMA-BUF length differs from contract");
    }
    app->video_width = message.width;
    app->video_height = message.height;
    app->video_stride = message.stride;
    app->dma_buf_size = message.size;
    app->uv_offset = message.uv_offset;
    app->projection_mode = message.projection_mode;
    app->stereo_layout = message.stereo_layout;
    app->swap_eyes = message.swap_eyes != 0;
    need(app->video_width > 0 && app->video_height > 0 &&
             !(app->video_width & 1U) && !(app->video_height & 1U) &&
             app->video_stride >= app->video_width &&
             app->uv_offset ==
                 (uint64_t)app->video_stride * app->video_height &&
             app->dma_buf_size == app->uv_offset +
                 (uint64_t)app->video_stride * app->video_height / 2U,
         "NV12 IPC layout is not contiguous linear 4:2:0");
    need(app->projection_mode <= NV12_PROJECTION_EQUIRECT_360,
         "invalid projection mode in IPC contract");
    need(app->stereo_layout <= NV12_STEREO_TB,
         "invalid stereo layout in IPC contract");
    printf("render_config projection=%u stereo=%u swap_eyes=%u"
           " view0=left view1=right\n",
           app->projection_mode, app->stereo_layout, app->swap_eyes);
}

static void send_ipc(struct app *app, uint32_t type, uint32_t index)
{
    struct nv12_ipc_message message = {.type = type, .index = index};
    ssize_t written;
    do {
        written = send(app->ipc_fd, &message, sizeof(message), MSG_NOSIGNAL);
    } while (written < 0 && errno == EINTR);
    if (written < 0) { perror("send NV12 IPC response"); exit(2); }
    need((size_t)written == sizeof(message), "short NV12 IPC response");
}

static void release_current_video(struct app *app)
{
    if (app->current_video_index < 0)
        return;
    if (!app->current_video_presented) {
        if (app->xr_should_render)
            app->pipeline_skipped_frames++;
        else
            app->hidden_video_frames++;
    }
    send_ipc(app, NV12_IPC_RELEASE, (uint32_t)app->current_video_index);
    app->released_frames++;
    app->current_video_index = -1;
}

static void receive_frame_messages(struct app *app)
{
    for (;;) {
        struct nv12_ipc_message message = {0};
        ssize_t bytes = recv(app->ipc_fd, &message, sizeof(message),
                             MSG_DONTWAIT);
        if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (bytes < 0 && errno == EINTR)
            continue;
        if (bytes == 0) {
            app->eos_received = true;
            return;
        }
        if (bytes < 0) { perror("recv NV12 IPC message"); exit(2); }
        need((size_t)bytes == sizeof(message), "short NV12 IPC message");
        if (message.type == NV12_IPC_FRAME) {
            need(message.index < app->video_buffer_count,
                 "NV12 IPC frame index is invalid");
            release_current_video(app);
            app->current_video_index = (int)message.index;
            app->current_video_presented = false;
            app->video_buffers[message.index].producer_wait_pending = true;
            app->received_frames++;
        } else if (message.type == NV12_IPC_FLUSH) {
            release_current_video(app);
            send_ipc(app, NV12_IPC_FLUSHED, 0);
        } else if (message.type == NV12_IPC_EOS) {
            app->eos_received = true;
        } else if (message.type == NV12_IPC_STATUS) {
            app->player_state = message.player_state;
            app->position_us = message.position_us;
            app->duration_us = message.duration_us;
            app->ui_dirty = true;
        } else {
            need(false, "unknown NV12 IPC message");
        }
        /* The socket is the bounded FIFO. Consume at most one decoded frame
         * per XR iteration so a decoder burst cannot discard valid frames. */
        return;
    }
}

static uint32_t memory_type(VkPhysicalDevice physical, uint32_t bits,
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

static struct mat4 multiply(struct mat4 a, struct mat4 b)
{
    struct mat4 result = {{0}};
    for (unsigned col = 0; col < 4; ++col)
        for (unsigned row = 0; row < 4; ++row)
            for (unsigned k = 0; k < 4; ++k)
                result.v[col * 4 + row] += a.v[k * 4 + row] * b.v[col * 4 + k];
    return result;
}

static struct mat4 view_from_pose(XrPosef pose)
{
    float x = pose.orientation.x, y = pose.orientation.y;
    float z = pose.orientation.z, w = pose.orientation.w;
    /* Invert the rigid OpenXR view pose.  The rotational part of the view
     * matrix is the transpose of the pose's quaternion rotation. */
    struct mat4 result = {{
        1 - 2*y*y - 2*z*z, 2*x*y - 2*z*w,     2*x*z + 2*y*w,     0,
        2*x*y + 2*z*w,     1 - 2*x*x - 2*z*z, 2*y*z - 2*x*w,     0,
        2*x*z - 2*y*w,     2*y*z + 2*x*w,     1 - 2*x*x - 2*y*y, 0,
        0,                 0,                 0,                 1,
    }};
    result.v[12] = -(result.v[0] * pose.position.x + result.v[4] * pose.position.y + result.v[8] * pose.position.z);
    result.v[13] = -(result.v[1] * pose.position.x + result.v[5] * pose.position.y + result.v[9] * pose.position.z);
    result.v[14] = -(result.v[2] * pose.position.x + result.v[6] * pose.position.y + result.v[10] * pose.position.z);
    return result;
}

static struct mat4 rotation_from_pose(XrPosef pose)
{
    float x = pose.orientation.x, y = pose.orientation.y;
    float z = pose.orientation.z, w = pose.orientation.w;
    /* Camera-to-LOCAL rotation. Position is deliberately excluded so
     * equirectangular video cannot acquire geometry parallax from head or
     * physical eye translation. */
    return (struct mat4){{
        1 - 2*y*y - 2*z*z, 2*x*y + 2*z*w,     2*x*z - 2*y*w,     0,
        2*x*y - 2*z*w,     1 - 2*x*x - 2*z*z, 2*y*z + 2*x*w,     0,
        2*x*z + 2*y*w,     2*y*z - 2*x*w,     1 - 2*x*x - 2*y*y, 0,
        0,                 0,                 0,                 1,
    }};
}

static struct mat4 projection_from_fov(XrFovf fov)
{
    const float near_z = 0.05f, far_z = 100.0f;
    float left = tanf(fov.angleLeft), right = tanf(fov.angleRight);
    float down = tanf(fov.angleDown), up = tanf(fov.angleUp);
    struct mat4 result = {{0}};
    result.v[0] = 2.0f / (right - left);
    /* Vulkan's framebuffer Y direction is opposite to OpenXR's view-space
     * Y direction when a positive viewport height is used. */
    result.v[5] = 2.0f / (down - up);
    result.v[8] = (right + left) / (right - left);
    result.v[9] = (up + down) / (down - up);
    result.v[10] = far_z / (near_z - far_z);
    result.v[11] = -1.0f;
    result.v[14] = -(far_z * near_z) / (far_z - near_z);
    return result;
}

static void poll_events(struct app *app)
{
    for (;;) {
        XrEventDataBuffer event = {.type = XR_TYPE_EVENT_DATA_BUFFER};
        XrResult result = xrPollEvent(app->xr, &event);
        if (result == XR_EVENT_UNAVAILABLE) return;
        need_xr("xrPollEvent", result);
        if (event.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) continue;
        XrEventDataSessionStateChanged *change =
            (XrEventDataSessionStateChanged *)&event;
        app->state = change->state;
        ++app->session_changes;
        printf("openxr_session_state=%d time=%" PRId64 "\n", app->state,
               change->time);
        if (app->state == XR_SESSION_STATE_READY) {
            XrSessionBeginInfo begin = {
                .type = XR_TYPE_SESSION_BEGIN_INFO,
                .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            };
            need_xr("xrBeginSession", xrBeginSession(app->session, &begin));
            app->running = true;
        } else if (app->state == XR_SESSION_STATE_STOPPING) {
            need_xr("xrEndSession", xrEndSession(app->session));
            app->running = false;
        } else if (app->state == XR_SESSION_STATE_EXITING ||
                   app->state == XR_SESSION_STATE_LOSS_PENDING) {
            app->running = false;
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

static VkShaderModule shader(VkDevice device, const char *path)
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

static void initialize_xr_vulkan(struct app *app)
{
    const char *extensions[] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    XrInstanceCreateInfo info = {
        .type = XR_TYPE_INSTANCE_CREATE_INFO,
        .applicationInfo = {.applicationVersion = 1, .engineVersion = 1,
                            .apiVersion = XR_MAKE_VERSION(1, 0, 0)},
        .enabledExtensionCount = ARRAY_COUNT(extensions),
        .enabledExtensionNames = extensions,
    };
    snprintf(info.applicationInfo.applicationName,
             sizeof(info.applicationInfo.applicationName), "stereonaut-xr-quad");
    snprintf(info.applicationInfo.engineName, sizeof(info.applicationInfo.engineName), "stereonaut");
    need_xr("xrCreateInstance", xrCreateInstance(&info, &app->xr));
    XrSystemGetInfo system_info = {.type = XR_TYPE_SYSTEM_GET_INFO,
                                   .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    need_xr("xrGetSystem", xrGetSystem(app->xr, &system_info, &app->system));

    PFN_xrCreateVulkanInstanceKHR create_instance = NULL;
    PFN_xrGetVulkanGraphicsDevice2KHR get_device = NULL;
    PFN_xrCreateVulkanDeviceKHR create_device = NULL;
    PFN_xrGetVulkanGraphicsRequirements2KHR get_requirements = NULL;
    need_xr("xrGetInstanceProcAddr(create instance)", xrGetInstanceProcAddr(app->xr, "xrCreateVulkanInstanceKHR", (PFN_xrVoidFunction *)&create_instance));
    need_xr("xrGetInstanceProcAddr(get device)", xrGetInstanceProcAddr(app->xr, "xrGetVulkanGraphicsDevice2KHR", (PFN_xrVoidFunction *)&get_device));
    need_xr("xrGetInstanceProcAddr(create device)", xrGetInstanceProcAddr(app->xr, "xrCreateVulkanDeviceKHR", (PFN_xrVoidFunction *)&create_device));
    need_xr("xrGetInstanceProcAddr(requirements)", xrGetInstanceProcAddr(app->xr, "xrGetVulkanGraphicsRequirements2KHR", (PFN_xrVoidFunction *)&get_requirements));
    need(create_instance && get_device && create_device && get_requirements, "missing Vulkan-enable2 entry point");
    XrGraphicsRequirementsVulkanKHR requirements = {
        .type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR,
    };
    need_xr("xrGetVulkanGraphicsRequirements2KHR", get_requirements(app->xr, app->system, &requirements));
    printf("openxr_vulkan_requirements min=%" PRIu64 " max=%" PRIu64 "\n",
           requirements.minApiVersionSupported, requirements.maxApiVersionSupported);

    VkApplicationInfo vk_app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "stereonaut-xr-quad", .applicationVersion = 1,
        .pEngineName = "stereonaut", .engineVersion = 1, .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo vk_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &vk_app};
    XrVulkanInstanceCreateInfoKHR xr_vk_info = {
        .type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR, .systemId = app->system,
        .pfnGetInstanceProcAddr = vkGetInstanceProcAddr, .vulkanCreateInfo = &vk_info};
    VkResult vk_result = VK_SUCCESS;
    need_xr("xrCreateVulkanInstanceKHR", create_instance(app->xr, &xr_vk_info, &app->instance, &vk_result));
    need_vk("xrCreateVulkanInstanceKHR Vulkan", vk_result);
    XrVulkanGraphicsDeviceGetInfoKHR device_info = {
        .type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR,
        .systemId = app->system, .vulkanInstance = app->instance};
    need_xr("xrGetVulkanGraphicsDevice2KHR", get_device(app->xr, &device_info, &app->physical));

    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(app->physical, &count, NULL);
    VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
    need(families != NULL, "allocate queue families");
    vkGetPhysicalDeviceQueueFamilyProperties(app->physical, &count, families);
    bool found = false;
    for (uint32_t i = 0; i < count; ++i)
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            app->queue_family = i; found = true; break;
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
        need(has_device_extension(app->physical, device_extensions[i]),
             "required NV12 DMA-BUF Vulkan extension is missing");
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = app->queue_family, .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo vk_device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = ARRAY_COUNT(device_extensions),
        .ppEnabledExtensionNames = device_extensions};
    XrVulkanDeviceCreateInfoKHR xr_device_info = {
        .type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR, .systemId = app->system,
        .pfnGetInstanceProcAddr = vkGetInstanceProcAddr, .vulkanPhysicalDevice = app->physical,
        .vulkanCreateInfo = &vk_device_info};
    need_xr("xrCreateVulkanDeviceKHR", create_device(app->xr, &xr_device_info, &app->device, &vk_result));
    need_vk("xrCreateVulkanDeviceKHR Vulkan", vk_result);
    vkGetDeviceQueue(app->device, app->queue_family, 0, &app->queue);
    need(app->queue != VK_NULL_HANDLE, "vkGetDeviceQueue returned null");
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(app->physical, &properties);
    printf("openxr_vulkan_device=%s queue_family=%u\n", properties.deviceName, app->queue_family);
    app->import_semaphore_fd = (PFN_vkImportSemaphoreFdKHR)
        vkGetDeviceProcAddr(app->device, "vkImportSemaphoreFdKHR");
    app->get_memory_fd_properties = (PFN_vkGetMemoryFdPropertiesKHR)
        vkGetDeviceProcAddr(app->device, "vkGetMemoryFdPropertiesKHR");
    app->get_modifier_properties = (PFN_vkGetImageDrmFormatModifierPropertiesEXT)
        vkGetDeviceProcAddr(app->device,
                            "vkGetImageDrmFormatModifierPropertiesEXT");
    need(app->import_semaphore_fd && app->get_memory_fd_properties &&
             app->get_modifier_properties,
         "required external-memory entry point is missing");
}

static void initialize_swapchain(struct app *app)
{
    uint32_t format_count = 0;
    need_xr("xrEnumerateSwapchainFormats(count)", xrEnumerateSwapchainFormats(app->session, 0, &format_count, NULL));
    int64_t *formats = calloc(format_count, sizeof(*formats));
    need(formats != NULL, "allocate swapchain formats");
    need_xr("xrEnumerateSwapchainFormats(list)", xrEnumerateSwapchainFormats(app->session, format_count, &format_count, formats));
    app->color_format = VK_FORMAT_B8G8R8A8_SRGB;
    bool supported = false;
    for (uint32_t i = 0; i < format_count; ++i) if (formats[i] == app->color_format) supported = true;
    if (!supported) { app->color_format = (VkFormat)formats[0]; supported = true; }
    free(formats);
    XrViewConfigurationView views[2] = {
        {.type = XR_TYPE_VIEW_CONFIGURATION_VIEW}, {.type = XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    uint32_t view_count = 0;
    need_xr("xrEnumerateViewConfigurationViews", xrEnumerateViewConfigurationViews(app->xr, app->system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &view_count, views));
    need(view_count == 2, "Steam Frame did not report stereo views");
    app->width = views[0].recommendedImageRectWidth;
    app->height = views[0].recommendedImageRectHeight;
    need(views[1].recommendedImageRectWidth == app->width && views[1].recommendedImageRectHeight == app->height, "eyes have different recommended dimensions");
    XrSwapchainCreateInfo info = {.type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
        .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT,
        .format = app->color_format, .sampleCount = 1, .width = app->width,
        .height = app->height, .faceCount = 1, .arraySize = 2, .mipCount = 1};
    need_xr("xrCreateSwapchain", xrCreateSwapchain(app->session, &info, &app->swapchain));
    need_xr("xrEnumerateSwapchainImages(count)", xrEnumerateSwapchainImages(app->swapchain, 0, &app->image_count, NULL));
    need(app->image_count > 0 && app->image_count <= MAX_IMAGES, "unexpected OpenXR swapchain image count");
    for (uint32_t i = 0; i < app->image_count; ++i) app->images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
    need_xr("xrEnumerateSwapchainImages(list)", xrEnumerateSwapchainImages(app->swapchain, app->image_count, &app->image_count, (XrSwapchainImageBaseHeader *)app->images));
    printf("openxr_swapchain format=%d dimensions=%ux%u images=%u\n", app->color_format, app->width, app->height, app->image_count);
}

static void import_nv12_frames(struct app *app)
{
    VkDrmFormatModifierPropertiesListEXT list = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
    };
    VkFormatProperties2 format_properties = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &list,
    };
    vkGetPhysicalDeviceFormatProperties2(
        app->physical, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        &format_properties);
    need(list.drmFormatModifierCount > 0, "NV12 exposes no DRM modifiers");
    list.pDrmFormatModifierProperties =
        calloc(list.drmFormatModifierCount,
               sizeof(*list.pDrmFormatModifierProperties));
    need(list.pDrmFormatModifierProperties != NULL,
         "allocate NV12 modifier properties");
    vkGetPhysicalDeviceFormatProperties2(
        app->physical, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        &format_properties);
    VkFormatFeatureFlags features = 0;
    for (uint32_t i = 0; i < list.drmFormatModifierCount; ++i) {
        VkDrmFormatModifierPropertiesEXT *item =
            &list.pDrmFormatModifierProperties[i];
        if (item->drmFormatModifier == DRM_FORMAT_MOD_LINEAR &&
            item->drmFormatModifierPlaneCount == 2)
            features = item->drmFormatModifierTilingFeatures;
    }
    free(list.pDrmFormatModifierProperties);
    need(features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
         "linear two-plane NV12 is not sampleable");
    need(features &
             VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT,
         "linear-filtered NV12 YCbCr conversion is unsupported");
    VkChromaLocation chroma_location;
    if (features & VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT)
        chroma_location = VK_CHROMA_LOCATION_MIDPOINT;
    else if (features & VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT)
        chroma_location = VK_CHROMA_LOCATION_COSITED_EVEN;
    else
        need(false, "NV12 exposes no supported chroma sample location");

    VkSamplerYcbcrConversionCreateInfo conversion = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
        .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        .ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,
        .ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW,
        .components = {VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY},
        .xChromaOffset = chroma_location,
        .yChromaOffset = chroma_location,
        .chromaFilter = VK_FILTER_LINEAR,
    };
    need_vk("vkCreateSamplerYcbcrConversion",
            vkCreateSamplerYcbcrConversion(app->device, &conversion, NULL,
                                           &app->conversion));
    VkSamplerYcbcrConversionInfo conversion_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
        .conversion = app->conversion,
    };
    VkSamplerCreateInfo sampler = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = &conversion_info,
        .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 0.0f,
    };
    need_vk("vkCreateSampler(NV12)",
            vkCreateSampler(app->device, &sampler, NULL,
                            &app->video_sampler));

    const VkSubresourceLayout planes[2] = {
        {.offset = 0,
         .size = (VkDeviceSize)app->video_stride * app->video_height,
         .rowPitch = app->video_stride},
        {.offset = app->uv_offset,
         .size = (VkDeviceSize)app->video_stride * app->video_height / 2U,
         .rowPitch = app->video_stride},
    };
    for (uint32_t i = 0; i < app->video_buffer_count; ++i) {
        struct video_buffer *buffer = &app->video_buffers[i];
        VkImageDrmFormatModifierExplicitCreateInfoEXT modifier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
            .drmFormatModifier = DRM_FORMAT_MOD_LINEAR,
            .drmFormatModifierPlaneCount = 2, .pPlaneLayouts = planes,
        };
        VkExternalMemoryImageCreateInfo external = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .pNext = &modifier,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        };
        VkImageCreateInfo image = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &external,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
            .extent = {app->video_width, app->video_height, 1},
            .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        need_vk("vkCreateImage(NV12 DMA-BUF)",
                vkCreateImage(app->device, &image, NULL, &buffer->image));
        VkImageDrmFormatModifierPropertiesEXT chosen = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
        };
        need_vk("vkGetImageDrmFormatModifierPropertiesEXT",
                app->get_modifier_properties(app->device, buffer->image,
                                             &chosen));
        need(chosen.drmFormatModifier == DRM_FORMAT_MOD_LINEAR,
             "Vulkan substituted the NV12 modifier");
        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(app->device, buffer->image, &requirements);
        need(requirements.size <= app->dma_buf_size,
             "NV12 Vulkan memory requirement exceeds DMA-BUF");
        VkMemoryFdPropertiesKHR fd_properties = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
        };
        need_vk("vkGetMemoryFdPropertiesKHR",
                app->get_memory_fd_properties(
                    app->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                    buffer->dma_buf_fd, &fd_properties));
        uint32_t bits =
            requirements.memoryTypeBits & fd_properties.memoryTypeBits;
        need(bits != 0, "NV12 DMA-BUF memory-type intersection is empty");
        uint32_t type = memory_type(app->physical, bits, 0);
        int imported_fd = dup(buffer->dma_buf_fd);
        if (imported_fd < 0) { perror("dup NV12 DMA-BUF"); exit(2); }
        VkImportMemoryFdInfoKHR import = {
            .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
            .fd = imported_fd,
        };
        VkMemoryDedicatedAllocateInfo dedicated = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
            .pNext = &import, .image = buffer->image,
        };
        VkMemoryAllocateInfo allocation = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &dedicated, .allocationSize = app->dma_buf_size,
            .memoryTypeIndex = type,
        };
        need_vk("vkAllocateMemory(import NV12)",
                vkAllocateMemory(app->device, &allocation, NULL,
                                 &buffer->memory));
        need_vk("vkBindImageMemory(import NV12)",
                vkBindImageMemory(app->device, buffer->image,
                                  buffer->memory, 0));
        VkImageViewCreateInfo view = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = &conversion_info, .image = buffer->image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .levelCount = 1, .layerCount = 1},
        };
        need_vk("vkCreateImageView(NV12)",
                vkCreateImageView(app->device, &view, NULL, &buffer->view));
        VkSemaphoreCreateInfo semaphore = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        };
        need_vk("vkCreateSemaphore(producer)",
                vkCreateSemaphore(app->device, &semaphore, NULL,
                                  &buffer->producer_semaphore));
        printf("nv12_import index=%u memory_type=%u\n", i, type);
    }
    printf("nv12_contract format=%d modifier=0 memory_planes=2"
           " plane0_offset=0 plane0_pitch=%u plane1_offset=%" PRIu64
           " plane1_pitch=%u bytes=%" PRIu64
           " sampled=1 ycbcr_709=1 range=narrow chroma_location=%s\n",
           VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, app->video_stride,
           (uint64_t)app->uv_offset, app->video_stride,
           (uint64_t)app->dma_buf_size,
           chroma_location == VK_CHROMA_LOCATION_MIDPOINT ? "midpoint"
                                                          : "cosited_even");
}

static void create_pipeline(struct app *app)
{
    VkAttachmentDescription attachment = {.format = app->color_format, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference reference = {.attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &reference};
    VkRenderPassCreateInfo render_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &subpass};
    need_vk("vkCreateRenderPass", vkCreateRenderPass(app->device, &render_info, NULL, &app->render_pass));
    for (uint32_t image = 0; image < app->image_count; ++image) for (uint32_t eye = 0; eye < 2; ++eye) {
        VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = app->images[image].image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = app->color_format,
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .baseArrayLayer = eye, .layerCount = 1}};
        need_vk("vkCreateImageView", vkCreateImageView(app->device, &view, NULL, &app->image_views[image][eye]));
        VkFramebufferCreateInfo fb = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = app->render_pass, .attachmentCount = 1, .pAttachments = &app->image_views[image][eye], .width = app->width, .height = app->height, .layers = 1};
        need_vk("vkCreateFramebuffer", vkCreateFramebuffer(app->device, &fb, NULL, &app->framebuffers[image][eye]));
    }
    VkDescriptorSetLayoutBinding bindings[2] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
                       VK_SHADER_STAGE_FRAGMENT_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
         .pImmutableSamplers = &app->video_sampler},
    };
    VkDescriptorSetLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bindings};
    need_vk("vkCreateDescriptorSetLayout", vkCreateDescriptorSetLayout(app->device, &layout, NULL, &app->descriptor_layout));
    VkPipelineLayoutCreateInfo pipeline_layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &app->descriptor_layout};
    need_vk("vkCreatePipelineLayout", vkCreatePipelineLayout(app->device, &pipeline_layout, NULL, &app->pipeline_layout));
    VkShaderModule vert = shader(app->device, "video-projection.vert.spv"), frag = shader(app->device, "nv12-projection.frag.spv");
    VkPipelineShaderStageCreateInfo stages[2] = {{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert, .pName = "main"}, {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vertex = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP};
    VkViewport viewport = {.width = (float)app->width, .height = (float)app->height, .minDepth = 0, .maxDepth = 1};
    VkRect2D scissor = {.extent = {app->width, app->height}};
    VkPipelineViewportStateCreateInfo view = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
    VkPipelineRasterizationStateCreateInfo raster = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo multisample = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState blend_attachment = {.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &blend_attachment};
    VkGraphicsPipelineCreateInfo pipeline = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex, .pInputAssemblyState = &assembly, .pViewportState = &view, .pRasterizationState = &raster, .pMultisampleState = &multisample, .pColorBlendState = &blend, .layout = app->pipeline_layout, .renderPass = app->render_pass};
    need_vk("vkCreateGraphicsPipelines", vkCreateGraphicsPipelines(app->device, VK_NULL_HANDLE, 1, &pipeline, NULL, &app->pipeline));
    vkDestroyShaderModule(app->device, vert, NULL); vkDestroyShaderModule(app->device, frag, NULL);
}

static uint32_t ui_rgba(struct app *app, uint8_t r, uint8_t g,
                        uint8_t b, uint8_t a)
{
    if (app->ui_bgra) {
        uint8_t temporary = r;
        r = b;
        b = temporary;
    }
    return (uint32_t)r | ((uint32_t)g << 8U) |
           ((uint32_t)b << 16U) | ((uint32_t)a << 24U);
}

static void ui_rectangle(struct app *app, int x, int y, int width,
                         int height, uint32_t color)
{
    int left = x < 0 ? 0 : x;
    int top = y < 0 ? 0 : y;
    int right = x + width > (int)UI_WIDTH ? (int)UI_WIDTH : x + width;
    int bottom = y + height > (int)UI_HEIGHT ? (int)UI_HEIGHT : y + height;
    for (int row = top; row < bottom; ++row)
        for (int column = left; column < right; ++column)
            app->ui_pixels[(size_t)row * UI_WIDTH + (size_t)column] = color;
}

static const uint8_t ui_letter_rows[36][5] = {
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

static uint8_t ui_glyph_row(unsigned char character, unsigned row)
{
    if (character >= 'a' && character <= 'z')
        character = (unsigned char)(character - 'a' + 'A');
    if (character >= 'A' && character <= 'Z')
        return ui_letter_rows[character - 'A'][row];
    if (character >= '0' && character <= '9')
        return ui_letter_rows[26U + character - '0'][row];
    if (character == ':')
        return row == 1 || row == 3 ? 2 : 0;
    if (character == '-')
        return row == 2 ? 7 : 0;
    if (character == '+')
        return row == 2 ? 7 : (row == 1 || row == 3 ? 2 : 0);
    if (character == '<')
        return (uint8_t[]){1, 2, 4, 2, 1}[row];
    if (character == '>')
        return (uint8_t[]){4, 2, 1, 2, 4}[row];
    if (character == ' ')
        return 0;
    return (uint8_t[]){7, 5, 5, 5, 7}[row];
}

static void ui_text(struct app *app, int x, int y, int scale,
                    const char *text, uint32_t color)
{
    for (const unsigned char *cursor = (const unsigned char *)text;
         *cursor; ++cursor) {
        for (unsigned row = 0; row < 5; ++row) {
            uint8_t bits = ui_glyph_row(*cursor, row);
            for (unsigned column = 0; column < 3; ++column)
                if (bits & (1U << (2U - column)))
                    ui_rectangle(app, x + (int)column * scale,
                                 y + (int)row * scale,
                                 scale, scale, color);
        }
        x += 4 * scale;
    }
}

static void format_time(char text[32], int64_t microseconds)
{
    if (microseconds < 0)
        microseconds = 0;
    int64_t seconds = microseconds / INT64_C(1000000);
    snprintf(text, 32, "%02" PRId64 ":%02" PRId64,
             seconds / 60, seconds % 60);
}

static void render_ui_pixels(struct app *app)
{
    uint32_t transparent = ui_rgba(app, 0, 0, 0, 0);
    uint32_t panel = ui_rgba(app, 12, 22, 38, 235);
    uint32_t button = ui_rgba(app, 37, 56, 84, 245);
    uint32_t hover = ui_rgba(app, 22, 139, 202, 255);
    uint32_t text = ui_rgba(app, 242, 248, 255, 255);
    ui_rectangle(app, 0, 0, UI_WIDTH, UI_HEIGHT, transparent);
    ui_rectangle(app, 8, 8, 1008, 240, panel);
    char position[32], duration[32], clock_text[80];
    format_time(position, app->position_us);
    format_time(duration, app->duration_us);
    snprintf(clock_text, sizeof(clock_text), "%s / %s", position, duration);
    ui_text(app, 410, 34, 5, clock_text, text);
    struct {
        int x, width, action;
        const char *label;
    } buttons[] = {
        {28, 190, NV12_ACTION_BACK, "< ZURUECK"},
        {238, 150, NV12_ACTION_SEEK_BACK, "-10 S"},
        {408, 230, NV12_ACTION_TOGGLE_PAUSE,
         app->player_state == NV12_PLAYER_PAUSED ? "WEITER" : "PAUSE"},
        {658, 150, NV12_ACTION_SEEK_FORWARD, "+10 S"},
    };
    for (size_t i = 0; i < ARRAY_COUNT(buttons); ++i) {
        ui_rectangle(app, buttons[i].x, 104, buttons[i].width, 108,
                     app->ui_hover == buttons[i].action ? hover : button);
        ui_text(app, buttons[i].x + 20, 142, 5, buttons[i].label, text);
    }
}

static void create_ui_swapchain(struct app *app)
{
    uint32_t count = 0;
    need_xr("xrEnumerateSwapchainFormats(UI count)",
            xrEnumerateSwapchainFormats(app->session, 0, &count, NULL));
    int64_t *formats = calloc(count, sizeof(*formats));
    need(formats != NULL, "allocate UI swapchain formats");
    need_xr("xrEnumerateSwapchainFormats(UI list)",
            xrEnumerateSwapchainFormats(app->session, count, &count, formats));
    VkFormat preferred[] = {
        VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB,
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
    };
    for (size_t candidate = 0; candidate < ARRAY_COUNT(preferred); ++candidate)
        for (uint32_t i = 0; i < count; ++i)
            if (!app->ui_format && formats[i] == preferred[candidate])
                app->ui_format = preferred[candidate];
    free(formats);
    need(app->ui_format != VK_FORMAT_UNDEFINED,
         "no supported UI swapchain format");
    app->ui_bgra = app->ui_format == VK_FORMAT_B8G8R8A8_SRGB ||
                   app->ui_format == VK_FORMAT_B8G8R8A8_UNORM;
    XrSwapchainCreateInfo info = {
        .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
        .usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                      XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
        .format = app->ui_format,
        .sampleCount = 1,
        .width = UI_WIDTH,
        .height = UI_HEIGHT,
        .faceCount = 1,
        .arraySize = 1,
        .mipCount = 1,
    };
    need_xr("xrCreateSwapchain(UI)",
            xrCreateSwapchain(app->session, &info, &app->ui_swapchain));
    need_xr("xrEnumerateSwapchainImages(UI count)",
            xrEnumerateSwapchainImages(app->ui_swapchain, 0,
                                       &app->ui_image_count, NULL));
    need(app->ui_image_count > 0 && app->ui_image_count <= MAX_IMAGES,
         "unexpected UI swapchain image count");
    for (uint32_t i = 0; i < app->ui_image_count; ++i)
        app->ui_images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
    need_xr("xrEnumerateSwapchainImages(UI list)",
            xrEnumerateSwapchainImages(
                app->ui_swapchain, app->ui_image_count,
                &app->ui_image_count,
                (XrSwapchainImageBaseHeader *)app->ui_images));
    printf("ui_swapchain format=%d dimensions=%ux%u images=%u\n",
           app->ui_format, UI_WIDTH, UI_HEIGHT, app->ui_image_count);
}

static void create_ui_resources(struct app *app)
{
    VkCommandBufferAllocateInfo commands = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = app->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    need_vk("vkAllocateCommandBuffers(UI)",
            vkAllocateCommandBuffers(app->device, &commands,
                                     &app->ui_command));
    VkFenceCreateInfo fence = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    need_vk("vkCreateFence(UI)",
            vkCreateFence(app->device, &fence, NULL, &app->ui_fence));
    VkBufferCreateInfo buffer = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = UI_BYTES,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    need_vk("vkCreateBuffer(UI staging)",
            vkCreateBuffer(app->device, &buffer, NULL,
                           &app->ui_staging_buffer));
    VkMemoryRequirements memory;
    vkGetBufferMemoryRequirements(app->device,
                                  app->ui_staging_buffer, &memory);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memory.size,
        .memoryTypeIndex = memory_type(
            app->physical, memory.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    need_vk("vkAllocateMemory(UI staging)",
            vkAllocateMemory(app->device, &allocation, NULL,
                             &app->ui_staging_memory));
    need_vk("vkBindBufferMemory(UI staging)",
            vkBindBufferMemory(app->device, app->ui_staging_buffer,
                               app->ui_staging_memory, 0));
    need_vk("vkMapMemory(UI staging)",
            vkMapMemory(app->device, app->ui_staging_memory, 0,
                        UI_BYTES, 0, (void **)&app->ui_pixels));
    app->ui_dirty = true;
}

static void upload_ui(struct app *app)
{
    render_ui_pixels(app);
    XrSwapchainImageAcquireInfo acquire = {
        .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO,
    };
    uint32_t index = 0;
    need_xr("xrAcquireSwapchainImage(UI)",
            xrAcquireSwapchainImage(app->ui_swapchain, &acquire, &index));
    XrSwapchainImageWaitInfo wait = {
        .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
        .timeout = XR_INFINITE_DURATION,
    };
    need_xr("xrWaitSwapchainImage(UI)",
            xrWaitSwapchainImage(app->ui_swapchain, &wait));
    need(index < app->ui_image_count, "invalid UI swapchain index");
    need_vk("vkWaitForFences(UI)",
            vkWaitForFences(app->device, 1, &app->ui_fence,
                            VK_TRUE, UINT64_C(5000000000)));
    need_vk("vkResetFences(UI)",
            vkResetFences(app->device, 1, &app->ui_fence));
    need_vk("vkResetCommandBuffer(UI)",
            vkResetCommandBuffer(app->ui_command, 0));
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    need_vk("vkBeginCommandBuffer(UI)",
            vkBeginCommandBuffer(app->ui_command, &begin));
    VkImageMemoryBarrier transfer = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = app->ui_image_initialized[index]
                             ? VK_ACCESS_SHADER_READ_BIT : 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = app->ui_image_initialized[index]
                         ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                         : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .image = app->ui_images[index].image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1, .layerCount = 1,
        },
    };
    vkCmdPipelineBarrier(app->ui_command,
                         app->ui_image_initialized[index]
                             ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                             : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 1, &transfer);
    VkBufferImageCopy copy = {
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .layerCount = 1,
        },
        .imageExtent = {UI_WIDTH, UI_HEIGHT, 1},
    };
    vkCmdCopyBufferToImage(app->ui_command, app->ui_staging_buffer,
                           app->ui_images[index].image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    VkImageMemoryBarrier sample = transfer;
    sample.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    sample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    sample.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    sample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(app->ui_command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, NULL, 0, NULL, 1, &sample);
    need_vk("vkEndCommandBuffer(UI)",
            vkEndCommandBuffer(app->ui_command));
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &app->ui_command,
    };
    need_vk("vkQueueSubmit(UI)",
            vkQueueSubmit(app->queue, 1, &submit, app->ui_fence));
    need_vk("vkWaitForFences(UI completion)",
            vkWaitForFences(app->device, 1, &app->ui_fence,
                            VK_TRUE, UINT64_C(5000000000)));
    app->ui_image_initialized[index] = true;
    XrSwapchainImageReleaseInfo release = {
        .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO,
    };
    need_xr("xrReleaseSwapchainImage(UI)",
            xrReleaseSwapchainImage(app->ui_swapchain, &release));
    app->ui_dirty = false;
}

static void create_render_resources(struct app *app)
{
    VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = app->queue_family};
    need_vk("vkCreateCommandPool", vkCreateCommandPool(app->device, &pool, NULL, &app->command_pool));
    VkCommandBufferAllocateInfo commands = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = app->command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    need_vk("vkAllocateCommandBuffers", vkAllocateCommandBuffers(app->device, &commands, &app->command));
    VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    need_vk("vkCreateFence", vkCreateFence(app->device, &fence, NULL, &app->fence));
    create_pipeline(app);
    VkBufferCreateInfo buffer = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 2 * UBO_STRIDE, .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    need_vk("vkCreateBuffer(uniform)", vkCreateBuffer(app->device, &buffer, NULL, &app->uniform_buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(app->device, app->uniform_buffer, &requirements);
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type(app->physical, requirements.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    need_vk("vkAllocateMemory(uniform)", vkAllocateMemory(app->device, &allocation, NULL, &app->uniform_memory));
    need_vk("vkBindBufferMemory(uniform)", vkBindBufferMemory(app->device, app->uniform_buffer, app->uniform_memory, 0));
    need_vk("vkMapMemory(uniform)", vkMapMemory(app->device, app->uniform_memory, 0, VK_WHOLE_SIZE, 0, &app->uniform_mapping));
    VkDescriptorPoolSize sizes[2] = {
        {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
         .descriptorCount = app->video_buffer_count},
        {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = app->video_buffer_count},
    };
    VkDescriptorPoolCreateInfo descriptor_pool = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = app->video_buffer_count, .poolSizeCount = 2,
        .pPoolSizes = sizes};
    need_vk("vkCreateDescriptorPool", vkCreateDescriptorPool(app->device, &descriptor_pool, NULL, &app->descriptor_pool));
    VkDescriptorSetLayout layouts[NV12_IPC_MAX_BUFFERS];
    VkDescriptorSet sets[NV12_IPC_MAX_BUFFERS];
    for (uint32_t i = 0; i < app->video_buffer_count; ++i)
        layouts[i] = app->descriptor_layout;
    VkDescriptorSetAllocateInfo descriptor_allocate = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = app->descriptor_pool,
        .descriptorSetCount = app->video_buffer_count,
        .pSetLayouts = layouts};
    need_vk("vkAllocateDescriptorSets",
            vkAllocateDescriptorSets(app->device, &descriptor_allocate,
                                     sets));
    VkDescriptorBufferInfo descriptor_buffer = {
        .buffer = app->uniform_buffer,
        .offset = 0,
        .range = sizeof(struct view_uniform),
    };
    for (uint32_t i = 0; i < app->video_buffer_count; ++i) {
        app->video_buffers[i].descriptor_set = sets[i];
        VkDescriptorImageInfo descriptor_image = {
            .sampler = app->video_sampler,
            .imageView = app->video_buffers[i].view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        VkWriteDescriptorSet writes[2] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = sets[i], .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
             .pBufferInfo = &descriptor_buffer},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = sets[i], .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .pImageInfo = &descriptor_image},
        };
        vkUpdateDescriptorSets(app->device, 2, writes, 0, NULL);
    }
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

static int ui_hit_test(float u, float v)
{
    int x = (int)(u * UI_WIDTH);
    int y = (int)(v * UI_HEIGHT);
    if (y < 104 || y >= 212)
        return 0;
    if (x >= 28 && x < 218)
        return NV12_ACTION_BACK;
    if (x >= 238 && x < 388)
        return NV12_ACTION_SEEK_BACK;
    if (x >= 408 && x < 638)
        return NV12_ACTION_TOGGLE_PAUSE;
    if (x >= 658 && x < 808)
        return NV12_ACTION_SEEK_FORWARD;
    return 0;
}

static void sync_player_input(struct app *app, XrTime time)
{
    XrActiveActionSet active = {.actionSet = app->action_set};
    XrActionsSyncInfo sync = {
        .type = XR_TYPE_ACTIONS_SYNC_INFO,
        .countActiveActionSets = 1,
        .activeActionSets = &active,
    };
    need_xr("xrSyncActions", xrSyncActions(app->session, &sync));
    int hover = 0;
    bool pressed = false;
    for (unsigned hand = 0; hand < 2; ++hand) {
        XrActionStateGetInfo pose_info = {
            .type = XR_TYPE_ACTION_STATE_GET_INFO,
            .action = app->aim_action,
            .subactionPath = app->hands[hand],
        };
        XrActionStatePose pose = {.type = XR_TYPE_ACTION_STATE_POSE};
        need_xr("xrGetActionStatePose",
                xrGetActionStatePose(app->session, &pose_info, &pose));
        if (pose.isActive) {
            XrSpaceLocation location = {.type = XR_TYPE_SPACE_LOCATION};
            need_xr("xrLocateSpace(player aim)",
                    xrLocateSpace(app->aim_spaces[hand], app->view_space,
                                  time, &location));
            XrSpaceLocationFlags valid =
                XR_SPACE_LOCATION_POSITION_VALID_BIT |
                XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            if ((location.locationFlags & valid) == valid) {
                XrVector3f direction = rotate_vector(
                    location.pose.orientation, (XrVector3f){0, 0, -1});
                if (direction.z < -0.0001f) {
                    float distance =
                        (-1.3f - location.pose.position.z) / direction.z;
                    if (distance > 0.0f) {
                        float x = location.pose.position.x +
                                  direction.x * distance;
                        float y = location.pose.position.y +
                                  direction.y * distance;
                        float u = x / UI_PANEL_WIDTH_METERS + 0.5f;
                        float v = 0.5f -
                            (y + 0.45f) / UI_PANEL_HEIGHT_METERS;
                        if (u >= 0 && u < 1 && v >= 0 && v < 1)
                            hover = ui_hit_test(u, v);
                    }
                }
            }
        }
        XrActionStateGetInfo select_info = {
            .type = XR_TYPE_ACTION_STATE_GET_INFO,
            .action = app->select_action,
            .subactionPath = app->hands[hand],
        };
        XrActionStateBoolean select = {
            .type = XR_TYPE_ACTION_STATE_BOOLEAN,
        };
        need_xr("xrGetActionStateBoolean",
                xrGetActionStateBoolean(app->session,
                                        &select_info, &select));
        if (select.isActive && select.changedSinceLastSync &&
            select.currentState)
            pressed = true;
    }
    if (hover != app->ui_hover) {
        app->ui_hover = hover;
        app->ui_dirty = true;
    }
    if (pressed && hover)
        send_ipc(app, NV12_IPC_ACTION, (uint32_t)hover);
}

static void render_frame(struct app *app)
{
    XrFrameState frame_state = {.type = XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo wait = {.type = XR_TYPE_FRAME_WAIT_INFO};
    need_xr("xrWaitFrame", xrWaitFrame(app->session, &wait, &frame_state));
    XrFrameBeginInfo begin = {.type = XR_TYPE_FRAME_BEGIN_INFO};
    need_xr("xrBeginFrame", xrBeginFrame(app->session, &begin));
    ++app->xr_frames;
    app->xr_should_render = frame_state.shouldRender;
    sync_player_input(app, frame_state.predictedDisplayTime);
    if (app->ui_dirty)
        upload_ui(app);
    if (!frame_state.shouldRender) {
        XrFrameEndInfo end = {.type = XR_TYPE_FRAME_END_INFO, .displayTime = frame_state.predictedDisplayTime,
            .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE};
        need_xr("xrEndFrame(empty)", xrEndFrame(app->session, &end));
        return;
    }
    if (app->current_video_index < 0) {
        XrFrameEndInfo end = {.type = XR_TYPE_FRAME_END_INFO,
            .displayTime = frame_state.predictedDisplayTime,
            .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE};
        need_xr("xrEndFrame(no video)", xrEndFrame(app->session, &end));
        return;
    }
    struct video_buffer *video =
        &app->video_buffers[app->current_video_index];
    XrView views[2] = {{.type = XR_TYPE_VIEW}, {.type = XR_TYPE_VIEW}};
    XrViewState view_state = {.type = XR_TYPE_VIEW_STATE};
    uint32_t count = 0;
    XrViewLocateInfo locate = {.type = XR_TYPE_VIEW_LOCATE_INFO,
        .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        .displayTime = frame_state.predictedDisplayTime, .space = app->space};
    need_xr("xrLocateViews", xrLocateViews(app->session, &locate, &view_state, 2, &count, views));
    need(count == 2 && (view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
             (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT), "OpenXR returned no valid stereo pose");
    XrSpaceLocation center = {.type = XR_TYPE_SPACE_LOCATION};
    need_xr("xrLocateSpace(VIEW)",
            xrLocateSpace(app->view_space, app->space,
                          frame_state.predictedDisplayTime, &center));
    need((center.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0,
         "OpenXR returned no valid VIEW orientation");
    if (!app->view_geometry_logged) {
        printf("openxr_view_geometry"
               " left_pose=(%.9f,%.9f,%.9f;%.9f,%.9f,%.9f,%.9f)"
               " left_fov=(%.9f,%.9f,%.9f,%.9f)"
               " right_pose=(%.9f,%.9f,%.9f;%.9f,%.9f,%.9f,%.9f)"
               " right_fov=(%.9f,%.9f,%.9f,%.9f)"
               " center_pose=(%.9f,%.9f,%.9f;%.9f,%.9f,%.9f,%.9f)\n",
               views[0].pose.position.x, views[0].pose.position.y,
               views[0].pose.position.z, views[0].pose.orientation.x,
               views[0].pose.orientation.y, views[0].pose.orientation.z,
               views[0].pose.orientation.w, views[0].fov.angleLeft,
               views[0].fov.angleRight, views[0].fov.angleDown,
               views[0].fov.angleUp, views[1].pose.position.x,
               views[1].pose.position.y, views[1].pose.position.z,
               views[1].pose.orientation.x, views[1].pose.orientation.y,
               views[1].pose.orientation.z, views[1].pose.orientation.w,
               views[1].fov.angleLeft, views[1].fov.angleRight,
               views[1].fov.angleDown, views[1].fov.angleUp,
               center.pose.position.x, center.pose.position.y,
               center.pose.position.z, center.pose.orientation.x,
               center.pose.orientation.y, center.pose.orientation.z,
               center.pose.orientation.w);
        app->view_geometry_logged = true;
    }
    XrSwapchainImageAcquireInfo acquire = {.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t image_index = 0;
    need_xr("xrAcquireSwapchainImage", xrAcquireSwapchainImage(app->swapchain, &acquire, &image_index));
    XrSwapchainImageWaitInfo image_wait = {.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION};
    need_xr("xrWaitSwapchainImage", xrWaitSwapchainImage(app->swapchain, &image_wait));
    need(image_index < app->image_count, "invalid OpenXR swapchain index");
    for (uint32_t eye = 0; eye < 2; ++eye) {
        struct mat4 projection = projection_from_fov(views[eye].fov);
        struct mat4 view = view_from_pose(views[eye].pose);
        struct view_uniform uniform = {
            .mvp = multiply(projection, view),
            .camera_to_world = rotation_from_pose(views[eye].pose),
            .fov_tangents = {
                tanf(views[eye].fov.angleLeft),
                tanf(views[eye].fov.angleRight),
                tanf(views[eye].fov.angleDown),
                tanf(views[eye].fov.angleUp),
            },
            .modes = {
                (int32_t)app->projection_mode,
                (int32_t)app->stereo_layout,
                (int32_t)eye,
                app->swap_eyes,
            },
            .source_info = {
                (float)app->video_width,
                (float)app->video_height,
                1.0f / (float)app->video_width,
                1.0f / (float)app->video_height,
            },
        };
        memcpy((uint8_t *)app->uniform_mapping + eye * UBO_STRIDE,
               &uniform, sizeof(uniform));
    }
    need_vk("vkWaitForFences(render)", vkWaitForFences(app->device, 1, &app->fence, VK_TRUE, UINT64_C(5000000000)));
    need_vk("vkResetFences(render)", vkResetFences(app->device, 1, &app->fence));
    need_vk("vkResetCommandPool(render)", vkResetCommandPool(app->device, app->command_pool, 0));
    VkCommandBufferBeginInfo command_begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    need_vk("vkBeginCommandBuffer(render)", vkBeginCommandBuffer(app->command, &command_begin));
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .image = app->images[image_index].image, .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 2}};
    vkCmdPipelineBarrier(app->command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    VkImageMemoryBarrier video_acquire = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
        .dstQueueFamilyIndex = app->queue_family,
        .image = video->image,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .levelCount = 1, .layerCount = 1},
    };
    vkCmdPipelineBarrier(app->command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL,
                         0, NULL, 1, &video_acquire);
    for (uint32_t eye = 0; eye < 2; ++eye) {
        VkClearValue clear = {.color = {{0.005f, 0.005f, 0.008f, 1.0f}}};
        VkRenderPassBeginInfo pass = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = app->render_pass, .framebuffer = app->framebuffers[image_index][eye],
            .renderArea = {.extent = {app->width, app->height}}, .clearValueCount = 1, .pClearValues = &clear};
        vkCmdBeginRenderPass(app->command, &pass, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(app->command, VK_PIPELINE_BIND_POINT_GRAPHICS, app->pipeline);
        uint32_t offset = eye * UBO_STRIDE;
        vkCmdBindDescriptorSets(app->command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                app->pipeline_layout, 0, 1,
                                &video->descriptor_set, 1, &offset);
        vkCmdDraw(app->command, 4, 1, 0, 0);
        vkCmdEndRenderPass(app->command);
    }
    VkImageMemoryBarrier video_release = video_acquire;
    video_release.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    video_release.dstAccessMask = 0;
    video_release.srcQueueFamilyIndex = app->queue_family;
    video_release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    vkCmdPipelineBarrier(app->command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL,
                         0, NULL, 1, &video_release);
    need_vk("vkEndCommandBuffer(render)", vkEndCommandBuffer(app->command));
    VkPipelineStageFlags producer_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &app->command};
    if (video->producer_wait_pending) {
        struct dma_buf_export_sync_file sync_export = {
            .flags = DMA_BUF_SYNC_READ, .fd = -1,
        };
        if (ioctl(video->dma_buf_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE,
                  &sync_export) < 0) {
            perror("DMA_BUF_IOCTL_EXPORT_SYNC_FILE");
            exit(2);
        }
        VkImportSemaphoreFdInfoKHR semaphore_import = {
            .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
            .semaphore = video->producer_semaphore,
            .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
            .fd = sync_export.fd,
        };
        need_vk("vkImportSemaphoreFdKHR(producer)",
                app->import_semaphore_fd(app->device, &semaphore_import));
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &video->producer_semaphore;
        submit.pWaitDstStageMask = &producer_stage;
        video->producer_wait_pending = false;
    }
    need_vk("vkQueueSubmit(render)", vkQueueSubmit(app->queue, 1, &submit, app->fence));
    need_vk("vkWaitForFences(render completion)", vkWaitForFences(app->device, 1, &app->fence, VK_TRUE, UINT64_C(5000000000)));
    XrSwapchainImageReleaseInfo release = {.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    need_xr("xrReleaseSwapchainImage", xrReleaseSwapchainImage(app->swapchain, &release));
    XrCompositionLayerProjectionView projection_views[2] = {{.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    for (uint32_t eye = 0; eye < 2; ++eye) {
        projection_views[eye].pose = views[eye].pose; projection_views[eye].fov = views[eye].fov;
        projection_views[eye].subImage = (XrSwapchainSubImage){.swapchain = app->swapchain, .imageRect = {.extent = {(int32_t)app->width, (int32_t)app->height}}, .imageArrayIndex = eye};
    }
    XrCompositionLayerProjection layer = {.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .space = app->space, .viewCount = 2, .views = projection_views};
    XrCompositionLayerQuad ui_layer = {
        .type = XR_TYPE_COMPOSITION_LAYER_QUAD,
        .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
        .space = app->view_space,
        .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
        .subImage = {
            .swapchain = app->ui_swapchain,
            .imageRect = {.extent = {UI_WIDTH, UI_HEIGHT}},
        },
        .pose = {{0, 0, 0, 1}, {0, -0.45f, -1.3f}},
        .size = {UI_PANEL_WIDTH_METERS, UI_PANEL_HEIGHT_METERS},
    };
    const XrCompositionLayerBaseHeader *layers[] = {
        (const XrCompositionLayerBaseHeader *)&layer,
        (const XrCompositionLayerBaseHeader *)&ui_layer,
    };
    XrFrameEndInfo end = {.type = XR_TYPE_FRAME_END_INFO, .displayTime = frame_state.predictedDisplayTime,
        .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
        .layerCount = ARRAY_COUNT(layers), .layers = layers};
    need_xr("xrEndFrame", xrEndFrame(app->session, &end));
    ++app->rendered_frames;
    if (app->current_video_presented)
        ++app->repeated_video_frames;
    else {
        ++app->unique_video_frames;
        app->current_video_presented = true;
    }
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc != 2) {
        fprintf(stderr, "usage: %s IPC_FD\n", argv[0]);
        return 2;
    }
    struct app app = {0};
    app.ipc_fd = (int)parse_u64(argv[1], "IPC fd");
    app.current_video_index = -1;
    app.player_state = NV12_PLAYER_PLAYING;
    app.ui_dirty = true;
    receive_initial_buffers(&app);
    initialize_xr_vulkan(&app);
    XrGraphicsBindingVulkan2KHR binding = {.type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR,
        .instance = app.instance, .physicalDevice = app.physical, .device = app.device, .queueFamilyIndex = app.queue_family, .queueIndex = 0};
    XrSessionCreateInfo session_info = {.type = XR_TYPE_SESSION_CREATE_INFO, .next = &binding, .systemId = app.system};
    need_xr("xrCreateSession", xrCreateSession(app.xr, &session_info, &app.session));
    initialize_actions(&app);
    /* SteamVR on Frame requires READY/xrBeginSession before it permits a
     * reference space or swapchain to be created. */
    struct timespec pause = {.tv_nsec = 10000000};
    struct timespec started, now;
    clock_gettime(CLOCK_MONOTONIC, &started);
    while (!app.running) {
        poll_events(&app);
        clock_gettime(CLOCK_MONOTONIC, &now);
        need(now.tv_sec - started.tv_sec < 10,
             "OpenXR session did not enter READY within ten seconds");
        if (!app.running) nanosleep(&pause, NULL);
    }
    XrReferenceSpaceCreateInfo space_info = {.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO, .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL, .poseInReferenceSpace = {{0,0,0,1},{0,0,0}}};
    need_xr("xrCreateReferenceSpace", xrCreateReferenceSpace(app.session, &space_info, &app.space));
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    need_xr("xrCreateReferenceSpace(VIEW)",
            xrCreateReferenceSpace(app.session, &space_info, &app.view_space));
    initialize_swapchain(&app);
    create_ui_swapchain(&app);
    import_nv12_frames(&app);
    create_render_resources(&app);
    create_ui_resources(&app);
    send_ipc(&app, NV12_IPC_READY, 0);
    for (;;) {
        receive_frame_messages(&app);
        poll_events(&app);
        if (app.running) render_frame(&app); else nanosleep(&pause, NULL);
        if (app.eos_received) {
            release_current_video(&app);
            break;
        }
    }
    printf("openxr_summary xr_frames=%" PRIu64
           " rendered_frames=%" PRIu64 " received_frames=%" PRIu64
           " unique_video_frames=%" PRIu64
           " repeated_video_frames=%" PRIu64
           " hidden_video_frames=%" PRIu64
           " pipeline_skipped_frames=%" PRIu64
           " released_frames=%" PRIu64
           " session_changes=%" PRIu64 "\n",
           app.xr_frames, app.rendered_frames, app.received_frames,
           app.unique_video_frames, app.repeated_video_frames,
           app.hidden_video_frames, app.pipeline_skipped_frames,
           app.released_frames,
           app.session_changes);
    return 0;
}
