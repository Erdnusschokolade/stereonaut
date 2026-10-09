#ifndef STEREONAUT_AUDIO_OUTPUT_H
#define STEREONAUT_AUDIO_OUTPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libavcodec/avcodec.h>

struct audio_output;

struct audio_output_stats {
    uint64_t decoded_frames;
    uint64_t output_samples;
    uint64_t recoveries;
    int64_t clock_us;
    int64_t device_delay_samples;
    bool hardware_pause;
};

int audio_output_open(struct audio_output **result,
                      const AVCodecParameters *parameters,
                      AVRational packet_time_base, bool enable_device,
                      char *error, size_t error_size);
int audio_output_push(struct audio_output *audio, const AVPacket *packet,
                      char *error, size_t error_size);
int audio_output_finish(struct audio_output *audio,
                        char *error, size_t error_size);
int audio_output_pause(struct audio_output *audio, bool paused,
                       char *error, size_t error_size);
int audio_output_reset(struct audio_output *audio, int64_t target_us,
                       char *error, size_t error_size);
int64_t audio_output_clock_us(struct audio_output *audio);
void audio_output_get_stats(struct audio_output *audio,
                            struct audio_output_stats *stats);
void audio_output_close(struct audio_output **audio);

#endif
