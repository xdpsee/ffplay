/**
 * @file
 * simple media player based on the FFmpeg libraries
 */

#include "config.h"
#include <math.h>
#include <signal.h>
#include <stdint.h>

#include "libavutil/avstring.h"
#include "libavutil/eval.h"
#include "libavutil/time.h"
#include "libavformat/avformat.h"
#include "libavdevice/avdevice.h"
#include "libavutil/opt.h"
#include "libswresample/swresample.h"

#if CONFIG_AVFILTER

# include "libavfilter/avfilter.h"

#endif

#include <SDL.h>
#include <SDL_thread.h>

#include "cmdutils.h"
#include "packet_queue.h"
#include "frame_queue.h"
#include "sclock.h"
#include "opts.h"
#include "enums.h"
#include "media_state.h"
#include "utils.h"


static void fill_rectangle(int x, int y, int w, int h);

static int video_open(MediaState *is);

static void video_refresh(void *opaque, double *remaining_time);

static void video_display(MediaState *is);

static void video_audio_display(MediaState *s);

static void video_image_display(MediaState *is);

static int
realloc_texture(SDL_Texture **texture, Uint32 new_format, int new_width, int new_height, SDL_BlendMode blendmode,
                int init_texture);

void set_sdl_yuv_conversion_mode(AVFrame *frame);

static void sdl_audio_callback(void *opaque, Uint8 *stream, int len);

static void media_audio_pause_callback(MediaState *state);

static void media_audio_close_callback(MediaState *state);

static void media_stream_closed_callback(MediaState *state);

const char program_name[] = "ffplay";
const int program_birth_year = 2003;


#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10

/* Step size for volume control in dB */
#define SDL_VOLUME_STEP (0.75)


/* polls for possible required screen refresh at least this often, should be less than 1/fps */
#define REFRESH_RATE 0.01


#define CURSOR_HIDE_DELAY 1000000

#define USE_ONEPASS_SUBTITLE_RENDER 1

extern const char *input_filename;
extern int decoder_reorder_pts;

int screen_width = 0;
int screen_height = 0;
enum ShowMode show_mode = SHOW_MODE_NONE;

const char *audio_codec_name;
const char *subtitle_codec_name;
const char *video_codec_name;


int show_status = 1;
const char *window_title;

const char *wanted_stream_spec[AVMEDIA_TYPE_NB] = {0};
int seek_by_bytes = -1;
float seek_interval = 10;
int border_less;
int always_on_top;
int startup_volume = 100;

int fast = 0;
int gen_pts = 0;
int low_res = 0;
int autorotate = 1;
int find_stream_info = 1;
int filter_nbthreads = 0;
int auto_exit;
int exit_on_keydown;
int exit_on_mousedown;
int loop = 1;
int frame_drop = -1;
int infinite_buffer = -1;

double rdft_speed = 0.02;
int64_t cursor_last_shown;
int cursor_hidden = 0;


/* current context */
int is_full_screen;
int64_t audio_callback_time;

// global
AVPacket flush_pkt;

#define FF_QUIT_EVENT    (SDL_USEREVENT + 2)

/**
 * SDL Video
 */
SDL_Window *window;
SDL_Renderer *renderer;
SDL_RendererInfo renderer_info = {0};

SDL_Texture *vis_texture;
SDL_Texture *sub_texture;
SDL_Texture *vid_texture;

static const struct TextureFormatEntry {
    enum AVPixelFormat format;
    int texture_fmt;
} sdl_texture_format_map[] = {
        {AV_PIX_FMT_RGB8,           SDL_PIXELFORMAT_RGB332},
        {AV_PIX_FMT_RGB444,         SDL_PIXELFORMAT_RGB444},
        {AV_PIX_FMT_RGB555,         SDL_PIXELFORMAT_RGB555},
        {AV_PIX_FMT_BGR555,         SDL_PIXELFORMAT_BGR555},
        {AV_PIX_FMT_RGB565,         SDL_PIXELFORMAT_RGB565},
        {AV_PIX_FMT_BGR565,         SDL_PIXELFORMAT_BGR565},
        {AV_PIX_FMT_RGB24,          SDL_PIXELFORMAT_RGB24},
        {AV_PIX_FMT_BGR24,          SDL_PIXELFORMAT_BGR24},
        {AV_PIX_FMT_0RGB32,         SDL_PIXELFORMAT_RGB888},
        {AV_PIX_FMT_0BGR32,         SDL_PIXELFORMAT_BGR888},
        {AV_PIX_FMT_NE(RGB0, 0BGR), SDL_PIXELFORMAT_RGBX8888},
        {AV_PIX_FMT_NE(BGR0, 0RGB), SDL_PIXELFORMAT_BGRX8888},
        {AV_PIX_FMT_RGB32,          SDL_PIXELFORMAT_ARGB8888},
        {AV_PIX_FMT_RGB32_1,        SDL_PIXELFORMAT_RGBA8888},
        {AV_PIX_FMT_BGR32,          SDL_PIXELFORMAT_ABGR8888},
        {AV_PIX_FMT_BGR32_1,        SDL_PIXELFORMAT_BGRA8888},
        {AV_PIX_FMT_YUV420P,        SDL_PIXELFORMAT_IYUV},
        {AV_PIX_FMT_YUYV422,        SDL_PIXELFORMAT_YUY2},
        {AV_PIX_FMT_UYVY422,        SDL_PIXELFORMAT_UYVY},
        {AV_PIX_FMT_NONE,           SDL_PIXELFORMAT_UNKNOWN},
};


