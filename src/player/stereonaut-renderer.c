/* Continuously render Iris NV12 DMA-BUF frames on the validated OpenXR quad. */
#define _POSIX_C_SOURCE 200809L
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_PLATFORM_XLIB

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/dma-buf.h>
#include <math.h>
#include <signal.h>
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

#include "browser-ui.h"
#include "player-ui.h"
#include "trickplay.h"
#include "ui-draw.h"
#include "nv12-playback-ipc.h"
#include "player-ui-ipc.h"

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define MAX_IMAGES 8U
#define VIDEO_BUFFER_COUNT 12U
#define UBO_STRIDE 256U
#define DRM_FORMAT_MOD_LINEAR UINT64_C(0)
#define DRM_FORMAT_MOD_QCOM_COMPRESSED UINT64_C(0x0500000000000001)
#define UI_WIDTH PLAYER_UI_WIDTH
#define UI_HEIGHT PLAYER_UI_HEIGHT
#define UI_BYTES (UI_WIDTH * UI_HEIGHT * 4U)
#define UI_PANEL_WIDTH_METERS PLAYER_UI_PANEL_WIDTH_METERS
#define UI_PANEL_HEIGHT_METERS PLAYER_UI_PANEL_HEIGHT_METERS
#define GRAB_RECENTER_COS 0.5f
/* The select action is analog so the press point can be lower than the
 * trigger's full click travel. */
#define SELECT_PRESS_THRESHOLD 0.50f
#define SELECT_RELEASE_THRESHOLD 0.03f
/* Thumbstick left/right: same low threshold as the trigger, one action per
 * deflection. */
#define STICK_PRESS_THRESHOLD 0.30f
#define STICK_RELEASE_THRESHOLD 0.15f

struct mat4 { float v[16]; };

struct view_uniform {
    struct mat4 mvp;
    struct mat4 camera_to_world;
    float fov_tangents[4];
    int32_t modes[4];
    float source_info[4];
    float video_scale[4]; /* TP10: visible / padded size of the planes */
};

_Static_assert(sizeof(struct view_uniform) <= UBO_STRIDE,
               "view uniform exceeds dynamic UBO stride");

struct video_buffer {
    int dma_buf_fd;
    bool tp10;
    VkImage image;          /* NV12, or the TP10 luma plane */
    VkDeviceMemory memory;
    VkImageView view;
    VkImage image_uv;       /* TP10 chroma plane */
    VkDeviceMemory memory_uv;
    VkImageView view_uv;
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
    VkPipeline pipeline_tp10;
    VkDescriptorSetLayout descriptor_layout;
    VkDescriptorPool descriptor_pool;
    VkSamplerYcbcrConversion conversion;
    VkSampler video_sampler;
    VkSampler plane_sampler; /* TP10 planes: plain linear sampling */
    struct video_buffer video_buffers[NV12_IPC_MAX_BUFFERS];
    /* Last frame kept on screen while the session restarts after a seek. */
    struct video_buffer held;
    bool has_held;
    VkDescriptorSet spare_set;
    bool awaiting_frame;
    bool first_init_done;
    uint32_t video_buffer_count;
    int current_video_index;
    bool current_video_presented;
    int ui_fd;              /* control socket to the launcher */
    bool playing;           /* a playback session is attached (ipc_fd) */
    bool quit;
    bool xr_exit;           /* the runtime ended the session */
    bool awaiting_command;  /* a selection was sent, wait for PLAY/BROWSE */
    struct browser_ui browser;
    XrSwapchain browser_swapchain;
    uint32_t browser_image_count;
    XrSwapchainImageVulkan2KHR browser_images[MAX_IMAGES];
    bool browser_image_initialized[MAX_IMAGES];
    VkBuffer browser_staging_buffer;
    VkDeviceMemory browser_staging_memory;
    uint32_t *browser_pixels; /* mapped staging memory: write-only */
    uint32_t *browser_cpu;    /* the browser draws here (cached memory) */
    VkCommandBuffer browser_command;
    /* Pointer dot as its own small quad layer, so moving the pointer does
     * not require redrawing and uploading a panel texture. */
    XrSwapchain pointer_swapchain;
    bool pointer_layer_visible;
    XrPosef pointer_pose;
    float pointer_size;
    VkFence browser_fence;
    int ipc_fd;
    bool eos_received;
    PFN_vkImportSemaphoreFdKHR import_semaphore_fd;
    PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties;
    PFN_vkGetImageDrmFormatModifierPropertiesEXT get_modifier_properties;
    VkDeviceSize dma_buf_size;
    uint32_t video_width, video_height, video_stride;
    uint32_t video_format; /* enum nv12_pixel_format */
    uint32_t coded_width, coded_height;
    uint32_t projection_mode, stereo_layout;
    bool swap_eyes;
    XrActionSet action_set;
    XrAction aim_action;
    XrAction select_action;
    XrAction squeeze_action;
    XrAction stick_action;
    int stick_direction[2]; /* -1 left, +1 right, 0 centred */
    bool select_down[2];
    bool has_valve_frame_controller;
    bool has_generic_controller;
    XrPath hands[2];
    XrSpace aim_spaces[2];
    int ui_hover;
    bool ui_pointer_visible;
    int ui_pointer_x;
    int ui_pointer_y;
    bool ui_visible;
    bool ui_dirty;
    /* Panel placement in the local reference space. The panel is world-fixed
     * and upright; only the yaw rotates it towards the viewer. */
    float panel_x, panel_y, panel_z, panel_yaw;
    int grab_hand;
    XrVector3f grab_offset;
    int scrub_hand;
    int64_t scrub_us;
    bool seek_pending;
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
    uint32_t *ui_pixels; /* mapped staging memory: write-only */
    uint32_t *ui_cpu;    /* the panel draws here */
    struct player_ui player_ui;
    bool popup_open;
    char title[256];
    /* Seek bar preview: trickplay pictures of the playing Jellyfin item. */
    struct trickplay *trickplay;
    struct ui_image *preview;
    bool preview_visible;
    int64_t preview_us;
    VkCommandBuffer ui_command;
    VkFence ui_fence;
    uint64_t xr_frames, rendered_frames, session_changes;
    uint64_t received_frames, released_frames;
    uint64_t unique_video_frames, repeated_video_frames;
    uint64_t hidden_video_frames, pipeline_skipped_frames;
};

