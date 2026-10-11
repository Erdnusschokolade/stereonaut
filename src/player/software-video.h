#ifndef STEREONAUT_SOFTWARE_VIDEO_H
#define STEREONAUT_SOFTWARE_VIDEO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Experimental AV1 playback without the hardware decoder (Iris on the
 * shipped kernel has no AV1): dav1d, through libavcodec, decodes on the CPU
 * into linear NV12 buffers that the renderer imports like Iris CAPTURE
 * buffers (memfd-backed udmabuf DMA-BUFs). Opt-in ("av1_software" in
 * settings.json, passed on to the media layer and the Jellyfin profile as
 * SOFTWARE_VIDEO_ENV) and limited to 4K 8-bit: dav1d reached 1.3 to 2.3
 * times real time for 3840x1920 at 60 fps on the Frame, but only 0.8 times
 * for 8192x4096 (docs/hardware-investigation.md). */
#define SOFTWARE_VIDEO_ENV "STEREONAUT_AV1"
#define SOFTWARE_VIDEO_MAX_WIDTH 4096
#define SOFTWARE_VIDEO_MAX_HEIGHT 2160
#define SOFTWARE_VIDEO_BUFFERS 8U

struct media_pipeline;
struct software_video;

struct software_video_layout {
    uint32_t width, height; /* visible size; the buffers have no padding */
    uint32_t stride;
    uint64_t uv_offset;
    uint64_t size;          /* bytes of every buffer */
    unsigned count;
    int fds[SOFTWARE_VIDEO_BUFFERS];               /* DMA-BUFs */
    const uint8_t *mappings[SOFTWARE_VIDEO_BUFFERS]; /* CPU view of each */
};

/* Starts decoding the pipeline's video packets in a worker thread. Frames
 * before start_us (seek preroll) are decoded but not delivered. */
int software_video_open(struct software_video **result,
                        struct media_pipeline *pipeline, int64_t start_us,
                        char *error, size_t error_size);
const struct software_video_layout *software_video_layout(
    const struct software_video *video);
/* The next frame in presentation order: 1 with its buffer and time, 0 if
 * none arrived within timeout_ms, -1 at the end of the stream or on failure
 * (then `error` is set). The buffer belongs to the caller until released. */
int software_video_next(struct software_video *video, unsigned *index,
                        int64_t *pts_us, int timeout_ms, char *error,
                        size_t error_size);
void software_video_release(struct software_video *video, unsigned index);
/* Frames decoded but dropped before start_us. */
uint64_t software_video_preroll_drops(struct software_video *video);
void software_video_close(struct software_video **video);

#endif
