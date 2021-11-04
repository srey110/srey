#ifndef EVTYPE_H_
#define EVTYPE_H_

#include "chan.h"

#define EV_TIME    0x01
#define EV_ACCEPT  0x02
#define EV_CONNECT 0x03
#define EV_CLOSE   0x04
#define EV_RECV    0x05
#define EV_SEND    0x06
typedef struct ev_ctx
{
    int32_t evtype;     //类型  
    int32_t result;     //ERR_OK 成功  EV_RECV  EV_SEND 的時候表示数据长度
}ev_ctx;
typedef struct ev_time_ctx
{
    struct ev_ctx ev;
    void *data;              //用户数据    
    struct chan_ctx *chan;  //接收消息的chan
    struct ev_time_ctx *next;
    u_long expires;         //超时时间
}ev_time_ctx;
typedef struct ev_sock_ctx
{
    struct ev_ctx ev;
    SOCKET sock;
    struct sock_ctx *sockctx;
}ev_sock_ctx;
static inline struct ev_sock_ctx *ev_sock_accept(SOCKET sock)
{
    struct ev_sock_ctx *pev = (struct ev_sock_ctx *)MALLOC(sizeof(struct ev_sock_ctx));
    ASSERTAB(NULL != pev, ERRSTR_MEMORY);
    pev->ev.result = ERR_OK;
    pev->ev.evtype = EV_ACCEPT;
    pev->sock = sock;
    pev->sockctx = NULL;
    return pev;
};
static inline struct ev_sock_ctx *ev_sock_connect(SOCKET sock, const int32_t ierr)
{
    struct ev_sock_ctx *pev = (struct ev_sock_ctx *)MALLOC(sizeof(struct ev_sock_ctx));
    ASSERTAB(NULL != pev, ERRSTR_MEMORY);
    pev->ev.result = ierr;
    pev->ev.evtype = EV_CONNECT;
    pev->sock = sock;
    pev->sockctx = NULL;
    return pev;
};
static inline struct ev_sock_ctx *ev_sock_close(SOCKET sock, struct sock_ctx *psockctx)
{
    struct ev_sock_ctx *pev = (struct ev_sock_ctx *)MALLOC(sizeof(struct ev_sock_ctx));
    ASSERTAB(NULL != pev, ERRSTR_MEMORY);
    pev->ev.result = ERR_OK;
    pev->ev.evtype = EV_CLOSE;
    pev->sock = sock;
    pev->sockctx = psockctx;
    return pev;
};
static inline struct ev_sock_ctx *ev_sock_recv(SOCKET sock, const int32_t ilens, 
    struct sock_ctx *psockctx)
{
    struct ev_sock_ctx *pev = (struct ev_sock_ctx *)MALLOC(sizeof(struct ev_sock_ctx));
    ASSERTAB(NULL != pev, ERRSTR_MEMORY);
    pev->ev.result = ilens;
    pev->ev.evtype = EV_RECV;
    pev->sock = sock;
    pev->sockctx = psockctx;
    return pev;
};
static inline struct ev_sock_ctx *ev_sock_send(SOCKET sock, const int32_t ilens,
    struct sock_ctx *psockctx)
{
    struct ev_sock_ctx *pev = (struct ev_sock_ctx *)MALLOC(sizeof(struct ev_sock_ctx));
    ASSERTAB(NULL != pev, ERRSTR_MEMORY);
    pev->ev.result = ilens;
    pev->ev.evtype = EV_SEND;
    pev->sock = sock;
    pev->sockctx = psockctx;
    return pev;
};

#endif//EVTYPE_H_
