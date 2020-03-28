#ifndef __PLAYGROUND_PACKET_QUEUE_H
#define __PLAYGROUND_PACKET_QUEUE_H

#include <stdint.h>

#include "libavcodec/avcodec.h"

#include <SDL.h>
#include <SDL_thread.h>

#include "cmdutils.h"

#include <assert.h>

typedef struct PacketListNode {
    AVPacket pkt;
    struct PacketListNode *next;
    int serial;
} PacketListNode;

typedef struct PacketQueue {
    PacketListNode *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    int64_t duration;
    int abort_request;
    int serial;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

extern int packet_queue_put_private(PacketQueue *q, AVPacket *pkt);

extern int packet_queue_put(PacketQueue *q, AVPacket *pkt);

extern int packet_queue_put_null_packet(PacketQueue *q, int stream_index);

/* packet queue handling */
extern int packet_queue_init(PacketQueue *q);

extern void packet_queue_flush(PacketQueue *q);

extern void packet_queue_destroy(PacketQueue *q);

extern void packet_queue_abort(PacketQueue *q);

extern void packet_queue_start(PacketQueue *q);

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
extern int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial);


#endif