static void need_vk(const char *what, VkResult result);
static void need(bool condition, const char *what);
static void hold_current_frame(struct app *app);
static void send_control(struct app *app, uint32_t type, const char *path);
static void discard_held_frame(struct app *app);
static void reinitialize_video(struct app *app,
                               const struct nv12_ipc_message *message,
                               const struct msghdr *received);

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
    create_action(app->action_set, XR_ACTION_TYPE_FLOAT_INPUT,
                  "select", "Auswaehlen", app->hands,
                  &app->select_action);
    create_action(app->action_set, XR_ACTION_TYPE_BOOLEAN_INPUT,
                  "squeeze", "Greifen", app->hands, &app->squeeze_action);
    create_action(app->action_set, XR_ACTION_TYPE_VECTOR2F_INPUT,
                  "stick", "Blaettern und Spulen", app->hands,
                  &app->stick_action);
    struct {
        const char *profile;
        const char *select_component;
        const char *squeeze_component;
        bool thumbstick;
        bool enabled;
    } profiles[] = {
        {"/interaction_profiles/valve/frame_controller",
         "trigger/value", "squeeze/click", true,
         app->has_valve_frame_controller},
        {"/interaction_profiles/khr/generic_controller",
         "trigger/value", "squeeze/value", true, app->has_generic_controller},
        {"/interaction_profiles/valve/index_controller",
         "trigger/value", "squeeze/value", true, true},
        /* No squeeze button or stick: panel grab, show/hide, page flips and
         * stick seeking are unavailable. */
        {"/interaction_profiles/khr/simple_controller",
         "select/click", NULL, false, true},
    };
    static const char *const hand_names[2] = {"left", "right"};
    unsigned accepted = 0;
    for (size_t p = 0; p < ARRAY_COUNT(profiles); ++p) {
        if (!profiles[p].enabled)
            continue;
        XrActionSuggestedBinding bindings[8];
        uint32_t count = 0;
        char text[4][2][96];
        for (unsigned hand = 0; hand < 2; ++hand) {
            snprintf(text[0][hand], sizeof(text[0][hand]),
                     "/user/hand/%s/input/aim/pose", hand_names[hand]);
            bindings[count++] = (XrActionSuggestedBinding){
                app->aim_action, xr_path(app, text[0][hand])};
            snprintf(text[1][hand], sizeof(text[1][hand]),
                     "/user/hand/%s/input/%s", hand_names[hand],
                     profiles[p].select_component);
            bindings[count++] = (XrActionSuggestedBinding){
                app->select_action, xr_path(app, text[1][hand])};
            if (profiles[p].squeeze_component) {
                snprintf(text[2][hand], sizeof(text[2][hand]),
                         "/user/hand/%s/input/%s", hand_names[hand],
                         profiles[p].squeeze_component);
                bindings[count++] = (XrActionSuggestedBinding){
                    app->squeeze_action, xr_path(app, text[2][hand])};
            }
            if (profiles[p].thumbstick) {
                snprintf(text[3][hand], sizeof(text[3][hand]),
                         "/user/hand/%s/input/thumbstick", hand_names[hand]);
                bindings[count++] = (XrActionSuggestedBinding){
                    app->stick_action, xr_path(app, text[3][hand])};
            }
        }
        XrInteractionProfileSuggestedBinding suggestion = {
            .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
            .interactionProfile = xr_path(app, profiles[p].profile),
            .countSuggestedBindings = count,
            .suggestedBindings = bindings,
        };
        XrResult result =
            xrSuggestInteractionProfileBindings(app->xr, &suggestion);
        printf("controller_bindings profile=%s result=%d\n",
               profiles[p].profile, result);
        if (XR_SUCCEEDED(result))
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

static void apply_init(struct app *app, const struct nv12_ipc_message *message,
                       const struct msghdr *received)
{
    /* Iris chooses the count per stream (H.264 needs more references at
     * small sizes), so it may differ between titles and seeks. */
    need(message->buffer_count >= 1 &&
             message->buffer_count <= NV12_IPC_MAX_BUFFERS,
         "NV12 IPC buffer count out of range");
    struct cmsghdr *header = CMSG_FIRSTHDR(received);
    need(header && header->cmsg_level == SOL_SOCKET &&
             header->cmsg_type == SCM_RIGHTS,
         "NV12 IPC init lacks DMA-BUF file descriptors");
    size_t fd_bytes = header->cmsg_len - CMSG_LEN(0);
    need(fd_bytes == message->buffer_count * sizeof(int),
         "NV12 IPC descriptor count differs from buffer count");
    int *fds = (int *)CMSG_DATA(header);
    app->video_buffer_count = message->buffer_count;
    for (uint32_t i = 0; i < app->video_buffer_count; ++i) {
        app->video_buffers[i].dma_buf_fd = fds[i];
        struct stat status;
        if (fstat(fds[i], &status) < 0) {
            perror("fstat IPC DMA-BUF");
            exit(2);
        }
        need((uint64_t)status.st_size == message->size,
             "IPC DMA-BUF length differs from contract");
    }
    app->video_width = message->width;
    app->video_height = message->height;
    app->video_stride = message->stride;
    app->dma_buf_size = message->size;
    app->uv_offset = message->uv_offset;
    app->video_format = message->pixel_format;
    app->coded_width = message->coded_width;
    app->coded_height = message->coded_height;
    need(app->video_format == NV12_FORMAT_NV12 ||
             (app->video_format == NV12_FORMAT_TP10 &&
              app->coded_width >= app->video_width &&
              app->coded_height >= app->video_height &&
              app->coded_width % 3U == 0 &&
              app->video_stride == app->coded_width / 3U * 4U),
         "unsupported video pixel format in IPC contract");
    app->projection_mode = message->projection_mode;
    app->stereo_layout = message->stereo_layout;
    app->swap_eyes = message->swap_eyes != 0;
    need(app->video_width > 0 && app->video_height > 0 &&
             !(app->video_width & 1U) && !(app->video_height & 1U) &&
             (app->video_format == NV12_FORMAT_TP10 ||
              app->video_stride >= app->video_width) &&
             app->uv_offset >=
                 (uint64_t)app->video_stride * app->video_height &&
             app->dma_buf_size >= app->uv_offset +
                 (uint64_t)app->video_stride * app->video_height / 2U,
         "NV12 IPC layout is not linear 4:2:0");
    need(app->projection_mode <= NV12_PROJECTION_EQUIRECT_360,
         "invalid projection mode in IPC contract");
    need(app->stereo_layout <= NV12_STEREO_TB,
         "invalid stereo layout in IPC contract");
    /* The host owns the panel placement for its whole lifetime. */
    printf("render_config projection=%u stereo=%u swap_eyes=%u"
           " view0=left view1=right\n",
           app->projection_mode, app->stereo_layout, app->swap_eyes);
}

static void send_message(struct app *app, const struct nv12_ipc_message *message)
{
    ssize_t written;
    do {
        written = send(app->ipc_fd, message, sizeof(*message), MSG_NOSIGNAL);
    } while (written < 0 && errno == EINTR);
    if (written < 0 && (errno == EPIPE || errno == ECONNRESET ||
                        errno == ENOTCONN)) {
        app->eos_received = true; /* the session is gone: end playback */
        return;
    }
    if (written < 0) { perror("send NV12 IPC response"); exit(2); }
    need((size_t)written == sizeof(*message), "short NV12 IPC response");
}

static void send_ipc(struct app *app, uint32_t type, uint32_t index)
{
    struct nv12_ipc_message message = {.type = type, .index = index};
    send_message(app, &message);
}

static void send_seek_to(struct app *app, int64_t target_us)
{
    struct nv12_ipc_message message = {
        .type = NV12_IPC_ACTION, .index = NV12_ACTION_SEEK_TO,
        .position_us = target_us,
    };
    send_message(app, &message);
}

/* Applies a choice from the view popup at once and reports it to the
 * session, which stores it for this title. */
static void change_view(struct app *app, int control)
{
    if (control >= PLAYER_UI_PROJECTION &&
        control <= PLAYER_UI_PROJECTION + NV12_PROJECTION_EQUIRECT_360)
        app->projection_mode = (uint32_t)(control - PLAYER_UI_PROJECTION);
    else if (control >= PLAYER_UI_STEREO &&
             control <= PLAYER_UI_STEREO + NV12_STEREO_TB)
        app->stereo_layout = (uint32_t)(control - PLAYER_UI_STEREO);
    else if (control == PLAYER_UI_SWAP_EYES)
        app->swap_eyes = !app->swap_eyes;
    else
        return;
    app->ui_dirty = true;
    struct nv12_ipc_message message = {
        .type = NV12_IPC_VIEW,
        .projection_mode = app->projection_mode,
        .stereo_layout = app->stereo_layout,
        .swap_eyes = app->swap_eyes,
    };
    send_message(app, &message);
    printf("player_view projection=%u stereo=%u swap_eyes=%u\n",
           app->projection_mode, app->stereo_layout, app->swap_eyes);
}

static void send_panel_state(struct app *app)
{
    struct nv12_ipc_message message = {
        .type = NV12_IPC_PANEL,
        .panel_x = app->panel_x, .panel_y = app->panel_y,
        .panel_z = app->panel_z, .panel_yaw = app->panel_yaw,
        .panel_visible = app->ui_visible,
        .panel_valid = 1,
    };
    send_message(app, &message);
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
        char control[CMSG_SPACE(sizeof(int) * NV12_IPC_MAX_BUFFERS)] = {0};
        struct iovec iov = {.iov_base = &message, .iov_len = sizeof(message)};
        struct msghdr received = {
            .msg_iov = &iov, .msg_iovlen = 1,
            .msg_control = control, .msg_controllen = sizeof(control),
        };
        ssize_t bytes = recvmsg(app->ipc_fd, &received, MSG_DONTWAIT);
        if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (bytes < 0 && errno == EINTR)
            continue;
        if (bytes == 0) {
            app->eos_received = true;
            return;
        }
        if (bytes < 0 && errno == ECONNRESET) {
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
            if (app->awaiting_frame) {
                app->awaiting_frame = false;
                app->seek_pending = false;
                app->ui_dirty = true;
            }
            /* The held frame is superseded by the new stream. */
            discard_held_frame(app);
            app->received_frames++;
        } else if (message.type == NV12_IPC_FLUSH) {
            release_current_video(app);
            send_ipc(app, NV12_IPC_FLUSHED, 0);
        } else if (message.type == NV12_IPC_HOLD) {
            hold_current_frame(app);
            send_ipc(app, NV12_IPC_HELD, 0);
        } else if (message.type == NV12_IPC_INIT) {
            reinitialize_video(app, &message, &received);
        } else if (message.type == NV12_IPC_EOS) {
            app->eos_received = true;
        } else if (message.type == NV12_IPC_STATUS) {
            /* Redraw only for a visible change: the shown second, the
             * seek bar knob (whole pixels) or the play state. */
            int64_t old_second = app->position_us / 1000000;
            int old_knob = app->duration_us > 0
                ? (int)(1000.0 * app->position_us / app->duration_us) : -1;
            bool changed = app->player_state != message.player_state ||
                           app->duration_us != message.duration_us;
            app->player_state = message.player_state;
            if (!app->seek_pending)
                app->position_us = message.position_us;
            app->duration_us = message.duration_us;
            int knob = app->duration_us > 0
                ? (int)(1000.0 * app->position_us / app->duration_us) : -1;
            if (changed || old_second != app->position_us / 1000000 ||
                old_knob != knob)
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

/* Started from the desktop overlay inside the SteamVR dashboard, the app
 * becomes visible but keeps the dashboard in front (session state VISIBLE
 * instead of FOCUSED) until the viewer clicks "back to game". Hide it with
 * SteamVR's own tool, only during startup so a dashboard the viewer opens
 * later stays alone. */
static void hide_dashboard_at_startup(const struct app *app)
{
    enum { WINDOW_S = 20, MAX_ATTEMPTS = 4 };
    static time_t first_call_s;
    static time_t last_attempt_s;
    static int attempts;
    time_t now = time(NULL);
    if (!first_call_s)
        first_call_s = now;
    if (app->state != XR_SESSION_STATE_VISIBLE ||
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

static void poll_events(struct app *app)
{
    for (;;) {
        XrEventDataBuffer event = {.type = XR_TYPE_EVENT_DATA_BUFFER};
        XrResult result = xrPollEvent(app->xr, &event);
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
                        app->session, app->hands[hand], &profile)) &&
                    profile.interactionProfile != XR_NULL_PATH) {
                    uint32_t length = 0;
                    xrPathToString(app->xr, profile.interactionProfile,
                                   sizeof(text), &length, text);
                }
                printf("interaction_profile hand=%s profile=%s\n",
                       hand_names[hand], text);
            }
            continue;
        }
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

/* One TP10 plane as a 32-bpp UBWC image: TP10 packs three 10-bit samples
 * per word, so the plane has the byte geometry (and Turnip's UBWC layout) of
 * an RGBA8 image a third as wide. The patched Turnip samples such views as
 * FMT6_TP10_Y (RGBA8) or FMT6_TP10_UV (RGB10A2) when STEREONAUT_TP10 is set
 * (tools/mesa-tp10). */
static void import_tp10_plane(struct app *app, struct video_buffer *buffer,
                              VkFormat format, VkDeviceSize offset,
                              uint32_t rows, VkImage *image,
                              VkDeviceMemory *memory, VkImageView *view)
{
    const VkSubresourceLayout plane = {
        .offset = offset, .rowPitch = app->video_stride,
    };
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier = DRM_FORMAT_MOD_QCOM_COMPRESSED,
        .drmFormatModifierPlaneCount = 1, .pPlaneLayouts = &plane,
    };
    VkExternalMemoryImageCreateInfo external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &modifier,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &external,
        .imageType = VK_IMAGE_TYPE_2D, .format = format,
        .extent = {app->coded_width / 3U, rows, 1},
        .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    need_vk("vkCreateImage(TP10 plane)",
            vkCreateImage(app->device, &info, NULL, image));
    VkImageDrmFormatModifierPropertiesEXT chosen = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
    };
    need_vk("vkGetImageDrmFormatModifierPropertiesEXT(TP10)",
            app->get_modifier_properties(app->device, *image, &chosen));
    need(chosen.drmFormatModifier == DRM_FORMAT_MOD_QCOM_COMPRESSED,
         "Vulkan substituted the TP10 modifier");
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(app->device, *image, &requirements);
    need(requirements.size <= app->dma_buf_size,
         "TP10 plane layout exceeds the DMA-BUF");
    VkMemoryFdPropertiesKHR fd_properties = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
    };
    need_vk("vkGetMemoryFdPropertiesKHR(TP10)",
            app->get_memory_fd_properties(
                app->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                buffer->dma_buf_fd, &fd_properties));
    uint32_t bits = requirements.memoryTypeBits & fd_properties.memoryTypeBits;
    need(bits != 0, "TP10 DMA-BUF memory-type intersection is empty");
    int imported_fd = dup(buffer->dma_buf_fd);
    if (imported_fd < 0) { perror("dup TP10 DMA-BUF"); exit(2); }
    VkImportMemoryFdInfoKHR import = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = imported_fd,
    };
    VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &import, .image = *image,
    };
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated, .allocationSize = app->dma_buf_size,
        .memoryTypeIndex = memory_type(app->physical, bits, 0),
    };
    need_vk("vkAllocateMemory(import TP10)",
            vkAllocateMemory(app->device, &allocation, NULL, memory));
    need_vk("vkBindImageMemory(import TP10)",
            vkBindImageMemory(app->device, *image, *memory, 0));
    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = *image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .levelCount = 1, .layerCount = 1},
    };
    need_vk("vkCreateImageView(TP10)",
            vkCreateImageView(app->device, &view_info, NULL, view));
}

