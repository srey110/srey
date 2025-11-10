#ifndef PROTOS_H_
#define PROTOS_H_

#include "event/evpub.h"

typedef enum pack_type {
    PACK_NONE = 0x0,
    PACK_HTTP,
    PACK_WEBSOCK,
    PACK_MQTT,
    PACK_CUSTZ_FIXED,
    PACK_CUSTZ_FLAG,
    PACK_CUSTZ_VAR,

    PACK_REDIS = 0x50,
    PACK_MYSQL,
    PACK_PGSQL,
    PACK_MGDB
}pack_type;
typedef enum proto_status {
    PROTO_NONE = 0x00,
    PROTO_SLICE_START = 0x01,
    PROTO_SLICE = 0x02,
    PROTO_SLICE_END = 0x04,
    PROTO_ERROR = 0x08,
    PROTO_MOREDATA = 0x10
}proto_status;
typedef int32_t(*_handshaked_push)(SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud, int32_t erro, void *data, size_t lens);

void protos_init(_handshaked_push hspush);
void protos_free(void);
void protos_pkfree(pack_type pktype, int32_t mtype, void *data);
void protos_udfree(void *arg);
void protos_closed(ud_cxt *ud);
int32_t protos_ssl_exchanged(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud);
void *protos_unpack(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client,
    buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status);

#endif//PROTOS_H_
