#include "frame_queue.h"


int frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last)
{
    int err, i;
    memset(f, 0, sizeof(FrameQueue));

    err = pthread_mutex_init(&f->mutex, NULL);
    if (err) {
        av_log(NULL, AV_LOG_FATAL, "pthread_mutex_init(): error");
        return AVERROR(ENOMEM);
    }

    err = pthread_cond_init(&f->cond, NULL);
    if (err) {
        av_log(NULL, AV_LOG_FATAL, "pthread_cond_init(): error");
        return AVERROR(ENOMEM);
    }
    f->pktq = pktq;
    f->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
    f->keep_last = !!keep_last;
    for (i = 0; i < f->max_size; i++)
        if (!(f->queue[i].frame = av_frame_alloc()))
            return AVERROR(ENOMEM);
    return 0;
}

void frame_queue_destroy(FrameQueue *f)
{
    int i;
    for (i = 0; i < f->max_size; i++) {
        Frame *vp = &f->queue[i];
        frame_queue_unref_item(vp);
        av_frame_free(&vp->frame);
    }
    pthread_mutex_destroy(&f->mutex);
    pthread_cond_destroy(&f->cond);
}

void frame_queue_signal(FrameQueue *f)
{
    pthread_mutex_lock(&f->mutex);
    pthread_cond_signal(&f->cond);
    pthread_mutex_unlock(&f->mutex);
}

Frame *frame_queue_peek(FrameQueue *f)
{
    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

Frame *frame_queue_peek_next(FrameQueue *f)
{
    return &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
}

Frame *frame_queue_peek_last(FrameQueue *f)
{
    return &f->queue[f->rindex];
}

Frame *frame_queue_peek_writable(FrameQueue *f)
{
    /* wait until we have space to put a new frame */
    pthread_mutex_lock(&f->mutex);
    while (f->size >= f->max_size &&
           !f->pktq->abort_request) {
        pthread_cond_wait(&f->cond, &f->mutex);
    }
    pthread_mutex_unlock(&f->mutex);

    if (f->pktq->abort_request)
        return NULL;

    return &f->queue[f->windex];
}

Frame *frame_queue_peek_readable(FrameQueue *f)
{
    /* wait until we have a readable a new frame */
    pthread_mutex_lock(&f->mutex);
    while (f->size - f->rindex_shown <= 0 &&
           !f->pktq->abort_request) {
        pthread_cond_wait(&f->cond, &f->mutex);
    }
    pthread_mutex_unlock(&f->mutex);

    if (f->pktq->abort_request)
        return NULL;

    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

void frame_queue_push(FrameQueue *f)
{
    if (++f->windex == f->max_size)
        f->windex = 0;
    pthread_mutex_lock(&f->mutex);
    f->size++;
    pthread_cond_signal(&f->cond);
    pthread_mutex_unlock(&f->mutex);
}

void frame_queue_next(FrameQueue *f)
{
    if (f->keep_last && !f->rindex_shown) {
        f->rindex_shown = 1;
        return;
    }
    frame_queue_unref_item(&f->queue[f->rindex]);
    if (++f->rindex == f->max_size)
        f->rindex = 0;
    pthread_mutex_lock(&f->mutex);
    f->size--;
    pthread_cond_signal(&f->cond);
    pthread_mutex_unlock(&f->mutex);
}

/* return the number of undisplayed frames in the queue */
int frame_queue_nb_remaining(FrameQueue *f)
{
    return f->size - f->rindex_shown;
}

void frame_queue_unref_item(Frame *vp)
{
    av_frame_unref(vp->frame);
    avsubtitle_free(&vp->sub);
}

/* return last shown position */
int64_t frame_queue_last_pos(FrameQueue *f)
{
    Frame *fp = &f->queue[f->rindex];
    if (f->rindex_shown && fp->serial == f->pktq->serial)
        return fp->pos;
    else
        return -1;
}