static void import_video_buffer(struct app *app, uint32_t i)
{
    struct video_buffer *tp10_buffer = &app->video_buffers[i];
    tp10_buffer->tp10 = app->video_format == NV12_FORMAT_TP10;
    if (tp10_buffer->tp10) {
        import_tp10_plane(app, tp10_buffer, VK_FORMAT_R8G8B8A8_UNORM, 0,
                          app->coded_height, &tp10_buffer->image,
                          &tp10_buffer->memory, &tp10_buffer->view);
        import_tp10_plane(app, tp10_buffer,
                          VK_FORMAT_A2B10G10R10_UNORM_PACK32, app->uv_offset,
                          (app->coded_height + 1U) / 2U,
                          &tp10_buffer->image_uv, &tp10_buffer->memory_uv,
                          &tp10_buffer->view_uv);
        VkSemaphoreCreateInfo semaphore = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        };
        need_vk("vkCreateSemaphore(producer)",
                vkCreateSemaphore(app->device, &semaphore, NULL,
                                  &tp10_buffer->producer_semaphore));
        printf("tp10_import index=%u coded=%ux%u\n", i, app->coded_width,
               app->coded_height);
        return;
    }
    VkSamplerYcbcrConversionInfo conversion_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
        .conversion = app->conversion,
    };
    const VkSubresourceLayout planes[2] = {
        {.offset = 0,
         .size = (VkDeviceSize)app->video_stride * app->video_height,
         .rowPitch = app->video_stride},
        {.offset = app->uv_offset,
         .size = (VkDeviceSize)app->video_stride * app->video_height / 2U,
         .rowPitch = app->video_stride},
    };
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

/* Format check and the YCbCr sampler shared by every imported frame. */
static void create_video_sampler(struct app *app)
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
    VkSamplerCreateInfo plane_sampler = sampler;
    plane_sampler.pNext = NULL;
    need_vk("vkCreateSampler(TP10)",
            vkCreateSampler(app->device, &plane_sampler, NULL,
                            &app->plane_sampler));

    printf("nv12_sampler format=%d ycbcr_709=1 range=narrow"
           " chroma_location=%s\n",
           VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
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
    /* Binding 1 is the NV12 frame (immutable YCbCr sampler), 2 and 3 the
     * TP10 luma and chroma planes; each pipeline uses only its own. */
    VkDescriptorSetLayoutBinding bindings[4] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
                       VK_SHADER_STAGE_FRAGMENT_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
         .pImmutableSamplers = &app->video_sampler},
        {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
        {.binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
    };
    VkDescriptorSetLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 4, .pBindings = bindings};
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
    VkShaderModule tp10_frag = shader(app->device, "tp10-projection.frag.spv");
    stages[1].module = tp10_frag;
    need_vk("vkCreateGraphicsPipelines(TP10)", vkCreateGraphicsPipelines(app->device, VK_NULL_HANDLE, 1, &pipeline, NULL, &app->pipeline_tp10));
    vkDestroyShaderModule(app->device, vert, NULL); vkDestroyShaderModule(app->device, frag, NULL);
    vkDestroyShaderModule(app->device, tp10_frag, NULL);
}

static struct player_ui_state player_state(const struct app *app)
{
    return (struct player_ui_state){
        .title = app->title,
        .position_us = app->position_us,
        .duration_us = app->duration_us,
        .scrub_us = app->scrub_us,
        .scrubbing = app->scrub_hand >= 0 || app->seek_pending,
        .loading = app->awaiting_frame,
        .paused = app->player_state == NV12_PLAYER_PAUSED,
        .grabbed = app->grab_hand >= 0,
        .popup_open = app->popup_open,
        .projection_mode = app->projection_mode,
        .stereo_layout = app->stereo_layout,
        .swap_eyes = app->swap_eyes,
        .hover = app->ui_hover,
        .preview_visible = app->preview_visible,
        .preview_us = app->preview_us,
        .preview = app->preview,
    };
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
    app->ui_cpu = calloc(1, UI_BYTES);
    need(app->ui_cpu != NULL, "allocate panel pixels");
    app->ui_dirty = true;
}

static void upload_ui(struct app *app)
{
    struct player_ui_state state = player_state(app);
    player_ui_render(&app->player_ui, &state, app->ui_cpu, app->ui_bgra);
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
    memcpy(app->ui_pixels, app->ui_cpu, UI_BYTES);
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

static void write_video_descriptor(struct app *app, VkDescriptorSet set,
                                   const struct video_buffer *buffer)
{
    VkImageView view = buffer->view;
    VkDescriptorBufferInfo descriptor_buffer = {
        .buffer = app->uniform_buffer,
        .offset = 0,
        .range = sizeof(struct view_uniform),
    };
    VkDescriptorImageInfo descriptor_image = {
        .sampler = app->video_sampler,
        .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    VkDescriptorImageInfo planes[2] = {
        {.sampler = app->plane_sampler, .imageView = buffer->view,
         .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
        {.sampler = app->plane_sampler, .imageView = buffer->view_uv,
         .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
    };
    VkWriteDescriptorSet writes[3] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
         .pBufferInfo = &descriptor_buffer},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = set, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .pImageInfo = &descriptor_image},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = set, .dstBinding = 2, .descriptorCount = 2,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .pImageInfo = planes},
    };
    if (buffer->tp10) {
        vkUpdateDescriptorSets(app->device, 1, &writes[0], 0, NULL);
        vkUpdateDescriptorSets(app->device, 1, &writes[2], 0, NULL);
    } else {
        vkUpdateDescriptorSets(app->device, 2, writes, 0, NULL);
    }
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
    /* A set for every possible buffer (the count varies per stream) plus
     * one extra, which keeps a frame on screen across a session restart
     * while the regular buffers are imported again. */
    const uint32_t set_count = NV12_IPC_MAX_BUFFERS + 1U;
    VkDescriptorPoolSize sizes[2] = {
        {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
         .descriptorCount = set_count},
        {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 3U * set_count},
    };
    VkDescriptorPoolCreateInfo descriptor_pool = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = set_count, .poolSizeCount = 2,
        .pPoolSizes = sizes};
    need_vk("vkCreateDescriptorPool", vkCreateDescriptorPool(app->device, &descriptor_pool, NULL, &app->descriptor_pool));
    VkDescriptorSetLayout layouts[NV12_IPC_MAX_BUFFERS + 1U];
    VkDescriptorSet sets[NV12_IPC_MAX_BUFFERS + 1U];
    for (uint32_t i = 0; i < set_count; ++i)
        layouts[i] = app->descriptor_layout;
    VkDescriptorSetAllocateInfo descriptor_allocate = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = app->descriptor_pool,
        .descriptorSetCount = set_count,
        .pSetLayouts = layouts};
    need_vk("vkAllocateDescriptorSets",
            vkAllocateDescriptorSets(app->device, &descriptor_allocate,
                                     sets));
    for (uint32_t i = 0; i < NV12_IPC_MAX_BUFFERS; ++i)
        app->video_buffers[i].descriptor_set = sets[i];
    app->spare_set = sets[NV12_IPC_MAX_BUFFERS];
}

