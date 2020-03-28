//
// Created by chen zhenhui on 2020/3/28.
//

#ifndef FFPLAY_UTILS_H
#define FFPLAY_UTILS_H

#include <libavutil/rational.h>


typedef struct {
    int x, y;
    int w, h;
} Rect;


extern void calculate_display_rect(Rect *rect,
                                   int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                                   int pic_width, int pic_height, AVRational pic_sar);

#endif //FFPLAY_UTILS_H
