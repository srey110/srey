#ifndef MONGO_H_
#define MONGO_H_

#include "event/event.h"
#include "protocol/mongo/mongo_struct.h"

void _mongo_init(void *hspush);
void _mongo_pkfree(void *pack);
void _mongo_udfree(ud_cxt *ud);
void _mongo_closed(ud_cxt *ud);
int32_t _mongo_on_connected(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud, int32_t err);
int32_t _mongo_ssl_exchanged(ev_ctx *ev, ud_cxt *ud);
void *mongo_unpack(ev_ctx *ev, buffer_ctx *buf, ud_cxt *ud, int32_t *status);
//SCRAM-SHA-1 SCRAM-SHA-256
void mongo_init(mongo_ctx *mongo, const char *ip, uint16_t port, struct evssl_ctx *evssl,
    const char *user, const char *password, const char *authmod, const char *authdb);
void *mongo_pack_msg(mongo_ctx *mongo, const uint8_t *doc, size_t dlens, int32_t *reqid, size_t *size);

#endif//MONGO_H_