static void destroy_video_buffer(struct app *app, struct video_buffer *buffer)
{
    VkDescriptorSet set = buffer->descriptor_set;
    if (buffer->producer_semaphore)
        vkDestroySemaphore(app->device, buffer->producer_semaphore, NULL);
    if (buffer->view)
        vkDestroyImageView(app->device, buffer->view, NULL);
    if (buffer->image)
        vkDestroyImage(app->device, buffer->image, NULL);
    if (buffer->memory)
        vkFreeMemory(app->device, buffer->memory, NULL);
    if (buffer->view_uv)
        vkDestroyImageView(app->device, buffer->view_uv, NULL);
    if (buffer->image_uv)
        vkDestroyImage(app->device, buffer->image_uv, NULL);
    if (buffer->memory_uv)
        vkFreeMemory(app->device, buffer->memory_uv, NULL);
    if (buffer->dma_buf_fd >= 0)
        close(buffer->dma_buf_fd);
    *buffer = (struct video_buffer){.dma_buf_fd = -1, .descriptor_set = set};
}

/* Session restart (seek): keep only the frame on screen, free the other
 * imports so the new decoder can allocate its CAPTURE buffers, and keep
 * presenting the held frame until the first frame of the new stream. */
static void hold_current_frame(struct app *app)
{
    need_vk("vkDeviceWaitIdle(hold)", vkDeviceWaitIdle(app->device));
    int keep = app->current_video_index;
    if (keep >= 0) {
        discard_held_frame(app);
        app->held = app->video_buffers[keep];
        app->held.producer_wait_pending = false;
        app->video_buffers[keep] = (struct video_buffer){
            .dma_buf_fd = -1, .descriptor_set = app->spare_set,
        };
        app->spare_set = VK_NULL_HANDLE;
        app->has_held = true;
    }
    for (uint32_t i = 0; i < app->video_buffer_count; ++i)
        if ((int)i != keep)
            destroy_video_buffer(app, &app->video_buffers[i]);
    app->current_video_index = -1;
    app->awaiting_frame = true;
    app->ui_dirty = true;
    printf("video_hold kept_frame=%d\n", keep);
}

static void discard_held_frame(struct app *app)
{
    if (!app->has_held)
        return;
    need_vk("vkDeviceWaitIdle(discard held)", vkDeviceWaitIdle(app->device));
    VkDescriptorSet set = app->held.descriptor_set;
    destroy_video_buffer(app, &app->held);
    app->spare_set = set;
    app->has_held = false;
}

static void reinitialize_video(struct app *app,
                               const struct nv12_ipc_message *message,
                               const struct msghdr *received)
{
    apply_init(app, message, received);
    for (uint32_t i = 0; i < app->video_buffer_count; ++i) {
        import_video_buffer(app, i);
        write_video_descriptor(app, app->video_buffers[i].descriptor_set,
                               &app->video_buffers[i]);
    }
    app->current_video_index = -1;
    app->eos_received = false;
    send_ipc(app, NV12_IPC_READY, 0);
    printf("video_reinit buffers=%u\n", app->video_buffer_count);
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

static XrQuaternionf quaternion_conjugate(XrQuaternionf q)
{
    return (XrQuaternionf){-q.x, -q.y, -q.z, q.w};
}

static XrQuaternionf yaw_quaternion(float yaw)
{
    return (XrQuaternionf){0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f)};
}

