#ifndef PROTOS_H_
#define PROTOS_H_

#include "event/evpub.h"

typedef void(*_handshaked_push)(SOCKET fd, uint64_t skid, ud_cxt *ud, int32_t *closefd, int32_t erro);
typedef enum pack_type {
    PACK_NONE = 0x0,
    PACK_HTTP,
    PACK_WEBSOCK,
    PACK_SIMPLE
}pack_type;
typedef enum slice_type {
    SLICE_NONE = 0x00,
    SLICE_START,
    SLICE,
    SLICE_END,
}slice_type;

void protos_pkfree(pack_type type, void *data);
void protos_udfree(void *arg);
void protos_init(_handshaked_push hspush);
void *protos_unpack(ev_ctx *ev, SOCKET fd, uint64_t skid,
    buffer_ctx *buf, size_t *size, ud_cxt *ud, int32_t *closefd, int32_t *slice);

#endif//PROTOS_H_
