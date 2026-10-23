#ifndef STEREONAUT_PANEL_COMPOSITOR_H
#define STEREONAUT_PANEL_COMPOSITOR_H

#include <pthread.h>

#include "browser-ui.h"
#include "controller-bindings.h"
#include "jellyfin.h"
#include "player-ui.h"
#include "xr-math.h"
#include "xr-session.h"

/* The panels on their way to the headset: the player panel (drawn in a
 * thread of its own), the browser with the settings page over it and the
 * pointer dot. On the Frame they are quad layers; drawn into the eye
 * images (panels_in_eye_images, a PC) they are mipmapped images the
 * overlay samples with the panel pipeline. */

/* The settings page sits centred on the browser panel. */
#define SETTINGS_X ((int)(BROWSER_UI_WIDTH - PLAYER_UI_POPUP_W) / 2)
#define SETTINGS_Y ((int)(BROWSER_UI_HEIGHT - PLAYER_UI_POPUP_H) / 2)

/* A UI panel drawn into the overlay layer (see panels_in_eye_images): a
 * mipmapped image the renderer samples itself. As a quad layer
 * (STEREONAUT_UI_LAYER=quad) only width and height are used. */
struct ui_texture {
    uint32_t width, height;
    uint32_t mip_levels;
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkDescriptorSet descriptor_set;
    bool ready;
};

/* The player panel is drawn on the CPU in a thread of its own: in the
 * frame loop its several milliseconds pushed frames past their deadline
 * on a PC, where the video copy takes its share. The loop hands the thread
 * a snapshot of the panel's state (with its own copies of everything the
 * state points to) and uploads the finished pixels. */
struct panel_job {
    struct player_ui_state state;
    char title[256];
    struct jf_details details;
    float adjust[PLAYER_ADJUST_COUNT];
    char update_installed[64], update_latest[64], update_status[256];
    struct binding bindings[BINDING_ACTION_COUNT];
    char debug_text[192];
    struct ui_image *preview[PLAYER_UI_PREVIEW_STRIP];
};

struct panel_thread {
    pthread_t thread;
    bool started;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    bool stopping;
    bool pending; /* the job waits for the thread */
    bool busy;    /* the thread draws the job into pixels */
    bool done;    /* pixels hold a finished panel */
    struct panel_job job;
    uint32_t *pixels; /* UI_BYTES */
};

struct panel_compositor {
    VkFormat ui_format;
    bool ui_bgra;
    bool panels_in_eye_images;
    /* Player panel: quad swapchain or texture, staging buffer. */
    XrSwapchain ui_swapchain;
    uint32_t ui_image_count;
    XrSwapchainImageVulkan2KHR ui_images[MAX_IMAGES];
    bool ui_image_initialized[MAX_IMAGES];
    struct ui_texture ui_texture, browser_texture;
    VkBuffer ui_staging_buffer;
    VkDeviceMemory ui_staging_memory;
    uint32_t *ui_pixels; /* mapped staging memory: write-only */
    VkCommandBuffer ui_command;
    VkFence ui_fence;
    struct player_ui player_ui;
    /* The player panel's thread and the lock of player_ui (fonts), which
     * the settings page shares with it. */
    struct panel_thread panel_thread;
    pthread_mutex_t player_ui_mutex;
    uint32_t *settings_cpu; /* the settings page is drawn here */
    /* Browser panel. */
    XrSwapchain browser_swapchain;
    uint32_t browser_image_count;
    XrSwapchainImageVulkan2KHR browser_images[MAX_IMAGES];
    bool browser_image_initialized[MAX_IMAGES];
    VkBuffer browser_staging_buffer;
    VkDeviceMemory browser_staging_memory;
    uint32_t *browser_pixels; /* mapped staging memory: write-only */
    uint32_t *browser_cpu;    /* the browser draws here (cached memory) */
    VkCommandBuffer browser_command;
    VkFence browser_fence;
    /* Pointer dot as its own small quad layer, so moving the pointer does
     * not require redrawing and uploading a panel texture. */
    XrSwapchain pointer_swapchain;
    bool pointer_layer_visible;
    XrPosef pointer_pose;
    float pointer_size;
    /* Panels drawn into the eye images. */
    VkSampler panel_sampler;
    VkDescriptorSetLayout panel_descriptor_layout;
    VkDescriptorPool panel_descriptor_pool;
    VkPipelineLayout panel_pipeline_layout;
    VkPipeline panel_pipeline;
};

/* Where an aim ray meets a panel plane. */
struct panel_hit {
    bool plane;
    float u, v;
    float distance; /* metres along the aim ray */
};

struct app;

/* Where the panels are drawn. SteamVR makes quad layers overlays of its
 * own, which it composites over every projection layer: the controllers
 * and pointer beams (overlay layer, see record_overlay) vanished behind a
 * panel. On a PC quad layers also shimmer and step while the head moves.
 * The panels are therefore drawn into the overlay layer's eye images, from
 * mipmapped images with trilinear and anisotropic filtering, below the
 * controllers; the overlay has a higher resolution than the video for the
 * text (OVERLAY_SCALE). STEREONAUT_UI_LAYER=quad brings the quad layers
 * back (controllers then disappear behind the panels). */
bool panels_in_eye_images(void);
void create_ui_swapchain(struct app *app);
void create_ui_resources(struct app *app);
void start_panel_thread(struct app *app);
void stop_panel_thread(struct app *app);
void create_browser_texture(struct app *app);
/* Pipeline for the panels drawn into the eye images: a textured quad,
 * blended over the video with premultiplied alpha like a quad layer. */
void create_panel_pipeline(struct app *app);
void create_pointer_texture(struct app *app);
/* Uploads `pixels` (width x height RGBA, canvas layout) into every image of
 * a swapchain whose content never changes. */
void fill_static_swapchain(struct app *app, XrSwapchain swapchain,
                           const uint8_t *pixels, uint32_t width,
                           uint32_t height);
/* Copies the staged drawing into the quad layer's swapchain image (target)
 * or into the panel image, whose smaller mip levels are then filtered down
 * from it level by level. */
void record_ui_upload(VkCommandBuffer command,
                      struct ui_texture *texture, VkImage target,
                      VkBuffer staging, bool initialized);
/* Per frame while playing: uploads a panel the thread finished and hands
 * it the next one when the state changed. */
void sync_player_panel(struct app *app);
void upload_browser(struct app *app);
/* The browser stands in front of home at eye height. */
XrVector3f browser_center(const struct app *app);
/* Puts the pointer layer on the panel plane at the hit point, slightly in
 * front of the panel. */
void place_pointer(struct app *app, XrVector3f center, float yaw,
                   float width_meters, float height_meters,
                   struct panel_hit hit, float size);
/* Model matrix of a panel quad (unit square in the shader) with the pose
 * and size of its quad layer. */
struct mat4 panel_model(const XrCompositionLayerQuad *quad);
XrCompositionLayerQuad make_ui_layer(const struct app *app);
XrCompositionLayerQuad make_browser_layer(const struct app *app);
XrCompositionLayerQuad make_pointer_layer(const struct app *app);
bool panel_hit_inside(const struct panel_hit *hit);
/* Intersect the aim ray with the (unbounded) panel plane. u/v are panel
 * texture coordinates and may lie outside [0,1). */
struct panel_hit panel_ray_at(XrVector3f center, float yaw,
                              float width_meters, float height_meters,
                              XrPosef aim);
/* Intersect the aim ray with the (unbounded) player panel plane. u/v are
 * panel texture coordinates and may lie outside [0,1). */
struct panel_hit panel_ray(const struct app *app, XrPosef aim);

#endif
