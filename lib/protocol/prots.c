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
#include "protocol/kcp/kcp.h"
#include "event/event.h"

static prot_emit g_emit;

// 应用层握手完成推送：各协议握手完成时回调（注册见 prots_init），经消息汇推 MSG_TYPE_HANDSHAKED
static int32_t _prots_handshaked(SOCKET fd, uint64_t skid, int32_t client,
    ud_cxt *ud, int32_t erro, void *data, size_t lens) {
    void *target = g_emit.begin(ud->loader, ud->handle);
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
    msg.sess = ud->sess;
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
    _kcp_init(&g_emit);
}
void prots_free(void) {
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
void prots_udp_pkfree(pack_type pktype, void *data) {
    (void)pktype;
    if (NULL == data) {
        return;
    }
    FREE(data);
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
    case PACK_UDP_KCP:
        _kcp_udfree(ud);
        break;
    default:
        FREE(ud->context);
        break;
    }
}
// 连接关闭时通知各协议模块做清理（如发送挂断命令）
static void prots_closed(ud_cxt *ud) {
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
    case PACK_UDP_KCP:
        _kcp_fd_closed(ud);
        break;
    default:
        break;
    }
}
// 新连接被接受时的回调，按协议扩展；当前各协议均返回 ERR_OK
static int32_t prots_accepted(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud) {
    (void)ev;
    (void)fd;
    (void)skid;
    (void)ud;
    return ERR_OK;
}
// 主动连接建立后的回调，部分协议需在此发送初始化包
static int32_t prots_connected(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud, int32_t err) {
    switch (ud->pktype) {
    case PACK_PGSQL:
        return _pgsql_on_connected(ev, fd, skid, ud, err);
    default:
        break;
    }
    return err;
}
// SSL 握手完成后的回调，部分协议需在 SSL 建立后发送认证包（pgsql 用于 SCRAM-SHA-256-PLUS 通道绑定）
static int32_t prots_ssl_exchanged(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud, void *ssl) {
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
// 取单次 prots_unpack 结果携带的下一个包(仅 websock 承载子协议单帧多包场景非 NULL,其余协议恒 NULL)
static void *_prots_next_pack(pack_type pktype, void *pack) {
    if (PACK_WEBSOCK != pktype) {
        return NULL;
    }
    return _websock_pack_next(pack);
}
int32_t prots_net_accept(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud) {
    void *target = g_emit.begin(ud->loader, ud->handle);
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
// 构造并 emit 一条 CLOSE 消息；调用方负责 begin/end target
static void _prots_emit_close(void *target, SOCKET fd, uint64_t skid, int32_t client, int32_t erro, ud_cxt *ud) {
    message_ctx msg = { 0 };
    msg.mtype = MSG_TYPE_CLOSE;
    msg.subtype = ud->pktype;
    msg.sk.fd = fd;
    msg.sk.skid = skid;
    msg.client = client;
    msg.erro = erro;
    msg.sess = skid;// 始终尝试唤醒
    prots_closed(ud);
    g_emit.emit(target, &msg);
}
int32_t prots_net_connect(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t err, ud_cxt *ud) {
    void *target = g_emit.begin(ud->loader, ud->handle);
    if (NULL == target) {
        return ERR_FAILED;
    }
    // 用原始 err 判断：TCP 层本身已失败(fd 已被 event 层 remove,没人会再补 CLOSE)才需要我方补发；
    // TCP 成功但 prots_connected 才失败时 fd 仍在监听表中,_usk_on_connect_cb/_olp_on_connect_cb
    // 会因返回值非 ERR_OK 自行 _uev_disconnect 触发真实 CLOSE,此处再补会重复
    int32_t emitclose = ERR_OK != err;
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
    msg.sess = ud->sess;
    g_emit.emit(target, &msg);
    if (emitclose) {
        // CONNECT 只发生在客户端发起连接场景，client 恒为 1；erro 非 ERR_OK 标记这是因连接失败而补发的
        // 合成 CLOSE，供消费侧区分是否跳过 on_close 观察者
        _prots_emit_close(target, fd, skid, 1, err, ud);
    }
    g_emit.end(target);
    return err;
}
void prots_net_recv(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, buffer_ctx *buf, size_t size, ud_cxt *ud) {
    void *target = g_emit.begin(ud->loader, ud->handle);
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
    void *data, *next;
    int32_t status;
    size_t esize;
    for (;;) {
        size = buffer_size(buf);
        data = prots_unpack(ev, fd, skid, client, buf, ud, &msg.size, &status);
        while (NULL != data) {
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
            next = _prots_next_pack(ud->pktype, data);// 提前取出，防止消息(data)emit后在worker线程被释放.
            g_emit.emit(target, &msg);
            data = next;
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
    void *target = g_emit.begin(ud->loader, ud->handle);
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
    void *target = g_emit.begin(ud->loader, ud->handle);
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
        msg.sess = ud->sess;
        g_emit.emit(target, &msg);
    }
    g_emit.end(target);
    return rtn;
}
void prots_net_close(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud) {
    (void)ev;
    void *target = g_emit.begin(ud->loader, ud->handle);
    if (NULL == target) {
        return;
    }
    _prots_emit_close(target, fd, skid, client, ERR_OK, ud);
    g_emit.end(target);
}
static void _prots_udp_default(ev_ctx *ev, SOCKET fd, uint64_t skid, char *buf, size_t size, netaddr_ctx *addr, ud_cxt *ud) {
    void *target = g_emit.begin(ud->loader, ud->handle);
    if (NULL == target) {
        ev_close(ev, fd, skid, 1);
        return;
    }
    message_ctx msg = { 0 };
    msg.mtype = MSG_TYPE_RECVFROM;
    msg.subtype = ud->pktype;
    msg.sk.fd = fd;
    msg.sk.skid = skid;
    recvfrom_ctx *umsg;
    MALLOC(umsg, sizeof(recvfrom_ctx) + size);
    umsg->addr = *addr;
    umsg->len = size;
    memcpy(umsg->data, buf, size);
    msg.data = umsg;
    msg.size = size;
    msg.sess = ud->sess;
    g_emit.emit(target, &msg);
    g_emit.end(target);
}
void prots_net_recvfrom(ev_ctx *ev, SOCKET fd, uint64_t skid, char *buf, size_t size, netaddr_ctx *addr, ud_cxt *ud) {
    switch(ud->pktype) {
    case PACK_UDP_KCP:
        _kcp_unpack(fd, skid, buf, size, addr, ud);
        break;
    default:
        _prots_udp_default(ev, fd, skid, buf, size, addr, ud);
        break;
    }
}
