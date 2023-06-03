#ifndef PROTOS_H_
#define PROTOS_H_

#include "event/evpub.h"

typedef enum pack_type {
    PACK_NONE = 0x0,
    PACK_RPC,
    PACK_HTTP,
    PACK_WEBSOCK,
    PACK_SIMPLE,
}pack_type;

void protos_pkfree(pack_type type, void *data);
void protos_udfree(ud_cxt *ud);
void *protos_unpack(ev_ctx *ev, SOCKET fd, buffer_ctx *buf, size_t *size, ud_cxt *ud, int32_t *closefd);
void *protos_pack(pack_type type, void *data, size_t lens, size_t *size);
//如果需要握手则设置回调函数(ud->hscb)，握手成功后再发送MSG_TYPE_ACCEPT MSG_TYPE_CONNECT消息
//握手失败，设置关闭链接标志(closefd)，在链接关闭时会发送MSG_TYPE_CONNECT
int32_t protos_handshake(ev_ctx *ev, SOCKET fd, ud_cxt *ud, void *hscb);

#endif//PROTOS_H_
