#ifndef STEREONAUT_RENDERER_H
#define STEREONAUT_RENDERER_H

/* Shared declarations of the XR host: stereonaut-renderer.c (main loop,
 * launcher commands, playback start and end) and its modules. The
 * host's state is one struct app; each module keeps its own part in a
 * struct of its header, the player's state shared by all of them lies
 * in struct app itself. */

#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_PLATFORM_XLIB

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/dma-buf.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "browser-ui.h"
#include "controller-bindings.h"
#include "decoded-video.h"
#include "gltf-model.h"
#include "i18n.h"
#include "render-models.h"
#include "jellyfin.h"
#include "player-ui.h"
#include "settings.h"
#include "title-settings.h"
#include "trickplay.h"
#include "updater.h"
#include "ui-draw.h"
#include "nv12-playback-ipc.h"
#include "player-ui-ipc.h"
#include "xr-math.h"
#include "xr-session.h"

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
/* Entries of button_inputs (buttons beyond trigger and grip). */
#define BUTTON_INPUT_COUNT 14
#define VIDEO_BUFFER_COUNT 12U
#define UBO_STRIDE 512U
#define PI_F 3.14159265358979f
#define DRM_FORMAT_MOD_LINEAR UINT64_C(0)
#define DRM_FORMAT_MOD_QCOM_COMPRESSED UINT64_C(0x0500000000000001)
#define UI_WIDTH PLAYER_UI_WIDTH
#define UI_HEIGHT PLAYER_UI_HEIGHT
#define UI_BYTES (UI_WIDTH * UI_HEIGHT * 4U)
#define UI_PANEL_WIDTH_METERS PLAYER_UI_PANEL_WIDTH_METERS
#define PC_RENDER_SCALE 1.3f
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
/* Grip as a modifier of button bindings: its analog value, because the
 * Frame's grip click needs a firm squeeze that a hand also moving the stick
 * rarely keeps up. */
#define GRIP_PRESS_THRESHOLD 0.40f
#define GRIP_RELEASE_THRESHOLD 0.25f

/* Holding a stick direction or button repeats its action (volume, tilt,
 * zoom) after the delay, then at the interval. */
#define BINDING_REPEAT_DELAY 0.5
#define BINDING_REPEAT_INTERVAL 0.15

/* Seeking with the bindings (seek_binding): presses less than
 * SEEK_BURST_GAP apart form a burst. Up to its fourth press each one
 * seeks at once; from the fifth, or once a held input repeats, playback
 * pauses and only the seek bar (with the trickplay preview) moves until
 * the input rests for SEEK_SCRUB_SETTLE. Held, the steps grow. */
#define SEEK_STEP_US INT64_C(10000000)
#define SEEK_BURST_GAP 0.5
#define SEEK_BURST_SCRUB 5
#define SEEK_SCRUB_SETTLE 0.5

/* Hovering or dragging the seek bar: the position follows the pointer only
 * once it moved BAR_HYSTERESIS_PX (panel pixels) away, and the preview
 * redraws the panel at most every PREVIEW_REDRAW_INTERVAL seconds. The
 * hand's jitter otherwise redrew it (several ms on the CPU) every frame. */
#define BAR_HYSTERESIS_PX 5.0f
#define PREVIEW_REDRAW_INTERVAL 0.033

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

/* Autofocus measurement (stereo-disparity.comp). */
#define DISPARITY_CANDIDATES 128
#define DISPARITY_ZERO 16          /* candidates below zero disparity */
#define DISPARITY_STEP_DEGREES 0.15f
#define DISPARITY_WINDOW_DEGREES_X 4.0f
#define DISPARITY_WINDOW_DEGREES_Y 3.0f

struct disparity_push {
    float left[4];   /* window centre (texture uv), sample step */
    float right[4];  /* window centre, shift per candidate, zero index */
    float bounds[4]; /* right eye u range, TP10 plane scale */
    float sample_mode[4]; /* x > 0: measure the key colour at left.xy */
};

struct disparity_result {
    float score[DISPARITY_CANDIDATES];
    float left_variance;
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
    /* DMA-BUF: wait for the producer's fence. Host memory: copy the new
     * frame into the image before sampling. */
    bool producer_wait_pending;
    /* NV12_BUFFER_HOST: the memfd's mapping as Vulkan host memory, copied
     * into the device-local image(s) above for every new frame. */
    bool host;
    void *host_mapping;
    size_t host_size;
    VkBuffer host_buffer;
    VkDeviceMemory host_memory;
};

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

