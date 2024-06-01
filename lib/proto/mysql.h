#ifndef MYSQL_H_
#define MYSQL_H_

#include "event/event.h"
#include "srey/spub.h"

void mysql_pkfree(void *pack);
void mysql_udfree(ud_cxt *ud);
void _mysql_init(void *hspush);
int32_t mysql_ssl_exchanged(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud);
void *mysql_unpack(ev_ctx *ev, SOCKET fd, uint64_t skid, buffer_ctx *buf, ud_cxt *ud, int32_t *status);
SOCKET mysql_connect(task_ctx *task, const char *ip, uint16_t port, struct evssl_ctx *evssl,
    const char *user, const char *password, const char *database, const char *charset, int32_t maxpk, uint64_t *skid);

#endif//MYSQL_H_
