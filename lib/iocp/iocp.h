#ifndef IOCP_H_
#define IOCP_H_

#include "sock.h"

#if defined(OS_WIN)

struct netio_ctx *netio_new();
void netio_free(struct netio_ctx *pctx);
void netio_run(struct netio_ctx *pctx);

struct sock_ctx *netio_listen(struct netio_ctx *pctx, struct chan_ctx *pchan,
    const uint32_t uichancnt, const char *phost, const uint16_t usport);
struct sock_ctx *netio_connect(struct netio_ctx *pctx, struct chan_ctx *pchan,
    const char *phost, const uint16_t usport);
struct sock_ctx *netio_addsock(struct netio_ctx *pctx, SOCKET fd);

int32_t netio_enable_rw(struct sock_ctx *psock, struct chan_ctx *pchan, const uint8_t postsendev);
int32_t netio_send(struct sock_ctx *psock, void *pdata, const size_t uilens);
int32_t netio_sendto(struct sock_ctx *psock, const char *phost, const uint16_t usport,
    IOV_TYPE *wsabuf, const size_t uicnt);

#endif // OS_WIN
#endif//IOCP_H_
