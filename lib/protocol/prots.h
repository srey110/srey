#ifndef PROTS_H_
#define PROTS_H_

#include "event/evpub.h"

typedef enum pack_type {
    PACK_NONE = 0x00,
    PACK_HTTP,
    PACK_WEBSOCK,
    PACK_MQTT,
    PACK_SMTP,
    PACK_CUSTZ_FIXED,
    PACK_CUSTZ_FLAG,
    PACK_CUSTZ_VAR,

    PACK_REDIS = 0x50,
    PACK_MYSQL,
    PACK_PGSQL,
    PACK_MONGO
}pack_type;
typedef enum prot_status {
    PROT_INIT = 0x00,
    PROT_SLICE_START = 0x01,
    PROT_SLICE = 0x02,
    PROT_SLICE_END = 0x04,
    PROT_ERROR = 0x08,
    PROT_MOREDATA = 0x10,
    PROT_CLOSE = 0x20
}prot_status;
typedef int32_t(*_handshaked_push)(SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud, int32_t erro, void *data, size_t lens);

void prots_init(_handshaked_push hspush);
void prots_free(void);
void prots_pkfree(pack_type pktype, void *data);
void prots_hsfree(pack_type pktype, void *data);
void prots_udfree(void *arg);
void prots_closed(ud_cxt *ud);
int32_t prots_connected(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud);
int32_t prots_ssl_exchanged(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud);
void *prots_unpack(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client,
    buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status);

#endif//PROTS_H_
