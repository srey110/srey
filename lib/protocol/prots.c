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

// 消息 data 的归属方式。"哪些消息类型持有需要释放的堆数据"只在这一个 switch 里定义：
// message_should_clean 与 message_clean 都问它，新增带数据的消息类型只改这一处，
// 不会出现"清理加了、判定漏了"这种只在某一条消费路径上泄漏、编译器与测试都不相关的分歧
typedef enum msgdata_kind {
    MSGDATA_NONE = 0,   // 不持有堆数据
    MSGDATA_PROT,       // 协议层收包，prots_pkfree
    MSGDATA_UDP,        // UDP 收包，prots_udp_pkfree；当前与 MSGDATA_RAW 等效，独立成档留给将来按协议分化
    MSGDATA_HS,         // 握手数据，prots_hsfree
    MSGDATA_RAW         // 裸 MALLOC，FREE
}msgdata_kind;
// 各协议在 prots 层的挂钩。字段留 NULL 表示走该 hook 的默认动作。
typedef struct prot_vtbl {
    void (*pkfree)(void *data);// 释放 unpack 解出的包；NULL 表示包就是一块 malloc，走 FREE
    void (*udp_pkfree)(void *data);// 释放 UDP 收包；NULL 走 FREE。当前无协议实现是有意的，预留按协议分化，勿当死代码删
    void (*hsfree)(void *data);// 释放推给上层的握手载荷；NULL 走 FREE
    void (*udfree)(ud_cxt *ud);// 释放 ud 上挂的协议上下文；NULL 走 FREE(ud->context)
    int32_t (*accepted)(ev_ctx *ev, sock_ctx *sk, ud_cxt *ud);// accept 期准入判断，返非 ERR_OK 即拒收该连接；NULL 一律放行。当前无协议实现是有意的，预留准入钩子，勿当死代码删
    int32_t (*connected)(ev_ctx *ev, sock_ctx *sk, ud_cxt *ud, int32_t err);// 连上后发协议初始化包；NULL 原样返回入参 err
    int32_t (*ssl_exchanged)(ev_ctx *ev, ud_cxt *ud, void *ssl);// SSL 建好后发认证包；NULL 返 ERR_OK
    void (*closed)(ud_cxt *ud);// 连接关闭时清理协议状态；NULL 无动作
    void *(*unpack)(ev_ctx *ev, sock_ctx *sk, int32_t client,
                    buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status);// 从缓冲切出一个完整包；NULL 表示透传整段缓冲
    void *(*next_pack)(void *pack);// 取本次解包顺带解出的下一个包；NULL 表示不会带后继包
    int32_t (*may_resume)(void *data);// 判本包能否唤醒等待该 session 的协程；NULL 表示收到包即可唤醒
    void (*recvfrom)(ev_ctx *ev, sock_ctx *sk, char *buf, size_t size,
                     netaddr_ctx *addr, ud_cxt *ud);// UDP 收包入口；NULL 表示整个 datagram 原样上抛
    void *(*close_tail)(ud_cxt *ud);// 关连接时问"还要补一片吗"，返回要补的那片；NULL 表示该协议无此事
}prot_vtbl;

