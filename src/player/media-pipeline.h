#ifndef STEREONAUT_MEDIA_PIPELINE_H
#define STEREONAUT_MEDIA_PIPELINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "media-demux.h"

struct media_pipeline;

/* start_us and from_us as for media_pipeline_seek. */
int media_pipeline_open(struct media_pipeline **result, const char *path,
                        int64_t start_us, int64_t from_us,
                        bool enable_audio_device,
                        bool start_paused,
                        char *error, size_t error_size);
const struct media_stream_info *media_pipeline_info(
    const struct media_pipeline *pipeline);
int media_pipeline_copy_video_parameters(
    const struct media_pipeline *pipeline, AVCodecParameters *parameters);
int media_pipeline_pop_video(struct media_pipeline *pipeline,
                             struct media_packet *packet, int timeout_ms,
                             char *error, size_t error_size);
int64_t media_pipeline_clock_us(struct media_pipeline *pipeline);
int media_pipeline_pause(struct media_pipeline *pipeline, bool paused,
                         char *error, size_t error_size);
/* Where playback starts after media_pipeline_open or the last seek: the
 * keyframe nearest to the requested time (see media_demux_seek). */
int64_t media_pipeline_start_us(const struct media_pipeline *pipeline);
/* from_us is the position the seek starts from (negative if none). */
int media_pipeline_seek(struct media_pipeline *pipeline, int64_t target_us,
                        int64_t from_us, bool paused, char *error,
                        size_t error_size);
/* Output volume (linear gain 0 to 1.5); no effect without audio. */
void media_pipeline_set_volume(struct media_pipeline *pipeline, float volume);
void media_pipeline_close(struct media_pipeline **pipeline);

#endif