static inline int compute_mod(int a, int b) {
    return a < 0 ? a % b + b : a % b;
}

static void sigterm_handler(int sig) {
    exit(123);
}

static void do_exit(MediaState *is) {
    if (is) {
        stream_close(is);
    }
    if (renderer)
        SDL_DestroyRenderer(renderer);
    if (window)
        SDL_DestroyWindow(window);
    uninit_opts();
#if CONFIG_AVFILTER
    av_freep(&vfilters_list);
#endif
    avformat_network_deinit();
    if (show_status)
        printf("\n");
    SDL_Quit();
    av_log(NULL, AV_LOG_QUIET, "%s", "");
    exit(0);
}

static void refresh_loop_wait_event(MediaState *is, SDL_Event *event) {
    double remaining_time = 0.0;
    SDL_PumpEvents();
    while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
        if (!cursor_hidden && av_gettime_relative() - cursor_last_shown > CURSOR_HIDE_DELAY) {
            SDL_ShowCursor(0);
            cursor_hidden = 1;
        }
        if (remaining_time > 0.0)
            av_usleep((int64_t) (remaining_time * 1000000.0));
        remaining_time = REFRESH_RATE;
        SDL_PumpEvents();
    }
}

/* handle an event sent by the GUI */
static void event_loop(MediaState *cur_stream) {
    SDL_Event event;
    double incr, pos, frac;

    for (;;) {
        double x;
        refresh_loop_wait_event(cur_stream, &event);
        switch (event.type) {
            case SDL_KEYDOWN:
                if (exit_on_keydown || event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q) {
                    do_exit(cur_stream);
                    break;
                }
                switch (event.key.keysym.sym) {
                    case SDLK_p:
                    case SDLK_SPACE:
                        toggle_pause(cur_stream);
                        break;
                    case SDLK_m:
                        toggle_mute(cur_stream);
                        break;
                    case SDLK_KP_MULTIPLY:
                    case SDLK_0:
                        update_volume(cur_stream, 1, SDL_VOLUME_STEP);
                        break;
                    case SDLK_KP_DIVIDE:
                    case SDLK_9:
                        update_volume(cur_stream, -1, SDL_VOLUME_STEP);
                        break;
                    case SDLK_PAGEUP:
                        if (cur_stream->ic->nb_chapters <= 1) {
                            incr = 600.0;
                            goto do_seek;
                        }
                        seek_chapter(cur_stream, 1);
                        break;
                    case SDLK_PAGEDOWN:
                        if (cur_stream->ic->nb_chapters <= 1) {
                            incr = -600.0;
                            goto do_seek;
                        }
                        seek_chapter(cur_stream, -1);
                        break;
                    case SDLK_LEFT:
                        incr = seek_interval ? -seek_interval : -10.0;
                        goto do_seek;
                    case SDLK_RIGHT:
                        incr = seek_interval ? seek_interval : 10.0;
                        goto do_seek;
                    case SDLK_UP:
                        incr = 60.0;
                        goto do_seek;
                    case SDLK_DOWN:
                        incr = -60.0;
                    do_seek:
                        if (seek_by_bytes) {
                            pos = -1;
                            if (pos < 0 && cur_stream->audio_stream >= 0)
                                pos = frame_queue_last_pos(&cur_stream->sample_q);
                            if (pos < 0)
                                pos = avio_tell(cur_stream->ic->pb);
                            if (cur_stream->ic->bit_rate)
                                incr *= cur_stream->ic->bit_rate / 8.0;
                            else
                                incr *= 180000.0;
                            pos += incr;
                            stream_seek(cur_stream, pos, incr, 1);
                        } else {
                            pos = get_audio_clock(cur_stream);
                            if (isnan(pos))
                                pos = (double) cur_stream->seek_pos / AV_TIME_BASE;
                            pos += incr;
                            if (cur_stream->ic->start_time != AV_NOPTS_VALUE &&
                                pos < cur_stream->ic->start_time / (double) AV_TIME_BASE)
                                pos = cur_stream->ic->start_time / (double) AV_TIME_BASE;
                            stream_seek(cur_stream, (int64_t) (pos * AV_TIME_BASE), (int64_t) (incr * AV_TIME_BASE), 0);
                        }
                        break;
                    default:
                        break;
                }
                break;
            case SDL_QUIT:
            case FF_QUIT_EVENT:
                do_exit(cur_stream);
                break;
            default:
                break;
        }
    }
}


