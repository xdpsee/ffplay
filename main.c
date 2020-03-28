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
#include "decoder.h"
#include "sclock.h"
#include "opts.h"
#include "enums.h"
#include "media_state.h"

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
int default_width = 640;
int default_height = 480;

const int screen_left = SDL_WINDOWPOS_CENTERED;
const int screen_top = SDL_WINDOWPOS_CENTERED;
int audio_disable;
int display_disable;
int video_disable;
int subtitle_disable;

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

SDL_Window *window;
SDL_Renderer *renderer;
SDL_RendererInfo renderer_info = {0};


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
        if (is->show_mode != SHOW_MODE_NONE && (!is->paused || is->force_refresh))
            video_refresh(is, &remaining_time);
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
                // If we don't yet have a window, skip all key events, because read_thread might still be initializing...
                if (!cur_stream->width)
                    continue;
                switch (event.key.keysym.sym) {
                    case SDLK_f:
                        toggle_full_screen(cur_stream);
                        cur_stream->force_refresh = 1;
                        break;
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
                    case SDLK_s: // S: Step to next frame
                        step_to_next_frame(cur_stream);
                        break;
                    case SDLK_a:
                        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_AUDIO);
                        break;
                    case SDLK_v:
                        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_VIDEO);
                        break;
                    case SDLK_c:
                        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_VIDEO);
                        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_AUDIO);
                        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_SUBTITLE);
                        break;
                    case SDLK_t:
                        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_SUBTITLE);
                        break;
                    case SDLK_w:
#if CONFIG_AVFILTER
                        if (cur_stream->show_mode == SHOW_MODE_VIDEO && cur_stream->vfilter_idx < nb_vfilters - 1) {
                            if (++cur_stream->vfilter_idx >= nb_vfilters)
                                cur_stream->vfilter_idx = 0;
                        } else {
                            cur_stream->vfilter_idx = 0;
                            toggle_audio_display(cur_stream);
                        }
#else
                        toggle_audio_display(cur_stream);
