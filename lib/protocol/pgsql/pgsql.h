#ifndef PGSQL_H_
#define PGSQL_H_

#include "srey/spub.h"

typedef struct pgsql_ctx {
    uint16_t port;
    scram_authmod authmod;
    int32_t status;
    SOCKET fd;
    uint64_t skid;
    struct task_ctx *task;
    struct evssl_ctx *evssl;
    char ip[IP_LENS];
    char user[64];
    char password[64];
    char database[64];
}pgsql_ctx;

void _pgsql_init(void *hspush);
void _pgsql_pkfree(void *pack);
void _pgsql_udfree(ud_cxt *ud);
void _pgsql_closed(ud_cxt *ud);
int32_t _pgsql_connected(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud);
int32_t _pgsql_ssl_exchanged(ev_ctx *ev, ud_cxt *ud);

void *pgsql_unpack(ev_ctx *ev, buffer_ctx *buf, ud_cxt *ud, int32_t *status);

int32_t pgsql_init(pgsql_ctx *pg, const char *ip, uint16_t port, struct evssl_ctx *evssl,
    const char *user, const char *password, const char *database);
int32_t pgsql_try_connect(task_ctx *task, pgsql_ctx *pg);

#endif//PGSQL_H_
