#ifndef STEREONAUT_OVERLAY_H
#define STEREONAUT_OVERLAY_H

#include "gltf-model.h"
#include "render-models.h"
#include "xr-math.h"
#include "xr-session.h"

/* Overlay: a second projection layer above the video, transparent where
 * nothing is drawn: the panels first, then the controllers (with depth)
 * and the beams. Quad layers would not do for the panels, SteamVR
 * composites them over every projection layer and so over the
 * controllers. Only submitted while the pointer is in use: always in the
 * browser, during playback while the panel is visible or a key colour is
 * being picked. */

#define BEAM_DEFAULT_LENGTH 2.5f

/* A controller model on the GPU (record_overlay). */
struct gpu_model {
    struct gltf_model mesh;
    uint32_t source;          /* index in overlay.render_models */
    int *animation;           /* per glTF node: animatable node or -1 */
    VkBuffer vertices, indices;
    VkDeviceMemory vertex_memory, index_memory;
    VkImage texture;
    VkDeviceMemory texture_memory;
    VkImageView texture_view;
    VkDescriptorSet set;
};

/* Pointer beam of a hand from the last input sync: aim and the distance
 * to the panel it hits (<= 0: none). */
struct laser {
    bool valid;
    XrPosef aim;
    float panel_length;
};

struct overlay {
    /* Controller models (render-models.h); reloaded when the runtime
     * reports other devices. */
    struct render_models render_models;
    bool render_models_dirty;
    /* One swapchain per eye: SteamVR showed the first array layer of a
     * second projection layer to both eyes. */
    XrSwapchain swapchains[2];
    /* On a PC the overlay is drawn into the eye images (overlay_in_eye_
     * images): no swapchains of its own, framebuffers on the eye images. */
    bool in_eye_images;
    uint32_t image_counts[2];
    XrSwapchainImageVulkan2KHR images[2][MAX_IMAGES];
    VkImageView views[2][MAX_IMAGES];
    VkFramebuffer framebuffers[2][MAX_IMAGES];
    /* Depth of the controllers, one layer per eye. */
    VkImage depth;
    VkDeviceMemory depth_memory;
    VkImageView depth_views[2];
    VkRenderPass pass;
    VkSampler model_sampler;
    VkDescriptorSetLayout model_descriptor_layout;
    VkDescriptorPool model_descriptor_pool;
    VkPipelineLayout model_pipeline_layout, beam_pipeline_layout;
    VkPipeline model_pipeline, beam_pipeline;
    uint32_t width, height;
    struct gpu_model gpu_models[RENDER_MODELS_MAX];
    uint32_t gpu_model_count;
    struct laser lasers[2];
    bool wanted; /* submit the overlay in this frame */
};

struct app;
struct ui_texture;

/* SteamVR on a PC shows nothing of a second projection layer: there the
 * overlay is drawn into the eye images. */
bool overlay_in_eye_images(void);
/* Swapchains or framebuffers on the eye images, depth, pipelines. */
void create_overlay(struct app *app);
/* Turns the runtime's assets (render-models.c) into buffers and textures;
 * replaces the previous ones. */
void upload_render_models(struct app *app);
/* Records the overlay's two eye passes into `command`: `panel` (if not
 * NULL) drawn with `panel_matrix`, the controllers and the beams. */
void record_overlay(struct app *app, VkCommandBuffer command,
                    const uint32_t images[2], const XrView views[2],
                    XrTime time, const struct ui_texture *panel,
                    struct mat4 panel_matrix, const float beam_lengths[2],
                    const bool beam_dots[2]);

struct frame;
/* Beam lengths, overlay images and record_overlay for the frame. */
void record_frame_overlay(struct app *app, struct frame *f);

#endif
