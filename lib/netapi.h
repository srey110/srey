#ifndef NETAPI_H_
#define NETAPI_H_

#include "macro.h"

//UDP最大接收字节
#define MAX_RECV_FROM_IOV_SIZE   4096

struct netev_ctx *netev_new();
void netev_free(struct netev_ctx *pctx);
void netev_loop(struct netev_ctx *pctx);

SOCKET netev_listener(struct netev_ctx *piocpctx, struct chan_ctx *pchan,
    const uint32_t uichancnt, const char *phost, const uint16_t usport);
int32_t netev_addsock(struct netev_ctx *piocpctx, SOCKET fd);
SOCKET netev_connecter(struct netev_ctx *piocpctx, struct chan_ctx *pchan,
    const char *phost, const uint16_t usport);
struct sock_ctx *netev_enable_rw(struct netev_ctx *piocpctx, SOCKET fd,
    struct chan_ctx *pchan, const int32_t ipostsendev);

void sock_change_chan(struct sock_ctx *psockctx, struct chan_ctx *pchan);
SOCKET sock_handle(struct sock_ctx *psockctx);
struct buffer_ctx *sock_recv_buffer(struct sock_ctx *psockctx);
struct buffer_ctx *sock_send_buffer(struct sock_ctx *psockctx);
int32_t sock_type(struct sock_ctx *psockctx);
void sock_close(struct sock_ctx *psockctx);

int32_t sock_send(struct sock_ctx *psockctx, void *pdata, const size_t uilens);
int32_t sock_send_buf(struct sock_ctx *psockctx);
int32_t sock_sendto(struct sock_ctx *psockctx, void *pdata, const size_t uilens,
    const char *pip, const uint16_t uport);

int32_t _sock_can_free(struct sock_ctx *psockctx);
void _sock_free(struct sock_ctx *psockctx);

#endif NETAPI_H_
