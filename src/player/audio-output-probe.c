#define _POSIX_C_SOURCE 200809L
#include "audio-output.h"
#include "media-demux.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr,
                "usage: %s CONTAINER [--decode-only|--seek-test]\n", argv[0]);
        return 2;
    }
    bool device = argc == 2;
    bool seek_test = argc == 3 && strcmp(argv[2], "--seek-test") == 0;
    if (argc == 3 && strcmp(argv[2], "--decode-only") != 0 && !seek_test) {
        fprintf(stderr, "unknown option: %s\n", argv[2]);
        return 2;
    }
    char error[512] = {0};
    struct media_demux *demux = NULL;
    if (media_demux_open(&demux, argv[1], error, sizeof(error)) < 0) {
        fprintf(stderr, "demux_error=%s\n", error);
        return 1;
    }
    const struct media_stream_info *info = media_demux_info(demux);
    if (info->audio_stream_index < 0) {
        fprintf(stderr, "audio_error=no audio stream\n");
        media_demux_close(&demux);
        return 1;
    }
    AVCodecParameters *parameters = avcodec_parameters_alloc();
    if (!parameters || media_demux_copy_audio_parameters(
                           demux, parameters) < 0) {
        fprintf(stderr, "audio_error=copy codec parameters\n");
        avcodec_parameters_free(&parameters);
        media_demux_close(&demux);
        return 1;
    }
    struct audio_output *audio = NULL;
    if (audio_output_open(&audio, parameters, info->audio_time_base,
                          device, error, sizeof(error)) < 0) {
        fprintf(stderr, "audio_error=%s\n", error);
        avcodec_parameters_free(&parameters);
        media_demux_close(&demux);
        return 1;
    }
    avcodec_parameters_free(&parameters);
    int64_t seek_target_us = INT64_C(10000000);
    if (seek_test &&
        (media_demux_seek(demux, seek_target_us,
                          error, sizeof(error)) < 0 ||
         audio_output_reset(audio, seek_target_us,
                            error, sizeof(error)) < 0)) {
        fprintf(stderr, "seek_error=%s\n", error);
        audio_output_close(&audio);
        media_demux_close(&demux);
        return 1;
    }

    bool paused_once = false;
    while (true) {
        struct media_packet packet;
        int result = media_demux_read(demux, &packet, error, sizeof(error));
        if (result < 0) {
            fprintf(stderr, "demux_error=%s\n", error);
            audio_output_close(&audio);
            media_demux_close(&demux);
            return 1;
        }
        if (result == 0)
            break;
        if (packet.kind == MEDIA_PACKET_AUDIO &&
            audio_output_push(audio, packet.packet,
                              error, sizeof(error)) < 0) {
            fprintf(stderr, "audio_error=%s\n", error);
            media_packet_unref(&packet);
            audio_output_close(&audio);
            media_demux_close(&demux);
            return 1;
        }
        media_packet_unref(&packet);
        int64_t clock = audio_output_clock_us(audio);
        if (device && !paused_once && clock >= 1000000) {
            if (audio_output_pause(audio, true, error, sizeof(error)) < 0) {
                fprintf(stderr, "pause_error=%s\n", error);
                audio_output_close(&audio);
                media_demux_close(&demux);
                return 1;
            }
            struct timespec pause = {.tv_nsec = 250000000};
            nanosleep(&pause, NULL);
            if (audio_output_pause(audio, false, error, sizeof(error)) < 0) {
                fprintf(stderr, "resume_error=%s\n", error);
                audio_output_close(&audio);
                media_demux_close(&demux);
                return 1;
            }
            paused_once = true;
        }
        int64_t stop_clock = seek_test
                                 ? seek_target_us + INT64_C(1000000)
                                 : INT64_C(3000000);
        if (clock >= stop_clock)
            break;
    }
    struct audio_output_stats stats;
    audio_output_get_stats(audio, &stats);
    printf("audio codec=%s output=%s decoded_frames=%" PRIu64
           " samples=%" PRIu64 " clock_us=%" PRId64
           " delay_samples=%" PRId64 " recoveries=%" PRIu64
           " hardware_pause=%u pause_cycle=%u seek_target_us=%" PRId64 "\n",
           avcodec_get_name(info->audio_codec),
           device ? "alsa-default" : "decode-only",
           stats.decoded_frames, stats.output_samples, stats.clock_us,
           stats.device_delay_samples, stats.recoveries,
           stats.hardware_pause, paused_once,
           seek_test ? seek_target_us : INT64_C(0));
    bool success = !seek_test ||
                   (stats.clock_us >= seek_target_us &&
                    stats.clock_us < seek_target_us + INT64_C(2000000));
    audio_output_close(&audio);
    media_demux_close(&demux);
    return success ? 0 : 1;
}
