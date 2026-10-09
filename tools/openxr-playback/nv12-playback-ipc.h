#ifndef STEREONAUT_NV12_PLAYBACK_IPC_H
#define STEREONAUT_NV12_PLAYBACK_IPC_H

#include <stdint.h>

#define NV12_IPC_MAX_BUFFERS 32U

enum nv12_ipc_type {
    NV12_IPC_INIT = 1,
    NV12_IPC_FRAME = 2,
    NV12_IPC_RELEASE = 3,
    NV12_IPC_FLUSH = 4,
    NV12_IPC_FLUSHED = 5,
    NV12_IPC_EOS = 6,
    NV12_IPC_READY = 7,
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
};

#endif
