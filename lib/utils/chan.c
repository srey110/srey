#include "utils/chan.h"
//https://github.com/tylertreat/chan/tree/master
QUEUE_DECL(buf_ctx, qu_buf);
struct chan_ctx {
    int32_t  buffered;   /* 是否带缓存（只写一次，无需原子） */
    atomic_t closed;     /* 关闭标志：原子读，允许无锁轮询 */
    atomic_t r_waiting;  /* 等待接收的线程数：原子读，允许无锁探测 */
    atomic_t w_waiting;  /* 等待发送的线程数：原子读，允许无锁探测 */
    buf_ctx data;        /* 无缓存模式：当前传递的数据 */
    qu_buf_ctx qudata;   /* 缓存模式：队列 */
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
    } else {
        chan->buffered = 0;
        mutex_init(&chan->r_mu);
        mutex_init(&chan->w_mu);
    }
    mutex_init(&chan->m_mu);
    cond_init(&chan->r_cond);
    cond_init(&chan->w_cond);
    ATOMIC_SET(&chan->closed, 0);
    ATOMIC_SET(&chan->r_waiting, 0);
    ATOMIC_SET(&chan->w_waiting, 0);
    return chan;
}
void chan_free(chan_ctx *chan) {
    if (chan->buffered) {
        qu_buf_free(&chan->qudata);
    } else {
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
    if (!ATOMIC_GET(&chan->closed)) {
        ATOMIC_SET(&chan->closed, 1);
        cond_broadcast(&chan->r_cond);
        cond_broadcast(&chan->w_cond);
    }
    mutex_unlock(&chan->m_mu);
}
int32_t chan_is_closed(chan_ctx *chan) {
    /* closed 是 atomic_t：直接原子读，无需持锁 */
    return (int32_t)ATOMIC_GET(&chan->closed);
}
static int32_t _buffered_chan_send(chan_ctx *chan, buf_ctx *buf) {
    mutex_lock(&chan->m_mu);
    while (qu_buf_size(&chan->qudata) == qu_buf_maxsize(&chan->qudata)) {
        if (ATOMIC_GET(&chan->closed)) {
            mutex_unlock(&chan->m_mu);
            return ERR_FAILED;
        }
        //Block until something is removed.
        ATOMIC_ADD(&chan->w_waiting, 1);
        cond_wait(&chan->w_cond, &chan->m_mu);
        ATOMIC_ADD(&chan->w_waiting, (atomic_t)-1);
    }
    if (ATOMIC_GET(&chan->closed)) {
        mutex_unlock(&chan->m_mu);
        return ERR_FAILED;
    }
    qu_buf_push(&chan->qudata, buf);
    if (ATOMIC_GET(&chan->r_waiting) > 0) {
        //Signal waiting reader.
        cond_signal(&chan->r_cond);
    }
    mutex_unlock(&chan->m_mu);
    return ERR_OK;
}
static void *_buffered_chan_recv(chan_ctx *chan, size_t *lens) {
    mutex_lock(&chan->m_mu);
    while (0 == qu_buf_size(&chan->qudata)) {
        if (ATOMIC_GET(&chan->closed)) {
            mutex_unlock(&chan->m_mu);
            return NULL;
        }
        //Block until something is added.
        ATOMIC_ADD(&chan->r_waiting, 1);
        cond_wait(&chan->r_cond, &chan->m_mu);
        ATOMIC_ADD(&chan->r_waiting, (atomic_t)-1);
    }
    buf_ctx *msg = qu_buf_pop(&chan->qudata);
    if (ATOMIC_GET(&chan->w_waiting) > 0) {
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
    if (ATOMIC_GET(&chan->closed)) {
        mutex_unlock(&chan->m_mu);
        mutex_unlock(&chan->w_mu);
        return ERR_FAILED;
    }
    chan->data = *buf;
    ATOMIC_ADD(&chan->w_waiting, 1);
    if (ATOMIC_GET(&chan->r_waiting) > 0) {
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
    while (!ATOMIC_GET(&chan->closed)
        && !ATOMIC_GET(&chan->w_waiting)) {
        // Block until writer has set chan->data.
        ATOMIC_ADD(&chan->r_waiting, 1);
        cond_wait(&chan->r_cond, &chan->m_mu);
        ATOMIC_ADD(&chan->r_waiting, (atomic_t)-1);
    }
    if (ATOMIC_GET(&chan->closed)) {
        mutex_unlock(&chan->m_mu);
        mutex_unlock(&chan->r_mu);
        return NULL;
    }
    void *msg = chan->data.data;
    *lens = chan->data.lens;
    ATOMIC_ADD(&chan->w_waiting, (atomic_t)-1);
    //Signal waiting writer.
    cond_signal(&chan->w_cond);
    mutex_unlock(&chan->m_mu);
    mutex_unlock(&chan->r_mu);
    return msg;
}
int32_t chan_send(chan_ctx *chan, void *data, size_t lens, int32_t copy) {
    if (ATOMIC_GET(&chan->closed)) {
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
    } else {
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
    /* w_waiting 是 atomic_t，无锁读：有发送者等待即可接收 */
    return ATOMIC_GET(&chan->w_waiting) > 0;
}
int32_t chan_can_send(chan_ctx *chan) {
    if (chan->buffered) {
        /* 缓存队列大小需要持锁才能一致读 */
        int32_t send;
        mutex_lock(&chan->m_mu);
        send = qu_buf_size(&chan->qudata) < qu_buf_maxsize(&chan->qudata);
        mutex_unlock(&chan->m_mu);
        return send;
    }
    /* r_waiting 是 atomic_t，无锁读：有接收者等待才可发送 */
    return ATOMIC_GET(&chan->r_waiting) > 0;
}
