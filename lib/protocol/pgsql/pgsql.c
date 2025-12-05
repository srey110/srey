#include "protocol/pgsql/pgsql.h"
#include "srey/task.h"
#include "utils/utils.h"

typedef enum pgsql_client_status {
    LINKING = 0x01,
    AUTHED = 0x02
}pgsql_client_status;
typedef enum parse_status {
    INIT = 0,
    COMMAND
}parse_status;

static _handshaked_push _hs_push;

void _pgsql_init(void *hspush) {
    _hs_push = hspush;
}
void _pgsql_pkfree(void *pack) {
    
}
void _pgsql_udfree(ud_cxt *ud) {
    if (NULL == ud->extra) {
        return;
    }
    pgsql_ctx *pg = (pgsql_ctx *)ud->extra;
    pg->status = 0;
    ud->extra = NULL;
}
void _pgsql_closed(ud_cxt *ud) {
    _pgsql_udfree(ud);
}
int32_t _pgsql_connected(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud) {
    //ÇëÇóÊÇ·ñssl¼ÓÃÜ
    char buf[8];
    pack_integer(buf, 8, 4, 0);
    pack_integer(buf + 4, 80877103, 4, 0);
    ev_send(ev, fd, skid, buf, sizeof(buf), 1);
    return ERR_OK;
}
static void _pgsql_ssl_response(pgsql_ctx *pg, ev_ctx *ev, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    if (1 > buffer_size(buf)) {
        BIT_SET(*status, PROT_MOREDATA);
        return;
    }
    char ssl[1];
    ASSERTAB(sizeof(ssl) == buffer_remove(buf, ssl, sizeof(ssl)), "copy buffer failed.");
    switch (ssl[0]) {
    case 'S':
        if (NULL != pg->evssl) {
            ev_ssl(ev, pg->fd, pg->skid, 1, pg->evssl);
        } else {
            BIT_SET(*status, PROT_ERROR);
            LOG_WARN("certificate not set.");
        }
        break;
    case 'N':
        break;
    default:
        BIT_SET(*status, PROT_ERROR);
        break;
    }
}
int32_t _pgsql_ssl_exchanged(ev_ctx *ev, ud_cxt *ud) {
    return ERR_OK;
}
void *pgsql_unpack(ev_ctx *ev, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    pgsql_ctx *pg = (pgsql_ctx *)ud->extra;
    switch (ud->status) {
    case INIT:
        _pgsql_ssl_response(pg, ev, buf, ud, status);
        break;
    default:
        break;
    }
}
int32_t pgsql_init(pgsql_ctx *pg, const char *ip, uint16_t port, struct evssl_ctx *evssl,
    const char *user, const char *password, const char *database) {
    if (strlen(ip) > sizeof(pg->ip) - 1
        || strlen(user) > sizeof(pg->user) - 1
        || strlen(password) > sizeof(pg->password) - 1
        || (NULL != database && strlen(database) > sizeof(pg->database) - 1)) {
        return ERR_FAILED;
    }
    ZERO(pg, sizeof(pgsql_ctx));
    strcpy(pg->ip, ip);
    strcpy(pg->user, user);
    strcpy(pg->password, password);
    strcpy(pg->database, database);
    pg->port = 0 == port ? 5432 : port;
    pg->evssl = evssl;
    return ERR_OK;
}
int32_t pgsql_try_connect(task_ctx *task, pgsql_ctx *pg) {
    if (0 != pg->status) {
        return ERR_FAILED;
    }
    pg->task = task;
    BIT_SET(pg->status, LINKING);
    if (ERR_OK != task_connect(task, PACK_PGSQL, NULL, pg->ip, pg->port, NETEV_AUTHSSL, pg, &pg->fd, &pg->skid)) {
        BIT_REMOVE(pg->status, LINKING);
        return ERR_FAILED;
    }
    return ERR_OK;
}
