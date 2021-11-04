#ifndef EVENT_H_
#define EVENT_H_

#include "timer.h"
#include "thread.h"
#include "wot.h"
#include "netapi.h"

typedef struct event_ctx
{
    u_long accuracy;//计时器精度
    int32_t stop;//停止标志
    struct wot_ctx wot;//时间轮
    struct timer_ctx timer;//计时器
    struct chan_ctx chfree;//延迟释放
    struct thread_ctx thwot;//超时线程
    struct thread_ctx thfree;
    struct netev_ctx *netev;//网络
}event_ctx;
/*
* \brief             初始化
* \param ulaccuracy  时间精度 0 默认 10毫秒
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
* \param pchan     chan 接收EV_ACCEPT消息, 接收到的socket通过event_addsock加进管理,
 *                 需要手动释放ev_ctx(ev_sock_ctx)
* \param uichancnt pchan 数量
* \param phost     ip
* \param usport    port
* \return          INVALID_SOCK 失败
*/
static inline SOCKET event_listener(struct event_ctx *pctx, struct chan_ctx *pchan,
    const uint32_t uichancnt, const char *phost, const uint16_t usport)
{
    return netev_listener(pctx->netev, pchan, uichancnt, phost, usport);
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
    return netev_connecter(pctx->netev, pchan, phost, usport);
};
/*
* \brief            将已有SOCKET加进去,失败关闭fd句柄, EV_ACCEPT 和自定义socket
* \param fd         socket句柄
* \return           ERR_OK 成功
*/
static inline int32_t event_addsock(struct event_ctx *pctx, SOCKET fd)
{
    return netev_addsock(pctx->netev, fd);
};
/*
* \brief              开始接收数据。在调用event_addsock成功或收到EV_CONNECT消息后调用一次
*                     失败关闭fd句柄
* \param pchan        接收EV_RECV、EV_SEND、EV_CLOSE消息
* \param ipostsendev  0 不发送EV_SEND
* \return             sock_ctx
*/
static inline struct sock_ctx *event_enablerw(struct event_ctx *pctx, SOCKET fd, struct chan_ctx *pchan, const int32_t ipostsendev)
{
    return netev_enable_rw(pctx->netev, fd, pchan, ipostsendev);
};
/*
* \brief              释放sock_ctx
* \param pctx         event_ctx
* \param psockctx     psockctx
*/
static inline void event_freesock(struct event_ctx *pctx, struct sock_ctx *psockctx)
{
    if (ERR_OK == _sock_can_free(psockctx))
    {
        _sock_free(psockctx);
    }
    else
    {
        event_timeout(pctx, &pctx->chfree, 5, psockctx);
    }
};
/*
* \brief              关闭socket
* \param psockctx     sock_ctx
*/
static inline void event_closesock(struct sock_ctx *psockctx)
{
    sock_close(psockctx);
};
/*
* \brief              更改接收数据的pchan
* \param psockctx     event_enable_rw 返回值
* \param pchan        chan_ctx
*/
static inline void event_changechan(struct sock_ctx *psockctx, struct chan_ctx *pchan)
{
    sock_change_chan(psockctx, pchan);
};
static inline struct buffer_ctx *event_recvbuf(struct sock_ctx *psockctx)
{
    return sock_recvbuf(psockctx);
};
static inline struct buffer_ctx *event_sendbuf(struct sock_ctx *psockctx)
{
    return sock_sendbuf(psockctx);
};
static inline SOCKET event_handle(struct sock_ctx *psockctx)
{
    return sock_handle(psockctx);
};
static inline int32_t event_send(struct sock_ctx *psockctx, void *pdata, const size_t uilens)
{
    return sock_send(psockctx, pdata, uilens);
};
static inline int32_t event_send_buf(struct sock_ctx *psockctx)
{
    return sock_send_buf(psockctx);
};

#endif//EVENT_H_
