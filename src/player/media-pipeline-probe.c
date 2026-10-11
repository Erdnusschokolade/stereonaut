#define _POSIX_C_SOURCE 200809L
#include "media-pipeline.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <libavutil/avutil.h>

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s CONTAINER [--audio]\n", argv[0]);
        return 2;
    }
    bool audio_device = argc == 3 && strcmp(argv[2], "--audio") == 0;
    char error[512] = {0};
    struct media_pipeline *pipeline = NULL;
    if (media_pipeline_open(&pipeline, argv[1], 0, -1, audio_device, false,
                            error, sizeof(error)) < 0) {
        fprintf(stderr, "pipeline_error=%s\n", error);
        return 1;
    }
    uint64_t packets = 0;
    int64_t first_pts = AV_NOPTS_VALUE;
    bool paused_once = false;
    while (true) {
        struct media_packet packet;
        int result = media_pipeline_pop_video(pipeline, &packet, 100,
                                              error, sizeof(error));
        if (result < 0)
            break;
        if (result > 0) {
            if (first_pts == AV_NOPTS_VALUE)
                first_pts = packet.pts_us;
            ++packets;
            media_packet_unref(&packet);
        }
        int64_t clock = media_pipeline_clock_us(pipeline);
        if (audio_device && !paused_once && clock >= 1000000) {
            if (media_pipeline_pause(pipeline, true,
                                     error, sizeof(error)) < 0) {
                fprintf(stderr, "pause_error=%s\n", error);
                media_pipeline_close(&pipeline);
                return 1;
            }
            struct timespec delay = {.tv_nsec = 250000000};
            nanosleep(&delay, NULL);
            if (media_pipeline_pause(pipeline, false,
                                     error, sizeof(error)) < 0) {
                fprintf(stderr, "resume_error=%s\n", error);
                media_pipeline_close(&pipeline);
                return 1;
            }
            paused_once = true;
        }
        if (clock >= 3000000)
            break;
    }
    int64_t clock = media_pipeline_clock_us(pipeline);
    printf("pipeline video_packets=%" PRIu64 " first_pts_us=%" PRId64
           " audio_clock_us=%" PRId64 " audio_device=%u pause_cycle=%u\n",
           packets, first_pts, clock, audio_device, paused_once);
    media_pipeline_close(&pipeline);
    return packets > 0 && clock >= 3000000 ? 0 : 1;
}
