#include "media-demux.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/bsf.h>
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>

struct media_demux {
    AVFormatContext *format;
    AVBSFContext *video_bsf;
    AVPacket *input;
    AVPacket *filtered;
    struct media_stream_info info;
    bool input_eof;
    bool bsf_eof_sent;
};

static void set_error(char *error, size_t size, const char *format, ...)
{
    if (!error || size == 0)
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

static int64_t packet_time_us(int64_t value, AVRational time_base)
{
    if (value == AV_NOPTS_VALUE)
        return AV_NOPTS_VALUE;
    return av_rescale_q(value, time_base, AV_TIME_BASE_Q);
}

static bool is_main_8_bit(const AVCodecParameters *parameters)
{
    if (parameters->codec_id != AV_CODEC_ID_HEVC ||
        parameters->profile != AV_PROFILE_HEVC_MAIN)
        return false;
    if (parameters->format != AV_PIX_FMT_NONE &&
        parameters->format != AV_PIX_FMT_YUV420P)
        return false;
    return parameters->bits_per_raw_sample <= 8;
}

static int select_audio_stream(AVFormatContext *format)
{
    int best = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1,
                                   NULL, 0);
    if (best >= 0 && avcodec_find_decoder(
                         format->streams[best]->codecpar->codec_id))
        return best;
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        AVCodecParameters *parameters = format->streams[i]->codecpar;
        if (parameters->codec_type == AVMEDIA_TYPE_AUDIO &&
            avcodec_find_decoder(parameters->codec_id))
            return (int)i;
    }
    return -1;
}

static int initialize_video_filter(struct media_demux *demux,
                                   char *error, size_t error_size)
{
    const AVBitStreamFilter *filter =
        av_bsf_get_by_name("hevc_mp4toannexb");
    if (!filter) {
        set_error(error, error_size,
                  "FFmpeg provides no hevc_mp4toannexb filter");
        return -1;
    }
    int result = av_bsf_alloc(filter, &demux->video_bsf);
    if (result < 0) {
        set_av_error(error, error_size, "av_bsf_alloc", result);
        return -1;
    }
    AVStream *stream =
        demux->format->streams[demux->info.video_stream_index];
    result = avcodec_parameters_copy(demux->video_bsf->par_in,
                                     stream->codecpar);
    if (result < 0) {
        set_av_error(error, error_size, "avcodec_parameters_copy", result);
        return -1;
    }
    demux->video_bsf->time_base_in = stream->time_base;
    result = av_bsf_init(demux->video_bsf);
    if (result < 0) {
        set_av_error(error, error_size, "av_bsf_init", result);
        return -1;
    }
    return 0;
}

int media_demux_open(struct media_demux **result, const char *path,
                     char *error, size_t error_size)
{
    if (!result || !path) {
        set_error(error, error_size, "invalid media_demux_open arguments");
        return -1;
    }
    *result = NULL;
    struct media_demux *demux = calloc(1, sizeof(*demux));
    if (!demux) {
        set_error(error, error_size, "allocate demux context: %s",
                  strerror(errno));
        return -1;
    }
    demux->info.video_stream_index = -1;
    demux->info.audio_stream_index = -1;
    int av_result = avformat_open_input(&demux->format, path, NULL, NULL);
    if (av_result < 0) {
        set_av_error(error, error_size, "avformat_open_input", av_result);
        media_demux_close(&demux);
        return -1;
    }
    av_result = avformat_find_stream_info(demux->format, NULL);
    if (av_result < 0) {
        set_av_error(error, error_size, "avformat_find_stream_info",
                     av_result);
        media_demux_close(&demux);
        return -1;
    }
    int video = av_find_best_stream(demux->format, AVMEDIA_TYPE_VIDEO,
                                    -1, -1, NULL, 0);
    if (video < 0) {
        set_error(error, error_size, "container has no video stream");
        media_demux_close(&demux);
        return -1;
    }
    AVCodecParameters *video_parameters =
        demux->format->streams[video]->codecpar;
    if (!is_main_8_bit(video_parameters)) {
        const char *pixel = av_get_pix_fmt_name(video_parameters->format);
        set_error(error, error_size,
                  "unsupported video: codec=%s profile=%d pixel_format=%s",
                  avcodec_get_name(video_parameters->codec_id),
                  video_parameters->profile, pixel ? pixel : "unknown");
        media_demux_close(&demux);
        return -1;
    }
    demux->info.video_stream_index = video;
    demux->info.video_codec = video_parameters->codec_id;
    demux->info.video_profile = video_parameters->profile;
    demux->info.pixel_format = video_parameters->format;
    demux->info.width = video_parameters->width;
    demux->info.height = video_parameters->height;
    AVStream *video_stream = demux->format->streams[video];
    demux->info.frame_rate = av_guess_frame_rate(demux->format,
                                                 video_stream, NULL);

    int audio = select_audio_stream(demux->format);
    if (audio >= 0) {
        AVCodecParameters *audio_parameters =
            demux->format->streams[audio]->codecpar;
        demux->info.audio_stream_index = audio;
        demux->info.audio_codec = audio_parameters->codec_id;
        demux->info.sample_rate = audio_parameters->sample_rate;
        demux->info.audio_time_base = demux->format->streams[audio]->time_base;
        if (av_channel_layout_copy(&demux->info.channel_layout,
                                   &audio_parameters->ch_layout) < 0) {
            set_error(error, error_size, "copy audio channel layout failed");
            media_demux_close(&demux);
            return -1;
        }
    }
    demux->info.start_us = demux->format->start_time == AV_NOPTS_VALUE
                                ? 0 : demux->format->start_time;
    demux->info.duration_us = demux->format->duration;
    if (initialize_video_filter(demux, error, error_size) < 0) {
        media_demux_close(&demux);
        return -1;
    }
    demux->input = av_packet_alloc();
    demux->filtered = av_packet_alloc();
    if (!demux->input || !demux->filtered) {
        set_error(error, error_size, "allocate FFmpeg packets failed");
        media_demux_close(&demux);
        return -1;
    }
    *result = demux;
    return 0;
}

