#define _POSIX_C_SOURCE 200809L
#include "packet-queue.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int packet_queue_initialize(struct packet_queue *queue, size_t capacity)
{
    if (!queue || !capacity)
        return -1;
    memset(queue, 0, sizeof(*queue));
    queue->items = calloc(capacity, sizeof(*queue->items));
    if (!queue->items)
        return -1;
    queue->capacity = capacity;
    if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
        free(queue->items);
        memset(queue, 0, sizeof(*queue));
        return -1;
    }
    if (pthread_cond_init(&queue->readable, NULL) != 0) {
        pthread_mutex_destroy(&queue->mutex);
        free(queue->items);
        memset(queue, 0, sizeof(*queue));
        return -1;
    }
    if (pthread_cond_init(&queue->writable, NULL) != 0) {
        pthread_cond_destroy(&queue->readable);
        pthread_mutex_destroy(&queue->mutex);
        free(queue->items);
        memset(queue, 0, sizeof(*queue));
        return -1;
    }
    return 0;
}

int packet_queue_push(struct packet_queue *queue, struct media_packet *packet)
{
    pthread_mutex_lock(&queue->mutex);
    while (queue->count == queue->capacity && !queue->aborted)
        pthread_cond_wait(&queue->writable, &queue->mutex);
    if (queue->closed || queue->aborted) {
        pthread_mutex_unlock(&queue->mutex);
        return -1;
    }
    size_t tail = (queue->head + queue->count) % queue->capacity;
    queue->items[tail] = *packet;
    memset(packet, 0, sizeof(*packet));
    ++queue->count;
    pthread_cond_signal(&queue->readable);
    pthread_mutex_unlock(&queue->mutex);
    return 0;
}

static void timeout_from_now(struct timespec *deadline, int timeout_ms)
{
    clock_gettime(CLOCK_REALTIME, deadline);
    deadline->tv_sec += timeout_ms / 1000;
    deadline->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        ++deadline->tv_sec;
        deadline->tv_nsec -= 1000000000L;
    }
}

int packet_queue_pop(struct packet_queue *queue, struct media_packet *packet,
                     int timeout_ms)
{
    pthread_mutex_lock(&queue->mutex);
    int wait_result = 0;
    struct timespec deadline = {0};
    if (timeout_ms >= 0)
        timeout_from_now(&deadline, timeout_ms);
    while (!queue->count && !queue->closed && !queue->aborted) {
        if (timeout_ms < 0)
            wait_result = pthread_cond_wait(&queue->readable, &queue->mutex);
        else
            wait_result = pthread_cond_timedwait(&queue->readable,
                                                 &queue->mutex, &deadline);
        if (wait_result == ETIMEDOUT)
            break;
    }
    if (queue->aborted || (!queue->count && queue->closed)) {
        pthread_mutex_unlock(&queue->mutex);
        return -1;
    }
    if (!queue->count) {
        pthread_mutex_unlock(&queue->mutex);
        return 0;
    }
    *packet = queue->items[queue->head];
    memset(&queue->items[queue->head], 0, sizeof(queue->items[queue->head]));
    queue->head = (queue->head + 1) % queue->capacity;
    --queue->count;
    pthread_cond_signal(&queue->writable);
    pthread_mutex_unlock(&queue->mutex);
    return 1;
}

void packet_queue_close(struct packet_queue *queue)
{
    if (!queue || !queue->items)
        return;
    pthread_mutex_lock(&queue->mutex);
    queue->closed = true;
    pthread_cond_broadcast(&queue->readable);
    pthread_cond_broadcast(&queue->writable);
    pthread_mutex_unlock(&queue->mutex);
}

void packet_queue_abort(struct packet_queue *queue)
{
    if (!queue || !queue->items)
        return;
    pthread_mutex_lock(&queue->mutex);
    queue->aborted = true;
    pthread_cond_broadcast(&queue->readable);
    pthread_cond_broadcast(&queue->writable);
    pthread_mutex_unlock(&queue->mutex);
}

void packet_queue_destroy(struct packet_queue *queue)
{
    if (!queue || !queue->items)
        return;
    for (size_t i = 0; i < queue->capacity; ++i)
        media_packet_unref(&queue->items[i]);
    pthread_cond_destroy(&queue->readable);
    pthread_cond_destroy(&queue->writable);
    pthread_mutex_destroy(&queue->mutex);
    free(queue->items);
    memset(queue, 0, sizeof(*queue));
}
