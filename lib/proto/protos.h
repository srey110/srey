#ifndef PROTOS_H_
#define PROTOS_H_

#include "event/evpub.h"

typedef enum pack_type {
    PACK_NONE = 0x0,
    PACK_RPC,
    PACK_HTTP,
    PACK_WEBSOCK,
    PACK_MQTT,
    PACK_SIMPLE,
}pack_type;

typedef void(*push_handshaked)(SOCKET fd, ud_cxt *ud);

void protos_pkfree(pack_type type, void *data);
void protos_udfree(ud_cxt *ud);
void *protos_unpack(ev_ctx *ev, SOCKET fd, buffer_ctx *buf, size_t *size, ud_cxt *ud, int32_t *closefd);
void *protos_pack(pack_type type, void *data, size_t lens, size_t *size);
void protos_handshaked(ud_cxt *ud, push_handshaked cb);

#endif//PROTOS_H_
