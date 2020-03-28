#ifndef __PLAYGROUND_DECODER_H
#define __PLAYGROUND_DECODER_H

#include <stdint.h>

#include <pthread.h>
#include "libavformat/avformat.h"

#include "packet_queue.h"
#include "frame_queue.h"

typedef struct Decoder {
    AVPacket pkt;
    PacketQueue *queue;
    AVCodecContext *context;
    int pkt_serial;
    int finished;
    int packet_pending;
    pthread_cond_t *empty_queue_cond;
    int64_t start_pts;
    AVRational start_pts_tb;
    int64_t next_pts;
    AVRational next_pts_tb;
    pthread_t decoder_tid;
} Decoder;


extern void decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, pthread_cond_t *empty_queue_cond);

extern int decoder_start(Decoder *d, int (*thread_func)(void *), const char *thread_name, void *arg);

extern int decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub);

extern void decoder_abort(Decoder *d, FrameQueue *fq);

extern void decoder_destroy(Decoder *d);


#endif