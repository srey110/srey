#include "advance/harbor.h"
#include "protocol/http.h"
#include "advance/router.h"
#include "event/event.h"
#include "utils/binary.h"
#include "utils/utils.h"

// harbor_pack 请求行在 type 之后的部分与两条固定头，线格式同 http_pack_req + http_pack_head
#define HARBOR_REQ_TAIL " HTTP/1.1"FLAG_CRLF"Connection: Keep-Alive"FLAG_CRLF"Content-Type: application/octet-stream"FLAG_CRLF

// harbor 实例上下文（每 task 堆分配，存 task->arg，由 coro_get_arg 取；仅监听信息）
typedef struct harbor_ctx {
    uint16_t port;          // 监听端口
    uint64_t lsnid;         // 监听ID（ev_unlisten使用）
    struct evssl_ctx *ssl;  // SSL上下文（NULL表示不使用SSL）
    router_ctx *router;     // HTTP 路由器
    char ip[IP_LENS];       // 监听IP
}harbor_ctx;

// 填一条响应头：key/val 须在 router_req_respond 组包完成前保持有效(组包时按值拷进缓冲)
static inline void _harbor_set_head(http_header_ctx *hd, const char *key, const char *val) {
    hd->key.data = (void *)key;
    hd->key.lens = strlen(key);
    hd->value.data = (void *)val;
    hd->value.lens = strlen(val);
}
// 构造并发送 HTTP 响应。body 为空即发空 body(Content-Length: 0)，不以状态文本充当负载——
// 否则目标的零长成功 ack 会在线上多出 2 字节 "OK"，被按 RPC 返回值解码的调用方当成数据。
// erro 非 NULL 时经 X-Srey-Erro 头回带目标真实错误码；ctype 为 NULL 则不写 Content-Type。
// 组头后交 router_req_respond 发出：响应管线(pack_resp → 头 → content → ev_send copy=0 →
// 置 responded)只在 router.c 一处实现，harbor 这里只负责自己的头策略
static inline void _harbor_respond(router_req *ctx, int32_t code, const int32_t *erro,
                            const char *ctype, void *body, size_t lens) {
    char ebuf[INT2STR_MAX];
    http_header_ctx extra[3];
    int32_t n = 0;
    _harbor_set_head(&extra[n++], "Server", "Srey");
    if (NULL != erro) {
        i64tostr(ebuf, *erro, 10);
        _harbor_set_head(&extra[n++], "X-Srey-Erro", ebuf);
    }
    if (NULL != ctype
        && NULL != body && lens > 0) {
        _harbor_set_head(&extra[n++], "Content-Type", ctype);
    }
    router_req_respond(ctx, code, extra, n, (const char *)body, lens);
}
// harbor 自身的诊断响应(非转发目标响应)：以状态文本为 body。Content-Type 与 router_req_text
// 保持一致(带 charset)，避免同一框架的两个 HTTP 面对同类响应给出不同的类型串
static inline void _harbor_respond_text(router_req *ctx, int32_t code) {
    const char *txt = http_code_status(code);
    _harbor_respond(ctx, code, NULL, "text/plain; charset=utf-8", (void *)txt, strlen(txt));
}
// /call 与 /request 共用：解析 dst/type/body，grab 目标 task，404 兜底；
// is_call!=0 走 task_call(单向投递不等响应)，否则走 coro_request(请求-响应，本协程内 yield 等待)。
// 请求体用 http_take_data 整块交给目标 task(copy=0)，不再拷一份；它与 dup_zero 的副本一样以 '\0' 结尾
static void _harbor_dispatch(router_req *ctx, int32_t is_call) {
    size_t dn = 0;
    size_t tn = 0;
    size_t blen = 0;
    const char *ds = router_req_query(ctx, "dst", &dn);
    const char *tp = router_req_query(ctx, "type", &tn);
    void *body;
    uint64_t dv = 0;
    uint64_t tv = 0;
    // 必须按 lens 截断解析：url_parse(decode=1) 就地解码只缩短 lens、不搬移后续字节，
    // 切片尾部残留解码前的旧字节("%310" 解码为 "10" 但缓冲仍读作 "1010")
    if (ERR_OK != strtou64(ds, dn, UINT64_MAX, &dv)
        || ERR_OK != strtou64(tp, tn, UINT16_MAX, &tv)) {
        _harbor_respond_text(ctx, 404);
        return;
    }
    name_t dst = (name_t)dv;
    subtype_t type = (subtype_t)tv;
    if (subtype_reserved(type)) {
        _harbor_respond_text(ctx, 404);
        return;
    }
    task_ctx *to = task_grab(ctx->task->loader, dst);
    if (NULL == to) {
        _harbor_respond_text(ctx, 404);
        return;
    }
    body = http_take_data(ctx->pack, &blen);
    if (is_call) {
        task_call(to, type, body, blen, 0);
        task_ungrab(to);
        _harbor_respond(ctx, 200, NULL, NULL, NULL, 0);
        return;
    }
    int32_t err = ERR_OK;
    size_t rlen = 0;
    void *rtn = coro_request(to, ctx->task, type, body, blen, 0, &err, &rlen);
    task_ungrab(to);
    _harbor_respond(ctx, (ERR_OK != err) ? 400 : 200, &err, "application/octet-stream", rtn, rlen);
}
// POST /call：单向投递（task_call），不等响应
static void _harbor_call(router_req *ctx) {
    _harbor_dispatch(ctx, 1);
}
// POST /request：请求-响应（coro_request，handler 处于协程栈可 yield），回带目标 task 响应
static void _harbor_request(router_req *ctx) {
    _harbor_dispatch(ctx, 0);
}
// HTTP 接收回调：取出本服务的 router 后转 router_net_recv（chunked 与派发都在那里）
static void _net_recv(task_ctx *task, sock_ctx *sk, subtype_t pktype,
    uint8_t client, uint8_t slice, void *data, size_t size) {
    harbor_ctx *ctx = (harbor_ctx *)coro_get_arg(task);
    router_net_recv(ctx->router, task, sk, pktype, client, slice, data, size);
}
// 连接关闭回调：清掉该连接尚未收齐的流式请求。
// 本服务眼下没有流式路由，接着是为了以后加了不至于漏，理由见 router_closed 的说明
static void _net_close(task_ctx *task, sock_ctx *sk, subtype_t pktype, uint8_t client, int32_t erro) {
    (void)pktype;
    (void)client;
    (void)erro;
    harbor_ctx *ctx = (harbor_ctx *)coro_get_arg(task);
    router_closed(ctx->router, sk);
}
// harbor任务启动回调：建路由器 + 注册 /call 与 /request + 监听
static void _harbor_startup(task_ctx *harbor) {
    harbor_ctx *ctx = (harbor_ctx *)coro_get_arg(harbor);
    task_recved(harbor, _net_recv);
    task_closed(harbor, _net_close);
    ctx->router = router_new();
    // 参数校验不再单设中间件：dst/type 的存在性判定与 _harbor_dispatch 的 strtou64 重复，
    // 结果同一类坏请求分两处决定、给出两种行为（缺参数静默关连接 vs 畸形参数回 404）。
    // 统一收到 _harbor_dispatch 一处：一律 404
    router_post(ctx->router, NULL, "/call", _harbor_call, NULL, 0);
    router_post(ctx->router, NULL, "/request", _harbor_request, NULL, 0);
    if (ERR_OK != task_listen(harbor, PACK_HTTP, ctx->ssl, ctx->ip, ctx->port, &ctx->lsnid, 0)) {
        LOG_ERROR("harbor task_listen %s:%d error.", ctx->ip, ctx->port);
        return;
    }
    LOG_INFO("harbor on %s:%d", ctx->ip, ctx->port);
}
// 释放 harbor task 关联资源
static void _harbor_free(void *arg) {
    if (NULL == arg) {
        return;
    }
    harbor_ctx *ctx = (harbor_ctx *)arg;
    if (NULL != ctx->router) {
        router_free(ctx->router);
    }
    FREE(ctx);
}
// harbor任务关闭回调：取消监听
static void _harbor_closing(task_ctx *harbor) {
    harbor_ctx *ctx = (harbor_ctx *)coro_get_arg(harbor);
    if (NULL == ctx) {
        return;
    }
    if (0 != ctx->lsnid) {
        ev_unlisten(&harbor->loader->netev, ctx->lsnid);
        ctx->lsnid = 0;
    }
}
int32_t harbor_start(loader_ctx *loader, const char *tname, const char *ssl, const char *ip, uint16_t port) {
    if (EMPTYSTR(tname) || 0 == port) {
        return ERR_OK;
    }
    if (NULL == ip || strlen(ip) >= IP_LENS) {
        return ERR_FAILED;
    }
    struct evssl_ctx *evssl = NULL;
    if (!EMPTYSTR(ssl)) {
#if WITH_SSL
        evssl = evssl_qury(ssl);
#endif
        // 配了名字却拿不到证书就直接拒绝启动，不能静默退化成明文：跨节点鉴权全指望这张证书，
        // 而 /call、/request 能往任意 handle 的 task 投递消息，运维只会以为链路是加密的。
        // 名字来自 evssl 注册表, 调用方必须排在注册之后(装配顺序见 srey/startup.c)
        if (NULL == evssl) {
            LOG_ERROR("harbor: evssl '%s' not registered before harbor starts, "
                      "refuse to listen in plaintext.", ssl);
            return ERR_FAILED;
        }
#if WITH_SSL
        // 有证书只说明链路加密，不代表验了对端。这里判 FAIL_IF_NO_PEER_CERT 而不是 PEER：
        // 服务端只设 PEER 时客户端不交证书照样握手成功，等于没验
        if (0 == (SSL_VERIFY_FAIL_IF_NO_PEER_CERT & SSL_CTX_get_verify_mode(evssl_sslctx(evssl)))) {
            LOG_WARN("harbor: evssl '%s' does not require a peer certificate - "
                     "the link is encrypted but anyone who can reach %s:%u still passes the "
                     "handshake and may inject messages into any task. Register it with "
                     "SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT.",
                     ssl, ip, port);
        }
#endif
    } else {
        // /call、/request 能往任意 handle 的 task 投任意 subtype 并读回响应，而句柄顺序递增可枚举。
        // 明文这条路只在受信内网成立，配错了必须看得见
        LOG_WARN("harbor: listening on %s:%u in plaintext with no authentication - "
                 "any client that can reach this port may inject messages into any task. "
                 "Configure 'ssl' unless this is a trusted internal network.", ip, port);
    }
    harbor_ctx *ctx;
    CALLOC(ctx, 1, sizeof(harbor_ctx));
    ctx->port = port;
    ctx->ssl = evssl;
    // 上面的 strlen(ip) >= IP_LENS 已挡过，ctx->ip 正好 IP_LENS，装得下
    safe_fill_str(ctx->ip, sizeof(ctx->ip), ip);
    if (NULL == coro_task_register(loader, tname, 4 * ONEK,
                                   _harbor_startup, _harbor_closing,
                                   _harbor_free, ctx)) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
void *harbor_pack(name_t task, int32_t call, subtype_t reqtype, void *data, size_t size, size_t *lens) {
    const size_t head_reserve = 256;
    binary_ctx bwriter;
    binary_init_write(&bwriter, size + head_reserve, 0);
    if (0 != call) {
        binary_set_binary(&bwriter, "POST /call?dst=", sizeof("POST /call?dst=") - 1);
    } else {
        binary_set_binary(&bwriter, "POST /request?dst=", sizeof("POST /request?dst=") - 1);
    }
    binary_set_uint(&bwriter, task, 10);
    binary_set_binary(&bwriter, "&type=", sizeof("&type=") - 1);
    binary_set_uint(&bwriter, reqtype, 10);
    binary_set_binary(&bwriter, HARBOR_REQ_TAIL, sizeof(HARBOR_REQ_TAIL) - 1);
    http_pack_content(&bwriter, data, size);
    *lens = bwriter.offset;
    return bwriter.data;
}
