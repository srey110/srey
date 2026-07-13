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

// 构造并发送 HTTP 响应（有数据时 Content-Type 为 octet-stream，否则 text/plain 状态文本）
// router handler 内自行响应后置 ctx->responded = 1，防 router_dispatch 兜底 500
static void _harbor_respond(router_req *ctx, int32_t code, void *body, size_t lens) {
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    http_pack_resp(&bwriter, code);
    http_pack_head(&bwriter, "Server", "Srey");
    if (NULL != body && lens > 0) {
        http_pack_head(&bwriter, "Content-Type", "application/octet-stream");
        http_pack_content(&bwriter, body, lens);
    } else {
        http_pack_head(&bwriter, "Content-Type", "text/plain");
        const char *erro = http_code_status(code);
        http_pack_content(&bwriter, (void *)erro, strlen(erro));
    }
    ev_send(&ctx->task->loader->netev, ctx->sk.fd, ctx->sk.skid, bwriter.data, bwriter.offset, 0);
    ctx->responded = 1;
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
// POST /call：单向投递（task_call），不等响应
static void _harbor_call(router_req *ctx) {
    size_t dn = 0;
    size_t tn = 0;
    size_t blen = 0;
    const char *ds = router_req_query(ctx, "dst", &dn);
    const char *tp = router_req_query(ctx, "type", &tn);
    void *body = http_data(ctx->pack, &blen);
    name_t dst = (name_t)strtoull(ds, NULL, 10);
    subtype_t type = (subtype_t)strtoul(tp, NULL, 10);
    task_ctx *to = task_grab(ctx->task->loader, dst);
    if (NULL == to) {
        _harbor_respond(ctx, 404, NULL, 0);
        return;
    }
    task_call(to, type, body, blen, 1);
    task_ungrab(to);
    _harbor_respond(ctx, 200, NULL, 0);
}
// POST /request：请求-响应（coro_request，handler 处于协程栈可 yield），回带目标 task 响应
static void _harbor_request(router_req *ctx) {
    size_t dn = 0;
    size_t tn = 0;
    size_t blen = 0;
    const char *ds = router_req_query(ctx, "dst", &dn);
    const char *tp = router_req_query(ctx, "type", &tn);
    void *body = http_data(ctx->pack, &blen);
    name_t dst = (name_t)strtoull(ds, NULL, 10);
    subtype_t type = (subtype_t)strtoul(tp, NULL, 10);
    task_ctx *to = task_grab(ctx->task->loader, dst);
    if (NULL == to) {
        _harbor_respond(ctx, 404, NULL, 0);
        return;
    }
    int32_t err = ERR_FAILED;
    size_t rlen = 0;
    void *rtn = coro_request(to, ctx->task, type, body, blen, 1, &err, &rlen);
    task_ungrab(to);
    if (ERR_OK != err) {
        _harbor_respond(ctx, 400, rtn, rlen);
    } else {
        _harbor_respond(ctx, 200, rtn, rlen);
    }
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