#endif
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
                            if (pos < 0 && cur_stream->video_stream >= 0)
                                pos = frame_queue_last_pos(&cur_stream->pic_q);
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
                            pos = get_master_clock(cur_stream);
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
            case SDL_MOUSEBUTTONDOWN:
                if (exit_on_mousedown) {
                    do_exit(cur_stream);
                    break;
                }
                if (event.button.button == SDL_BUTTON_LEFT) {
                    static int64_t last_mouse_left_click = 0;
                    if (av_gettime_relative() - last_mouse_left_click <= 500000) {
                        toggle_full_screen(cur_stream);
                        cur_stream->force_refresh = 1;
                        last_mouse_left_click = 0;
                    } else {
                        last_mouse_left_click = av_gettime_relative();
                    }
                }
            case SDL_MOUSEMOTION:
                if (cursor_hidden) {
                    SDL_ShowCursor(1);
                    cursor_hidden = 0;
                }
                cursor_last_shown = av_gettime_relative();
                if (event.type == SDL_MOUSEBUTTONDOWN) {
                    if (event.button.button != SDL_BUTTON_RIGHT)
                        break;
                    x = event.button.x;
                } else {
                    if (!(event.motion.state & SDL_BUTTON_RMASK))
                        break;
                    x = event.motion.x;
                }
                if (seek_by_bytes || cur_stream->ic->duration <= 0) {
                    uint64_t size = avio_size(cur_stream->ic->pb);
                    stream_seek(cur_stream, size * x / cur_stream->width, 0, 1);
                } else {
                    int64_t ts;
                    int ns, hh, mm, ss;
                    int tns, thh, tmm, tss;
                    tns = cur_stream->ic->duration / 1000000LL;
                    thh = tns / 3600;
                    tmm = (tns % 3600) / 60;
                    tss = (tns % 60);
                    frac = x / cur_stream->width;
                    ns = frac * tns;
                    hh = ns / 3600;
                    mm = (ns % 3600) / 60;
                    ss = (ns % 60);
                    av_log(NULL, AV_LOG_INFO,
                           "Seek to %2.0f%% (%2d:%02d:%02d) of total duration (%2d:%02d:%02d)       \n", frac * 100,
                           hh, mm, ss, thh, tmm, tss);
                    ts = frac * cur_stream->ic->duration;
                    if (cur_stream->ic->start_time != AV_NOPTS_VALUE)
                        ts += cur_stream->ic->start_time;
                    stream_seek(cur_stream, ts, 0, 0);
                }
                break;
            case SDL_WINDOWEVENT:
                switch (event.window.event) {
                    case SDL_WINDOWEVENT_SIZE_CHANGED:
                        screen_width = cur_stream->width = event.window.data1;
                        screen_height = cur_stream->height = event.window.data2;
                        if (cur_stream->vis_texture) {
                            SDL_DestroyTexture(cur_stream->vis_texture);
                            cur_stream->vis_texture = NULL;
                        }
                    case SDL_WINDOWEVENT_EXPOSED:
                        cur_stream->force_refresh = 1;
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
        {"x", HAS_ARG, {.func_arg = opt_width}, "force displayed width", "width"},
        {"y", HAS_ARG, {.func_arg = opt_height}, "force displayed height", "height"},
        {"s", HAS_ARG | OPT_VIDEO, {.func_arg = opt_frame_size}, "set frame size (WxH or abbreviation)", "size"},
        {"fs", OPT_BOOL, {&is_full_screen}, "force full screen"},
        {"an", OPT_BOOL, {&audio_disable}, "disable audio"},
        {"vn", OPT_BOOL, {&video_disable}, "disable video"},
        {"sn", OPT_BOOL, {&subtitle_disable}, "disable subtitling"},
        {"ast", OPT_STRING | HAS_ARG | OPT_EXPERT, {&wanted_stream_spec[AVMEDIA_TYPE_AUDIO]},
         "select desired audio stream", "stream_specifier"},
        {"vst", OPT_STRING | HAS_ARG | OPT_EXPERT, {&wanted_stream_spec[AVMEDIA_TYPE_VIDEO]},
         "select desired video stream", "stream_specifier"},
        {"sst", OPT_STRING | HAS_ARG | OPT_EXPERT, {&wanted_stream_spec[AVMEDIA_TYPE_SUBTITLE]},
         "select desired subtitle stream", "stream_specifier"},
        {"ss", HAS_ARG, {.func_arg = opt_seek}, "seek to a given position in seconds", "pos"},
        {"t", HAS_ARG, {.func_arg = opt_duration}, "play  \"duration\" seconds of audio/video", "duration"},
        {"bytes", OPT_INT | HAS_ARG, {&seek_by_bytes}, "seek by bytes 0=off 1=on -1=auto", "val"},
        {"seek_interval", OPT_FLOAT | HAS_ARG, {&seek_interval}, "set seek interval for left/right keys, in seconds",
         "seconds"},
        {"nodisp", OPT_BOOL, {&display_disable}, "disable graphical display"},
        {"noborder", OPT_BOOL, {&border_less}, "borderless window"},
        {"alwaysontop", OPT_BOOL, {&always_on_top}, "window always on top"},
        {"volume", OPT_INT | HAS_ARG, {&startup_volume}, "set startup volume 0=min 100=max", "volume"},
        {"f", HAS_ARG, {.func_arg = opt_format}, "force format", "fmt"},
        {"pix_fmt", HAS_ARG | OPT_EXPERT | OPT_VIDEO, {.func_arg = opt_frame_pix_fmt}, "set pixel format", "format"},
        {"stats", OPT_BOOL | OPT_EXPERT, {&show_status}, "show status", ""},
        {"fast", OPT_BOOL | OPT_EXPERT, {&fast}, "non spec compliant optimizations", ""},
        {"genpts", OPT_BOOL | OPT_EXPERT, {&gen_pts}, "generate pts", ""},
        {"drp", OPT_INT | HAS_ARG | OPT_EXPERT, {&decoder_reorder_pts}, "let decoder reorder pts 0=off 1=on -1=auto",
         ""},
        {"lowres", OPT_INT | HAS_ARG | OPT_EXPERT, {&low_res}, "", ""},
        {"sync", HAS_ARG | OPT_EXPERT, {.func_arg = opt_sync}, "set audio-video sync. type (type=audio/video/ext)",
         "type"},
        {"autoexit", OPT_BOOL | OPT_EXPERT, {&auto_exit}, "exit at the end", ""},
        {"exitonkeydown", OPT_BOOL | OPT_EXPERT, {&exit_on_keydown}, "exit on key down", ""},
        {"exitonmousedown", OPT_BOOL | OPT_EXPERT, {&exit_on_mousedown}, "exit on mouse down", ""},
        {"loop", OPT_INT | HAS_ARG | OPT_EXPERT, {&loop}, "set number of times the playback shall be looped",
         "loop count"},
        {"framedrop", OPT_BOOL | OPT_EXPERT, {&frame_drop}, "drop frames when cpu is too slow", ""},
        {"infbuf", OPT_BOOL | OPT_EXPERT, {&infinite_buffer},
         "don't limit the input buffer size (useful with realtime streams)", ""},
        {"window_title", OPT_STRING | HAS_ARG, {&window_title}, "set window title", "window title"},
        {"left", OPT_INT | HAS_ARG | OPT_EXPERT, {&screen_left}, "set the x position for the left of the window",
         "x pos"},
        {"top", OPT_INT | HAS_ARG | OPT_EXPERT, {&screen_top}, "set the y position for the top of the window", "y pos"},
#if CONFIG_AVFILTER
        {"vf", OPT_EXPERT | HAS_ARG, {.func_arg = opt_add_vfilter}, "set video filters", "filter_graph"},
        {"af", OPT_STRING | HAS_ARG, {&afilters}, "set audio filters", "filter_graph"},
#endif
        {"rdftspeed", OPT_INT | HAS_ARG | OPT_AUDIO | OPT_EXPERT, {&rdft_speed}, "rdft speed", "msecs"},
        {"showmode", HAS_ARG, {.func_arg = opt_show_mode}, "select show mode (0 = video, 1 = waves, 2 = RDFT)", "mode"},
        {"default", HAS_ARG | OPT_AUDIO | OPT_VIDEO | OPT_EXPERT, {.func_arg = opt_default}, "generic catch all option",
         ""},
        {"i", OPT_BOOL, {&dummy}, "read specified file", "input_file"},
        {"codec", HAS_ARG, {.func_arg = opt_codec}, "force decoder", "decoder_name"},
        {"acodec", HAS_ARG | OPT_STRING | OPT_EXPERT, {&audio_codec_name}, "force audio decoder", "decoder_name"},
        {"scodec", HAS_ARG | OPT_STRING | OPT_EXPERT, {&subtitle_codec_name}, "force subtitle decoder", "decoder_name"},
        {"vcodec", HAS_ARG | OPT_STRING | OPT_EXPERT, {&video_codec_name}, "force video decoder", "decoder_name"},
        {"autorotate", OPT_BOOL, {&autorotate}, "automatically rotate video", ""},
        {"find_stream_info", OPT_BOOL | OPT_INPUT | OPT_EXPERT, {&find_stream_info},
         "read and decode the streams to fill missing information with heuristics"},
        {"filter_threads", HAS_ARG | OPT_INT | OPT_EXPERT, {&filter_nbthreads}, "number of filter threads per graph"},
        {NULL,},
};

