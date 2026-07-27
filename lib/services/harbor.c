#include "services/harbor.h"
#include "protocol/http.h"
#include "utils/router.h"
#include "event/event.h"
#include "utils/binary.h"
#include "utils/utils.h"

// harbor 实例上下文（每 task 堆分配，存 task->arg，由 coro_get_arg 取；仅监听信息）
typedef struct harbor_ctx {
    uint16_t port;          // 监听端口
    uint64_t lsnid;         // 监听ID（ev_unlisten使用）
    struct evssl_ctx *ssl;  // SSL上下文（NULL表示不使用SSL）
    router_ctx *router;     // HTTP 路由器
    char ip[IP_LENS];       // 监听IP
}harbor_ctx;

// 填一条响应头：key/val 须在 router_req_respond 组包完成前保持有效(组包时按值拷进缓冲)
static void _harbor_set_head(http_header_ctx *hd, const char *key, const char *val) {
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
static void _harbor_respond(router_req *ctx, int32_t code, const int32_t *erro,
                            const char *ctype, void *body, size_t lens) {
    char ebuf[16];
    http_header_ctx extra[3];
    int32_t n = 0;
    _harbor_set_head(&extra[n++], "Server", "Srey");
    if (NULL != erro) {
        SNPRINTF(ebuf, sizeof(ebuf), "%d", *erro);
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
static void _harbor_respond_text(router_req *ctx, int32_t code) {
    const char *txt = http_code_status(code);
    _harbor_respond(ctx, code, NULL, "text/plain; charset=utf-8", (void *)txt, strlen(txt));
}
// 参数校验中间件（router_group 承载）：非法请求静默关连接（不暴露），合法则 router_next
static void _harbor_check(router_req *ctx) {
    struct http_pack_ctx *pack = ctx->pack;
    // 仅拒分片(harbor 不支持);空 body 放行(无参 RPC)
    if (0 != http_chunked(pack)) {
        ev_close(&ctx->task->loader->netev, ctx->sk.fd, ctx->sk.skid, 1);
        ctx->responded = 1;
        return;
    }
    size_t dn = 0;
    size_t tn = 0;
    if (NULL == router_req_query(ctx, "dst", &dn) || 0 == dn
        || NULL == router_req_query(ctx, "type", &tn) || 0 == tn) {
        ev_close(&ctx->task->loader->netev, ctx->sk.fd, ctx->sk.skid, 1);
        ctx->responded = 1;
        return;
    }
    router_next(ctx);
}
// /call 与 /request 共用：解析 dst/type/body，grab 目标 task，404 兜底；
// is_call!=0 走 task_call(单向投递不等响应)，否则走 coro_request(请求-响应，本协程内 yield 等待)
static void _harbor_dispatch(router_req *ctx, int32_t is_call) {
    size_t dn = 0;
    size_t tn = 0;
    size_t blen = 0;
    const char *ds = router_req_query(ctx, "dst", &dn);
    const char *tp = router_req_query(ctx, "type", &tn);
    void *body = http_data(ctx->pack, &blen);
    name_t dst = (name_t)strtoull(ds, NULL, 10);
    subtype_t type = (subtype_t)strtoul(tp, NULL, 10);
    if (subtype_reserved(type)) {
        _harbor_respond_text(ctx, 404);
        return;
    }
    task_ctx *to = task_grab(ctx->task->loader, dst);
    if (NULL == to) {
        _harbor_respond_text(ctx, 404);
        return;
    }
    if (is_call) {
        task_call(to, type, body, blen, 1);
        task_ungrab(to);
        _harbor_respond(ctx, 200, NULL, NULL, NULL, 0);
        return;
    }
    int32_t err = ERR_OK;
    size_t rlen = 0;
    void *rtn = coro_request(to, ctx->task, type, body, blen, 1, &err, &rlen);
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
// HTTP 接收回调：完整请求到达后交 router 派发（参数校验在 group 中间件，转发在 handler）；不支持 chunked，收到即拒绝并关闭连接
static void _net_recv(task_ctx *task, sk_id *sk, subtype_t pktype,
    uint8_t client, uint8_t slice, void *data, size_t size) {
    (void)pktype;
    (void)client;
    (void)size;
    if (0 != slice) {
        if (PROT_SLICE_START == slice) {
            router_reject_chunked(task, sk->fd, sk->skid);
        }
        return;
    }
    harbor_ctx *ctx = (harbor_ctx *)coro_get_arg(task);
    router_dispatch(ctx->router, task, sk->fd, sk->skid, (struct http_pack_ctx *)data);
}
// harbor任务启动回调：建路由器 + 注册参数校验中间件的 group + 监听
static void _harbor_startup(task_ctx *harbor) {
    harbor_ctx *ctx = (harbor_ctx *)coro_get_arg(harbor);
    task_recved(harbor, _net_recv);
    ctx->router = router_new();
    // 参数校验统一放 group 中间件，/call 与 /request 共享
    router_define(ctx->router, "check", _harbor_check);
    const char *mws[] = { "check" };
    router_group g;
    router_group_root(ctx->router, &g, "", mws, 1);
    router_post(ctx->router, &g, "/call", _harbor_call, NULL, 0);
    router_post(ctx->router, &g, "/request", _harbor_request, NULL, 0);
    if (ERR_OK != task_listen(harbor, PACK_HTTP, ctx->ssl, ctx->ip, ctx->port, &ctx->lsnid, 0)) {
        LOG_ERROR("task_listen %s:%d error", ctx->ip, ctx->port);
    }
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
    harbor_ctx *ctx;
    CALLOC(ctx, 1, sizeof(harbor_ctx));
    ctx->port = port;
#if WITH_SSL
    ctx->ssl = evssl_qury(ssl);
#else
    (void)ssl;
#endif
    safe_fill_str(ctx->ip, sizeof(ctx->ip), ip);
    if (NULL == coro_task_register(loader, tname, 4 * ONEK,
                                   _harbor_startup, _harbor_closing,
                                   _harbor_free, ctx)) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
void *harbor_pack(name_t task, int32_t call, subtype_t reqtype, void *data, size_t size, size_t *lens) {
    char url[512];
    if (0 != call) {
        SNPRINTF(url, sizeof(url), "/call?dst=%"PRIu64"&type=%u", task, reqtype);
    } else {
        SNPRINTF(url, sizeof(url), "/request?dst=%"PRIu64"&type=%u", task, reqtype);
    }
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    http_pack_req(&bwriter, "POST", url);
    http_pack_head(&bwriter, "Connection", "Keep-Alive");
    http_pack_head(&bwriter, "Content-Type", "application/octet-stream");
    http_pack_content(&bwriter, data, size);
    *lens = bwriter.offset;
    return bwriter.data;
}
