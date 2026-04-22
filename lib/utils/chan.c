#include "utils/chan.h"
//https://github.com/tylertreat/chan/tree/master
QUEUE_DECL(buf_ctx, qu_buf);
struct chan_ctx {
    int32_t buffered;//是否带缓存
    int32_t closed;//是否关闭
    int32_t r_waiting;
    int32_t w_waiting;
    buf_ctx data;//不带缓存的数据
    qu_buf_ctx qudata;//带缓存的数据
    mutex_ctx r_mu;
    mutex_ctx w_mu;
    mutex_ctx m_mu;
    cond_ctx r_cond;
    cond_ctx w_cond;
};

chan_ctx *chan_init(uint32_t capacity) {
    chan_ctx *chan;
    CALLOC(chan, 1, sizeof(chan_ctx));
    if (capacity > 0) {
        chan->buffered = 1;
        qu_buf_init(&chan->qudata, capacity);
    }
    else {
        chan->buffered = 0;
        mutex_init(&chan->r_mu);
        mutex_init(&chan->w_mu);
    }
    mutex_init(&chan->m_mu);
    cond_init(&chan->r_cond);
    cond_init(&chan->w_cond);
    chan->closed = 0;
    chan->r_waiting = 0;
    chan->w_waiting = 0;
    return chan;
}
void chan_free(chan_ctx *chan) {
    if (chan->buffered) {
        qu_buf_free(&chan->qudata);
    }
    else {
        mutex_free(&chan->r_mu);
        mutex_free(&chan->w_mu);
    }
    mutex_free(&chan->m_mu);
    cond_free(&chan->r_cond);
    cond_free(&chan->w_cond);
    FREE(chan);
}
void chan_close(chan_ctx *chan) {
    mutex_lock(&chan->m_mu);
    if (!chan->closed) {
        chan->closed = 1;
        cond_broadcast(&chan->r_cond);
        cond_broadcast(&chan->w_cond);
    }
    mutex_unlock(&chan->m_mu);
}
int32_t chan_is_closed(chan_ctx *chan) {
    int32_t closed;
    mutex_lock(&chan->m_mu);
    closed = chan->closed;
    mutex_unlock(&chan->m_mu);
    return closed;
}
static int32_t _buffered_chan_send(chan_ctx *chan, buf_ctx *buf) {
    mutex_lock(&chan->m_mu);
    while (qu_buf_size(&chan->qudata) == qu_buf_maxsize(&chan->qudata)) {
        if (chan->closed) {
            mutex_unlock(&chan->m_mu);
            return ERR_FAILED;
        }
        //Block until something is removed.
        chan->w_waiting++;
        cond_wait(&chan->w_cond, &chan->m_mu);
        chan->w_waiting--;
    }
    if (chan->closed) {
        mutex_unlock(&chan->m_mu);
        return ERR_FAILED;
    }
    qu_buf_push(&chan->qudata, buf);
    if (chan->r_waiting > 0) {
        //Signal waiting reader.
        cond_signal(&chan->r_cond);
    }
    mutex_unlock(&chan->m_mu);
    return ERR_OK;
}
static void *_buffered_chan_recv(chan_ctx *chan, size_t *lens) {
    mutex_lock(&chan->m_mu);
    while (0 == qu_buf_size(&chan->qudata)) {
        if (chan->closed) {
            mutex_unlock(&chan->m_mu);
            return NULL;
        }
        //Block until something is added.
        chan->r_waiting++;
        cond_wait(&chan->r_cond, &chan->m_mu);
        chan->r_waiting--;
    }
    buf_ctx *msg = qu_buf_pop(&chan->qudata);
    if (chan->w_waiting > 0) {
        //Signal waiting writer.
        cond_signal(&chan->w_cond);
    }
    mutex_unlock(&chan->m_mu);
    *lens = msg->lens;
    return msg->data;
}
static int32_t _unbuffered_chan_send(chan_ctx *chan, buf_ctx *buf) {
    mutex_lock(&chan->w_mu);
    mutex_lock(&chan->m_mu);
    if (chan->closed) {
        mutex_unlock(&chan->m_mu);
        mutex_unlock(&chan->w_mu);
        return ERR_FAILED;
    }
    chan->data = *buf;
    chan->w_waiting++;
    if (chan->r_waiting > 0) {
        //Signal waiting reader.
        cond_signal(&chan->r_cond);
    }
    cond_wait(&chan->w_cond, &chan->m_mu);
    mutex_unlock(&chan->m_mu);
    mutex_unlock(&chan->w_mu);
    return ERR_OK;
}
static void *_unbuffered_chan_recv(chan_ctx *chan, size_t *lens) {
    mutex_lock(&chan->r_mu);
    mutex_lock(&chan->m_mu);
    while (!chan->closed
        && !chan->w_waiting) {
        // Block until writer has set chan->data.
        chan->r_waiting++;
        cond_wait(&chan->r_cond, &chan->m_mu);
        chan->r_waiting--;
    }
    if (chan->closed) {
        mutex_unlock(&chan->m_mu);
        mutex_unlock(&chan->r_mu);
        return NULL;
    }
    void *msg = chan->data.data;
    *lens = chan->data.lens;
    chan->w_waiting--;
    //Signal waiting writer.
    cond_signal(&chan->w_cond);
    mutex_unlock(&chan->m_mu);
    mutex_unlock(&chan->r_mu);
    return msg;
}
int32_t chan_send(chan_ctx *chan, void *data, size_t lens, int32_t copy) {
    if (chan->closed) {
        return ERR_FAILED;
    }
    buf_ctx buf;
    buf.lens = lens;
    if (copy) {
        char *msg;
        MALLOC(msg, lens + 1);
        memcpy(msg, data, lens);
        msg[lens] = '\0';
        buf.data = msg;
    }
    else {
        buf.data = data;
    }
    int32_t rtn = chan->buffered ? _buffered_chan_send(chan, &buf) : _unbuffered_chan_send(chan, &buf);
    if (ERR_OK != rtn) {
        if (copy) {
            FREE(buf.data);
        }
    }
    return rtn;
}
void *chan_recv(chan_ctx *chan, size_t *lens) {
    return chan->buffered ? _buffered_chan_recv(chan, lens) : _unbuffered_chan_recv(chan, lens);
}
uint32_t chan_size(chan_ctx *chan) {
    uint32_t size = 0;
    if (chan->buffered) {
        mutex_lock(&chan->m_mu);
        size = qu_buf_size(&chan->qudata);
        mutex_unlock(&chan->m_mu);
    }
    return size;
}
int32_t chan_can_recv(chan_ctx *chan) {
    if (chan->buffered) {
        return chan_size(chan) > 0;
    }
    int32_t sender;
    mutex_lock(&chan->m_mu);
    sender = chan->w_waiting > 0;
    mutex_unlock(&chan->m_mu);
    return sender;
}
int32_t chan_can_send(chan_ctx *chan) {
    int32_t send;
    if (chan->buffered) {
        //Can send if buffered channel is not full.
        mutex_lock(&chan->m_mu);
        send = qu_buf_size(&chan->qudata) < qu_buf_maxsize(&chan->qudata);
        mutex_unlock(&chan->m_mu);
    }
    else {
        //Can send if unbuffered channel has receiver.
        mutex_lock(&chan->m_mu);
        send = chan->r_waiting > 0;
        mutex_unlock(&chan->m_mu);
    }
    return send;
}
