//
// Created by chen zhenhui on 2020/3/28.
//

#ifndef __PLAYGROUND_MEDIA_STATE_H
#define __PLAYGROUND_MEDIA_STATE_H

#include <stdint.h>

#include <pthread.h>
#include "libavformat/avformat.h"
#include "libavcodec/avfft.h"

#include "sclock.h"
#include "frame_queue.h"
#include "decoder.h"
#include "packet_queue.h"
#include "enums.h"

/* NOTE: the size must be big enough to compensate the hardware audio buffersize size */
/* TODO: We assume that a decoded and resampled frame fits into this buffer */
#define SAMPLE_ARRAY_SIZE (8 * 65536)

typedef void (*stream_closed_callback_proc)(void *);
typedef void (*pause_system_audio_device_proc)(void *);
typedef void (*close_system_audio_device_proc)(void *);

typedef struct AudioParams {
    int freq;
    int channels;
    int64_t channel_layout;
    enum AVSampleFormat fmt;
    int frame_size;
    int bytes_per_sec;
} AudioParams;


typedef struct MediaState {
    pthread_t read_tid;
    AVInputFormat *input_format;
    int abort_request;
    int paused;
    int last_paused;
    int seek_req;
    int seek_flags;
    int64_t seek_pos;
    int64_t seek_rel;
    int read_pause_return;
    AVFormatContext *ic;
    int realtime;

    Clock audio_clk;

    FrameQueue sample_q;

    Decoder audio_dec;

    int audio_stream;

    double audio_clock;
    int audio_clock_serial;
    AVStream *audio_st;
    PacketQueue audio_q;
    int audio_hw_buf_size;
    uint8_t *audio_buf;
    uint8_t *audio_buf1;
    unsigned int audio_buf_size; /* in bytes */
    unsigned int audio_buf1_size;
    int audio_buf_index; /* in bytes */
    int audio_write_buf_size;
    int audio_volume;
    int muted;
    struct AudioParams audio_src;
#if CONFIG_AVFILTER
    struct AudioParams audio_filter_src;
#endif
    struct AudioParams audio_tgt;
    struct SwrContext *swr_ctx;

    int16_t sample_array[SAMPLE_ARRAY_SIZE];
    int sample_array_index;
    RDFTContext *rdft;
    FFTSample *rdft_data;

    int eof;

    char *filename;

#if CONFIG_AVFILTER
    AVFilterContext *in_audio_filter;   // the first filter in the audio chain
    AVFilterContext *out_audio_filter;  // the last filter in the audio chain
    AVFilterGraph *agraph;              // audio filter graph
#endif

    pthread_cond_t continue_read_thread;


    stream_closed_callback_proc stream_closed_callback;
    pause_system_audio_device_proc pause_system_audio_proc;
    close_system_audio_device_proc close_system_audio_proc;

} MediaState;

extern MediaState *stream_open(const char *filename, AVInputFormat *input_format);

extern void stream_close(MediaState *is);

extern void toggle_pause(MediaState *is);

extern void toggle_mute(MediaState *is);

extern void update_volume(MediaState *is, int sign, double step);

extern void seek_chapter(MediaState *is, int incr);

extern void stream_seek(MediaState *is, int64_t pos, int64_t rel, int seek_by_bytes);

extern void stream_toggle_pause(MediaState *is);

extern double get_audio_clock(MediaState *is);

extern int audio_decode_frame(MediaState *is);

extern void update_sample_display(MediaState *is, short *samples, int samples_size);

#endif //__PLAYGROUND_MEDIA_STATE_H