static float vector_dot(XrVector3f a, XrVector3f b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

struct panel_hit {
    bool plane;
    float u, v;
};

static bool panel_hit_inside(const struct panel_hit *hit)
{
    return hit->plane && hit->u >= 0.0f && hit->u < 1.0f &&
           hit->v >= 0.0f && hit->v < 1.0f;
}

/* Intersect the aim ray with the (unbounded) panel plane. u/v are panel
 * texture coordinates and may lie outside [0,1). */
static struct panel_hit panel_ray_at(XrVector3f center, float yaw,
                                     float width_meters, float height_meters,
                                     XrPosef aim)
{
    struct panel_hit hit = {0};
    XrQuaternionf orientation = yaw_quaternion(yaw);
    XrVector3f normal = rotate_vector(orientation, (XrVector3f){0, 0, 1});
    XrVector3f direction =
        rotate_vector(aim.orientation, (XrVector3f){0, 0, -1});
    float denominator = vector_dot(direction, normal);
    if (denominator > -0.0001f)
        return hit;
    XrVector3f to_center = {center.x - aim.position.x,
                            center.y - aim.position.y,
                            center.z - aim.position.z};
    float distance = vector_dot(to_center, normal) / denominator;
    if (distance <= 0.0f)
        return hit;
    XrVector3f point = {aim.position.x + direction.x * distance - center.x,
                        aim.position.y + direction.y * distance - center.y,
                        aim.position.z + direction.z * distance - center.z};
    XrVector3f local = rotate_vector(quaternion_conjugate(orientation), point);
    hit.plane = true;
    hit.u = local.x / width_meters + 0.5f;
    hit.v = 0.5f - local.y / height_meters;
    return hit;
}

/* Intersect the aim ray with the (unbounded) player panel plane. u/v are
 * panel texture coordinates and may lie outside [0,1). */
static struct panel_hit panel_ray(const struct app *app, XrPosef aim)
{
    return panel_ray_at((XrVector3f){app->panel_x, app->panel_y, app->panel_z},
                        app->panel_yaw, UI_PANEL_WIDTH_METERS,
                        UI_PANEL_HEIGHT_METERS, aim);
}

static int64_t scrub_target_us(const struct app *app, float u)
{
    return (int64_t)((double)player_ui_seek_ratio(u) *
                     (double)app->duration_us);
}

/* Hit on a visible part of the player panel (not its transparent area). */
static bool on_player_panel(const struct app *app,
                            const struct panel_hit *hit)
{
    struct player_ui_state state = player_state(app);
    return panel_hit_inside(hit) &&
           player_ui_on_panel(&state, hit->u, hit->v);
}

/* Puts the pointer layer on the panel plane at the hit point, slightly in
 * front of the panel. */
static void place_pointer(struct app *app, XrVector3f center, float yaw,
                          float width_meters, float height_meters,
                          struct panel_hit hit, float size)
{
    XrQuaternionf orientation = yaw_quaternion(yaw);
    XrVector3f local = {(hit.u - 0.5f) * width_meters,
                        (0.5f - hit.v) * height_meters, 0.006f};
    XrVector3f offset = rotate_vector(orientation, local);
    app->pointer_pose = (XrPosef){
        orientation,
        {center.x + offset.x, center.y + offset.y, center.z + offset.z}};
    app->pointer_size = size;
    app->pointer_layer_visible = true;
}

struct hand_input {
    bool aim_valid;
    XrPosef aim;
    struct panel_hit hit;
    bool select_edge;
    bool select_held;
    XrActionStateBoolean squeeze;
};

static void read_boolean(struct app *app, XrAction action, XrPath hand,
                         XrActionStateBoolean *state)
{
    XrActionStateGetInfo info = {
        .type = XR_TYPE_ACTION_STATE_GET_INFO,
        .action = action,
        .subactionPath = hand,
    };
    *state = (XrActionStateBoolean){.type = XR_TYPE_ACTION_STATE_BOOLEAN};
    need_xr("xrGetActionStateBoolean",
            xrGetActionStateBoolean(app->session, &info, state));
}

/* Horizontal thumbstick flicks: -1/+1 once per deflection past the
 * threshold (vertical movement is ignored), 0 otherwise. */
static int read_stick(struct app *app, unsigned hand)
{
    XrActionStateGetInfo info = {
        .type = XR_TYPE_ACTION_STATE_GET_INFO,
        .action = app->stick_action,
        .subactionPath = app->hands[hand],
    };
    XrActionStateVector2f state = {.type = XR_TYPE_ACTION_STATE_VECTOR2F};
    need_xr("xrGetActionStateVector2f",
            xrGetActionStateVector2f(app->session, &info, &state));
    float x = state.isActive ? state.currentState.x : 0.0f;
    float y = state.isActive ? state.currentState.y : 0.0f;
    int direction = app->stick_direction[hand];
    if (direction && direction * x < STICK_RELEASE_THRESHOLD) {
        app->stick_direction[hand] = 0;
        return 0;
    }
    if (!direction) {
        if (fabsf(x) < STICK_PRESS_THRESHOLD || fabsf(y) > fabsf(x))
            return 0;
        direction = x > 0.0f ? 1 : -1;
        app->stick_direction[hand] = direction;
        return direction;
    }
    return 0;
}

static void read_select(struct app *app, unsigned hand,
                        struct hand_input *input)
{
    XrActionStateGetInfo info = {
        .type = XR_TYPE_ACTION_STATE_GET_INFO,
        .action = app->select_action,
        .subactionPath = app->hands[hand],
    };
    XrActionStateFloat state = {.type = XR_TYPE_ACTION_STATE_FLOAT};
    need_xr("xrGetActionStateFloat",
            xrGetActionStateFloat(app->session, &info, &state));
    bool down = app->select_down[hand];
    if (!state.isActive)
        down = false;
    else if (!down && state.currentState >= SELECT_PRESS_THRESHOLD)
        down = true;
    else if (down && state.currentState < SELECT_RELEASE_THRESHOLD)
        down = false;
    input->select_edge = down && !app->select_down[hand];
    input->select_held = down;
    app->select_down[hand] = down;
}

static bool pressed_edge(const XrActionStateBoolean *state)
{
    return state->isActive && state->changedSinceLastSync &&
           state->currentState;
}

static bool held(const XrActionStateBoolean *state)
{
    return state->isActive && state->currentState;
}

/* Show the panel again. If it is far outside the gaze cone it is moved in
 * front of the viewer, otherwise it keeps its world position. */
static void show_panel(struct app *app, const XrSpaceLocation *head,
                       bool head_valid)
{
    app->ui_visible = true;
    if (head_valid) {
        XrVector3f gaze = rotate_vector(head->pose.orientation,
                                        (XrVector3f){0, 0, -1});
        XrVector3f to_panel = {app->panel_x - head->pose.position.x,
                               app->panel_y - head->pose.position.y,
                               app->panel_z - head->pose.position.z};
        float length = sqrtf(vector_dot(to_panel, to_panel));
        if (length < 0.1f ||
            vector_dot(gaze, to_panel) / length < GRAB_RECENTER_COS) {
            float flat = sqrtf(gaze.x * gaze.x + gaze.z * gaze.z);
            if (flat > 0.01f) {
                gaze.x /= flat;
                gaze.z /= flat;
                app->panel_x = head->pose.position.x + gaze.x * 1.3f;
                app->panel_y = head->pose.position.y - 0.35f;
                app->panel_z = head->pose.position.z + gaze.z * 1.3f;
                app->panel_yaw = atan2f(-gaze.x, -gaze.z);
            }
        }
    }
    app->ui_dirty = true;
    send_panel_state(app);
    printf("player_panel=visible x=%.3f y=%.3f z=%.3f yaw=%.3f\n",
           app->panel_x, app->panel_y, app->panel_z, app->panel_yaw);
}

/* Shows the scrub preview for `position_us` (-1 hides it) and asks the
 * trickplay worker for the matching picture. */
static void update_preview(struct app *app, int64_t position_us)
{
    bool visible = position_us >= 0 && app->duration_us > 0;
    if (visible != app->preview_visible ||
        (visible && position_us / 1000000 != app->preview_us / 1000000))
        app->ui_dirty = true;
    app->preview_visible = visible;
    if (!visible)
        return;
    app->preview_us = position_us;
    trickplay_request(app->trickplay, position_us, app->stereo_layout);
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
    XrSpaceLocation head = {.type = XR_TYPE_SPACE_LOCATION};
    need_xr("xrLocateSpace(player head)",
            xrLocateSpace(app->view_space, app->space, time, &head));
    const XrSpaceLocationFlags valid =
        XR_SPACE_LOCATION_POSITION_VALID_BIT |
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    bool head_valid = (head.locationFlags & valid) == valid;

    struct hand_input hands[2] = {0};
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
                    xrLocateSpace(app->aim_spaces[hand], app->space,
                                  time, &location));
            if ((location.locationFlags & valid) == valid) {
                hands[hand].aim_valid = true;
                hands[hand].aim = location.pose;
                hands[hand].hit = panel_ray(app, location.pose);
            }
        }
        read_select(app, hand, &hands[hand]);
        read_boolean(app, app->squeeze_action, app->hands[hand],
                     &hands[hand].squeeze);
        int flick = read_stick(app, hand);
        if (flick && app->scrub_hand < 0 && !app->seek_pending)
            send_ipc(app, NV12_IPC_ACTION,
                     flick > 0 ? NV12_ACTION_SEEK_FORWARD
                               : NV12_ACTION_SEEK_BACK);
    }

    /* Grip: grab the panel, or show/hide it when pressed on empty space. */
    if (app->grab_hand >= 0 &&
        !held(&hands[app->grab_hand].squeeze)) {
        app->grab_hand = -1;
        app->ui_dirty = true;
        send_panel_state(app);
        printf("player_panel=released x=%.3f y=%.3f z=%.3f yaw=%.3f\n",
               app->panel_x, app->panel_y, app->panel_z, app->panel_yaw);
    }
    for (unsigned hand = 0; hand < 2; ++hand) {
        if (!pressed_edge(&hands[hand].squeeze) || app->grab_hand >= 0 ||
            app->scrub_hand >= 0)
            continue;
        if (app->ui_visible && hands[hand].aim_valid &&
            on_player_panel(app, &hands[hand].hit)) {
            XrPosef aim = hands[hand].aim;
            XrVector3f center = {app->panel_x - aim.position.x,
                                 app->panel_y - aim.position.y,
                                 app->panel_z - aim.position.z};
            app->grab_offset = rotate_vector(
                quaternion_conjugate(aim.orientation), center);
            app->grab_hand = (int)hand;
            app->ui_dirty = true;
            printf("player_panel=grabbed hand=%u\n", hand);
        } else if (app->ui_visible) {
            app->ui_visible = false;
            send_panel_state(app);
            printf("player_panel=hidden\n");
        } else {
            show_panel(app, &head, head_valid);
        }
    }
    if (app->grab_hand >= 0 && hands[app->grab_hand].aim_valid) {
        XrPosef aim = hands[app->grab_hand].aim;
        XrVector3f offset = rotate_vector(aim.orientation, app->grab_offset);
        app->panel_x = aim.position.x + offset.x;
        app->panel_y = aim.position.y + offset.y;
        app->panel_z = aim.position.z + offset.z;
        if (head_valid)
            app->panel_yaw = atan2f(head.pose.position.x - app->panel_x,
                                    head.pose.position.z - app->panel_z);
    }

    /* Trigger: buttons on press, seek bar while held. */
    if (app->scrub_hand >= 0) {
        struct hand_input *input = &hands[app->scrub_hand];
        if (input->aim_valid && input->hit.plane) {
            int64_t target = scrub_target_us(app, input->hit.u);
            if (target != app->scrub_us) {
                app->scrub_us = target;
                app->ui_dirty = true;
            }
        }
        if (!input->select_held) {
            printf("player_seek_bar target_us=%" PRId64 "\n", app->scrub_us);
            app->scrub_hand = -1;
            app->seek_pending = true;
            app->ui_dirty = true;
            send_seek_to(app, app->scrub_us);
        }
    }
    for (unsigned hand = 0; hand < 2; ++hand) {
        if (!app->ui_visible || app->grab_hand >= 0 ||
            app->scrub_hand >= 0 || app->seek_pending ||
            !hands[hand].select_edge || !hands[hand].aim_valid ||
            !on_player_panel(app, &hands[hand].hit))
            continue;
        struct player_ui_state state = player_state(app);
        int action = player_ui_hit_test(&state, hands[hand].hit.u,
                                        hands[hand].hit.v);
        if (action == PLAYER_UI_SEEK_BAR) {
            app->scrub_hand = (int)hand;
            app->scrub_us = scrub_target_us(app, hands[hand].hit.u);
            app->ui_dirty = true;
        } else if (action == PLAYER_UI_VIEW_POPUP) {
            app->popup_open = !app->popup_open;
            app->ui_dirty = true;
        } else if (action >= PLAYER_UI_PROJECTION) {
            change_view(app, action);
        } else if (action) {
            send_ipc(app, NV12_IPC_ACTION, (uint32_t)action);
            /* Before the first frame the session may be blocked opening the
             * source and cannot read the action. */
            if (action == NV12_ACTION_BACK && app->awaiting_frame)
                send_control(app, PLAYER_UI_STOP_REQUESTED, NULL);
        }
    }

    int hover = 0;
    int64_t hover_us = -1;
    bool pointer_visible = false;
    int pointer_x = 0;
    int pointer_y = 0;
    app->pointer_layer_visible = false;
    if (app->ui_visible && app->grab_hand < 0) {
        for (unsigned hand = 0; hand < 2; ++hand) {
            if (!hands[hand].aim_valid ||
                !on_player_panel(app, &hands[hand].hit))
                continue;
            pointer_visible = true;
            pointer_x = (int)(hands[hand].hit.u * UI_WIDTH);
            pointer_y = (int)(hands[hand].hit.v * UI_HEIGHT);
            struct player_ui_state state = player_state(app);
            hover = player_ui_hit_test(&state, hands[hand].hit.u,
                                       hands[hand].hit.v);
            if (hover == PLAYER_UI_SEEK_BAR)
                hover_us = scrub_target_us(app, hands[hand].hit.u);
            place_pointer(app,
                          (XrVector3f){app->panel_x, app->panel_y,
                                       app->panel_z},
                          app->panel_yaw, UI_PANEL_WIDTH_METERS,
                          UI_PANEL_HEIGHT_METERS, hands[hand].hit, 0.03f);
        }
        if (app->scrub_hand >= 0)
            hover = PLAYER_UI_SEEK_BAR;
    }
    if (hover != app->ui_hover) {
        app->ui_hover = hover;
        app->ui_dirty = true;
    }
    update_preview(app, app->scrub_hand >= 0 ? app->scrub_us : hover_us);
    if (pointer_visible != app->ui_pointer_visible ||
        (pointer_visible &&
         (pointer_x != app->ui_pointer_x ||
          pointer_y != app->ui_pointer_y))) {
        app->ui_pointer_visible = pointer_visible;
        app->ui_pointer_x = pointer_x;
        app->ui_pointer_y = pointer_y;
    }
}

