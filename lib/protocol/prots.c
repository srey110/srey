#include "protocol/prots.h"
#include "protocol/custz.h"
#include "protocol/dns.h"
#include "protocol/http.h"
#include "protocol/websock.h"
#include "protocol/mqtt/mqtt.h"
#include "protocol/redis.h"
#include "protocol/mysql/mysql.h"
#include "protocol/pgsql/pgsql.h"
#include "protocol/mongo/mongo.h"
#include "protocol/smtp/smtp.h"
#include "event/event.h"

static prot_emit g_emit;

// 应用层握手完成推送：各协议握手完成时回调（注册见 prots_init），经消息汇推 MSG_TYPE_HANDSHAKED
static int32_t _prots_handshaked(SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud, int32_t erro, void *data, size_t lens) {
    void *target = g_emit.begin(ud);
    if (NULL == target) {
        prots_hsfree(ud->pktype, data);
        return ERR_FAILED;
    }
    message_ctx msg = { 0 };
    msg.mtype = MSG_TYPE_HANDSHAKED;
    msg.subtype = ud->pktype;
    msg.sk.fd = fd;
    msg.sk.skid = skid;
    msg.client = client;
    msg.erro = erro;
    msg.data = data;
    msg.size = lens;
    msg.sess = skid;
    g_emit.emit(target, &msg);
    g_emit.end(target);
    return ERR_OK;
}
void prots_init(prot_emit *emit) {
    g_emit = *emit;
    _websock_init(_prots_handshaked);
    _smtp_init(_prots_handshaked);
    _mysql_init(_prots_handshaked);
    _pgsql_init(_prots_handshaked);
    _mongo_init(_prots_handshaked);
}
void prots_free(void) {
}
// 设置子协议 ud_cxt 的额外上下文数据（目前仅 WebSocket 子协议需要）
void _evpub_set_secextra(ud_cxt *ud, void *val) {
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
    if (NULL == data) {
        return;
    }
    switch (pktype) {
    case PACK_MONGO:
        _mongo_pkfree(data);
        break;
    default:
        FREE(data);
        break;
    }
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
        FREE(ud->context);
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
    (void)ev;
    (void)fd;
    (void)skid;
    (void)ud;
    return ERR_OK;
}
int32_t prots_connected(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud, int32_t err) {
    switch (ud->pktype) {
    case PACK_PGSQL:
        return _pgsql_on_connected(ev, fd, skid, ud, err);
    default:
        break;
    }
    return err;
}
int32_t prots_ssl_exchanged(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud, void *ssl) {
    (void)fd;
    (void)skid;
    (void)client;
    switch (ud->pktype) {
    case PACK_MYSQL:
        return _mysql_ssl_exchanged(ev, ud);
    case PACK_PGSQL:
        return _pgsql_ssl_exchanged(ev, ud, ssl);
    default:
        break;
    }
    return ERR_OK;
}
// 默认解包：将缓冲区所有数据一次性取出，适用于 PACK_NONE（透传）场景
static void *_prots_unpack_default(buffer_ctx *buf, size_t *size, ud_cxt *ud) {
    (void)ud;
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
int32_t prots_may_resume(pack_type pktype, void *data) {
    switch (pktype) {
    case PACK_PGSQL:
        return _pgsql_may_resume(data);
    default:
        break;
    }
    return ERR_OK;
}
void *prots_unpack(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client,
    buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status) {
    *size = 0;
    *status = PROT_INIT;
    void *unpack = NULL;
    switch (ud->pktype) {
    case PACK_DNS:
        unpack = dns_unpack(buf, size, status);
        break;
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
        unpack = _prots_unpack_default(buf, size, ud);
        break;
    }
    return unpack;
}
int32_t prots_net_accept(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud) {
    void *target = g_emit.begin(ud);
    if (NULL == target) {
        return ERR_FAILED;
    }
    int32_t rtn = prots_accepted(ev, fd, skid, ud);
    if (ERR_OK == rtn) {
        message_ctx msg = { 0 };
        msg.mtype = MSG_TYPE_ACCEPT;
        msg.subtype = ud->pktype;
        msg.sk.fd = fd;
        msg.sk.skid = skid;
        g_emit.emit(target, &msg);
    }
    g_emit.end(target);
    return rtn;
}
int32_t prots_net_connect(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t err, ud_cxt *ud) {
    void *target = g_emit.begin(ud);
    if (NULL == target) {
        return ERR_FAILED;
    }
    int32_t rtn = prots_connected(ev, fd, skid, ud, err);
    if (ERR_OK != rtn) {
        err = rtn;
    }
    message_ctx msg = { 0 };
    msg.mtype = MSG_TYPE_CONNECT;
    msg.subtype = ud->pktype;
    msg.sk.fd = fd;
    msg.sk.skid = skid;
    msg.erro = err;
    msg.sess = skid;
    g_emit.emit(target, &msg);
    g_emit.end(target);
    return err;
}
void prots_net_recv(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, buffer_ctx *buf, size_t size, ud_cxt *ud) {
    void *target = g_emit.begin(ud);
    if (NULL == target) {
        ev_close(ev, fd, skid, 1);
        return;
    }
    message_ctx msg = { 0 };
    msg.mtype = MSG_TYPE_RECV;
    msg.subtype = ud->pktype;
    msg.sk.fd = fd;
    msg.sk.skid = skid;
    msg.client = client;
    void *data;
    int32_t status;
    size_t esize;
    for (;;) {
        size = buffer_size(buf);
        data = prots_unpack(ev, fd, skid, client, buf, ud, &msg.size, &status);
        if (NULL != data) {
            msg.data = data;
            msg.sess = ud->sess;
            if (BIT_CHECK(status, PROT_SLICE_START)) {
                msg.slice = PROT_SLICE_START;
            } else if(BIT_CHECK(status, PROT_SLICE)) {
                msg.slice = PROT_SLICE;
            } else if(BIT_CHECK(status, PROT_SLICE_END)) {
                msg.slice = PROT_SLICE_END;
            } else {
                msg.slice = 0;
            }
            g_emit.emit(target, &msg);
        }
        if (BIT_CHECK(status, PROT_ERROR)) {
            ev_close(ev, fd, skid, 1);
            break;
        }
        if (BIT_CHECK(status, PROT_CLOSE)) {
            // 协议层正常关闭信号(如 WebSocket close frame),业务应答 close frame
            // 可能仍在 buf_s,immed=0 让其发完再关
            ev_close(ev, fd, skid, 0);
            break;
        }
        esize = buffer_size(buf);
        if (0 == esize
            || size == esize
            || BIT_CHECK(status, PROT_MOREDATA)) {
            break;
        }
    }
    g_emit.end(target);
}
void prots_net_send(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, size_t size, ud_cxt *ud) {
    void *target = g_emit.begin(ud);
    if (NULL == target) {
        ev_close(ev, fd, skid, 1);
        return;
    }
    message_ctx msg = { 0 };
    msg.mtype = MSG_TYPE_SEND;
    msg.subtype = ud->pktype;
    msg.sk.fd = fd;
    msg.sk.skid = skid;
    msg.client = client;
    msg.size = size;
    g_emit.emit(target, &msg);
    g_emit.end(target);
}
int32_t prots_net_ssl_exchanged(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud, void *ssl) {
    void *target = g_emit.begin(ud);
    if (NULL == target) {
        return ERR_FAILED;
    }
    int32_t rtn = prots_ssl_exchanged(ev, fd, skid, client, ud, ssl);
    if (ERR_OK == rtn) {
        message_ctx msg = { 0 };
        msg.mtype = MSG_TYPE_SSLEXCHANGED;
        msg.subtype = ud->pktype;
        msg.sk.fd = fd;
        msg.sk.skid = skid;
        msg.client = client;
        msg.sess = skid;
        g_emit.emit(target, &msg);
    }
    g_emit.end(target);
    return rtn;
}
void prots_net_close(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud) {
    (void)ev;
    void *target = g_emit.begin(ud);
    if (NULL == target) {
        return;
    }
    message_ctx msg = { 0 };
    msg.mtype = MSG_TYPE_CLOSE;
    msg.subtype = ud->pktype;
    msg.sk.fd = fd;
    msg.sk.skid = skid;
    msg.client = client;
    msg.sess = skid;
    prots_closed(ud);
    g_emit.emit(target, &msg);
    g_emit.end(target);
}
void prots_net_recvfrom(ev_ctx *ev, SOCKET fd, uint64_t skid, char *buf, size_t size, netaddr_ctx *addr, ud_cxt *ud) {
    void *target = g_emit.begin(ud);
    if (NULL == target) {
        ev_close(ev, fd, skid, 1);
        return;
    }
    message_ctx msg = { 0 };
    msg.mtype = MSG_TYPE_RECVFROM;
    msg.subtype = PACK_NONE; // UDP 路径透传原始数据，避免 _message_clean→prots_pkfree 进入特定协议释放路径
    msg.sk.fd = fd;
    msg.sk.skid = skid;
    char *umsg;
    MALLOC(umsg, sizeof(netaddr_ctx) + size);
    memcpy(umsg, addr, sizeof(netaddr_ctx));
    memcpy(umsg + sizeof(netaddr_ctx), buf, size);
    msg.data = umsg;
    msg.size = size;
    msg.sess = ud->sess;
    ud->sess = 0;
    g_emit.emit(target, &msg);
    g_emit.end(target);
}
