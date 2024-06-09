#include "proto/protos.h"
#include "proto/custz.h"
#include "proto/http.h"
#include "proto/websock.h"
#include "proto/redis.h"
#include "proto/mysql/mysql.h"

void protos_pkfree(pack_type type, void *data) {
    if (NULL == data) {
        return;
    }
    switch (type) {
    case PACK_HTTP:
        _http_pkfree(data);
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
    default:
        break;
    }
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
void protos_init(_handshaked_push hspush) {
    _websock_init(hspush);
    _mysql_init(hspush);
}
void protos_free(void) {
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
void *protos_unpack(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client,
    buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status) {
    *size = 0;
    *status = PROTO_NONE;
    void *unpack;
    switch (ud->pktype) {
    case PACK_CUSTZ:
        unpack = custz_unpack(buf, ud, size, status);
        break;
    case PACK_HTTP:
        unpack = http_unpack(buf, ud, status);
        break;
    case PACK_WEBSOCK:
        unpack = websock_unpack(ev, fd, skid, client, buf, ud, status);
        break;
    case PACK_REDIS:
        unpack = redis_unpack(buf, ud, status);
        break;
    case PACK_MYSQL:
        unpack = mysql_unpack(ev, buf, ud, status);
        break;
    default:
        unpack = _unpack_default(buf, size, ud);
        break;
    }
    return unpack;
}
