#ifndef SOCK_H_
#define SOCK_H_

#include "evtype.h"
#include "netaddr.h"
#include "mutex.h"
#include "chan.h"
#include "utils.h"

typedef struct netio_ctx
{
    int32_t threadcnt;
    struct thread_ctx *thread;
}netio_ctx;
typedef struct sock_ctx
{
    uint8_t listener;//是否为监听socket    
    uint8_t postsendev;//是否发送EV_SEND
    uint8_t socktype;//SOCK_STREAM  SOCK_DGRAM
    volatile atomic_t ref;//引用计数
    volatile atomic_t refsend;//引用计数
    volatile atomic_t closed;
    uint32_t reffree;
    uint32_t chancnt;  //chan数量，除了listen，其他的都为1
    void *exdata;      //IOCP 中用于存储OVERLAPPED
    struct chan_ctx *chan;//chan_ctx
    mutex_ctx *chanmutex;
    struct buffer_ctx *bufrecv;
    struct buffer_ctx *bufsend;
    SOCKET sock;     //socket    
    struct netaddr_ctx addr;
}sock_ctx;
static inline struct sock_ctx *sockctx_new(const uint8_t uclistener, const uint8_t socktype)
{
    struct sock_ctx *pctx = (struct sock_ctx *)MALLOC(sizeof(struct sock_ctx));
    ASSERTAB(NULL != pctx, ERRSTR_MEMORY);
    pctx->sock = INVALID_SOCK;
    pctx->listener = uclistener;
    pctx->socktype = socktype;
    pctx->chancnt = 1;
    pctx->ref = 0;
    pctx->refsend = 0;
    pctx->closed = 0;
    pctx->reffree = 3;
    pctx->chan = NULL;
    pctx->exdata = NULL;
    if (0 == pctx->listener)
    {
        pctx->chanmutex = (mutex_ctx *)MALLOC(sizeof(mutex_ctx));
        ASSERTAB(NULL != pctx->chanmutex, ERRSTR_MEMORY);
        mutex_init(pctx->chanmutex);
        pctx->bufrecv = (struct buffer_ctx *)MALLOC(sizeof(struct buffer_ctx));
        ASSERTAB(NULL != pctx->bufrecv, ERRSTR_MEMORY);
        buffer_init(pctx->bufrecv);
        if (SOCK_STREAM == pctx->socktype)
        {
            pctx->bufsend = (struct buffer_ctx *)MALLOC(sizeof(struct buffer_ctx));
            ASSERTAB(NULL != pctx->bufsend, ERRSTR_MEMORY);
            buffer_init(pctx->bufsend);
        }
        else
        {
            pctx->bufsend = NULL;
        }
    }

    return pctx;
}
static inline void sockctx_free(struct sock_ctx *pctx, const uint8_t closesock)
{
    if (0 == pctx->listener)
    {
        if (SOCK_STREAM == pctx->socktype)
        {
            buffer_free(pctx->bufsend);
        }
        buffer_free(pctx->bufrecv);
        mutex_free(pctx->chanmutex);

        SAFE_FREE(pctx->bufsend);
        SAFE_FREE(pctx->bufrecv);
        SAFE_FREE(pctx->chanmutex);        
    }
    if (0 != closesock)
    {
        SAFE_CLOSESOCK(pctx->sock);
    }
    SAFE_FREE(pctx->exdata);
    SAFE_FREE(pctx);
}
static inline int32_t sockctx_post_ev(struct sock_ctx *pctx, struct ev_ctx *pev)
{
    int32_t irtn;
    if (0 == pctx->listener)
    {
        mutex_lock(pctx->chanmutex);
        irtn = chan_send(pctx->chan, pev);
        mutex_unlock(pctx->chanmutex);
    }
    else
    {
        if (1 == pctx->chancnt)
        {
            irtn = chan_send(pctx->chan, pev);
        }
        else
        {
            irtn = chan_send(&pctx->chan[rand() % pctx->chancnt], pev);
        }
    }
    
    return irtn;
}
static inline void sockctx_change_chan(struct sock_ctx *pctx, struct chan_ctx *pchan)
{
    if (0 == pctx->listener)
    {
        mutex_lock(pctx->chanmutex);
        pctx->chan = pchan;
        mutex_unlock(pctx->chanmutex);
    }
}

#endif//SOCK_H_
