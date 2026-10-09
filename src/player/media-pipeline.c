#define _POSIX_C_SOURCE 200809L
#include "media-pipeline.h"

#include "audio-output.h"
#include "packet-queue.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VIDEO_QUEUE_CAPACITY 24
#define AUDIO_QUEUE_CAPACITY 256

struct media_pipeline {
    struct media_demux *demux;
    struct audio_output *audio;
    struct packet_queue video_queue;
    struct packet_queue audio_queue;
    pthread_t demux_thread;
    pthread_t audio_thread;
    pthread_mutex_t state_mutex;
    pthread_cond_t state_changed;
    pthread_mutex_t audio_mutex;
    bool demux_started;
    bool audio_started;
    bool state_mutex_initialized;
    bool audio_mutex_initialized;
    bool state_condition_initialized;
    bool stopping;
    bool paused;
    bool audio_has_started;
    bool failed;
    char failure[512];
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

static void fail_pipeline(struct media_pipeline *pipeline, const char *detail)
{
    pthread_mutex_lock(&pipeline->state_mutex);
    if (!pipeline->failed) {
        pipeline->failed = true;
        snprintf(pipeline->failure, sizeof(pipeline->failure), "%s", detail);
    }
    pthread_cond_broadcast(&pipeline->state_changed);
    pthread_mutex_unlock(&pipeline->state_mutex);
    packet_queue_abort(&pipeline->video_queue);
    packet_queue_abort(&pipeline->audio_queue);
}

static void *demux_worker(void *argument)
{
    struct media_pipeline *pipeline = argument;
    char error[512] = {0};
    for (;;) {
        pthread_mutex_lock(&pipeline->state_mutex);
        bool stopping = pipeline->stopping;
        pthread_mutex_unlock(&pipeline->state_mutex);
        if (stopping)
            break;
        struct media_packet packet;
        int result = media_demux_read(pipeline->demux, &packet,
                                      error, sizeof(error));
        if (result < 0) {
            fail_pipeline(pipeline, error);
            return NULL;
        }
        if (result == 0)
            break;
        struct packet_queue *queue = packet.kind == MEDIA_PACKET_VIDEO
                                         ? &pipeline->video_queue
                                         : &pipeline->audio_queue;
        if (packet_queue_push(queue, &packet) < 0) {
            media_packet_unref(&packet);
            break;
        }
    }
    packet_queue_close(&pipeline->video_queue);
    packet_queue_close(&pipeline->audio_queue);
    return NULL;
}

static bool wait_until_playing(struct media_pipeline *pipeline)
{
    pthread_mutex_lock(&pipeline->state_mutex);
    while (pipeline->paused && !pipeline->stopping && !pipeline->failed)
        pthread_cond_wait(&pipeline->state_changed, &pipeline->state_mutex);
    bool keep_running = !pipeline->stopping && !pipeline->failed;
    pthread_mutex_unlock(&pipeline->state_mutex);
    return keep_running;
}

static void *audio_worker(void *argument)
{
    struct media_pipeline *pipeline = argument;
    char error[512] = {0};
    bool reached_eof = false;
    while (wait_until_playing(pipeline)) {
        struct media_packet packet;
        int result = packet_queue_pop(&pipeline->audio_queue, &packet, 100);
        if (result < 0) {
            pthread_mutex_lock(&pipeline->state_mutex);
            reached_eof = !pipeline->stopping && !pipeline->failed;
            pthread_mutex_unlock(&pipeline->state_mutex);
            break;
        }
        if (result == 0)
            continue;
        pthread_mutex_lock(&pipeline->audio_mutex);
        result = audio_output_push(pipeline->audio, packet.packet,
                                   error, sizeof(error));
        pthread_mutex_unlock(&pipeline->audio_mutex);
        media_packet_unref(&packet);
        if (result < 0) {
            fail_pipeline(pipeline, error);
            return NULL;
        }
        pthread_mutex_lock(&pipeline->state_mutex);
        pipeline->audio_has_started = true;
        pthread_mutex_unlock(&pipeline->state_mutex);
    }
    if (reached_eof) {
        pthread_mutex_lock(&pipeline->audio_mutex);
        int result = audio_output_finish(pipeline->audio,
                                         error, sizeof(error));
        pthread_mutex_unlock(&pipeline->audio_mutex);
        if (result < 0)
            fail_pipeline(pipeline, error);
    }
    return NULL;
}

int media_pipeline_open(struct media_pipeline **result, const char *path,
                        int64_t start_us, bool enable_audio_device,
                        bool start_paused,
                        char *error, size_t error_size)
{
    if (!result) {
        set_error(error, error_size, "invalid pipeline result pointer");
        return -1;
    }
    *result = NULL;
    struct media_pipeline *pipeline = calloc(1, sizeof(*pipeline));
    if (!pipeline) {
        set_error(error, error_size, "allocate pipeline: %s", strerror(errno));
        return -1;
    }
    if (pthread_mutex_init(&pipeline->state_mutex, NULL) != 0) {
        set_error(error, error_size, "initialize pipeline synchronization");
        free(pipeline);
        return -1;
    }
    pipeline->state_mutex_initialized = true;
    if (pthread_mutex_init(&pipeline->audio_mutex, NULL) != 0) {
        set_error(error, error_size, "initialize audio mutex");
        media_pipeline_close(&pipeline);
        return -1;
    }
    pipeline->audio_mutex_initialized = true;
    if (pthread_cond_init(&pipeline->state_changed, NULL) != 0) {
        set_error(error, error_size, "initialize player condition");
        media_pipeline_close(&pipeline);
        return -1;
    }
    pipeline->state_condition_initialized = true;
    pipeline->paused = start_paused;
    if (packet_queue_initialize(&pipeline->video_queue,
                                VIDEO_QUEUE_CAPACITY) < 0 ||
        packet_queue_initialize(&pipeline->audio_queue,
                                AUDIO_QUEUE_CAPACITY) < 0) {
        set_error(error, error_size, "initialize packet queues");
        media_pipeline_close(&pipeline);
        return -1;
    }
    if (media_demux_open(&pipeline->demux, path,
                         error, error_size) < 0) {
        media_pipeline_close(&pipeline);
        return -1;
    }
    const struct media_stream_info *info = media_demux_info(pipeline->demux);
    if (info->audio_stream_index < 0) {
        set_error(error, error_size, "container has no supported audio stream");
        media_pipeline_close(&pipeline);
        return -1;
    }
    AVCodecParameters *parameters = avcodec_parameters_alloc();
    if (!parameters || media_demux_copy_audio_parameters(
                           pipeline->demux, parameters) < 0) {
        set_error(error, error_size, "copy audio parameters failed");
        avcodec_parameters_free(&parameters);
        media_pipeline_close(&pipeline);
        return -1;
    }
    int open_result = audio_output_open(&pipeline->audio, parameters,
                                        info->audio_time_base,
                                        enable_audio_device,
                                        error, error_size);
    avcodec_parameters_free(&parameters);
    if (open_result < 0) {
        media_pipeline_close(&pipeline);
        return -1;
    }
    if (start_us > 0 && media_demux_seek(pipeline->demux, start_us,
                                         error, error_size) < 0) {
        media_pipeline_close(&pipeline);
        return -1;
    }
    if (pthread_create(&pipeline->demux_thread, NULL,
                       demux_worker, pipeline) != 0) {
        set_error(error, error_size, "start demux thread failed");
        media_pipeline_close(&pipeline);
        return -1;
    }
    pipeline->demux_started = true;
    if (pthread_create(&pipeline->audio_thread, NULL,
                       audio_worker, pipeline) != 0) {
        set_error(error, error_size, "start audio thread failed");
        media_pipeline_close(&pipeline);
        return -1;
    }
    pipeline->audio_started = true;
    *result = pipeline;
    return 0;
}

const struct media_stream_info *media_pipeline_info(
    const struct media_pipeline *pipeline)
{
    return pipeline ? media_demux_info(pipeline->demux) : NULL;
}

static int copy_failure(struct media_pipeline *pipeline,
                        char *error, size_t error_size)
{
    pthread_mutex_lock(&pipeline->state_mutex);
    bool failed = pipeline->failed;
    if (failed)
        set_error(error, error_size, "%s", pipeline->failure);
    pthread_mutex_unlock(&pipeline->state_mutex);
    return failed ? -1 : 0;
}

int media_pipeline_pop_video(struct media_pipeline *pipeline,
                             struct media_packet *packet, int timeout_ms,
                             char *error, size_t error_size)
{
    if (!pipeline || !packet)
        return -1;
    int result = packet_queue_pop(&pipeline->video_queue,
                                  packet, timeout_ms);
    if (result < 0 && copy_failure(pipeline, error, error_size) < 0)
        return -1;
    return result;
}

int64_t media_pipeline_clock_us(struct media_pipeline *pipeline)
{
    if (!pipeline)
        return AV_NOPTS_VALUE;
    pthread_mutex_lock(&pipeline->audio_mutex);
    int64_t result = audio_output_clock_us(pipeline->audio);
    pthread_mutex_unlock(&pipeline->audio_mutex);
    return result;
}

int media_pipeline_pause(struct media_pipeline *pipeline, bool paused,
                         char *error, size_t error_size)
{
    if (!pipeline)
        return -1;
    pthread_mutex_lock(&pipeline->state_mutex);
    bool audio_has_started = pipeline->audio_has_started;
    pipeline->paused = paused;
    pthread_cond_broadcast(&pipeline->state_changed);
    pthread_mutex_unlock(&pipeline->state_mutex);
    if (!audio_has_started)
        return 0;
    pthread_mutex_lock(&pipeline->audio_mutex);
    int result = audio_output_pause(pipeline->audio, paused,
                                    error, error_size);
    pthread_mutex_unlock(&pipeline->audio_mutex);
    if (result < 0)
        return -1;
    return 0;
}

void media_pipeline_close(struct media_pipeline **pointer)
{
    if (!pointer || !*pointer)
        return;
    struct media_pipeline *pipeline = *pointer;
    if (pipeline->state_mutex_initialized) {
        pthread_mutex_lock(&pipeline->state_mutex);
        pipeline->stopping = true;
        if (pipeline->state_condition_initialized)
            pthread_cond_broadcast(&pipeline->state_changed);
        pthread_mutex_unlock(&pipeline->state_mutex);
    }
    packet_queue_abort(&pipeline->video_queue);
    packet_queue_abort(&pipeline->audio_queue);
    if (pipeline->demux_started)
        pthread_join(pipeline->demux_thread, NULL);
    if (pipeline->audio_started)
        pthread_join(pipeline->audio_thread, NULL);
    audio_output_close(&pipeline->audio);
    media_demux_close(&pipeline->demux);
    packet_queue_destroy(&pipeline->video_queue);
    packet_queue_destroy(&pipeline->audio_queue);
    if (pipeline->state_condition_initialized)
        pthread_cond_destroy(&pipeline->state_changed);
    if (pipeline->audio_mutex_initialized)
        pthread_mutex_destroy(&pipeline->audio_mutex);
    if (pipeline->state_mutex_initialized)
        pthread_mutex_destroy(&pipeline->state_mutex);
    free(pipeline);
    *pointer = NULL;
}
