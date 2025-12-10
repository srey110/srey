#ifndef PGSQL_H_
#define PGSQL_H_

#include "protocol/pgsql/pgsql_struct.h"

void _pgsql_init(void *hspush);
void _pgsql_pkfree(void *pack);
void _pgsql_udfree(ud_cxt *ud);
void _pgsql_closed(ud_cxt *ud);
int32_t _pgsql_on_connected(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud, int32_t err);
int32_t _pgsql_ssl_exchanged(ev_ctx *ev, ud_cxt *ud);
void *pgsql_unpack(ev_ctx *ev, buffer_ctx *buf, ud_cxt *ud, int32_t *status);
int32_t pgsql_init(pgsql_ctx *pg, const char *ip, uint16_t port, struct evssl_ctx *evssl,
    const char *user, const char *password, const char *database);
int32_t pgsql_try_connect(task_ctx *task, pgsql_ctx *pg);

void *pgsql_pack_quit(pgsql_ctx *pg, size_t *size);


#endif//PGSQL_H_
