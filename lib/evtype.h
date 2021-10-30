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
    int32_t code;       //ERR_OK 成功  EV_RECV  EV_SEND 的時候表示数据长度
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
    uint8_t socktpe;
    uint16_t port;
    SOCKET sock;
    struct buffer_ctx *buffer;
    char ip[IP_LENS];
}ev_sock_ctx;

static inline int32_t post_ev(struct chan_ctx *pchan, const uint8_t ucnt, struct ev_ctx *pev)
{
    if (1 == ucnt)
    {
        return chan_send(pchan, pev);
    }
    else
    {
        return chan_send(&pchan[rand() % ucnt], pev);
    }
};
static inline struct ev_sock_ctx *ev_sock_accept(SOCKET sock)
{
    struct ev_sock_ctx *pev = (struct ev_sock_ctx *)MALLOC(sizeof(struct ev_sock_ctx));
    ASSERTAB(NULL != pev, ERRSTR_MEMORY);
    pev->ev.code = ERR_OK;
    pev->ev.evtype = EV_ACCEPT;
    pev->sock = sock;
    pev->socktpe = SOCK_STREAM;

    return pev;
};
static inline struct ev_sock_ctx *ev_sock_connect(SOCKET sock, const int32_t ierr)
{
    struct ev_sock_ctx *pev = (struct ev_sock_ctx *)MALLOC(sizeof(struct ev_sock_ctx));
    ASSERTAB(NULL != pev, ERRSTR_MEMORY);
    pev->ev.code = ierr;
    pev->ev.evtype = EV_CONNECT;
    pev->sock = sock;
    pev->socktpe = SOCK_STREAM;

    return pev;
};
static inline struct ev_sock_ctx *ev_sock_close(SOCKET sock, struct buffer_ctx *pbuffer, const uint8_t utype)
{
    struct ev_sock_ctx *pev = (struct ev_sock_ctx *)MALLOC(sizeof(struct ev_sock_ctx));
    ASSERTAB(NULL != pev, ERRSTR_MEMORY);
    pev->ev.code = ERR_OK;
    pev->ev.evtype = EV_CLOSE;
    pev->sock = sock;
    pev->buffer = pbuffer;
    pev->socktpe = utype;

    return pev;
};
static inline struct ev_sock_ctx *ev_sock_recv(SOCKET sock, const int32_t ilens, struct buffer_ctx *pbuffer, const uint8_t utype)
{
    struct ev_sock_ctx *pev = (struct ev_sock_ctx *)MALLOC(sizeof(struct ev_sock_ctx));
    ASSERTAB(NULL != pev, ERRSTR_MEMORY);
    pev->ev.code = ilens;
    pev->ev.evtype = EV_RECV;
    pev->sock = sock;
    pev->buffer = pbuffer;
    pev->socktpe = utype;

    return pev;
};

#endif//EVTYPE_H_
