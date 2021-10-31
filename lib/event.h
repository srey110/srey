#ifndef EVENT_H_
#define EVENT_H_

#include "timer.h"
#include "thread.h"
#include "wot.h"
#include "iocp/iocp.h"
#include "epoll/epoll.h"
#include "evport/evport.h"
#include "kqueue/kqueue.h"

typedef struct event_ctx
{
    u_long accuracy;//计时器精度
    volatile atomic_t stop;//停止标志
    struct wot_ctx wot;//时间轮
    struct timer_ctx timer;//计时器
    struct chan_ctx chfree;//延迟释放
    struct thread_ctx thwot;//超时线程
    struct thread_ctx thfree;
    struct netev_ctx netev;//网络
}event_ctx;
/*
* \brief             初始化
* \param ulaccuracy  时间精度
*/
void event_init(struct event_ctx *pctx, const u_long ulaccuracy);
/*
* \brief             停止并释放
*/
void event_free(struct event_ctx *pctx);
/*
* \brief             开始运行
*/
void event_loop(struct event_ctx *pctx);
/*
* \brief             当前tick
*/
static inline u_long event_tick(struct event_ctx *pctx)
{
    return (u_long)(timer_nanosec(&pctx->timer) / pctx->accuracy);
};
/*
* \brief           添加一超时事件
* \param  pchan    接收超时消息, 需要手动释放 ev_ctx（ev_time_ctx）
* \param  uitick   超时时间  多少个tick
* \param  pdata    用户数据
* \return          ERR_OK 成功
*/
static inline void event_timeout(struct event_ctx *pctx,
    struct chan_ctx *pchan, const uint32_t uitick, const void *pdata)
{
    wot_add(&pctx->wot, pchan, event_tick(pctx), uitick, pdata);
};
/*
* \brief           新建一监听
* \param pchan     chan 接收EV_ACCEPT消息, 需要手动释放ev_ctx(ev_sock_ctx)
* \param uchancnt  pchan 数量
* \param phost     ip
* \param usport    port
* \return          INVALID_SOCK 失败
*/
static inline SOCKET event_listener(struct event_ctx *pctx, struct chan_ctx *pchan,
    const uint16_t uchancnt, const char *phost, const uint16_t usport)
{
    return netev_listener(&pctx->netev, pchan, uchancnt, phost, usport);
};
/*
* \brief            新建一连接
* \param pchan      chan 接收EV_CONNECT消息, 需要手动释放ev_ctx(ev_sock_ctx)
* \param phost      ip
* \param usport     port
* \return           INVALID_SOCK 失败
*/
static inline SOCKET event_connecter(struct event_ctx *pctx, struct chan_ctx *pchan,
    const char *phost, const uint16_t usport)
{
    return netev_connecter(&pctx->netev, pchan, phost, usport);
};
/*
* \brief            将已有SOCKET加进去,失败关闭fd句柄
* \param fd         socket句柄
* \return           ERR_OK 成功
*/
static inline int32_t event_addsock(struct event_ctx *pctx, SOCKET fd)
{
    return netev_addsock(&pctx->netev, fd);
};
/*
* \brief              开始接收数据。在调用event_addsock成功或收到 EV_ACCEPT、EV_CONNECT消息后调用一次
*                     失败关闭fd句柄
* \param pchan        接收EV_RECV、EV_SEND、EV_CLOSE消息
* \param upostsendev  0 不发送EV_SEND
* \return             sock_ctx
*/
static inline struct sock_ctx *event_enable_rw(SOCKET fd, struct chan_ctx *pchan, const uint16_t upostsendev)
{
    return netev_enable_rw(fd, pchan, upostsendev);
};
/*
* \brief              开始接收数据。在调用event_addsock成功或收到 EV_ACCEPT、EV_CONNECT消息后调用一次
*                     失败关闭fd句柄
* \param pchan        接收EV_RECV、EV_SEND、EV_CLOSE消息
* \param upostsendev  0 不发送EV_SEND
* \return             sender_ctx
*/
static inline void event_sock_free(struct event_ctx *pctx, struct sock_ctx *psockctx)
{
    if (ERR_OK == _sock_can_free(psockctx))
    {
        _sock_free(psockctx);
    }
    else
    {
        event_timeout(pctx, &pctx->chfree, 10, psockctx);
    }
}

#endif//EVENT_H_