static int dummy;

static const OptionDef options[] = {
        CMDUTILS_COMMON_OPTIONS
        {"i", OPT_BOOL, {&dummy}, "read specified file", "input_file"},
        {NULL,},
};

/* Called from the main */
int main(int argc, char **argv) {
    int flags;
    MediaState *is;

    init_dynload();

    av_log_set_flags(AV_LOG_SKIP_REPEATED);
    parse_loglevel(argc, argv, options);

    /* register all codecs, demux and protocols */
#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif
    avformat_network_init();

    init_opts();

    signal(SIGINT, sigterm_handler); /* Interrupt (ANSI).    */
    signal(SIGTERM, sigterm_handler); /* Termination (ANSI).  */

    show_banner(argc, argv, options);

    parse_options(NULL, argc, argv, options, opt_input_file);

    if (!input_filename) {
        av_log(NULL, AV_LOG_FATAL, "An input file must be specified\n");
        av_log(NULL, AV_LOG_FATAL,
               "Use -h to get full help or, even better, run 'man %s'\n", program_name);
        exit(1);
    }

    flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;

    /* Try to work around an occasional ALSA buffer underflow issue when the
     * period size is NPOT due to ALSA resampling by forcing the buffer size. */
    if (!SDL_getenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE"))
        SDL_setenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE", "1", 1);

    if (SDL_Init(flags)) {
        av_log(NULL, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
        av_log(NULL, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
        exit(1);
    }

    SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
    SDL_EventState(SDL_USEREVENT, SDL_IGNORE);

    av_init_packet(&flush_pkt);
    flush_pkt.data = (uint8_t *) &flush_pkt;

    if (1) {
        int flags = SDL_WINDOW_HIDDEN;
        if (always_on_top)
#if SDL_VERSION_ATLEAST(2, 0, 5)
            flags |= SDL_WINDOW_ALWAYS_ON_TOP;
#else
        av_log(NULL, AV_LOG_WARNING, "Your SDL version doesn't support SDL_WINDOW_ALWAYS_ON_TOP. Feature will be inactive.\n");
#endif
        if (border_less)
            flags |= SDL_WINDOW_BORDERLESS;
        else
            flags |= SDL_WINDOW_RESIZABLE;
        window = SDL_CreateWindow(program_name, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 100,
                                  100, flags);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        if (window) {
            renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
            if (!renderer) {
                av_log(NULL, AV_LOG_WARNING, "Failed to initialize a hardware accelerated renderer: %s\n",
                       SDL_GetError());
                renderer = SDL_CreateRenderer(window, -1, 0);
            }
            if (renderer) {
                if (!SDL_GetRendererInfo(renderer, &renderer_info))
                    av_log(NULL, AV_LOG_VERBOSE, "Initialized %s renderer.\n", renderer_info.name);
            }
        }
        if (!window || !renderer || !renderer_info.num_texture_formats) {
            av_log(NULL, AV_LOG_FATAL, "Failed to create window or renderer: %s", SDL_GetError());
            do_exit(NULL);
        }
    }

    is = stream_open(input_filename, file_iformat);
    if (!is) {
        av_log(NULL, AV_LOG_FATAL, "Failed to initialize MediaState!\n");
        do_exit(NULL);
    }

    is->stream_closed_callback = media_stream_closed_callback;
    is->pause_system_audio_proc = media_audio_pause_callback;
    is->close_system_audio_proc = media_audio_close_callback;

    event_loop(is);

    /* never returns */

    return 0;
}

void media_stream_closed_callback(MediaState *state) {


}

/**
 * SDL Audio handle
 */
static void sdl_audio_callback(void *opaque, Uint8 *stream, int len);
/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

static SDL_AudioDeviceID audio_dev;

void media_audio_pause_callback(MediaState *state) {
    SDL_PauseAudioDevice(audio_dev, 0);
}

void media_audio_close_callback(MediaState *state) {
    SDL_CloseAudioDevice(audio_dev);
}

int audio_open(void *opaque, int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate,
               struct AudioParams *audio_hw_params) {
    SDL_AudioSpec wanted_spec, spec;
    const char *env;
    static const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};
    static const int next_sample_rates[] = {0, 44100, 48000, 96000, 192000};
    int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;

    env = SDL_getenv("SDL_AUDIO_CHANNELS");
    if (env) {
        wanted_nb_channels = atoi(env);
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
    }
    if (!wanted_channel_layout || wanted_nb_channels != av_get_channel_layout_nb_channels(wanted_channel_layout)) {
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
        wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
    }
    wanted_nb_channels = av_get_channel_layout_nb_channels(wanted_channel_layout);
    wanted_spec.channels = wanted_nb_channels;
    wanted_spec.freq = wanted_sample_rate;
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
        av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
        return -1;
    }
    while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
        next_sample_rate_idx--;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.silence = 0;
    wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE,
                                2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = opaque;
    while (!(audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec,
                                             SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE))) {
        av_log(NULL, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
               wanted_spec.channels, wanted_spec.freq, SDL_GetError());
        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
        if (!wanted_spec.channels) {
            wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
            wanted_spec.channels = wanted_nb_channels;
            if (!wanted_spec.freq) {
                av_log(NULL, AV_LOG_ERROR,
                       "No more combinations to try, audio open failed\n");
                return -1;
            }
        }
        wanted_channel_layout = av_get_default_channel_layout(wanted_spec.channels);
    }
    if (spec.format != AUDIO_S16SYS) {
        av_log(NULL, AV_LOG_ERROR,
               "SDL advised audio format %d is not supported!\n", spec.format);
        return -1;
    }
    if (spec.channels != wanted_spec.channels) {
        wanted_channel_layout = av_get_default_channel_layout(spec.channels);
        if (!wanted_channel_layout) {
            av_log(NULL, AV_LOG_ERROR,
                   "SDL advised channel count %d is not supported!\n", spec.channels);
            return -1;
        }
    }

    audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
    audio_hw_params->freq = spec.freq;
    audio_hw_params->channel_layout = wanted_channel_layout;
    audio_hw_params->channels = spec.channels;
    audio_hw_params->frame_size = av_samples_get_buffer_size(NULL, audio_hw_params->channels, 1, audio_hw_params->fmt,
                                                             1);
    audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(NULL, audio_hw_params->channels, audio_hw_params->freq,
                                                                audio_hw_params->fmt, 1);
    if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) {
        av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
        return -1;
    }
    return spec.size;
}

