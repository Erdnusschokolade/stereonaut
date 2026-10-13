#ifndef STEREONAUT_DECODED_VIDEO_H
#define STEREONAUT_DECODED_VIDEO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Video decoded outside Iris into linear buffers that the renderer imports
 * like Iris CAPTURE buffers (memfd-backed udmabuf DMA-BUFs), with a worker
 * thread that feeds the decoder from the media pipeline:
 *
 * - DECODED_VIDEO_DAV1D, AV1 on the Frame (Iris on the shipped kernel has
 *   no AV1): dav1d decodes on the CPU into NV12. On unless switched off
 *   ("av1_software" in settings.json, passed on to the media layer and the
 *   Jellyfin profile as AV1_SOFTWARE_ENV) and limited to 4K 8-bit: dav1d
 *   reached 1.3 to 2.3 times real time for 3840x1920 at 60 fps on the Frame,
 *   but only 0.8 times for 8192x4096 (docs/hardware-investigation.md).
 * - DECODED_VIDEO_HARDWARE, PCVR on a desktop GPU: FFmpeg decodes H.264,
 *   HEVC and AV1 with CUDA, Vulkan Video or VA-API (whichever the GPU
 *   offers, see decoded_video_open) and each frame is downloaded into a
 *   buffer, NV12 or for 10-bit streams P010. */
#define AV1_SOFTWARE_ENV "STEREONAUT_AV1"
#define AV1_SOFTWARE_MAX_WIDTH 4096
#define AV1_SOFTWARE_MAX_HEIGHT 2160
#define DECODED_VIDEO_BUFFERS 8U

/* The Steam Frame's Iris decoder; without it the session plays on a PC. */
#define DECODED_VIDEO_IRIS_DEVICE "/dev/video-dec0"

enum decoded_video_decoder {
    DECODED_VIDEO_DAV1D,
    DECODED_VIDEO_HARDWARE,
};

struct media_pipeline;
struct decoded_video;

struct decoded_video_layout {
    uint32_t width, height; /* visible size; the buffers have no padding */
    uint32_t stride;
    bool p010;              /* 16-bit samples instead of NV12 */
    /* fds are memfds (renderer copies from host memory, PC) rather than
     * udmabuf DMA-BUFs (sampled in place, Frame). */
    bool host_memory;
    uint64_t uv_offset;
    uint64_t size;          /* bytes of every buffer */
    unsigned count;
    int fds[DECODED_VIDEO_BUFFERS];  /* DMA-BUFs or memfds, see above */
    const uint8_t *mappings[DECODED_VIDEO_BUFFERS]; /* CPU view of each */
};

/* Starts decoding the pipeline's video packets in a worker thread. Frames
 * before start_us (seek preroll) are decoded but not delivered. The
 * hardware decoder is the first that works of CUDA, Vulkan Video and
 * VA-API; STEREONAUT_HWDEC=cuda|vulkan|vaapi picks one. */
int decoded_video_open(struct decoded_video **result,
                       struct media_pipeline *pipeline, int64_t start_us,
                       enum decoded_video_decoder decoder, char *error,
                       size_t error_size);
/* "dav1d", "vulkan", "cuda" or "vaapi". */
const char *decoded_video_decoder_name(const struct decoded_video *video);
const struct decoded_video_layout *decoded_video_layout(
    const struct decoded_video *video);
/* The next frame in presentation order: 1 with its buffer and time, 0 if
 * none arrived within timeout_ms, -1 at the end of the stream or on failure
 * (then `error` is set). The buffer belongs to the caller until released. */
int decoded_video_next(struct decoded_video *video, unsigned *index,
                       int64_t *pts_us, int timeout_ms, char *error,
                       size_t error_size);
void decoded_video_release(struct decoded_video *video, unsigned index);
/* Seek: decoded_video_stop ends decoding and drops decoded frames (the
 * buffers the caller holds stay taken); after the pipeline has seeked,
 * decoded_video_restart decodes again from its new position. */
void decoded_video_stop(struct decoded_video *video);
void decoded_video_restart(struct decoded_video *video, int64_t start_us);
/* Frames decoded but dropped before start_us. */
uint64_t decoded_video_preroll_drops(struct decoded_video *video);
void decoded_video_close(struct decoded_video **video);

#endif
