#ifndef __PLAYGROUND_DECODER_H
#define __PLAYGROUND_DECODER_H

#include <stdint.h>

#include "libavformat/avformat.h"

#include <SDL.h>
#include <SDL_thread.h>

#include "packet_queue.h"
#include "frame_queue.h"

typedef struct Decoder {
    AVPacket pkt;
    PacketQueue *queue;
    AVCodecContext *avctx;
    int pkt_serial;
    int finished;
    int packet_pending;
    SDL_cond *empty_queue_cond;
    int64_t start_pts;
    AVRational start_pts_tb;
    int64_t next_pts;
    AVRational next_pts_tb;
    SDL_Thread *decoder_tid;
} Decoder;


extern void decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond);

extern int decoder_start(Decoder *d, int (*fn)(void *), const char *thread_name, void* arg);

extern int decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub);

extern void decoder_abort(Decoder *d, FrameQueue *fq);

extern void decoder_destroy(Decoder *d);



#endif