#ifndef STEREONAUT_XR_SESSION_H
#define STEREONAUT_XR_SESSION_H

#ifndef XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN
#endif
#ifndef XR_USE_PLATFORM_XLIB
#define XR_USE_PLATFORM_XLIB
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

/* The OpenXR instance and session with the eye swapchain, the Vulkan
 * device OpenXR chose, and the helpers every part of the host uses. */

#define MAX_IMAGES 8U

struct app;

struct xr_state {
    XrInstance instance;
    XrSystemId system;
    XrSession session;
    XrSpace space;      /* LOCAL */
    XrSpace view_space; /* VIEW: the head */
    XrSessionState state;
    bool running;
    bool should_render;
    bool view_geometry_logged;
    /* Eye tracking (XR_EXT_eye_gaze_interaction): the gaze ray steers the
     * autofocus measurement; without it the head's forward direction. */
    bool has_eye_gaze_extension, has_eye_gaze;
    bool has_valve_frame_controller;
    bool has_pcvr_controller;
    bool has_generic_controller;
    /* The runtime blends with the room (XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND,
     * for passthrough). */
    bool alpha_blend_supported;
    /* The eyes' swapchain: one image with a layer per eye. */
    VkFormat color_format;
    XrSwapchain swapchain;
    uint32_t width, height, image_count;
    XrSwapchainImageVulkan2KHR images[MAX_IMAGES];
};

struct gpu {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    PFN_vkImportSemaphoreFdKHR import_semaphore_fd;
    PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties;
    PFN_vkGetImageDrmFormatModifierPropertiesEXT get_modifier_properties;
    /* VK_EXT_external_memory_host, for PC decoder buffers (optional). */
    PFN_vkGetMemoryHostPointerPropertiesEXT get_host_pointer_properties;
    float panel_anisotropy; /* 1 without samplerAnisotropy */
};

/* Exit with a message unless the call succeeded. */
void need(bool condition, const char *what);
void need_vk(const char *what, VkResult result);
void need_xr(const char *what, XrResult result);
void die_xr(const char *what, XrResult result);

double monotonic_seconds(void);
uint32_t memory_type(VkPhysicalDevice physical, uint32_t bits,
                     VkMemoryPropertyFlags wanted);
/* SPIR-V module from a file in the working directory. */
VkShaderModule shader(VkDevice device, const char *path);

/* Instance (with the optional extensions the runtime offers), system,
 * Vulkan instance and device. */
void initialize_xr_vulkan(struct app *app);
/* The eye swapchain at the recommended size (larger on a PC, see
 * PC_RENDER_SCALE). */
void initialize_swapchain(struct app *app);
/* Session state changes, recenter, controller profile changes. */
void poll_events(struct app *app);
void hide_dashboard_at_startup(const struct app *app);

struct frame;
/* The eyes' views and the head for the display time, and the eye image
 * to draw into. */
void locate_views(struct app *app, struct frame *f);

#endif
