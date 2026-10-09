#ifndef STEREONAUT_MEDIA_PIPELINE_H
#define STEREONAUT_MEDIA_PIPELINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "media-demux.h"

struct media_pipeline;

int media_pipeline_open(struct media_pipeline **result, const char *path,
                        int64_t start_us, bool enable_audio_device,
                        char *error, size_t error_size);
const struct media_stream_info *media_pipeline_info(
    const struct media_pipeline *pipeline);
int media_pipeline_pop_video(struct media_pipeline *pipeline,
                             struct media_packet *packet, int timeout_ms,
                             char *error, size_t error_size);
int64_t media_pipeline_clock_us(struct media_pipeline *pipeline);
int media_pipeline_pause(struct media_pipeline *pipeline, bool paused,
                         char *error, size_t error_size);
void media_pipeline_close(struct media_pipeline **pipeline);

#endif
