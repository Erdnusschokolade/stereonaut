#ifndef STEREONAUT_PACKET_QUEUE_H
#define STEREONAUT_PACKET_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

#include "media-demux.h"

struct packet_queue {
    struct media_packet *items;
    size_t capacity;
    size_t head;
    size_t count;
    bool closed;
    bool aborted;
    pthread_mutex_t mutex;
    pthread_cond_t readable;
    pthread_cond_t writable;
};

int packet_queue_initialize(struct packet_queue *queue, size_t capacity);
int packet_queue_push(struct packet_queue *queue, struct media_packet *packet);
int packet_queue_pop(struct packet_queue *queue, struct media_packet *packet,
                     int timeout_ms);
void packet_queue_close(struct packet_queue *queue);
void packet_queue_abort(struct packet_queue *queue);
void packet_queue_destroy(struct packet_queue *queue);

#endif