static XrCompositionLayerQuad make_ui_layer(const struct app *app)
{
    return (XrCompositionLayerQuad){
        .type = XR_TYPE_COMPOSITION_LAYER_QUAD,
        .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
        .space = app->space,
        .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
        .subImage = {
            .swapchain = app->ui_swapchain,
            .imageRect = {.extent = {UI_WIDTH, UI_HEIGHT}},
        },
        .pose = {yaw_quaternion(app->panel_yaw),
                 {app->panel_x, app->panel_y, app->panel_z}},
        .size = {UI_PANEL_WIDTH_METERS, UI_PANEL_HEIGHT_METERS},
    };
}


/* ---- browser mode ---------------------------------------------------- */

#define BROWSER_PANEL_Z (-2.4f)

static void create_browser_texture(struct app *app)
{
    XrSwapchainCreateInfo info = {
        .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
        .usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                      XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
        .format = app->ui_format,
        .sampleCount = 1,
        .width = BROWSER_UI_WIDTH,
        .height = BROWSER_UI_HEIGHT,
        .faceCount = 1,
        .arraySize = 1,
        .mipCount = 1,
    };
    need_xr("xrCreateSwapchain(browser)",
            xrCreateSwapchain(app->session, &info, &app->browser_swapchain));
    need_xr("xrEnumerateSwapchainImages(browser count)",
            xrEnumerateSwapchainImages(app->browser_swapchain, 0,
                                       &app->browser_image_count, NULL));
    need(app->browser_image_count > 0 && app->browser_image_count <= MAX_IMAGES,
         "unexpected browser swapchain image count");
    for (uint32_t i = 0; i < app->browser_image_count; ++i)
        app->browser_images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
    need_xr("xrEnumerateSwapchainImages(browser list)",
            xrEnumerateSwapchainImages(
                app->browser_swapchain, app->browser_image_count,
                &app->browser_image_count,
                (XrSwapchainImageBaseHeader *)app->browser_images));
    VkCommandBufferAllocateInfo commands = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = app->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    need_vk("vkAllocateCommandBuffers(browser)",
            vkAllocateCommandBuffers(app->device, &commands,
                                     &app->browser_command));
    VkFenceCreateInfo fence = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    need_vk("vkCreateFence(browser)",
            vkCreateFence(app->device, &fence, NULL, &app->browser_fence));
    const VkDeviceSize bytes = (VkDeviceSize)BROWSER_UI_WIDTH *
                               BROWSER_UI_HEIGHT * 4U;
    VkBufferCreateInfo buffer = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    need_vk("vkCreateBuffer(browser staging)",
            vkCreateBuffer(app->device, &buffer, NULL,
                           &app->browser_staging_buffer));
    VkMemoryRequirements memory;
    vkGetBufferMemoryRequirements(app->device, app->browser_staging_buffer,
                                  &memory);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memory.size,
        .memoryTypeIndex = memory_type(
            app->physical, memory.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    need_vk("vkAllocateMemory(browser staging)",
            vkAllocateMemory(app->device, &allocation, NULL,
                             &app->browser_staging_memory));
    need_vk("vkBindBufferMemory(browser staging)",
            vkBindBufferMemory(app->device, app->browser_staging_buffer,
                               app->browser_staging_memory, 0));
    need_vk("vkMapMemory(browser staging)",
            vkMapMemory(app->device, app->browser_staging_memory, 0, bytes, 0,
                        (void **)&app->browser_pixels));
    app->browser_cpu = calloc(1, (size_t)bytes);
    need(app->browser_cpu != NULL, "allocate browser pixels");
    app->browser.dirty = true;
}

/* Uploads `pixels` (width x height RGBA, canvas layout) into every image of
 * a swapchain whose content never changes. */
static void fill_static_swapchain(struct app *app, XrSwapchain swapchain,
                                  const uint8_t *pixels, uint32_t width,
                                  uint32_t height)
{
    uint32_t count = 0;
    XrSwapchainImageVulkan2KHR images[MAX_IMAGES];
    need_xr("xrEnumerateSwapchainImages(static count)",
            xrEnumerateSwapchainImages(swapchain, 0, &count, NULL));
    need(count > 0 && count <= MAX_IMAGES, "unexpected static image count");
    for (uint32_t i = 0; i < count; ++i)
        images[i] = (XrSwapchainImageVulkan2KHR){
            .type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR};
    need_xr("xrEnumerateSwapchainImages(static list)",
            xrEnumerateSwapchainImages(swapchain, count, &count,
                                       (XrSwapchainImageBaseHeader *)images));
    VkDeviceSize bytes = (VkDeviceSize)width * height * 4U;
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buffer;
    need_vk("vkCreateBuffer(static)",
            vkCreateBuffer(app->device, &buffer_info, NULL, &buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(app->device, buffer, &requirements);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type(
            app->physical, requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    VkDeviceMemory memory;
    need_vk("vkAllocateMemory(static)",
            vkAllocateMemory(app->device, &allocation, NULL, &memory));
    need_vk("vkBindBufferMemory(static)",
            vkBindBufferMemory(app->device, buffer, memory, 0));
    void *mapping;
    need_vk("vkMapMemory(static)",
            vkMapMemory(app->device, memory, 0, bytes, 0, &mapping));
    memcpy(mapping, pixels, (size_t)bytes);
    VkCommandBufferAllocateInfo commands = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = app->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer command;
    need_vk("vkAllocateCommandBuffers(static)",
            vkAllocateCommandBuffers(app->device, &commands, &command));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    need_vk("vkCreateFence(static)",
            vkCreateFence(app->device, &fence_info, NULL, &fence));
    for (uint32_t n = 0; n < count; ++n) {
        XrSwapchainImageAcquireInfo acquire = {
            .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        uint32_t index = 0;
        need_xr("xrAcquireSwapchainImage(static)",
                xrAcquireSwapchainImage(swapchain, &acquire, &index));
        XrSwapchainImageWaitInfo wait = {
            .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
            .timeout = XR_INFINITE_DURATION};
        need_xr("xrWaitSwapchainImage(static)",
                xrWaitSwapchainImage(swapchain, &wait));
        VkCommandBufferBeginInfo begin = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        need_vk("vkBeginCommandBuffer(static)",
                vkBeginCommandBuffer(command, &begin));
        VkImageMemoryBarrier transfer = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .image = images[index].image,
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .levelCount = 1, .layerCount = 1},
        };
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                             NULL, 1, &transfer);
        VkBufferImageCopy copy = {
            .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .layerCount = 1},
            .imageExtent = {width, height, 1},
        };
        vkCmdCopyBufferToImage(command, buffer, images[index].image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        VkImageMemoryBarrier sample = transfer;
        sample.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        sample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sample.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        sample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL,
                             0, NULL, 1, &sample);
        need_vk("vkEndCommandBuffer(static)", vkEndCommandBuffer(command));
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                               .commandBufferCount = 1,
                               .pCommandBuffers = &command};
        need_vk("vkQueueSubmit(static)",
                vkQueueSubmit(app->queue, 1, &submit, fence));
        need_vk("vkWaitForFences(static)",
                vkWaitForFences(app->device, 1, &fence, VK_TRUE,
                                UINT64_C(5000000000)));
        need_vk("vkResetFences(static)",
                vkResetFences(app->device, 1, &fence));
        need_vk("vkResetCommandBuffer(static)",
                vkResetCommandBuffer(command, 0));
        XrSwapchainImageReleaseInfo release = {
            .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        need_xr("xrReleaseSwapchainImage(static)",
                xrReleaseSwapchainImage(swapchain, &release));
    }
    vkDestroyFence(app->device, fence, NULL);
    vkFreeCommandBuffers(app->device, app->command_pool, 1, &command);
    vkUnmapMemory(app->device, memory);
    vkDestroyBuffer(app->device, buffer, NULL);
    vkFreeMemory(app->device, memory, NULL);
}

#define POINTER_PIXELS 64U

static void create_pointer_texture(struct app *app)
{
    XrSwapchainCreateInfo info = {
        .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
        .usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                      XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
        .format = app->ui_format,
        .sampleCount = 1,
        .width = POINTER_PIXELS,
        .height = POINTER_PIXELS,
        .faceCount = 1,
        .arraySize = 1,
        .mipCount = 1,
    };
    need_xr("xrCreateSwapchain(pointer)",
            xrCreateSwapchain(app->session, &info, &app->pointer_swapchain));
    uint8_t pixels[POINTER_PIXELS * POINTER_PIXELS * 4] = {0};
    struct ui_canvas canvas = {pixels, POINTER_PIXELS, POINTER_PIXELS,
                               app->ui_bgra};
    const float c = POINTER_PIXELS / 2.0f;
    ui_fill_circle(&canvas, c, c, c - 2, UI_RGBA(0, 0, 0, 110));
    ui_fill_circle(&canvas, c, c, c - 8, UI_RGBA(255, 255, 255, 240));
    ui_fill_circle(&canvas, c, c, c - 17, UI_RGBA(56, 160, 255, 255));
    fill_static_swapchain(app, app->pointer_swapchain, pixels,
                          POINTER_PIXELS, POINTER_PIXELS);
}

static XrCompositionLayerQuad make_pointer_layer(const struct app *app)
{
    return (XrCompositionLayerQuad){
        .type = XR_TYPE_COMPOSITION_LAYER_QUAD,
        .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
        .space = app->space,
        .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
        .subImage = {
            .swapchain = app->pointer_swapchain,
            .imageRect = {.extent = {POINTER_PIXELS, POINTER_PIXELS}},
        },
        .pose = app->pointer_pose,
        .size = {app->pointer_size, app->pointer_size},
    };
}

static void upload_browser(struct app *app)
{
    /* Draw in cached memory: blending reads pixels back, which is very slow
     * on the uncached staging mapping. */
    browser_ui_render(&app->browser, app->browser_cpu, app->ui_bgra);
    XrSwapchainImageAcquireInfo acquire = {
        .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO,
    };
    uint32_t index = 0;
    need_xr("xrAcquireSwapchainImage(browser)",
            xrAcquireSwapchainImage(app->browser_swapchain, &acquire, &index));
    XrSwapchainImageWaitInfo wait = {
        .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
        .timeout = XR_INFINITE_DURATION,
    };
    need_xr("xrWaitSwapchainImage(browser)",
            xrWaitSwapchainImage(app->browser_swapchain, &wait));
    need(index < app->browser_image_count, "invalid browser swapchain index");
    need_vk("vkWaitForFences(browser)",
            vkWaitForFences(app->device, 1, &app->browser_fence, VK_TRUE,
                            UINT64_C(5000000000)));
    need_vk("vkResetFences(browser)",
            vkResetFences(app->device, 1, &app->browser_fence));
    memcpy(app->browser_pixels, app->browser_cpu,
           (size_t)BROWSER_UI_WIDTH * BROWSER_UI_HEIGHT * 4U);
    need_vk("vkResetCommandBuffer(browser)",
            vkResetCommandBuffer(app->browser_command, 0));
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    need_vk("vkBeginCommandBuffer(browser)",
            vkBeginCommandBuffer(app->browser_command, &begin));
    bool initialized = app->browser_image_initialized[index];
    VkImageMemoryBarrier transfer = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = initialized ? VK_ACCESS_SHADER_READ_BIT : 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                 : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .image = app->browser_images[index].image,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .levelCount = 1, .layerCount = 1},
    };
    vkCmdPipelineBarrier(app->browser_command,
                         initialized ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                     : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         1, &transfer);
    VkBufferImageCopy copy = {
        .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .layerCount = 1},
        .imageExtent = {BROWSER_UI_WIDTH, BROWSER_UI_HEIGHT, 1},
    };
    vkCmdCopyBufferToImage(app->browser_command, app->browser_staging_buffer,
                           app->browser_images[index].image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    VkImageMemoryBarrier sample = transfer;
    sample.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    sample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    sample.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    sample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(app->browser_command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &sample);
    need_vk("vkEndCommandBuffer(browser)",
            vkEndCommandBuffer(app->browser_command));
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &app->browser_command,
    };
    /* SteamVR samples the image as soon as it is released, so the copy must
     * be complete by then (otherwise it shows stale or empty content). */
    need_vk("vkQueueSubmit(browser)",
            vkQueueSubmit(app->queue, 1, &submit, app->browser_fence));
    need_vk("vkWaitForFences(browser completion)",
            vkWaitForFences(app->device, 1, &app->browser_fence, VK_TRUE,
                            UINT64_C(5000000000)));
    app->browser_image_initialized[index] = true;
    XrSwapchainImageReleaseInfo release = {
        .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO,
    };
    need_xr("xrReleaseSwapchainImage(browser)",
            xrReleaseSwapchainImage(app->browser_swapchain, &release));
}

