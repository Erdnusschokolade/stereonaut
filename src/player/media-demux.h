#ifndef STEREONAUT_MEDIA_DEMUX_H
#define STEREONAUT_MEDIA_DEMUX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

enum media_packet_kind {
    MEDIA_PACKET_VIDEO,
    MEDIA_PACKET_AUDIO,
};

struct media_stream_info {
    int video_stream_index;
    int audio_stream_index;
    enum AVCodecID video_codec;
    enum AVCodecID audio_codec;
    int video_profile;
    enum AVPixelFormat pixel_format;
    int width;
    int height;
    AVRational frame_rate;
    int sample_rate;
    AVChannelLayout channel_layout;
    int64_t start_us;
    int64_t duration_us;
};

struct media_packet {
    enum media_packet_kind kind;
    AVPacket *packet;
    int64_t pts_us;
    int64_t dts_us;
    int64_t duration_us;
    bool keyframe;
};

struct media_demux;

int media_demux_open(struct media_demux **result, const char *path,
                     char *error, size_t error_size);
const struct media_stream_info *media_demux_info(const struct media_demux *demux);
int media_demux_read(struct media_demux *demux, struct media_packet *packet,
                     char *error, size_t error_size);
int media_demux_seek(struct media_demux *demux, int64_t target_us,
                     char *error, size_t error_size);
void media_packet_unref(struct media_packet *packet);
void media_demux_close(struct media_demux **demux);

#endif
