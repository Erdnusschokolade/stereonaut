#ifndef STEREONAUT_NV12_PLAYBACK_IPC_H
#define STEREONAUT_NV12_PLAYBACK_IPC_H

#include <stdint.h>

#define NV12_IPC_MAX_BUFFERS 32U

enum nv12_projection_mode {
    NV12_PROJECTION_FLAT = 0,
    NV12_PROJECTION_EQUIRECT_180 = 1,
    NV12_PROJECTION_EQUIRECT_360 = 2,
};

enum nv12_stereo_layout {
    NV12_STEREO_MONO = 0,
    NV12_STEREO_SBS = 1,
    NV12_STEREO_TB = 2,
};

enum nv12_ipc_type {
    NV12_IPC_INIT = 1,
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
};

#endif
