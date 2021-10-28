#ifndef EVENT_H_
#define EVENT_H_

#include "macro.h"
#include "timer.h"
#include "thread.h"
#include "wot.h"
#include "iocp/iocp.h"
#include "epoll/epoll.h"
#include "evport/evport.h"
#include "kqueue/kqueue.h"

typedef struct server_ctx
{
    uint64_t accuracy;
    struct netio_ctx *netio;
    volatile atomic_t stop;//停止标志
    struct wot_ctx wot;
    struct timer_ctx timer;
    struct chan_ctx chan;
    struct chan_ctx chanfree;
    struct thread_ctx thread;
}server_ctx;
/*
* \brief               初始化
* \param ulaccuracy    时间精度
*/
void server_init(struct server_ctx *pctx, const uint64_t ulaccuracy);
/*
* \brief               停止并释放
*/
void server_free(struct server_ctx *pctx);
/*
* \brief               开始运行
*/
void server_run(struct server_ctx *pctx);
/*
* \brief               当前时间tick
*/
static inline u_long server_tick(struct server_ctx *pctx)
{
    return (u_long)(timer_nanosec(&pctx->timer) / pctx->accuracy);
}
/*
* \brief               添加一超时事件
* \param  pchan        接收超时消息, 需要手动释放 ev_ctx（twnode_ctx）
* \param  uitick       超时时间  多少个tick
* \param  pdata        用户数据
* \return              ERR_OK 成功
*/
static inline int32_t server_timeout(struct server_ctx *pctx,
    struct chan_ctx *pchan, const uint32_t uitick, const void *pdata)
{
    return wot_add(&pctx->wot, pchan, server_tick(pctx), uitick, pdata);
};
/*
* \brief           新建一监听
* \param pchan     chan 接收EV_ACCEPT消息, 需要手动释放ev_ctx
* \param uichancnt pchan 数量
* \param phost     ip
* \param usport    port
* \return          sock_ctx
*/
static inline struct sock_ctx *server_listen(struct server_ctx *pctx, struct chan_ctx *pchan,
    const uint32_t uichancnt, const char *phost, const uint16_t usport)
{
    return netio_listen(pctx->netio, pchan, uichancnt, phost, usport);
};
/*
* \brief          新建一连接
* \param pchan    chan 接收EV_CONNECT消息, 需要手动释放ev_ctx
* \param phost    ip
* \param usport   port
* \return         sock_ctx
*/
static inline struct sock_ctx *server_connect(struct server_ctx *pctx, struct chan_ctx *pchan,
    const char *phost, const uint16_t usport)
{
    return netio_connect(pctx->netio, pchan, phost, usport);
};
/*
* \brief          将已有SOCKET加进去
* \param fd       socket句柄
* \return         sock_ctx
*/
static inline struct sock_ctx *server_addsock(struct server_ctx *pctx, SOCKET fd)
{
    return netio_addsock(pctx->netio, fd);
};
/*
* \brief                 可读写。在调用server_addsock成功或收到 EV_ACCEPT、EV_CONNECT消息后调用一次
*                        需要手动释放ev_ctx
* \param pchan           chan 接收EV_RECV、EV_SEND、EV_CLOSE消息
* \param postsendev      是否投递EV_SEND消息, 0不投递
* \return                ERR_OK 成功
*/
static inline int32_t server_sock_enablerw(struct sock_ctx *psock, struct chan_ctx *pchan, const uint8_t postsendev)
{
    return netio_enable_rw(psock, pchan, postsendev);
};
/*
* \brief          释放sock_ctx  收到EV_CLOSE处理完成后调用
* \param fd       socket句柄
* \return         sock_ctx
*/
static inline void server_sock_free(struct server_ctx *pctx, struct sock_ctx *psock)
{
    SAFE_CLOSESOCK(psock->sock);//放在IOCP工作线程里，有时候GetQueuedCompletionStatus触发不了
    if (0 == ATOMIC_GET(&psock->ref)
        && 0 == ATOMIC_GET(&psock->refsend))
    {
        sockctx_free(psock, 0);
    }
    else
    {
        if (ERR_OK != server_timeout(pctx, &pctx->chanfree, 10, psock))
        {
            sockctx_free(psock, 0);
        }
    }
};
/*
* \brief          关闭socket  收到EV_CLOSE后调用server_sock_free释放资源
* \param fd       socket句柄
* \return         sock_ctx
*/
static inline void server_sock_close(struct server_ctx *pctx, struct sock_ctx *psock)
{
    if (0 == psock->listener)
    {
        SAFE_CLOSESOCK(psock->sock);
    }
    else
    {
        if (ATOMIC_CAS(&psock->closed, 0, 1))
        {
            SAFE_CLOSESOCK(psock->sock);
            server_sock_free(pctx, psock);
        }
    }
};
/*
* \brief          发送消息
* \return         ERR_OK 成功
*/
static inline int32_t server_sock_send(struct sock_ctx *psock, void *pdata, const size_t uilens)
{
    return netio_send(psock, pdata, uilens);
};
/*
* \brief          udp发送消息
* \return         ERR_OK 成功
*/
static inline int32_t server_sock_sendto(struct sock_ctx *psock, const char *phost, const uint16_t usport,
    IOV_TYPE *wsabuf, const size_t uicnt)
{
    return netio_sendto(psock, phost, usport, wsabuf, uicnt);
};

#endif//EVENT_H_