const struct media_stream_info *media_demux_info(const struct media_demux *demux)
{
    return demux ? &demux->info : NULL;
}

int media_demux_copy_audio_parameters(const struct media_demux *demux,
                                      AVCodecParameters *parameters)
{
    if (!demux || !parameters || demux->info.audio_stream_index < 0)
        return AVERROR(EINVAL);
    return avcodec_parameters_copy(
        parameters,
        demux->format->streams[demux->info.audio_stream_index]->codecpar);
}

static int return_packet(struct media_packet *output, AVPacket *packet,
                         enum media_packet_kind kind, AVRational time_base,
                         char *error, size_t error_size)
{
    output->packet = av_packet_alloc();
    if (!output->packet) {
        set_error(error, error_size, "allocate output packet failed");
        return -1;
    }
    av_packet_move_ref(output->packet, packet);
    output->kind = kind;
    output->pts_us = packet_time_us(output->packet->pts, time_base);
    output->dts_us = packet_time_us(output->packet->dts, time_base);
    output->duration_us = output->packet->duration > 0
                              ? av_rescale_q(output->packet->duration,
                                             time_base, AV_TIME_BASE_Q)
                              : 0;
    output->keyframe = (output->packet->flags & AV_PKT_FLAG_KEY) != 0;
    return 1;
}

int media_demux_read(struct media_demux *demux, struct media_packet *packet,
                     char *error, size_t error_size)
{
    if (!demux || !packet) {
        set_error(error, error_size, "invalid media_demux_read arguments");
        return -1;
    }
    memset(packet, 0, sizeof(*packet));
    for (;;) {
        int result = av_bsf_receive_packet(demux->video_bsf,
                                           demux->filtered);
        if (result >= 0)
            return return_packet(packet, demux->filtered,
                                 MEDIA_PACKET_VIDEO,
                                 demux->video_bsf->time_base_out,
                                 error, error_size);
        if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
            set_av_error(error, error_size, "av_bsf_receive_packet", result);
            return -1;
        }
        if (demux->input_eof) {
            if (!demux->bsf_eof_sent) {
                result = av_bsf_send_packet(demux->video_bsf, NULL);
                if (result < 0 && result != AVERROR_EOF) {
                    set_av_error(error, error_size,
                                 "av_bsf_send_packet(eof)", result);
                    return -1;
                }
                demux->bsf_eof_sent = true;
                continue;
            }
            return 0;
        }

        av_packet_unref(demux->input);
        result = av_read_frame(demux->format, demux->input);
        if (result == AVERROR_EOF) {
            demux->input_eof = true;
            continue;
        }
        if (result < 0) {
            set_av_error(error, error_size, "av_read_frame", result);
            return -1;
        }
        if (demux->input->stream_index == demux->info.video_stream_index) {
            result = av_bsf_send_packet(demux->video_bsf, demux->input);
            if (result < 0) {
                set_av_error(error, error_size, "av_bsf_send_packet",
                             result);
                return -1;
            }
            continue;
        }
        if (demux->input->stream_index == demux->info.audio_stream_index) {
            AVStream *audio_stream = demux->format->streams[
                demux->info.audio_stream_index];
            return return_packet(packet, demux->input,
                                 MEDIA_PACKET_AUDIO,
                                 audio_stream->time_base,
                                 error, error_size);
        }
    }
}

int media_demux_seek(struct media_demux *demux, int64_t target_us,
                     char *error, size_t error_size)
{
    if (!demux || target_us < 0) {
        set_error(error, error_size, "invalid seek target");
        return -1;
    }
    AVStream *stream =
        demux->format->streams[demux->info.video_stream_index];
    int64_t target = av_rescale_q(target_us, AV_TIME_BASE_Q,
                                  stream->time_base);
    int result = avformat_seek_file(demux->format,
                                    demux->info.video_stream_index,
                                    INT64_MIN, target, target,
                                    AVSEEK_FLAG_BACKWARD);
    if (result < 0) {
        set_av_error(error, error_size, "avformat_seek_file", result);
        return -1;
    }
    avformat_flush(demux->format);
    av_bsf_flush(demux->video_bsf);
    av_packet_unref(demux->input);
    av_packet_unref(demux->filtered);
    demux->input_eof = false;
    demux->bsf_eof_sent = false;
    return 0;
}

void media_packet_unref(struct media_packet *packet)
{
    if (!packet)
        return;
    av_packet_free(&packet->packet);
    memset(packet, 0, sizeof(*packet));
}

void media_demux_close(struct media_demux **pointer)
{
    if (!pointer || !*pointer)
        return;
    struct media_demux *demux = *pointer;
    av_packet_free(&demux->input);
    av_packet_free(&demux->filtered);
    av_bsf_free(&demux->video_bsf);
    avformat_close_input(&demux->format);
    av_channel_layout_uninit(&demux->info.channel_layout);
    free(demux);
    *pointer = NULL;
}