/* prepare a new audio buffer */
void sdl_audio_callback(void *opaque, Uint8 *stream, int len) {
    MediaState *is = opaque;
    int audio_size, len1;

    audio_callback_time = av_gettime_relative();

    while (len > 0) {
        if (is->audio_buf_index >= is->audio_buf_size) {
            audio_size = audio_decode_frame(is);
            if (audio_size < 0) {
                /* if error, just output silence */
                is->audio_buf = NULL;
                is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / is->audio_tgt.frame_size * is->audio_tgt.frame_size;
            } else {
                is->audio_buf_size = audio_size;
            }
            is->audio_buf_index = 0;
        }
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len)
            len1 = len;
        if (!is->muted && is->audio_buf && is->audio_volume == SDL_MIX_MAXVOLUME)
            memcpy(stream, (uint8_t *) is->audio_buf + is->audio_buf_index, len1);
        else {
            memset(stream, 0, len1);
            if (!is->muted && is->audio_buf)
                SDL_MixAudioFormat(stream, (uint8_t *) is->audio_buf + is->audio_buf_index, AUDIO_S16SYS, len1,
                                   is->audio_volume);
        }
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
    is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
    /* Let's assume the audio driver that is used by SDL has two periods. */
    if (!isnan(is->audio_clock)) {
        set_clock_at(&is->audio_clk, is->audio_clock - (double) (2 * is->audio_hw_buf_size + is->audio_write_buf_size) /
                                                       is->audio_tgt.bytes_per_sec, is->audio_clock_serial,
                     audio_callback_time / 1000000.0);
    }
}

