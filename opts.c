//
// Created by chen zhenhui on 2020/3/27.
//

#include "opts.h"
#include "enums.h"

extern int64_t duration;

/* options specified by the user */
AVInputFormat *file_iformat;
const char *input_filename;

#if CONFIG_AVFILTER
const char **vfilters_list = NULL;
int nb_vfilters = 0;
char *afilters = NULL;
#endif


void opt_input_file(void *optctx, const char *filename) {
    if (input_filename) {
        av_log(NULL, AV_LOG_FATAL,
               "Argument '%s' provided as input filename, but '%s' was already specified.\n",
               filename, input_filename);
        exit(1);
    }
    if (!strcmp(filename, "-"))
        filename = "pipe:";
    input_filename = filename;
}
