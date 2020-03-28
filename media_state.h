//
// Created by chen zhenhui on 2020/3/28.
//

#ifndef __PLAYGROUND_MEDIA_STATE_H
#define __PLAYGROUND_MEDIA_STATE_H

#include <stdint.h>

#include "libavformat/avformat.h"
#include "libavcodec/avfft.h"

#include <SDL.h>

#include "sclock.h"
#include "frame_queue.h"
#include "decoder.h"
#include "packet_queue.h"
#include "enums.h"


/* NOTE: the size must be big enough to compensate the hardware audio buffersize size */
/* TODO: We assume that a decoded and resampled frame fits into this buffer */
#define SAMPLE_ARRAY_SIZE (8 * 65536)

typedef struct AudioParams {
    int freq;
    int channels;
    int64_t channel_layout;
    enum AVSampleFormat fmt;
    int frame_size;
    int bytes_per_sec;
} AudioParams;


typedef struct MediaState {
    SDL_Thread *read_tid;
    AVInputFormat *input_format;
    int abort_request;
    int force_refresh;
    int paused;
    int last_paused;
    int queue_attachments_req;
    int seek_req;
    int seek_flags;
    int64_t seek_pos;
    int64_t seek_rel;
    int read_pause_return;
    AVFormatContext *ic;
    int realtime;

    Clock audio_clk;
    Clock video_clk;
    Clock ext_clk;

    FrameQueue pic_q;
    FrameQueue sub_pic_q;
    FrameQueue sample_q;

    Decoder audio_dec;
    Decoder video_dec;
    Decoder subtitle_dec;

    int audio_stream;

    int av_sync_type;

    double audio_clock;
    int audio_clock_serial;
    double audio_diff_cum; /* used for AV difference average computation */
    double audio_diff_avg_coef;
    double audio_diff_threshold;
    int audio_diff_avg_count;
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
    int frame_drops_early;
    int frame_drops_late;

    enum ShowMode show_mode;
    int16_t sample_array[SAMPLE_ARRAY_SIZE];
    int sample_array_index;
    int last_i_start;
    RDFTContext *rdft;
    int rdft_bits;
    FFTSample *rdft_data;
    int xpos;
    double last_vis_time;
    SDL_Texture *vis_texture;
    SDL_Texture *sub_texture;
    SDL_Texture *vid_texture;

    int subtitle_stream;
    AVStream *subtitle_st;
    PacketQueue subtitle_q;

    double frame_timer;
    double frame_last_returned_time;
    double frame_last_filter_delay;
    int video_stream;
    AVStream *video_st;
    PacketQueue video_q;
    double max_frame_duration;      // maximum duration of a frame - above this, we consider the jump a timestamp discontinuity
    struct SwsContext *img_convert_ctx;
    struct SwsContext *sub_convert_ctx;
    int eof;

    char *filename;
    int width, height, xleft, ytop;
    int step;

#if CONFIG_AVFILTER
    int vfilter_idx;
    AVFilterContext *in_video_filter;   // the first filter in the video chain
    AVFilterContext *out_video_filter;  // the last filter in the video chain
    AVFilterContext *in_audio_filter;   // the first filter in the audio chain
    AVFilterContext *out_audio_filter;  // the last filter in the audio chain
    AVFilterGraph *agraph;              // audio filter graph
#endif

    int last_video_stream, last_audio_stream, last_subtitle_stream;

    SDL_cond *continue_read_thread;
} MediaState;

extern MediaState* stream_open(const char *filename, AVInputFormat *iformat);

extern void stream_close(MediaState *is);

extern void video_refresh(void *opaque, double *remaining_time);

extern void toggle_full_screen(MediaState *is);

extern void toggle_pause(MediaState *is);

extern void toggle_mute(MediaState *is);

extern void update_volume(MediaState *is, int sign, double step);

extern void step_to_next_frame(MediaState *is);

extern void stream_cycle_channel(MediaState *is, int codec_type);

extern void toggle_audio_display(MediaState *is);

extern void seek_chapter(MediaState *is, int incr);

extern void stream_seek(MediaState *is, int64_t pos, int64_t rel, int seek_by_bytes);

extern void stream_toggle_pause(MediaState *is);

extern double get_master_clock(MediaState *is);

#endif //__PLAYGROUND_MEDIA_STATE_H