static prot_emit g_emit;
static int32_t _prots_ws_may_resume(void *data);
static const prot_vtbl _vtbl_none = { 0 };
static const prot_vtbl _vtbl_dns = {
    .unpack = dns_unpack,
};
static const prot_vtbl _vtbl_http = {
    .pkfree = _http_pkfree, .udfree = _http_udfree,
    .unpack = http_unpack, .close_tail = _http_on_close,
};
static const prot_vtbl _vtbl_websock = {
    .pkfree = _websock_pkfree, .udfree = _websock_udfree,
    .unpack = websock_unpack, .next_pack = _websock_pack_next,
    .may_resume = _prots_ws_may_resume,
};
static const prot_vtbl _vtbl_mqtt = {
    .pkfree = _mqtt_pkfree, .udfree = _mqtt_udfree,
    .unpack = mqtt_unpack, .may_resume = _mqtt_may_resume,
};
static const prot_vtbl _vtbl_smtp = {
    .udfree = _smtp_udfree, .closed = _smtp_udfree,
    .unpack = smtp_unpack,
};
static const prot_vtbl _vtbl_custz = {
    .unpack = custz_unpack,
};
static const prot_vtbl _vtbl_redis = {
    .pkfree = _redis_pkfree, .udfree = _redis_udfree,
    .unpack = redis_unpack,
};
static const prot_vtbl _vtbl_mysql = {
    .pkfree = _mysql_pkfree, .udfree = _mysql_udfree,
    .ssl_exchanged = _mysql_ssl_exchanged, .closed = _mysql_udfree,
    .unpack = mysql_unpack,
};
static const prot_vtbl _vtbl_pgsql = {
    .pkfree = _pgsql_pkfree, .udfree = _pgsql_udfree,
    .connected = _pgsql_on_connected, .ssl_exchanged = _pgsql_ssl_exchanged,
    .closed = _pgsql_udfree, .unpack = pgsql_unpack,
    .may_resume = _pgsql_may_resume,
};
static const prot_vtbl _vtbl_mongo = {
    .pkfree = _mongo_pkfree, .hsfree = _mongo_pkfree,
    .udfree = _mongo_udfree, .closed = _mongo_udfree,
    .unpack = mongo_unpack,
};
static const prot_vtbl _vtbl_kcp = {
    .udfree = _kcp_udfree, .closed = _kcp_udfree,
    .recvfrom = _kcp_unpack,
};
// 唯一的按 pktype 分派点。不写 default：新增 pack_type 时这里编译报错，逼着给它挂一张表
static const prot_vtbl *_prots_vtbl(pack_type pktype) {
    switch (pktype) {
    case PACK_NONE: return &_vtbl_none;
    case PACK_DNS: return &_vtbl_dns;
    case PACK_HTTP: return &_vtbl_http;
    case PACK_WEBSOCK: return &_vtbl_websock;
    case PACK_MQTT: return &_vtbl_mqtt;
    case PACK_SMTP: return &_vtbl_smtp;
    // 三个 custz 共一张表：custz_unpack 内部再按 pktype 选头部编码
    case PACK_CUSTZ_FIXED:
    case PACK_CUSTZ_FLAG:
    case PACK_CUSTZ_VAR: return &_vtbl_custz;
    case PACK_REDIS: return &_vtbl_redis;
    case PACK_MYSQL: return &_vtbl_mysql;
    case PACK_PGSQL: return &_vtbl_pgsql;
    case PACK_MONGO: return &_vtbl_mongo;
    case PACK_UDP_KCP: return &_vtbl_kcp;
    }
    return &_vtbl_none;// 到不了：上面已穷举 pack_type，这句只为消 -Wreturn-type
}
// 应用层握手完成推送：各协议握手完成时回调（注册见 prots_init），经消息汇推 MSG_TYPE_HANDSHAKED
static int32_t _prots_handshaked(sock_ctx *sk, int32_t client,
    ud_cxt *ud, int32_t erro, void *data, size_t lens) {
    void *target = g_emit.begin(ud->loader, ud->handle);
    if (NULL == target) {
        prots_hsfree(ud->pktype, data);
        return ERR_FAILED;
    }
    message_ctx msg = { 0 };
    msg.mtype = MSG_TYPE_HANDSHAKED;
    msg.subtype = ud->pktype;
    msg.sk = *sk;
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
    ZERO(&g_emit, sizeof(g_emit));
}
void prots_pkfree(pack_type pktype, void *data) {
    if (NULL == data) {
        return;
    }
    const prot_vtbl *v = _prots_vtbl(pktype);
    if (NULL != v->pkfree) {
        v->pkfree(data);
    } else {
        FREE(data);// 没挂 pkfree 的协议，pack 就是一块 malloc
    }
}
void prots_udp_pkfree(pack_type pktype, void *data) {
    if (NULL == data) {
        return;
    }
    const prot_vtbl *v = _prots_vtbl(pktype);
    if (NULL != v->udp_pkfree) {
        v->udp_pkfree(data);
    } else {
        FREE(data);
    }
}
void prots_hsfree(pack_type pktype, void *data) {
    if (NULL == data) {
        return;
    }
    const prot_vtbl *v = _prots_vtbl(pktype);
    if (NULL != v->hsfree) {
        v->hsfree(data);
    } else {
        FREE(data);
    }
}
void prots_udfree(void *arg) {
    if (NULL == arg) {
        return;
    }
    ud_cxt *ud = arg;
    const prot_vtbl *v = _prots_vtbl(ud->pktype);
    if (NULL != v->udfree) {
        v->udfree(ud);
    } else {
        FREE(ud->context);// 无状态协议不挂 context，这个 FREE 只是防有人挂了忘清
    }
}
// 连接关闭时通知各协议模块清理协议状态。挂的多是各自的 udfree——关连接清理与 ud 释放清理
// 本就是同一件事，两处都调也幂等。它比 udfree 少挂几个协议：状态会被上层观察到的
// （DB / smtp / kcp）才挂，图的是"先清理后唤醒"——emit CLOSE 之前状态已置断开；纯解析状态
// 的那几个（http / websock / mqtt / redis）等 ud_free 兜底就够
static inline void prots_closed(ud_cxt *ud) {
    if (NULL == ud) {
        return;
    }
    const prot_vtbl *v = _prots_vtbl(ud->pktype);
    if (NULL != v->closed) {
        v->closed(ud);
    }
}
// 新连接被接受时的回调。返回非 ERR_OK 即拒收该连接(evpub.h 的 accept_cb 契约:失败则自动关闭)
static inline int32_t prots_accepted(ev_ctx *ev, sock_ctx *sk, ud_cxt *ud) {
    const prot_vtbl *v = _prots_vtbl(ud->pktype);
    return (NULL != v->accepted) ? v->accepted(ev, sk, ud) : ERR_OK;
}
// 主动连接建立后的回调，部分协议需在此发送初始化包
static inline int32_t prots_connected(ev_ctx *ev, sock_ctx *sk, ud_cxt *ud, int32_t err) {
    const prot_vtbl *v = _prots_vtbl(ud->pktype);
    return (NULL != v->connected) ? v->connected(ev, sk, ud, err) : err;
}
// SSL 握手完成后的回调，部分协议需在 SSL 建立后发送认证包（pgsql 用于 SCRAM-SHA-256-PLUS 通道绑定）
static inline int32_t prots_ssl_exchanged(ev_ctx *ev, sock_ctx *sk, int32_t client, ud_cxt *ud, void *ssl) {
    (void)sk;
    (void)client;
    const prot_vtbl *v = _prots_vtbl(ud->pktype);
    return (NULL != v->ssl_exchanged) ? v->ssl_exchanged(ev, ud, ssl) : ERR_OK;
}
// 默认解包：将缓冲区所有数据一次性取出，适用于 PACK_NONE（透传）场景
static inline void *_prots_unpack_default(buffer_ctx *buf, size_t *size, ud_cxt *ud) {
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
// WS 承载子协议:只有真带出子协议包的帧才可能是响应。控制帧(PING/PONG/CLOSE)与零长数据帧
// 的 secpack 为 NULL,放过去会撞上 prots_may_resume 入口的 NULL 短路变成"恒可唤醒",
// 一个保活 PING 就能唤醒正等响应的协程,请求-响应从此错开一格
static int32_t _prots_ws_may_resume(void *data) {
    pack_type secprot = (pack_type)websock_secprot(data);
    void *secpack = websock_secpack(data);
    if (PACK_NONE != secprot
        && NULL == secpack) {
        return ERR_FAILED;
    }
    return prots_may_resume(secprot, secpack);
}
int32_t prots_may_resume(pack_type pktype, void *data) {
    if (NULL == data) {
        return ERR_OK;
    }
    const prot_vtbl *v = _prots_vtbl(pktype);
    return (NULL != v->may_resume) ? v->may_resume(data) : ERR_OK;
}
// 同 prots_unpack，表由调用方已取好的 v 给出
static inline void *_prots_unpack_v(const prot_vtbl *v, ev_ctx *ev, sock_ctx *sk, int32_t client,
    buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status) {
    *size = 0;
    *status = PROT_INIT;
    if (NULL != v->unpack) {
        return v->unpack(ev, sk, client, buf, ud, size, status);
    }
    // 透传：PACK_NONE 本就不解包，KCP 的分包在 _kcp_unpack 里按 UDP 路径走
    return _prots_unpack_default(buf, size, ud);
}
void *prots_unpack(ev_ctx *ev, sock_ctx *sk, int32_t client,
    buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status) {
    return _prots_unpack_v(_prots_vtbl(ud->pktype), ev, sk, client, buf, ud, size, status);
}
int32_t prots_net_accept(ev_ctx *ev, sock_ctx *sk, ud_cxt *ud) {
    void *target = g_emit.begin(ud->loader, ud->handle);
    if (NULL == target) {
        return ERR_FAILED;
    }
    // 被 prots_accepted 拒收时不投 ACCEPT 消息：上层不该看到一条随即被关掉的连接
    int32_t rtn = prots_accepted(ev, sk, ud);
    if (ERR_OK == rtn) {
        message_ctx msg = { 0 };
        msg.mtype = MSG_TYPE_ACCEPT;
        msg.subtype = ud->pktype;
        msg.sk = *sk;
        g_emit.emit(target, &msg);
    }
    g_emit.end(target);
    return rtn;
}
// 构造并 emit 一条 CLOSE 消息；调用方负责 begin/end target。erro 取 close_type
static inline void _prots_emit_close(void *target, sock_ctx *sk, int32_t client,
                              int32_t erro, ud_cxt *ud) {
    message_ctx msg = { 0 };
    msg.mtype = MSG_TYPE_CLOSE;
    msg.subtype = ud->pktype;
    msg.sk = *sk;
    msg.client = client;
    msg.erro = erro;
    msg.sess = sk->skid;// 始终尝试唤醒
    prots_closed(ud);
    g_emit.emit(target, &msg);
}
int32_t prots_net_connect(ev_ctx *ev, sock_ctx *sk, int32_t err, ud_cxt *ud) {
    void *target = g_emit.begin(ud->loader, ud->handle);
    if (NULL == target) {
        return ERR_FAILED;
    }
    // 用原始 err 判断：TCP 层本身已失败(fd 已被 event 层 remove,没人会再补 CLOSE)才需要我方补发；
    // TCP 成功但 prots_connected 才失败时 fd 仍在监听表中,_usk_on_connect_cb/_olp_on_connect_cb
    // 会因返回值非 ERR_OK 自行 _uev_disconnect 触发真实 CLOSE,此处再补会重复
    int32_t emitclose = ERR_OK != err;
    int32_t rtn = prots_connected(ev, sk, ud, err);
    if (ERR_OK != rtn) {
        err = rtn;
    }
    message_ctx msg = { 0 };
    msg.mtype = MSG_TYPE_CONNECT;
    msg.subtype = ud->pktype;
    msg.sk = *sk;
    msg.erro = err;
    msg.sess = ud->sess;
    g_emit.emit(target, &msg);
    if (emitclose) {
        // CONNECT 只发生在客户端发起连接场景，client 恒为 1；NEVERCONN 标记这是因连接失败补发的
        // 合成 CLOSE，分发层据此跳过 on_close 观察者。失败原因已在上面那条 CONNECT 的 erro 里
        _prots_emit_close(target, sk, 1, CLOSE_TYPE_NEVERCONN, ud);
    }
    g_emit.end(target);
    return err;
}
// RECV 消息里随连接固定的那几项。两个产出 RECV 的地方共用：逐包解出的 prots_net_recv，
// 与关闭时补末片的 _prots_emit_close_tail。data / size / slice / sess 由各自填——
// sess 在 prots_net_recv 那边是每包重读的，不能提到这里来
static inline void _prots_recv_msg_init(message_ctx *msg, sock_ctx *sk, int32_t client, ud_cxt *ud) {
    ZERO(msg, sizeof(*msg));
    msg->mtype = MSG_TYPE_RECV;
    msg->subtype = ud->pktype;
    msg->sk = *sk;
    msg->client = client;
}
void prots_net_recv(ev_ctx *ev, sock_ctx *sk, int32_t client, buffer_ctx *buf, size_t size, ud_cxt *ud) {
    void *target = g_emit.begin(ud->loader, ud->handle);
    if (NULL == target) {
        ev_close(ev, sk);
        return;
    }
    message_ctx msg;
    _prots_recv_msg_init(&msg, sk, client, ud);
    // 单帧多包只有 websock 承载子协议时才有，其余协议这里恒 NULL。pktype 在本次调用内不变
    // (msg.subtype 已按它快照)，故入口取一次，别每包查一遍表
    const prot_vtbl *v = _prots_vtbl(ud->pktype);
    void *data, *next;
    int32_t status;
    size_t esize;
    for (;;) {
        size = buffer_size(buf);
        data = _prots_unpack_v(v, ev, sk, client, buf, ud, &msg.size, &status);
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
            next = (NULL != v->next_pack) ? v->next_pack(data) : NULL;// 提前取出，防止消息(data)emit后在worker线程被释放.
            g_emit.emit(target, &msg);
            data = next;
        }
        if (BIT_CHECK(status, PROT_ERROR)) {
            ev_close(ev, sk);
            break;
        }
        if (BIT_CHECK(status, PROT_CLOSE)) {
            // 协议层正常关闭信号(如 WebSocket close frame):业务应答的那一帧可能还在 buf_s,
            // ev_close 关闭前会冲一次,小控制帧一次就写进内核了
            ev_close(ev, sk);
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
void prots_net_send(ev_ctx *ev, sock_ctx *sk, int32_t client, size_t size, ud_cxt *ud) {
    void *target = g_emit.begin(ud->loader, ud->handle);
    if (NULL == target) {
        ev_close(ev, sk);
        return;
    }
    message_ctx msg = { 0 };
    msg.mtype = MSG_TYPE_SEND;
    msg.subtype = ud->pktype;
    msg.sk = *sk;
    msg.client = client;
    msg.size = size;
    g_emit.emit(target, &msg);
    g_emit.end(target);
}
int32_t prots_net_ssl_exchanged(ev_ctx *ev, sock_ctx *sk, int32_t client, ud_cxt *ud, void *ssl) {
    void *target = g_emit.begin(ud->loader, ud->handle);
    if (NULL == target) {
        return ERR_FAILED;
    }
    int32_t rtn = prots_ssl_exchanged(ev, sk, client, ud, ssl);
    if (ERR_OK == rtn) {
        message_ctx msg = { 0 };
        msg.mtype = MSG_TYPE_SSLEXCHANGED;
        msg.subtype = ud->pktype;
        msg.sk = *sk;
        msg.client = client;
        msg.sess = ud->sess;
        g_emit.emit(target, &msg);
    }
    g_emit.end(target);
    return rtn;
}
// 关连接时补末片：只有"由连接关闭界定 body"的协议有这回事，且必须在 prots_closed 清状态之前跑。
// ORDERLY 与 TRUNCATED 才认为那类消息收完了，其余一律不补。TRUNCATED 也补是因为 TLS 少发一个
// close_notify 与真被截断在本层分不出，末片照给，信不信由收到 CLOSE 的一方按 erro 自行判
static inline void _prots_emit_close_tail(void *target, sock_ctx *sk, int32_t client,
                                   int32_t erro, ud_cxt *ud) {
    if (CLOSE_TYPE_ORDERLY != erro
        && CLOSE_TYPE_TRUNCATED != erro) {
        return;
    }
    const prot_vtbl *v = _prots_vtbl(ud->pktype);
    if (NULL == v->close_tail) {
        return;
    }
    void *pack = v->close_tail(ud);
    if (NULL == pack) {
        return;
    }
    message_ctx msg;
    _prots_recv_msg_init(&msg, sk, client, ud);
    msg.sess = ud->sess;
    msg.slice = PROT_SLICE_END;
    msg.data = pack;// size 留 0：末片是空载荷
    g_emit.emit(target, &msg);
}
void prots_net_close(ev_ctx *ev, sock_ctx *sk, int32_t client, int32_t erro, ud_cxt *ud) {
    (void)ev;
    void *target = g_emit.begin(ud->loader, ud->handle);
    if (NULL == target) {
        return;
    }
    _prots_emit_close_tail(target, sk, client, erro, ud);
    _prots_emit_close(target, sk, client, erro, ud);
    g_emit.end(target);
}
static inline void _prots_udp_default(ev_ctx *ev, sock_ctx *sk, char *buf, size_t size, netaddr_ctx *addr, ud_cxt *ud) {
    void *target = g_emit.begin(ud->loader, ud->handle);
    if (NULL == target) {
        ev_close(ev, sk);
        return;
    }
    message_ctx msg = { 0 };
    msg.mtype = MSG_TYPE_RECVFROM;
    msg.subtype = ud->pktype;
    msg.sk = *sk;
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
void prots_net_recvfrom(ev_ctx *ev, sock_ctx *sk, char *buf, size_t size, netaddr_ctx *addr, ud_cxt *ud) {
    const prot_vtbl *v = _prots_vtbl(ud->pktype);
    if (NULL != v->recvfrom) {
        v->recvfrom(ev, sk, buf, size, addr, ud);
    } else {
        // 非 KCP 的 UDP 一律把整个 datagram 原样上抛
        _prots_udp_default(ev, sk, buf, size, addr, ud);
    }
}
static inline msgdata_kind _message_data_kind(msg_type mtype) {
    switch (mtype) {
    case MSG_TYPE_RECV:
        return MSGDATA_PROT;
    case MSG_TYPE_RECVFROM:
        return MSGDATA_UDP;
    case MSG_TYPE_HANDSHAKED:
        return MSGDATA_HS;
    case MSG_TYPE_REQUEST:
    case MSG_TYPE_RESPONSE:
        return MSGDATA_RAW;
    // CLOSE 既不能加 data 也不能加 shared:协程 task 的 _coro_handle_closed 把同一条消息交给全部等待者、自己不清理,close 回调也不清理。
    // data 由本表挡住;shared 靠"唯一写入点 task_multi_request 把 mtype 写死成 REQUEST"挡住
    default:
        return MSGDATA_NONE;
    }
}
int32_t message_should_clean(message_ctx *msg) {
    // shared 路径：task_multi_call / task_multi_request 广播,无论 data 是否为 NULL 都需 ref-- 防止泄漏
    if (NULL != msg->shared) {
        return ERR_OK;
    }
    if (MSGDATA_NONE != _message_data_kind(msg->mtype)
        && NULL != msg->data) {
        return ERR_OK;
    }
    return ERR_FAILED;
}
void message_clean(message_ctx *msg) {
    // task_multi_call / task_multi_request 广播路径：N 个 message 共享同一份 data,各 task ref-- 归 0 才 FREE
    if (NULL != msg->shared) {
        shared_data_free(msg->shared, _free);
        return;
    }
    switch (_message_data_kind(msg->mtype)) {
    case MSGDATA_PROT:
        prots_pkfree(msg->subtype, msg->data);
        break;
    case MSGDATA_UDP:
        prots_udp_pkfree(msg->subtype, msg->data);
        break;
    case MSGDATA_HS:
        prots_hsfree(msg->subtype, msg->data);
        break;
    case MSGDATA_RAW:
        FREE(msg->data);
        break;
    case MSGDATA_NONE:
        break;
    }
}
int32_t message_may_keep(msg_type type) {
    switch (type) {
    case MSG_TYPE_ACCEPT:
    case MSG_TYPE_CONNECT:
    case MSG_TYPE_SSLEXCHANGED:
    case MSG_TYPE_HANDSHAKED:
    case MSG_TYPE_RECV:
    case MSG_TYPE_RECVFROM:
        return 1;
    default:
        return 0;
    }
}
// mtype 名字表(日志、dump、stat 用)。用指定初始化器逐项落位而不是按顺序排:
// msg_type 里新增一项时这里漏补, 那一格是 NULL, 喂给 %s 就是 UB —— 取值一律走 message_str
static const char *_mtype_names[MSG_TYPE_ALL] = {
    [MSG_TYPE_NONE] = "NONE",
    [MSG_TYPE_STARTUP] = "STARTUP",
    [MSG_TYPE_CLOSING] = "CLOSING",
    [MSG_TYPE_TIMEOUT] = "TIMEOUT",
    [MSG_TYPE_ACCEPT] = "ACCEPT",
    [MSG_TYPE_CONNECT] = "CONNECT",
    [MSG_TYPE_SSLEXCHANGED] = "SSLEXCHANGED",
    [MSG_TYPE_HANDSHAKED] = "HANDSHAKED",
    [MSG_TYPE_RECV] = "RECV",
    [MSG_TYPE_SEND] = "SEND",
    [MSG_TYPE_CLOSE] = "CLOSE",
    [MSG_TYPE_RECVFROM] = "RECVFROM",
    [MSG_TYPE_REQUEST] = "REQUEST",
    [MSG_TYPE_RESPONSE] = "RESPONSE"
};
const char *message_str(msg_type type) {
    if (type >= MSG_TYPE_NONE
        && type < MSG_TYPE_ALL
        && NULL != _mtype_names[type]) {
        return _mtype_names[type];
    }
    return "";
}
