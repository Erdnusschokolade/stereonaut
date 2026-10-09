#include "media-demux.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>

static bool annex_b_start(const AVPacket *packet)
{
    return packet->size >= 4 && packet->data[0] == 0 &&
           packet->data[1] == 0 &&
           (packet->data[2] == 1 ||
            (packet->data[2] == 0 && packet->data[3] == 1));
}

static int64_t parse_seconds(const char *text)
{
    char *end = NULL;
    double seconds = strtod(text, &end);
    if (!end || *end || seconds < 0.0)
        return -1;
    return (int64_t)(seconds * 1000000.0 + 0.5);
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s CONTAINER [SEEK_SECONDS]\n", argv[0]);
        return 2;
    }
    char error[512] = {0};
    struct media_demux *demux = NULL;
    if (media_demux_open(&demux, argv[1], error, sizeof(error)) < 0) {
        fprintf(stderr, "open_error=%s\n", error);
        return 1;
    }
    const struct media_stream_info *info = media_demux_info(demux);
    char layout[128] = "none";
    if (info->audio_stream_index >= 0)
        av_channel_layout_describe(&info->channel_layout,
                                   layout, sizeof(layout));
    printf("media video_stream=%d codec=%s profile=%d format=%s"
           " dimensions=%dx%d fps=%d/%d audio_stream=%d audio_codec=%s"
           " sample_rate=%d channels=%s start_us=%" PRId64
           " duration_us=%" PRId64 "\n",
           info->video_stream_index, avcodec_get_name(info->video_codec),
           info->video_profile, av_get_pix_fmt_name(info->pixel_format),
           info->width, info->height, info->frame_rate.num,
           info->frame_rate.den, info->audio_stream_index,
           info->audio_stream_index >= 0
               ? avcodec_get_name(info->audio_codec) : "none",
           info->sample_rate, layout, info->start_us, info->duration_us);

    if (argc == 3) {
        int64_t target = parse_seconds(argv[2]);
        if (target < 0 ||
            media_demux_seek(demux, target, error, sizeof(error)) < 0) {
            fprintf(stderr, "seek_error=%s\n", error);
            media_demux_close(&demux);
            return 1;
        }
        printf("seek target_us=%" PRId64 "\n", target);
    }

    unsigned video_count = 0, audio_count = 0, keyframes = 0;
    int64_t first_video_pts = AV_NOPTS_VALUE;
    int64_t first_key_pts = AV_NOPTS_VALUE;
    int64_t last_video_dts = AV_NOPTS_VALUE;
    bool dts_monotone = true;
    while (video_count < 180 || audio_count < 180) {
        struct media_packet packet;
        int result = media_demux_read(demux, &packet, error, sizeof(error));
        if (result < 0) {
            fprintf(stderr, "read_error=%s\n", error);
            media_demux_close(&demux);
            return 1;
        }
        if (result == 0)
            break;
        if (packet.kind == MEDIA_PACKET_VIDEO) {
            if (!annex_b_start(packet.packet)) {
                fprintf(stderr, "video packet is not Annex-B\n");
                media_packet_unref(&packet);
                media_demux_close(&demux);
                return 1;
            }
            if (first_video_pts == AV_NOPTS_VALUE)
                first_video_pts = packet.pts_us;
            if (packet.keyframe) {
                ++keyframes;
                if (first_key_pts == AV_NOPTS_VALUE)
                    first_key_pts = packet.pts_us;
            }
            if (last_video_dts != AV_NOPTS_VALUE &&
                packet.dts_us != AV_NOPTS_VALUE &&
                packet.dts_us < last_video_dts)
                dts_monotone = false;
            if (packet.dts_us != AV_NOPTS_VALUE)
                last_video_dts = packet.dts_us;
            ++video_count;
        } else {
            ++audio_count;
        }
        media_packet_unref(&packet);
    }
    printf("packets video=%u audio=%u keyframes=%u first_video_pts_us=%" PRId64
           " first_key_pts_us=%" PRId64 " dts_monotone=%u annex_b=1\n",
           video_count, audio_count, keyframes, first_video_pts,
           first_key_pts, dts_monotone);
    media_demux_close(&demux);
    return dts_monotone && video_count > 0 ? 0 : 1;
}
