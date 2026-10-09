#define _POSIX_C_SOURCE 200809L
#include "audio-output.h"

#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

#define OUTPUT_RATE 48000
#define OUTPUT_CHANNELS 2
#define OUTPUT_LATENCY_US 100000

struct audio_output {
    AVCodecContext *decoder;
    SwrContext *resampler;
    AVFrame *frame;
    snd_pcm_t *pcm;
    bool device_enabled;
    bool hardware_pause;
    bool paused;
    int64_t base_pts_us;
    int64_t reset_target_us;
    bool base_valid;
    uint64_t decoded_frames;
    uint64_t output_samples;
    uint64_t recoveries;
};

static void set_error(char *error, size_t size, const char *format, ...)
{
    if (!error || !size)
        return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, size, format, arguments);
    va_end(arguments);
}

static void set_av_error(char *error, size_t size, const char *operation,
                         int result)
{
    char detail[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(result, detail, sizeof(detail));
    set_error(error, size, "%s: %s", operation, detail);
}

static int initialize_resampler(struct audio_output *audio,
                                char *error, size_t error_size)
{
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    int result = swr_alloc_set_opts2(
        &audio->resampler, &stereo, AV_SAMPLE_FMT_S16, OUTPUT_RATE,
        &audio->decoder->ch_layout, audio->decoder->sample_fmt,
        audio->decoder->sample_rate, 0, NULL);
    if (result < 0) {
        set_av_error(error, error_size, "swr_alloc_set_opts2", result);
        return -1;
    }
    result = swr_init(audio->resampler);
    if (result < 0) {
        set_av_error(error, error_size, "swr_init", result);
        return -1;
    }
    return 0;
}

static int initialize_pcm(struct audio_output *audio,
                          char *error, size_t error_size)
{
    int result = snd_pcm_open(&audio->pcm, "default",
                              SND_PCM_STREAM_PLAYBACK, 0);
    if (result < 0) {
        set_error(error, error_size, "snd_pcm_open(default): %s",
                  snd_strerror(result));
        return -1;
    }
    result = snd_pcm_set_params(audio->pcm, SND_PCM_FORMAT_S16_LE,
                                SND_PCM_ACCESS_RW_INTERLEAVED,
                                OUTPUT_CHANNELS, OUTPUT_RATE, 1,
                                OUTPUT_LATENCY_US);
    if (result < 0) {
        set_error(error, error_size, "snd_pcm_set_params: %s",
                  snd_strerror(result));
        return -1;
    }
    snd_pcm_hw_params_t *parameters = NULL;
    result = snd_pcm_hw_params_malloc(&parameters);
    if (result < 0) {
        set_error(error, error_size, "snd_pcm_hw_params_malloc: %s",
                  snd_strerror(result));
        return -1;
    }
    result = snd_pcm_hw_params_current(audio->pcm, parameters);
    if (result < 0) {
        set_error(error, error_size, "snd_pcm_hw_params_current: %s",
                  snd_strerror(result));
        snd_pcm_hw_params_free(parameters);
        return -1;
    }
    audio->hardware_pause = snd_pcm_hw_params_can_pause(parameters) != 0;
    snd_pcm_hw_params_free(parameters);
    return 0;
}

int audio_output_open(struct audio_output **result,
                      const AVCodecParameters *parameters,
                      AVRational packet_time_base, bool enable_device,
                      char *error, size_t error_size)
{
    if (!result || !parameters) {
        set_error(error, error_size, "invalid audio_output_open arguments");
        return -1;
    }
    *result = NULL;
    const AVCodec *codec = avcodec_find_decoder(parameters->codec_id);
    if (!codec) {
        set_error(error, error_size, "unsupported audio codec: %s",
                  avcodec_get_name(parameters->codec_id));
        return -1;
    }
    struct audio_output *audio = calloc(1, sizeof(*audio));
    if (!audio) {
        set_error(error, error_size, "allocate audio output: %s",
                  strerror(errno));
        return -1;
    }
    audio->base_pts_us = AV_NOPTS_VALUE;
    audio->decoder = avcodec_alloc_context3(codec);
    audio->frame = av_frame_alloc();
    if (!audio->decoder || !audio->frame) {
        set_error(error, error_size, "allocate audio decoder failed");
        audio_output_close(&audio);
        return -1;
    }
    int av_result = avcodec_parameters_to_context(audio->decoder, parameters);
    if (av_result < 0) {
        set_av_error(error, error_size, "avcodec_parameters_to_context",
                     av_result);
        audio_output_close(&audio);
        return -1;
    }
    audio->decoder->pkt_timebase = packet_time_base;
    av_result = avcodec_open2(audio->decoder, codec, NULL);
    if (av_result < 0) {
        set_av_error(error, error_size, "avcodec_open2(audio)", av_result);
        audio_output_close(&audio);
        return -1;
    }
    if (initialize_resampler(audio, error, error_size) < 0) {
        audio_output_close(&audio);
        return -1;
    }
    audio->device_enabled = enable_device;
    if (enable_device && initialize_pcm(audio, error, error_size) < 0) {
        audio_output_close(&audio);
        return -1;
    }
    *result = audio;
    return 0;
}

static int write_samples(struct audio_output *audio, const uint8_t *data,
                         int samples, char *error, size_t error_size)
{
    if (!audio->device_enabled) {
        audio->output_samples += (uint64_t)samples;
        return 0;
    }
    int remaining = samples;
    const int16_t *cursor = (const int16_t *)data;
    while (remaining > 0) {
        snd_pcm_sframes_t written = snd_pcm_writei(audio->pcm, cursor,
                                                   (snd_pcm_uframes_t)remaining);
        if (written == -EINTR)
            continue;
        if (written < 0) {
            int recovered = snd_pcm_recover(audio->pcm, (int)written, 1);
            if (recovered < 0) {
                set_error(error, error_size, "snd_pcm_writei: %s",
                          snd_strerror((int)written));
                return -1;
            }
            ++audio->recoveries;
            continue;
        }
        cursor += written * OUTPUT_CHANNELS;
        remaining -= (int)written;
        audio->output_samples += (uint64_t)written;
    }
    return 0;
}

static int consume_frames(struct audio_output *audio,
                          char *error, size_t error_size)
{
    for (;;) {
        int result = avcodec_receive_frame(audio->decoder, audio->frame);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
            return 0;
        if (result < 0) {
            set_av_error(error, error_size, "avcodec_receive_frame", result);
            return -1;
        }
        if (!audio->base_valid) {
            int64_t timestamp = audio->frame->best_effort_timestamp;
            audio->base_pts_us = timestamp == AV_NOPTS_VALUE
                ? audio->reset_target_us
                : av_rescale_q(timestamp, audio->decoder->pkt_timebase,
                               AV_TIME_BASE_Q);
            audio->base_valid = true;
        }
        int output_capacity = (int)av_rescale_rnd(
            swr_get_delay(audio->resampler, audio->decoder->sample_rate) +
                audio->frame->nb_samples,
            OUTPUT_RATE, audio->decoder->sample_rate, AV_ROUND_UP);
        uint8_t *output = NULL;
        int line_size = 0;
        result = av_samples_alloc(&output, &line_size, OUTPUT_CHANNELS,
                                  output_capacity, AV_SAMPLE_FMT_S16, 0);
        if (result < 0) {
            set_av_error(error, error_size, "av_samples_alloc", result);
            av_frame_unref(audio->frame);
            return -1;
        }
        uint8_t *outputs[] = {output};
        result = swr_convert(audio->resampler, outputs, output_capacity,
                             (const uint8_t *const *)audio->frame->extended_data,
                             audio->frame->nb_samples);
        av_frame_unref(audio->frame);
        if (result < 0) {
            set_av_error(error, error_size, "swr_convert", result);
            av_freep(&output);
            return -1;
        }
        ++audio->decoded_frames;
        int write_result = write_samples(audio, output, result,
                                         error, error_size);
        av_freep(&output);
        if (write_result < 0)
            return -1;
    }
}

int audio_output_push(struct audio_output *audio, const AVPacket *packet,
                      char *error, size_t error_size)
{
    if (!audio || !packet) {
        set_error(error, error_size, "invalid audio packet");
        return -1;
    }
    int result = avcodec_send_packet(audio->decoder, packet);
    if (result < 0) {
        set_av_error(error, error_size, "avcodec_send_packet(audio)", result);
        return -1;
    }
    return consume_frames(audio, error, error_size);
}

int audio_output_finish(struct audio_output *audio,
                        char *error, size_t error_size)
{
    if (!audio)
        return 0;
    int result = avcodec_send_packet(audio->decoder, NULL);
    if (result < 0 && result != AVERROR_EOF) {
        set_av_error(error, error_size, "avcodec_send_packet(audio eof)",
                     result);
        return -1;
    }
    if (consume_frames(audio, error, error_size) < 0)
        return -1;
    if (audio->device_enabled) {
        result = snd_pcm_drain(audio->pcm);
        if (result < 0) {
            set_error(error, error_size, "snd_pcm_drain: %s",
                      snd_strerror(result));
            return -1;
        }
    }
    return 0;
}

int audio_output_pause(struct audio_output *audio, bool paused,
                       char *error, size_t error_size)
{
    if (!audio || !audio->device_enabled) {
        if (audio)
            audio->paused = paused;
        return 0;
    }
    if (!audio->hardware_pause) {
        set_error(error, error_size,
                  "ALSA default device does not support hardware pause");
        return -1;
    }
    int result = snd_pcm_pause(audio->pcm, paused ? 1 : 0);
    if (result < 0) {
        set_error(error, error_size, "snd_pcm_pause: %s",
                  snd_strerror(result));
        return -1;
    }
    audio->paused = paused;
    return 0;
}

int audio_output_reset(struct audio_output *audio, int64_t target_us,
                       char *error, size_t error_size)
{
    if (!audio)
        return -1;
    if (audio->device_enabled) {
        int result = snd_pcm_drop(audio->pcm);
        if (result < 0) {
            set_error(error, error_size, "snd_pcm_drop: %s",
                      snd_strerror(result));
            return -1;
        }
        result = snd_pcm_prepare(audio->pcm);
        if (result < 0) {
            set_error(error, error_size, "snd_pcm_prepare: %s",
                      snd_strerror(result));
            return -1;
        }
    }
    avcodec_flush_buffers(audio->decoder);
    swr_close(audio->resampler);
    if (swr_init(audio->resampler) < 0) {
        set_error(error, error_size, "swr_init after reset failed");
        return -1;
    }
    audio->base_valid = false;
    audio->base_pts_us = AV_NOPTS_VALUE;
    audio->reset_target_us = target_us;
    audio->output_samples = 0;
    audio->paused = false;
    return 0;
}

int64_t audio_output_clock_us(struct audio_output *audio)
{
    if (!audio || !audio->base_valid)
        return AV_NOPTS_VALUE;
    snd_pcm_sframes_t delay = 0;
    if (audio->device_enabled && snd_pcm_delay(audio->pcm, &delay) < 0)
        delay = 0;
    int64_t played = (int64_t)audio->output_samples - delay;
    if (played < 0)
        played = 0;
    return audio->base_pts_us +
           av_rescale(played, AV_TIME_BASE, OUTPUT_RATE);
}

void audio_output_get_stats(struct audio_output *audio,
                            struct audio_output_stats *stats)
{
    if (!stats)
        return;
    memset(stats, 0, sizeof(*stats));
    if (!audio)
        return;
    stats->decoded_frames = audio->decoded_frames;
    stats->output_samples = audio->output_samples;
    stats->recoveries = audio->recoveries;
    stats->clock_us = audio_output_clock_us(audio);
    stats->hardware_pause = audio->hardware_pause;
    if (audio->device_enabled) {
        snd_pcm_sframes_t delay = 0;
        if (snd_pcm_delay(audio->pcm, &delay) >= 0)
            stats->device_delay_samples = delay;
    }
}

void audio_output_close(struct audio_output **pointer)
{
    if (!pointer || !*pointer)
        return;
    struct audio_output *audio = *pointer;
    if (audio->pcm)
        snd_pcm_close(audio->pcm);
    swr_free(&audio->resampler);
    av_frame_free(&audio->frame);
    avcodec_free_context(&audio->decoder);
    free(audio);
    *pointer = NULL;
}
