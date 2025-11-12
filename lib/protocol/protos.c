#include "protocol/protos.h"
#include "protocol/custz.h"
#include "protocol/http.h"
#include "protocol/websock.h"
#include "protocol/redis.h"
#include "protocol/mysql/mysql.h"
#include "protocol/smtp.h"

void protos_init(_handshaked_push hspush) {
    _websock_init(hspush);
    _mysql_init(hspush);
    _smtp_init(hspush);
}
void protos_free(void) {
}
void protos_pkfree(pack_type pktype, int32_t mtype, void *data) {
    if (NULL == data) {
        return;
    }
    switch (pktype) {
    case PACK_HTTP:
        _http_pkfree(data);
        break;
    case PACK_WEBSOCK:
        _websock_pkfree(data);
        break;
    case PACK_REDIS:
        _redis_pkfree(data);
        break;
    case PACK_MYSQL:
        _mysql_pkfree(data);
        break;
    default:
        FREE(data);
        break;
    }
}
void protos_udfree(void *arg) {
    ud_cxt *ud = arg;
    switch (ud->pktype) {
    case PACK_HTTP:
        _http_udfree(ud);
        break;
    case PACK_REDIS:
        _redis_udfree(ud);
        break;
    case PACK_MYSQL:
        _mysql_udfree(ud);
        break;
    case PACK_SMTP:
        _smtp_udfree(ud);
        break;
    default:
        FREE(ud->extra);
        break;
    }
}
void protos_closed(ud_cxt *ud) {
    switch (ud->pktype) {
    case PACK_MYSQL:
        _mysql_closed(ud);
        break;
    case PACK_SMTP:
        _smtp_closed(ud);
        break;
    default:
        break;
    }
}
int32_t protos_connected(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud) {
    return ERR_OK;
}
int32_t protos_ssl_exchanged(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud) {
    switch (ud->pktype) {
    case PACK_MYSQL:
        return _mysql_ssl_exchanged(ev, ud);
    default:
        break;
    }
    return ERR_OK;
}
static void *_unpack_default(buffer_ctx *buf, size_t *size, ud_cxt *ud) {
    size_t lens = buffer_size(buf);
    if (0 == lens) {
        return NULL;
    }
    void *unpack;
    MALLOC(unpack, lens);
    ASSERTAB(lens == buffer_remove(buf, unpack, lens), "copy buffer error.");
    *size = lens;
    return unpack;
}
void *protos_unpack(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client,
    buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status) {
    *size = 0;
    *status = PROTO_NONE;
    void *unpack;
    switch (ud->pktype) {
    case PACK_HTTP:
        unpack = http_unpack(buf, ud, status);
        break;
    case PACK_WEBSOCK:
        unpack = websock_unpack(ev, fd, skid, client, buf, ud, status);
        break;
    case PACK_MQTT:
        break;
    case PACK_SMTP:
        unpack = smtp_unpack(ev, fd, skid, buf, ud, size, status);
        break;
    case PACK_CUSTZ_FIXED:
    case PACK_CUSTZ_FLAG:
    case PACK_CUSTZ_VAR:
        unpack = custz_unpack(ud->pktype, buf, size, status);
        break;

    case PACK_REDIS:
        unpack = redis_unpack(buf, ud, status);
        break;
    case PACK_MYSQL:
        unpack = mysql_unpack(ev, buf, ud, status);
        break;
    case PACK_PGSQL:
        break;
    case PACK_MGDB:
        break;
    default:
        unpack = _unpack_default(buf, size, ud);
        break;
    }
    return unpack;
}
