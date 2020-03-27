#ifndef __PLAYGROUND_CLOCK_H
#define __PLAYGROUND_CLOCK_H

typedef struct Clock {
    double pts;           /* clock base */
    double pts_drift;     /* clock base minus time at which we updated the clock */
    double last_updated;
    double speed;
    int serial;           /* clock is based on a packet with this serial */
    int paused;
    int *queue_serial;    /* pointer to the current packet queue serial, used for obsolete clock detection */
} Clock;


extern double get_clock(Clock *c);

extern void set_clock_at(Clock *c, double pts, int serial, double time);

extern void set_clock(Clock *c, double pts, int serial);

extern void set_clock_speed(Clock *c, double speed);

extern void init_clock(Clock *c, int *queue_serial);

extern void sync_clock_to_slave(Clock *c, Clock *slave);

#endif