static void show_usage(void) {
    av_log(NULL, AV_LOG_INFO, "Simple media player\n");
    av_log(NULL, AV_LOG_INFO, "usage: %s [options] input_file\n", program_name);
    av_log(NULL, AV_LOG_INFO, "\n");
}

void show_help_default(const char *opt, const char *arg) {
    av_log_set_callback(log_callback_help);
    show_usage();
    show_help_options(options, "Main options:", 0, OPT_EXPERT, 0);
    show_help_options(options, "Advanced options:", OPT_EXPERT, 0, 0);
    printf("\n");
    show_help_children(avcodec_get_class(), AV_OPT_FLAG_DECODING_PARAM);
    show_help_children(avformat_get_class(), AV_OPT_FLAG_DECODING_PARAM);
#if !CONFIG_AVFILTER
    show_help_children(sws_get_class(), AV_OPT_FLAG_ENCODING_PARAM);
#else
    show_help_children(avfilter_get_class(), AV_OPT_FLAG_FILTERING_PARAM);
#endif
    printf("\nWhile playing:\n"
           "q, ESC              quit\n"
           "f                   toggle full screen\n"
           "p, SPC              pause\n"
           "m                   toggle mute\n"
           "9, 0                decrease and increase volume respectively\n"
           "/, *                decrease and increase volume respectively\n"
           "a                   cycle audio channel in the current program\n"
           "v                   cycle video channel\n"
           "t                   cycle subtitle channel in the current program\n"
           "c                   cycle program\n"
           "w                   cycle video filters or show modes\n"
           "s                   activate frame-step mode\n"
           "left/right          seek backward/forward 10 seconds or to custom interval if -seek_interval is set\n"
           "down/up             seek backward/forward 1 minute\n"
           "page down/page up   seek backward/forward 10 minutes\n"
           "right mouse click   seek to percentage in file corresponding to fraction of width\n"
           "left double-click   toggle full screen\n"
    );
}

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
        show_usage();
        av_log(NULL, AV_LOG_FATAL, "An input file must be specified\n");
        av_log(NULL, AV_LOG_FATAL,
               "Use -h to get full help or, even better, run 'man %s'\n", program_name);
        exit(1);
    }

    if (display_disable) {
        video_disable = 1;
    }
    flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
    if (audio_disable)
        flags &= ~SDL_INIT_AUDIO;
    else {
        /* Try to work around an occasional ALSA buffer underflow issue when the
         * period size is NPOT due to ALSA resampling by forcing the buffer size. */
        if (!SDL_getenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE"))
            SDL_setenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE", "1", 1);
    }
    if (display_disable)
        flags &= ~SDL_INIT_VIDEO;
    if (SDL_Init(flags)) {
        av_log(NULL, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
        av_log(NULL, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
        exit(1);
    }

    SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
    SDL_EventState(SDL_USEREVENT, SDL_IGNORE);

    av_init_packet(&flush_pkt);
    flush_pkt.data = (uint8_t *) &flush_pkt;

    if (!display_disable) {
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
        window = SDL_CreateWindow(program_name, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, default_width,
                                  default_height, flags);
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

    event_loop(is);

    /* never returns */

    return 0;
}