static XrCompositionLayerQuad make_browser_layer(const struct app *app)
{
    return (XrCompositionLayerQuad){
        .type = XR_TYPE_COMPOSITION_LAYER_QUAD,
        .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
        .space = app->space,
        .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
        .subImage = {
            .swapchain = app->browser_swapchain,
            .imageRect = {.extent = {BROWSER_UI_WIDTH, BROWSER_UI_HEIGHT}},
        },
        .pose = {{0, 0, 0, 1}, {0, 0, BROWSER_PANEL_Z}},
        .size = {BROWSER_UI_PANEL_WIDTH_METERS,
                 BROWSER_UI_PANEL_HEIGHT_METERS},
    };
}

/* ---- launcher control socket ---------------------------------------- */

static void send_control(struct app *app, uint32_t type, const char *path)
{
    struct player_ui_result result = {.type = type};
    if (path)
        snprintf(result.path, sizeof(result.path), "%s", path);
    ssize_t written;
    do {
        written = send(app->ui_fd, &result, sizeof(result), MSG_NOSIGNAL);
    } while (written < 0 && errno == EINTR);
    if (written != (ssize_t)sizeof(result))
        app->quit = true; /* the launcher is gone */
}

/* Back to the browser: drop every video import and reset player state. */
static void end_playback(struct app *app)
{
    app->popup_open = false;
    need_vk("vkDeviceWaitIdle(end playback)", vkDeviceWaitIdle(app->device));
    discard_held_frame(app);
    for (uint32_t i = 0; i < app->video_buffer_count; ++i)
        destroy_video_buffer(app, &app->video_buffers[i]);
    if (app->ipc_fd >= 0)
        close(app->ipc_fd);
    app->ipc_fd = -1;
    app->playing = false;
    app->eos_received = false;
    app->current_video_index = -1;
    app->current_video_presented = false;
    app->awaiting_frame = true;
    app->seek_pending = false;
    app->grab_hand = -1;
    app->scrub_hand = -1;
    app->ui_hover = 0;
    app->ui_pointer_visible = false;
    app->player_state = NV12_PLAYER_PLAYING;
    printf("openxr_summary xr_frames=%" PRIu64
           " rendered_frames=%" PRIu64 " received_frames=%" PRIu64
           " unique_video_frames=%" PRIu64
           " repeated_video_frames=%" PRIu64
           " hidden_video_frames=%" PRIu64
           " pipeline_skipped_frames=%" PRIu64
           " released_frames=%" PRIu64
           " session_changes=%" PRIu64 "\n",
           app->xr_frames, app->rendered_frames, app->received_frames,
           app->unique_video_frames, app->repeated_video_frames,
           app->hidden_video_frames, app->pipeline_skipped_frames,
           app->released_frames, app->session_changes);
    app->xr_frames = app->rendered_frames = app->received_frames = 0;
    app->unique_video_frames = app->repeated_video_frames = 0;
    app->hidden_video_frames = app->pipeline_skipped_frames = 0;
    app->released_frames = 0;
    app->position_us = 0;
    app->duration_us = 0;
    app->video_width = app->video_height = 0;
    /* The trigger that pressed "back" is usually still held: treat it as
     * held so the browser only reacts after a release (it could otherwise
     * land on "Beenden" and close the program). */
    app->select_down[0] = app->select_down[1] = true;
    trickplay_open(app->trickplay, NULL);
    ui_image_free(app->preview);
    app->preview = NULL;
    app->preview_visible = false;
    app->ui_dirty = true;
    app->browser.dirty = true;
    printf("playback_ended\n");
}

/* `location` is what the session plays; "jf:item:ID[:PARENT]" has
 * trickplay previews. */
static void begin_playback(struct app *app, int fd, const char *location)
{
    if (app->playing)
        end_playback(app);
    if (strncmp(location, "jf:item:", 8) == 0) {
        char item[64] = "";
        size_t length = strcspn(location + 8, ":");
        if (length < sizeof(item))
            memcpy(item, location + 8, length);
        item[length < sizeof(item) ? length : 0] = '\0';
        trickplay_open(app->trickplay, item);
    }
    app->ipc_fd = fd;
    app->playing = true;
    app->awaiting_command = false;
    app->eos_received = false;
    app->ui_dirty = true;
    app->select_down[0] = app->select_down[1] = true; /* see end_playback */
    printf("playback_attached\n");
}

static void handle_control(struct app *app)
{
    for (;;) {
        struct player_ui_command command = {0};
        char control[CMSG_SPACE(sizeof(int))] = {0};
        struct iovec iov = {.iov_base = &command, .iov_len = sizeof(command)};
        struct msghdr received = {
            .msg_iov = &iov, .msg_iovlen = 1,
            .msg_control = control, .msg_controllen = sizeof(control),
        };
        ssize_t bytes = recvmsg(app->ui_fd, &received, MSG_DONTWAIT);
        if (bytes < 0 && errno == EINTR)
            continue;
        if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (bytes <= 0) {
            app->quit = true; /* the launcher closed the socket */
            return;
        }
        if ((size_t)bytes != sizeof(command))
            continue;
        if (command.type == PLAYER_UI_PLAY) {
            struct cmsghdr *header = CMSG_FIRSTHDR(&received);
            if (header && header->cmsg_level == SOL_SOCKET &&
                header->cmsg_type == SCM_RIGHTS &&
                header->cmsg_len == CMSG_LEN(sizeof(int))) {
                int fd;
                memcpy(&fd, CMSG_DATA(header), sizeof(fd));
                command.path[sizeof(command.path) - 1] = '\0';
                begin_playback(app, fd, command.path);
            }
        } else if (command.type == PLAYER_UI_BROWSE) {
            if (app->playing)
                end_playback(app);
            command.path[sizeof(command.path) - 1] = '\0';
            command.message[sizeof(command.message) - 1] = '\0';
            browser_ui_navigate(&app->browser, command.path, command.message);
            app->awaiting_command = false;
        }
    }
}

