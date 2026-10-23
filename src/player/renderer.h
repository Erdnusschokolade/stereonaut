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
#include "overlay.h"
#include "autofocus.h"
#include "panel-compositor.h"
#include "video-import.h"
#include "session-ipc.h"

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
/* Entries of button_inputs (buttons beyond trigger and grip). */
#define BUTTON_INPUT_COUNT 14
#define UBO_STRIDE 512U
#define PI_F 3.14159265358979f
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

_Static_assert(sizeof(struct view_uniform) <= UBO_STRIDE,
               "view uniform exceeds dynamic UBO stride");

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
    struct autofocus autofocus;
    bool focus_threshold;   /* only subjects nearer than FOCUS_DISTANCE */
    bool av1_software;      /* settings.json, read by the next session */
    /* Passthrough chosen (settings.json); whether the runtime offers it is
     * xr.alpha_blend_supported. */
    bool passthrough;
    /* Chroma key colour picker: waiting for the trigger (the measurement
     * is in autofocus). */
    bool key_picking;
    /* Debug display ("Blick anzeigen", see struct autofocus). */
    bool gaze_debug;
    VkDescriptorSetLayout descriptor_layout;
    VkDescriptorPool descriptor_pool;
    struct video_import video;
    bool awaiting_frame; /* no frame of the stream yet (start, seek) */
    int ui_fd;              /* control socket to the launcher */
    bool playing;           /* a playback session is attached (link) */
    bool quit;
    bool xr_exit;           /* the runtime ended the session */
    bool awaiting_command;  /* a selection was sent, wait for PLAY/BROWSE */
    struct browser_ui browser;
    struct panel_compositor panels;
    struct session_link link;
    char location[PLAYER_UI_PATH_MAX]; /* what the session plays */
    int end_action;       /* enum player_end_action (settings.json) */
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
    struct overlay overlay;
    XrAction gaze_action;
    XrSpace gaze_space;
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
    VkBuffer uniform_buffer;
    VkDeviceMemory uniform_memory;
    void *uniform_mapping;
    VkImageView image_views[MAX_IMAGES][2];
    VkFramebuffer framebuffers[MAX_IMAGES][2];
    bool popup_open;
    int popup_tab;                       /* enum player_ui_tab */
    /* Settings page over the browser: the popup's settings tabs, drawn
     * into panels.settings_cpu and copied onto the browser panel. */
    bool settings_open;
    int settings_tab;
    int settings_hover;
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
    uint64_t xr_frames, rendered_frames, session_changes;
    uint64_t received_frames, released_frames;
    uint64_t unique_video_frames, repeated_video_frames;
    struct frame_timing timing;
    uint64_t hidden_video_frames, pipeline_skipped_frames;
};

/* Still in stereonaut-renderer.c, used by the modules split off so far. */
void save_adjust(const struct app *app, int index);
struct player_ui_state player_state(const struct app *app);
struct player_ui_state settings_state(const struct app *app);
void timing_ui(struct app *app, double started);

#endif
