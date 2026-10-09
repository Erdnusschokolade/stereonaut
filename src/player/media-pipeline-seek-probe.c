#define _POSIX_C_SOURCE 200809L
#include "media-pipeline.h"

#include <inttypes.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s CONTAINER TARGET_SECONDS\n", argv[0]);
        return 2;
    }
    char *end = NULL;
    double target_seconds = strtod(argv[2], &end);
    if (!end || *end || target_seconds < 0.0) {
        fprintf(stderr, "invalid seek target\n");
        return 2;
    }
    int64_t target_us = (int64_t)(target_seconds * 1000000.0);
    char error[512] = {0};
    struct media_pipeline *pipeline = NULL;
    if (media_pipeline_open(&pipeline, argv[1], 0, false, true,
                            error, sizeof(error)) < 0) {
        fprintf(stderr, "pipeline_error=%s\n", error);
        return 1;
    }
    struct media_packet before = {0};
    if (media_pipeline_pop_video(pipeline, &before, 2000,
                                 error, sizeof(error)) != 1) {
        fprintf(stderr, "initial_packet_error=%s\n", error);
        media_pipeline_close(&pipeline);
        return 1;
    }
    int64_t before_pts = before.pts_us;
    media_packet_unref(&before);
    if (media_pipeline_seek(pipeline, target_us, true,
                            error, sizeof(error)) < 0) {
        fprintf(stderr, "seek_error=%s\n", error);
        media_pipeline_close(&pipeline);
        return 1;
    }
    struct media_packet after = {0};
    if (media_pipeline_pop_video(pipeline, &after, 2000,
                                 error, sizeof(error)) != 1) {
        fprintf(stderr, "post_seek_packet_error=%s\n", error);
        media_pipeline_close(&pipeline);
        return 1;
    }
    printf("pipeline_seek before_pts_us=%" PRId64
           " target_us=%" PRId64 " first_pts_us=%" PRId64
           " first_dts_us=%" PRId64 " keyframe=%u\n",
           before_pts, target_us, after.pts_us, after.dts_us, after.keyframe);
    bool success = after.keyframe &&
                   (after.pts_us == AV_NOPTS_VALUE || after.pts_us <= target_us);
    media_packet_unref(&after);
    media_pipeline_close(&pipeline);
    return success ? 0 : 1;
}
