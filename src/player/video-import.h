#ifndef STEREONAUT_VIDEO_IMPORT_H
#define STEREONAUT_VIDEO_IMPORT_H

#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>

#include "nv12-playback-ipc.h"
#include "xr-session.h"

/* The session's decoded pictures as Vulkan images: DMA-BUFs (Iris, dav1d)
 * imported in place, NV12 through a YCbCr sampler or TP10/P010 as two
 * plane images, or host memory (PC decoders, memfds) copied into
 * device-local images for every new frame. Keeps the frame on screen
 * while the session restarts after a seek. */

#define VIDEO_BUFFER_COUNT 12U

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

struct video_import {
    VkSamplerYcbcrConversion conversion;
    VkSampler sampler;       /* NV12 (YCbCr) */
    VkSampler plane_sampler; /* TP10 planes: plain linear sampling */
    struct video_buffer buffers[NV12_IPC_MAX_BUFFERS];
    uint32_t buffer_count;
    /* Last frame kept on screen while the session restarts after a seek. */
    struct video_buffer held;
    bool has_held;
    VkDescriptorSet spare_set;
    /* Host buffers being imported by video_import_worker. */
    pthread_t import_thread;
    bool importing;
    atomic_bool import_done;
    int current;    /* buffer shown, -1 none */
    bool presented; /* the current buffer was shown at least once */
    /* Layout of the stream (NV12 IPC INIT). */
    VkDeviceSize dma_buf_size;
    uint32_t width, height, stride;
    uint32_t format;      /* enum nv12_pixel_format */
    uint32_t buffer_kind; /* enum nv12_buffer_kind */
    uint32_t coded_width, coded_height;
    VkDeviceSize uv_offset;
};

struct app;

/* Format check and the YCbCr sampler shared by every imported frame. The
 * sampler only reconstructs chroma (RGB_IDENTITY: r = Cr, g = Y, b = Cb);
 * the shader applies the title's levels and BT.709. */
void create_video_sampler(struct app *app);
/* NV12 IPC INIT: imports the stream's buffers (host memory in a worker)
 * and answers READY. */
void reinitialize_video(struct app *app,
                        const struct nv12_ipc_message *message,
                        const struct msghdr *received);
/* True while the worker still imports; completes the INIT once it is done
 * (or, with wait, after waiting for it). */
bool video_import_pending(struct app *app, bool wait);
/* Records the copy of a new host-memory frame into its image(s); the
 * images stay in GENERAL like imported DMA-BUFs. */
void record_host_upload(struct app *app,
                        const struct video_buffer *video);
void destroy_video_buffer(struct app *app, struct video_buffer *buffer);
/* Session restart (seek): keep only the frame on screen, free the other
 * imports so the new decoder can allocate its CAPTURE buffers, and keep
 * presenting the held frame until the first frame of the new stream. */
void hold_current_frame(struct app *app);
void discard_held_frame(struct app *app);

#endif
