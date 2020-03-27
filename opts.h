//
// Created by chen zhenhui on 2020/3/27.
//

#ifndef PLAYGROUND_OPTS_H
#define PLAYGROUND_OPTS_H

#include "config.h"
#include "cmdutils.h"

extern AVInputFormat *file_iformat;

#if CONFIG_AVFILTER
extern const char **vfilters_list;
extern int nb_vfilters;
extern char *afilters;
extern int opt_add_vfilter(void *optctx, const char *opt, const char *arg);
#endif

extern int opt_frame_size(void *optctx, const char *opt, const char *arg);

extern int opt_width(void *optctx, const char *opt, const char *arg);

extern int opt_height(void *optctx, const char *opt, const char *arg);

extern int opt_format(void *optctx, const char *opt, const char *arg);

extern int opt_frame_pix_fmt(void *optctx, const char *opt, const char *arg);

extern int opt_sync(void *optctx, const char *opt, const char *arg);

extern int opt_seek(void *optctx, const char *opt, const char *arg);

extern int opt_duration(void *optctx, const char *opt, const char *arg);

extern int opt_show_mode(void *optctx, const char *opt, const char *arg);

extern void opt_input_file(void *optctx, const char *filename);

extern int opt_codec(void *optctx, const char *opt, const char *arg);


#endif //PLAYGROUND_OPTS_H
