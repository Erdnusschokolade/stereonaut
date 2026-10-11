#ifndef STEREONAUT_NV12_PLAYBACK_IPC_H
#define STEREONAUT_NV12_PLAYBACK_IPC_H

#include <stdint.h>

#define NV12_IPC_MAX_BUFFERS 32U

enum nv12_projection_mode {
    NV12_PROJECTION_FLAT = 0,
    NV12_PROJECTION_EQUIRECT_180 = 1,
    NV12_PROJECTION_EQUIRECT_360 = 2,
    /* Equidistant fisheye circle per eye; lens angle in fisheye_fov. */
    NV12_PROJECTION_FISHEYE = 3,
};

#define NV12_FISHEYE_FOV_DEFAULT 190U

enum nv12_stereo_layout {
    NV12_STEREO_MONO = 0,
    NV12_STEREO_SBS = 1,
    NV12_STEREO_TB = 2,
};

/* Code values of black and white (per title): NORMAL is BT.709 limited
 * range (16-235), DEEP keeps limited white but puts black at 0 for masters
 * graded below video black, FULL is full range (0-255). */
enum nv12_video_levels {
    NV12_LEVELS_NORMAL = 0,
    NV12_LEVELS_DEEP = 1,
    NV12_LEVELS_FULL = 2,
};

/* Where a title's projection and stereo layout come from (INIT, FORMAT). */
enum nv12_format_source {
    NV12_FORMAT_SOURCE_DEFAULT = 0, /* nothing known: 180 degrees, SBS */
    NV12_FORMAT_SOURCE_NAME = 1,    /* tags in the file name, server metadata */
    NV12_FORMAT_SOURCE_IMAGE = 2,   /* picture analysis */
    NV12_FORMAT_SOURCE_USER = 3,    /* chosen on the panel */
};

enum nv12_ipc_type {
    NV12_IPC_INIT = 1,
    /* (EOS: index 1 = the title played to its end, 0 = stopped.) */
    NV12_IPC_FRAME = 2,
    NV12_IPC_RELEASE = 3,
    NV12_IPC_FLUSH = 4,
    NV12_IPC_FLUSHED = 5,
    NV12_IPC_EOS = 6,
    NV12_IPC_READY = 7,
    NV12_IPC_STATUS = 8,
    NV12_IPC_ACTION = 9,
    NV12_IPC_PANEL = 10,
    /* Session -> renderer before a seek restart: keep showing the current
     * frame, drop every other buffer and wait for a new INIT. The renderer
     * process survives the session re-exec. */
    NV12_IPC_HOLD = 11,
    NV12_IPC_HELD = 12,
    /* Renderer -> session: the viewer changed projection, stereo layout,
     * eye order or levels on the panel (projection_mode, stereo_layout,
     * swap_eyes, video_levels). */
    NV12_IPC_VIEW = 13,
    /* Session -> renderer: the format was detected from the first frames
     * (projection_mode, stereo_layout, format_source). */
    NV12_IPC_FORMAT = 14,
    /* Renderer -> session: output volume (volume, 1 = unchanged), sent after
     * READY and on every change. */
    NV12_IPC_VOLUME = 15,
    /* Session -> renderer: a seek within the running session started; keep
     * showing the current frame (released with the next FRAME as usual)
     * and show the loading state until then. */
    NV12_IPC_SEEKING = 16,
    /* Session -> renderer: ask whether this host can decode the requested
     * codec through Vulkan Video.  It is sent before DMA-BUF INIT, so a PC
     * without the Frame's V4L2 decoder can select its native path. */
    NV12_IPC_DECODER_QUERY = 17,
    NV12_IPC_DECODER_CAPABILITIES = 18,
    /* PCVR path: Annex-B H.264/HEVC packets are passed as a sealed memfd.
     * The renderer decodes and samples the resulting Vulkan image on its
     * OpenXR device; no decoded frame crosses the process boundary. */
    NV12_IPC_VULKAN_INIT = 19,
    NV12_IPC_VULKAN_PACKET = 20,
    NV12_IPC_VULKAN_FLUSH = 21,
};

/* INIT pixel_format: how the decoder buffers are laid out. */
enum nv12_pixel_format {
    NV12_FORMAT_NV12 = 0,
    /* Iris Q10C: TP10 (three 10-bit samples per 32-bit word) with UBWC;
     * needs the patched Turnip (STEREONAUT_TP10, tools/mesa-tp10). */
    NV12_FORMAT_TP10 = 1,
};

enum nv12_decoder_capability {
    NV12_DECODER_H264_VULKAN = 1U << 0,
    NV12_DECODER_HEVC_VULKAN = 1U << 1,
};

enum nv12_player_state {
    NV12_PLAYER_PLAYING = 1,
    NV12_PLAYER_PAUSED = 2,
};

enum nv12_player_action {
    NV12_ACTION_TOGGLE_PAUSE = 1,
    NV12_ACTION_SEEK_BACK = 2,
    NV12_ACTION_SEEK_FORWARD = 3,
    NV12_ACTION_BACK = 4,
    /* Absolute seek; the target is carried in position_us. */
    NV12_ACTION_SEEK_TO = 5,
};

struct nv12_ipc_message {
    uint32_t type;
    uint32_t index;
    uint32_t buffer_count;
    /* INIT: visible picture size; stride and uv_offset describe the decoder
     * buffer, which may be padded beyond it (coded width/height). */
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint64_t size;
    uint64_t uv_offset;
    uint64_t frame_index;
    uint32_t projection_mode;
    uint32_t stereo_layout;
    uint32_t swap_eyes;
    uint32_t player_state;
    int64_t position_us;
    int64_t duration_us;
    /* Panel placement in the local reference space. Session -> renderer in
     * INIT (when panel_valid) and renderer -> session in PANEL, so the layout
     * survives the session re-exec performed for every seek. */
    float panel_x, panel_y, panel_z, panel_yaw;
    uint32_t panel_visible;
    uint32_t panel_valid;
    /* INIT: enum nv12_pixel_format and the decoder's padded picture size. */
    uint32_t pixel_format;
    uint32_t coded_width;
    uint32_t coded_height;
    /* INIT and VIEW: enum nv12_video_levels. */
    uint32_t video_levels;
    /* INIT and FORMAT: enum nv12_format_source. */
    uint32_t format_source;
    /* VOLUME: linear gain, 0 to 1.5. */
    float volume;
    /* INIT, VIEW and FORMAT: fisheye lens angle in degrees. */
    uint32_t fisheye_fov;
    /* DECODER_QUERY: AVCodecID; DECODER_CAPABILITIES: bitset above.
     * VULKAN_INIT: AVCodecID.  The protocol is local and both peers use the
     * same FFmpeg headers, so AVCodecID is the most explicit codec identity. */
    uint32_t video_codec;
    uint32_t decoder_capabilities;
    /* VULKAN_PACKET: bytes in its sole SCM_RIGHTS memfd. */
    uint64_t packet_size;
};

#endif