/* Trigger state with hysteresis (button_update). */
struct button {
    bool down;       /* debounced physical state */
    bool suppressed; /* ignore the current press until a genuine release */
};

/* Frame timing, logged every FRAME_TIMING_INTERVAL seconds (frame_timing):
 * frames SteamVR waited for (missed: the predicted display time skipped a
 * period), the wait in xrWaitFrame, the CPU time of a frame and the GPU
 * time (the frame waits for its commands), split into frames that copied a
 * new video picture from system memory (PC) and frames that did not, and
 * the panel redraws. */
#define FRAME_TIMING_INTERVAL 5.0
struct frame_timing {
    double window_start; /* monotonic seconds */
    XrTime last_display;
    uint32_t frames, rendered, missed, ui_redraws;
    double wait_ms, cpu_ms, cpu_max_ms, ui_ms, ui_max_ms;
    double gpu_ms[2], gpu_max_ms[2]; /* [1]: with a video copy */
    uint32_t gpu_frames[2];
    double period_ms;
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

/* A controller model on the GPU (record_overlay). */
struct gpu_model {
    struct gltf_model mesh;
    uint32_t source;          /* index in app->render_models */
    int *animation;           /* per glTF node: animatable node or -1 */
    VkBuffer vertices, indices;
    VkDeviceMemory vertex_memory, index_memory;
    VkImage texture;
    VkDeviceMemory texture_memory;
    VkImageView texture_view;
    VkDescriptorSet set;
};

struct app {
    struct xr_state xr;
    struct gpu gpu;
    VkCommandPool command_pool;
    VkCommandBuffer command;
    VkFence fence;
    VkRenderPass render_pass;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    VkPipeline pipeline_tp10;
    /* Autofocus: disparity measurement (NV12 and TP10 variants) and its
     * host-visible result (struct disparity_result). */
    VkPipeline disparity_pipeline, disparity_pipeline_tp10;
    VkBuffer disparity_buffer;
    VkDeviceMemory disparity_memory;
    void *disparity_mapping;
    bool disparity_pending;
    uint64_t disparity_frames;
    double autofocus_log_time;
    float autofocus_offset; /* degrees the eye pictures are moved apart */
    float autofocus_speed;  /* degrees per second of the offset */
    double autofocus_time;  /* last advance of the offset */
    /* Subject the focus is set to (distance in metres and its disparity
     * in degrees); none: the video is shown unchanged. */
    bool autofocus_focused;
    float autofocus_distance, autofocus_focus_disparity;
    bool focus_threshold;   /* only subjects nearer than FOCUS_DISTANCE */
    bool av1_software;      /* settings.json, read by the next session */
    /* Passthrough chosen (settings.json); whether the runtime offers it is
     * xr.alpha_blend_supported. */
    bool passthrough;
    /* Chroma key colour picker: waiting for the trigger, the aim it was
     * pulled with (measured in the next frame), result being read back. */
    bool key_picking, key_pick_request, key_sample_pending;
    XrPosef key_pick_aim;
    bool autofocus_gaze;    /* last window followed the eye tracking */
    /* Last measurement, for the debug display. */
    bool autofocus_valid;
    float autofocus_disparity, autofocus_score, autofocus_variance;
    /* Debug display ("Blick anzeigen"): a frame of the measurement window
     * along the gaze (green: eye tracking, orange: head direction) and a
     * text line on the panel. */
    bool gaze_debug;
    XrSwapchain gaze_marker[2]; /* eye tracking, head direction */
    XrPosef gaze_marker_pose;
    bool gaze_marker_visible;
    double debug_text_time;
    char debug_text[192];
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
    /* Host buffers being imported by video_import_worker. */
    pthread_t video_import_thread;
    bool video_importing;
    atomic_bool video_import_done;
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
    bool title_finished;  /* EOS after the last frame, not "back" */
    char location[PLAYER_UI_PATH_MAX]; /* what the session plays */
    int end_action;       /* enum player_end_action (settings.json) */
    VkDeviceSize dma_buf_size;
    uint32_t video_width, video_height, video_stride;
    uint32_t video_format; /* enum nv12_pixel_format */
    uint32_t buffer_kind;  /* enum nv12_buffer_kind */
    uint32_t coded_width, coded_height;
    uint32_t projection_mode, stereo_layout;
    bool swap_eyes;
    uint32_t video_levels; /* enum nv12_video_levels */
    uint32_t format_source; /* enum nv12_format_source, shown on the panel */
    uint32_t fisheye_fov;   /* lens angle (degrees) for fisheye titles */
    float volume;           /* output gain (settings.json "volume") */
    XrActionSet action_set;
    XrAction aim_action;
    XrAction select_action;
    XrAction squeeze_action;
    XrAction stick_action;
    XrAction grip_value_action;
    bool grip_value_down[2];
    float grip_value[2];
    XrAction button_actions[BUTTON_INPUT_COUNT];
    /* Controller bindings (settings.json "binding_*") and their input
     * state: what was down last frame, whether a held button served as a
     * modifier (then it does not act on release), and the repeating action
     * + 1 per input. */
    struct binding bindings[BINDING_ACTION_COUNT];
    int learn_action; /* action waiting for its buttons, or -1 */
    bool input_held[BINDING_INPUT_COUNT];
    bool input_combined[BINDING_INPUT_COUNT];
    int input_repeat[BINDING_INPUT_COUNT];
    double input_repeat_time[BINDING_INPUT_COUNT];
    bool grip_toggle[2]; /* grip pressed on empty space: show/hide on release */
    /* "Next/previous video": the title to start once the session ended. */
    char skip_path[PLAYER_UI_PATH_MAX];
    char skip_title[256];
    /* Controller models (render-models.h); reloaded when the runtime
     * reports other devices. */
    struct render_models render_models;
    bool render_models_dirty;
    /* Overlay layer above the panels: controller models and pointer
     * beams with their own depth buffer (record_overlay). */
    /* One swapchain per eye: SteamVR showed the first array layer of a
     * second projection layer to both eyes. */
    XrSwapchain overlay_swapchains[2];
    /* On a PC the overlay is drawn into the eye images (overlay_in_eye_
     * images): no swapchains of its own, framebuffers on the eye images. */
    bool overlay_in_eye_images;
    uint32_t overlay_image_counts[2];
    XrSwapchainImageVulkan2KHR overlay_images[2][MAX_IMAGES];
    VkImageView overlay_views[2][MAX_IMAGES];
    VkFramebuffer overlay_framebuffers[2][MAX_IMAGES];
    VkImage overlay_depth;
    VkDeviceMemory overlay_depth_memory;
    VkImageView overlay_depth_views[2];
    VkRenderPass overlay_pass;
    VkSampler model_sampler;
    VkDescriptorSetLayout model_descriptor_layout;
    VkDescriptorPool model_descriptor_pool;
    VkPipelineLayout model_pipeline_layout, beam_pipeline_layout;
    VkPipeline model_pipeline, beam_pipeline;
    uint32_t overlay_width, overlay_height;
    struct gpu_model gpu_models[RENDER_MODELS_MAX];
    uint32_t gpu_model_count;
    /* Pointer beam per hand from the last input sync: aim and the distance
     * to the panel it hits (<= 0: none). */
    struct laser {
        bool valid;
        XrPosef aim;
        float panel_length;
    } lasers[2];
    bool overlay_wanted;
    XrAction gaze_action;
    XrSpace gaze_space;
    bool gaze_logged;
    int stick_direction[2]; /* -1 left, +1 right, 0 centred */
    struct button select[2];
    XrPath hands[2];
    XrSpace aim_spaces[2];
    int ui_hover;
    bool ui_pointer_visible;
    int ui_pointer_x;
    int ui_pointer_y;
    bool ui_visible;
    int autohide;            /* seconds without input, 0 = never */
    double last_input_time;  /* monotonic seconds */
    bool auto_hidden;        /* hidden by the timer, any trigger shows it */
    bool ui_dirty;
    /* Panel placement in the local reference space. The panel is world-fixed
     * and upright; only the yaw rotates it towards the viewer. */
    float panel_x, panel_y, panel_z, panel_yaw;
    int grab_hand;
    XrVector3f grab_offset;
    int scrub_hand;
    int64_t scrub_us;
    bool seek_pending;
    /* Seeking with the bindings (seek_binding). */
    int seek_burst;          /* quick presses in a row */
    double seek_press_time;  /* last press, monotonic seconds */
    double seek_step_time;   /* last press or repeat */
    bool seek_scrubbing;     /* paused, only scrub_us moves */
    bool seek_resume;        /* played before: play on after the seek */
    bool seek_show_panel;    /* show the panel in the next input pass */
    uint64_t seek_serial;    /* number of the last SEEK_TO sent */
    uint64_t seek_landing;   /* the one the session carries out (HOLD) */
    uint32_t player_state;
    int64_t position_us;
    int64_t duration_us;
    VkDeviceSize uv_offset;
    VkBuffer uniform_buffer;
    VkDeviceMemory uniform_memory;
    void *uniform_mapping;
    VkImageView image_views[MAX_IMAGES][2];
    VkFramebuffer framebuffers[MAX_IMAGES][2];
    XrSwapchain ui_swapchain;
    uint32_t ui_image_count;
    XrSwapchainImageVulkan2KHR ui_images[MAX_IMAGES];
    bool ui_image_initialized[MAX_IMAGES];
    VkFormat ui_format;
    bool ui_bgra;
    struct ui_texture ui_texture, browser_texture;
    bool panels_in_eye_images;
    VkSampler panel_sampler;
    VkDescriptorSetLayout panel_descriptor_layout;
    VkDescriptorPool panel_descriptor_pool;
    VkPipelineLayout panel_pipeline_layout;
    VkPipeline panel_pipeline;
    VkBuffer ui_staging_buffer;
    VkDeviceMemory ui_staging_memory;
    uint32_t *ui_pixels; /* mapped staging memory: write-only */
    struct player_ui player_ui;
    /* The player panel's thread and the lock of player_ui (fonts), which
     * the settings page shares with it. */
    struct panel_thread panel_thread;
    pthread_mutex_t player_ui_mutex;
    bool popup_open;
    int popup_tab;                       /* enum player_ui_tab */
    /* Settings page over the browser: the popup's settings tabs, drawn
     * into settings_cpu and copied onto the browser panel. */
    bool settings_open;
    int settings_tab;
    int settings_hover;
    uint32_t *settings_cpu;
    int system_language; /* what "automatic" finds, see i18n_detect */
    struct updater updater;
    bool update_auto;          /* check for a release at start */
    char update_status[256];   /* status line of the updates tab */
    float adjust[PLAYER_ADJUST_COUNT];   /* see player_adjust_info */
    char title_key[TITLE_KEY_SIZE];      /* titles.json entry, "" if none */
    /* Home: where the viewer looked when Stereonaut started or was last
     * centred ("Centre" in the player, SteamVR's recenter). The browser
     * faces it and every title starts turned towards it. */
    bool home_pending;
    XrTime home_after;        /* SteamVR's recenter applies from then on */
    XrVector3f home_position; /* head */
    float home_yaw;           /* as panel_yaw: 0 looks along -z */
    /* The dome (and the flat screen) is centred on this head position and
     * faces this gaze yaw; set at the first frame and by "Centre". */
    bool recenter_pending;
    bool recenter_yaw;
    XrVector3f dome_center;
    float dome_yaw;                      /* radians */
    char title[256];
    /* Seek bar preview: trickplay pictures of the playing Jellyfin item. */
    struct trickplay *trickplay;
    struct ui_image *preview[TRICKPLAY_STRIP];
    /* Jellyfin description of the playing title (chapters, info tab). */
    struct jf_details details;
    bool details_valid;
    bool preview_visible;
    int64_t preview_us;
    int64_t preview_spacing_us; /* between the strip's pictures */
    /* The whole strip only for seeks by binding or panel button: on the
     * hovered or dragged seek bar it was redrawn every frame (frame drops). */
    bool preview_strip;
    bool seek_by_binding; /* the pending seek came from seek_binding */
    float bar_u;          /* steady pointer position on the bar, -1 none */
    bool preview_redraw_pending;
    double preview_redraw_time; /* monotonic seconds */
    bool preview_perspective; /* straighten 180/360 degree previews */
    int preview_fov;          /* degrees, one of player_ui_preview_fovs */
    VkCommandBuffer ui_command;
    VkFence ui_fence;
    uint64_t xr_frames, rendered_frames, session_changes;
    uint64_t received_frames, released_frames;
    uint64_t unique_video_frames, repeated_video_frames;
    struct frame_timing timing;
    uint64_t hidden_video_frames, pipeline_skipped_frames;
};

/* Still in stereonaut-renderer.c, used by the modules split off so far. */
bool overlay_in_eye_images(void);

#endif