static void sync_browser_input(struct app *app, XrTime time)
{
    XrActiveActionSet active = {.actionSet = app->action_set};
    XrActionsSyncInfo sync = {
        .type = XR_TYPE_ACTIONS_SYNC_INFO,
        .countActiveActionSets = 1,
        .activeActionSets = &active,
    };
    need_xr("xrSyncActions(browser)", xrSyncActions(app->session, &sync));
    const XrSpaceLocationFlags valid =
        XR_SPACE_LOCATION_POSITION_VALID_BIT |
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    bool pointer_visible = false, pressed = false;
    app->pointer_layer_visible = false;
    int pointer_x = 0, pointer_y = 0, hover = -1;
    for (unsigned hand = 0; hand < 2; ++hand) {
        XrActionStateGetInfo pose_info = {
            .type = XR_TYPE_ACTION_STATE_GET_INFO,
            .action = app->aim_action,
            .subactionPath = app->hands[hand],
        };
        XrActionStatePose pose = {.type = XR_TYPE_ACTION_STATE_POSE};
        need_xr("xrGetActionStatePose(browser)",
                xrGetActionStatePose(app->session, &pose_info, &pose));
        struct hand_input input = {0};
        read_select(app, hand, &input);
        if (input.select_edge)
            pressed = true;
        int flick = read_stick(app, hand);
        if (flick)
            browser_ui_flip(&app->browser, flick);
        if (!pose.isActive)
            continue;
        XrSpaceLocation location = {.type = XR_TYPE_SPACE_LOCATION};
        need_xr("xrLocateSpace(browser aim)",
                xrLocateSpace(app->aim_spaces[hand], app->space, time,
                              &location));
        if ((location.locationFlags & valid) != valid)
            continue;
        struct panel_hit hit = panel_ray_at(
            (XrVector3f){0, 0, BROWSER_PANEL_Z}, 0.0f,
            BROWSER_UI_PANEL_WIDTH_METERS, BROWSER_UI_PANEL_HEIGHT_METERS,
            location.pose);
        if (panel_hit_inside(&hit)) {
            pointer_visible = true;
            pointer_x = (int)(hit.u * BROWSER_UI_WIDTH);
            pointer_y = (int)(hit.v * BROWSER_UI_HEIGHT);
            hover = browser_ui_hit_test(hit.u, hit.v);
            place_pointer(app, (XrVector3f){0, 0, BROWSER_PANEL_Z}, 0.0f,
                          BROWSER_UI_PANEL_WIDTH_METERS,
                          BROWSER_UI_PANEL_HEIGHT_METERS, hit, 0.04f);
        }
    }
    browser_ui_set_pointer(&app->browser, pointer_visible, pointer_x,
                           pointer_y, hover);
    if (!pressed || app->awaiting_command)
        return;
    char path[PLAYER_UI_PATH_MAX];
    switch (browser_ui_activate(&app->browser, path, sizeof(path))) {
    case BROWSER_UI_FILE_SELECTED:
        app->awaiting_command = true;
        snprintf(app->title, sizeof(app->title), "%s",
                 app->browser.selected_title);
        send_control(app, PLAYER_UI_FILE_SELECTED, path);
        break;
    case BROWSER_UI_EXIT_REQUESTED:
        app->awaiting_command = true;
        send_control(app, PLAYER_UI_EXIT_REQUESTED, NULL);
        app->quit = true;
        break;
    case BROWSER_UI_NONE:
        break;
    }
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
    if (app->playing) {
        sync_player_input(app, frame_state.predictedDisplayTime);
        if (app->ui_dirty)
            upload_ui(app);
    } else {
        sync_browser_input(app, frame_state.predictedDisplayTime);
        if (app->browser.dirty)
            upload_browser(app);
    }
    if (!frame_state.shouldRender) {
        XrFrameEndInfo end = {.type = XR_TYPE_FRAME_END_INFO, .displayTime = frame_state.predictedDisplayTime,
            .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE};
        need_xr("xrEndFrame(empty)", xrEndFrame(app->session, &end));
        return;
    }
    /* Without a decoded frame (browser, start, seek preroll) only the clear
     * colour and the panel are shown, so the headset never falls back to the
     * Steam environment. */
    struct video_buffer *video = NULL;
    if (app->playing && app->current_video_index >= 0)
        video = &app->video_buffers[app->current_video_index];
    else if (app->playing && app->has_held)
        video = &app->held;
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
                app->video_width ? 1.0f / (float)app->video_width : 0.0f,
                app->video_height ? 1.0f / (float)app->video_height : 0.0f,
            },
            .video_scale = {
                app->coded_width ? (float)app->video_width /
                                       (float)app->coded_width : 1.0f,
                app->coded_height ? (float)app->video_height /
                                        (float)app->coded_height : 1.0f,
                0.0f, 0.0f,
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
        .image = video ? video->image : VK_NULL_HANDLE,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .levelCount = 1, .layerCount = 1},
    };
    VkImageMemoryBarrier video_acquires[2] = {video_acquire, video_acquire};
    uint32_t video_images = video && video->tp10 ? 2U : 1U;
    if (video_images == 2U)
        video_acquires[1].image = video->image_uv;
    if (video)
        vkCmdPipelineBarrier(app->command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL,
                             0, NULL, video_images, video_acquires);
    for (uint32_t eye = 0; eye < 2; ++eye) {
        VkClearValue clear = {.color = {{0.005f, 0.005f, 0.008f, 1.0f}}};
        VkRenderPassBeginInfo pass = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = app->render_pass, .framebuffer = app->framebuffers[image_index][eye],
            .renderArea = {.extent = {app->width, app->height}}, .clearValueCount = 1, .pClearValues = &clear};
        vkCmdBeginRenderPass(app->command, &pass, VK_SUBPASS_CONTENTS_INLINE);
        if (video) {
            vkCmdBindPipeline(app->command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              video->tp10 ? app->pipeline_tp10 : app->pipeline);
            uint32_t offset = eye * UBO_STRIDE;
            vkCmdBindDescriptorSets(app->command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    app->pipeline_layout, 0, 1,
                                    &video->descriptor_set, 1, &offset);
            vkCmdDraw(app->command, 4, 1, 0, 0);
        }
        vkCmdEndRenderPass(app->command);
    }
    VkImageMemoryBarrier video_release = video_acquire;
    video_release.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    video_release.dstAccessMask = 0;
    video_release.srcQueueFamilyIndex = app->queue_family;
    video_release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    VkImageMemoryBarrier video_releases[2] = {video_release, video_release};
    if (video_images == 2U)
        video_releases[1].image = video->image_uv;
    if (video)
        vkCmdPipelineBarrier(app->command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL,
                             0, NULL, video_images, video_releases);
    need_vk("vkEndCommandBuffer(render)", vkEndCommandBuffer(app->command));
    VkPipelineStageFlags producer_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &app->command};
    if (video && video->producer_wait_pending) {
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
    XrCompositionLayerQuad ui_layer = app->playing
                                          ? make_ui_layer(app)
                                          : make_browser_layer(app);
    XrCompositionLayerQuad pointer_layer = make_pointer_layer(app);
    const XrCompositionLayerBaseHeader *layers[] = {
        (const XrCompositionLayerBaseHeader *)&layer,
        (const XrCompositionLayerBaseHeader *)&ui_layer,
        (const XrCompositionLayerBaseHeader *)&pointer_layer,
    };
    bool show_ui = app->playing ? app->ui_visible : true;
    uint32_t layer_count = 1;
    if (show_ui)
        layer_count = app->pointer_layer_visible ? 3U : 2U;
    XrFrameEndInfo end = {.type = XR_TYPE_FRAME_END_INFO, .displayTime = frame_state.predictedDisplayTime,
        .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
        .layerCount = layer_count,
        .layers = layers};
    need_xr("xrEndFrame", xrEndFrame(app->session, &end));
    ++app->rendered_frames;
    if (!video)
        return;
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
        fprintf(stderr, "usage: %s UI_FD\n", argv[0]);
        return 2;
    }
    struct app app = {0};
    app.ui_fd = (int)parse_u64(argv[1], "UI fd");
    app.ipc_fd = -1;
    app.video_buffer_count = VIDEO_BUFFER_COUNT;
    for (uint32_t i = 0; i < NV12_IPC_MAX_BUFFERS; ++i)
        app.video_buffers[i].dma_buf_fd = -1;
    app.held.dma_buf_fd = -1;
    app.current_video_index = -1;
    app.player_state = NV12_PLAYER_PLAYING;
    app.ui_visible = true;
    app.ui_dirty = true;
    app.panel_y = -0.35f;
    app.panel_z = -1.3f;
    app.grab_hand = -1;
    app.scrub_hand = -1;
    app.awaiting_frame = true;
    char browser_error[256] = {0};
    if (browser_ui_open(&app.browser, "root:", NULL, browser_error,
                        sizeof(browser_error)) < 0) {
        fprintf(stderr, "browser_error=%s\n", browser_error);
        return 2;
    }
    app.trickplay = trickplay_create();
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
    create_video_sampler(&app);
    create_render_resources(&app);
    create_ui_resources(&app);
    create_browser_texture(&app);
    create_pointer_texture(&app);
    printf("host_ready\n");
    while (!app.quit) {
        handle_control(&app);
        if (app.playing) {
            receive_frame_messages(&app);
            struct ui_image *preview = NULL;
            if (trickplay_poll(app.trickplay, &preview)) {
                ui_image_free(app.preview);
                app.preview = preview;
                if (app.preview_visible)
                    app.ui_dirty = true;
            }
        }
        browser_ui_update(&app.browser);
        poll_events(&app);
        hide_dashboard_at_startup(&app);
        if (app.quit)
            break;
        if (app.running) render_frame(&app); else nanosleep(&pause, NULL);
        if (app.playing && app.eos_received)
            end_playback(&app);
    }
    if (app.xr_exit)
        send_control(&app, PLAYER_UI_EXIT_REQUESTED, NULL);
    if (app.playing)
        end_playback(&app);
    browser_ui_close(&app.browser);
    player_ui_close(&app.player_ui);
    trickplay_destroy(app.trickplay);
    ui_image_free(app.preview);
    return 0;
}
