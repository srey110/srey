#include "protocol/prots.h"
#include "protocol/custz.h"
#include "protocol/http.h"
#include "protocol/websock.h"
#include "protocol/mqtt/mqtt.h"
#include "protocol/redis.h"
#include "protocol/mysql/mysql.h"
#include "protocol/pgsql/pgsql.h"
#include "protocol/mongo/mongo.h"
#include "protocol/smtp/smtp.h"

void prots_init(_handshaked_push hspush) {
    _websock_init(hspush);
    _smtp_init(hspush);
    _mysql_init(hspush);
    _pgsql_init(hspush);
    _mongo_init(hspush);
}
void prots_free(void) {
}
void _set_secextra(ud_cxt *ud, void *val) {
    switch (ud->pktype) {
    case PACK_WEBSOCK:
        _websock_secextra(ud, val);
        break;
    }
}
void prots_pkfree(pack_type pktype, void *data) {
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
    case PACK_MQTT:
        _mqtt_pkfree(data);
        break;
    case PACK_REDIS:
        _redis_pkfree(data);
        break;
    case PACK_MYSQL:
        _mysql_pkfree(data);
        break;
    case PACK_PGSQL:
        _pgsql_pkfree(data);
        break;
    case PACK_MONGO:
        _mongo_pkfree(data);
        break;
    default:
        FREE(data);
        break;
    }
}
void prots_hsfree(pack_type pktype, void *data) {
    FREE(data);
}
void prots_udfree(void *arg) {
    if (NULL == arg) {
        return;
    }
    ud_cxt *ud = arg;
    switch (ud->pktype) {
    case PACK_HTTP:
        _http_udfree(ud);
        break;
    case PACK_WEBSOCK:
        _websock_udfree(ud);
        break;
    case PACK_MQTT:
        _mqtt_udfree(ud);
        break;
    case PACK_SMTP:
        _smtp_udfree(ud);
        break;
    case PACK_REDIS:
        _redis_udfree(ud);
        break;
    case PACK_MYSQL:
        _mysql_udfree(ud);
        break;
    case PACK_PGSQL:
        _pgsql_udfree(ud);
        break;
    case PACK_MONGO:
        _mongo_udfree(ud);
        break;
    default:
        FREE(ud->extra);
        break;
    }
}
void prots_closed(ud_cxt *ud) {
    if (NULL == ud) {
        return;
    }
    switch (ud->pktype) {
    case PACK_SMTP:
        _smtp_closed(ud);
        break;
    case PACK_MYSQL:
        _mysql_closed(ud);
        break;
    case PACK_PGSQL:
        _pgsql_closed(ud);
        break;
    case PACK_MONGO:
        _mongo_closed(ud);
        break;
    default:
        break;
    }
}
int32_t prots_accepted(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud) {
    return ERR_OK;
}
int32_t prots_connected(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud, int32_t err) {
    switch (ud->pktype) {
    case PACK_SMTP:
        return _smtp_on_connected(ud, err);
    case PACK_MYSQL:
        return _mysql_on_connected(ud, err);
    case PACK_PGSQL:
        return _pgsql_on_connected(ev, fd, skid, ud, err);
    case PACK_MONGO:
        return _mongo_on_connected(ev, fd, skid, ud, err);
    default:
        break;
    }
    return err;
}
int32_t prots_ssl_exchanged(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud) {
    switch (ud->pktype) {
    case PACK_MYSQL:
        return _mysql_ssl_exchanged(ev, ud);
    case PACK_PGSQL:
        return _pgsql_ssl_exchanged(ev, ud);
    case PACK_MONGO:
        return _mongo_ssl_exchanged(ev, ud);
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
void *prots_unpack(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client,
    buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status) {
    *size = 0;
    *status = PROT_INIT;
    void *unpack = NULL;
    switch (ud->pktype) {
    case PACK_HTTP:
        unpack = http_unpack(buf, ud, status);
        break;
    case PACK_WEBSOCK:
        unpack = websock_unpack(ev, fd, skid, client, buf, ud, status);
        break;
    case PACK_MQTT:
        unpack = mqtt_unpack(client, buf, ud, status);
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
        unpack = pgsql_unpack(ev, buf, ud, status);
        break;
    case PACK_MONGO:
        unpack = mongo_unpack(ev, buf, ud, status);
        break;
    default:
        unpack = _unpack_default(buf, size, ud);
        break;
    }
    return unpack;
}
