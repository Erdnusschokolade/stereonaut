#ifndef STEREONAUT_RENDER_PIPELINE_H
#define STEREONAUT_RENDER_PIPELINE_H

#include "xr-math.h"
#include "xr-session.h"

/* The video in the eye images: the projection pipelines (NV12 through the
 * YCbCr sampler, TP10/P010 through plane images), their per-eye uniforms
 * (projection, placement, stereo offsets, picture, levels, chroma key) and
 * the command buffer of a frame. The shaders are
 * tools/openxr-playback/video-projection.vert and *-projection.frag. */

#define UBO_STRIDE 512U

struct view_uniform {
    struct mat4 mvp;
    struct mat4 camera_to_world;
    float fov_tangents[4];
    int32_t modes[4];
    float source_info[4];
    float video_scale[4]; /* TP10: visible / padded size of the planes */
    float levels[4];      /* black, white, chroma range (8-bit codes) */
    /* Eye position inside the video dome (radius 1 m, dome axes): head
     * motion (6DoF), zoom, height and slide. */
    float eye_offset[4];
    /* This eye's stereo offset: longitude and latitude shift (radians) and
     * the same as texture offset for the flat screen. */
    float stereo_offset[4];
    float picture[4];     /* brightness, contrast, saturation, gamma */
    float background[4];  /* premultiplied colour where there is no video */
    float chroma_key[4];  /* key direction (Cb, Cr), threshold, softness */
    float chroma_spill[4]; /* spill removal, key on (> 0) */
};

_Static_assert(sizeof(struct view_uniform) <= UBO_STRIDE,
               "view uniform exceeds dynamic UBO stride");

struct render_pipeline {
    VkCommandPool command_pool;
    VkCommandBuffer command;
    VkFence fence;
    VkRenderPass render_pass;
    VkPipelineLayout pipeline_layout; /* also the disparity measurement's */
    VkPipeline pipeline;
    VkPipeline pipeline_tp10;
    VkDescriptorSetLayout descriptor_layout;
    VkDescriptorPool descriptor_pool;
    /* Two view_uniform, UBO_STRIDE apart (left, right eye). */
    VkBuffer uniform_buffer;
    VkDeviceMemory uniform_memory;
    void *uniform_mapping;
    VkImageView image_views[MAX_IMAGES][2];
    VkFramebuffer framebuffers[MAX_IMAGES][2];
};

struct app;

/* Command pool and buffer, the pipelines, uniforms, the disparity buffer
 * and a descriptor set per video buffer (plus a spare). */
void create_render_resources(struct app *app);
/* Shader levels (enum nv12_video_levels): black and white luma and the
 * chroma range in 8-bit code values. */
void video_levels_uniform(uint32_t levels, float out[4]);
/* Passthrough shows the room where nothing is drawn, if the viewer chose
 * it and the runtime blends with the room (the Frame since SteamVR's
 * update of 2026-10-10; not the PC streaming to it). */
bool passthrough_active(const struct app *app);
/* Premultiplied colour where there is no video (clear colour, outside the
 * 180 degree image, around the flat screen, keyed out): the usual near
 * black, or with passthrough black at the dimming's opacity over the
 * room. */
void background_color(const struct app *app, float color[4]);
/* Chroma key of the title for the video shaders (chroma_key in
 * nv12-projection.frag): green and blue as directions in the Cb/Cr plane
 * (BT.709, normalized chroma), a picked colour as chroma and luma;
 * threshold, softness and spill removal. */
void chroma_key_uniform(const struct app *app, float key[4],
                        float spill[4]);
XrEnvironmentBlendMode environment_blend_mode(const struct app *app);
/* Where a ray (LOCAL space, unit direction) hits the video as render_frame
 * shows it: texture uv in the picture the left eye sees and the distance
 * along the ray (if `distance` is not NULL), false if it misses. The
 * dome is the unit sphere around the eye position in dome axes, the flat
 * screen the quad of screen_model; mirrors nv12-projection.frag without
 * the stereo offsets. */
bool video_uv_at_ray(const struct app *app, XrVector3f origin,
                     XrVector3f direction, XrVector3f head,
                     struct mat4 world_to_dome,
                     XrVector3f eye_position, struct mat4 screen_model,
                     float uv[2], float *distance);

#endif
