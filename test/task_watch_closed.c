#include "task_watch_closed.h"

#define WATCH_LOG_MAX 8

typedef struct watch_ctx {
    uint16_t httpport;
    int32_t nlog;
    int32_t real;               // 普通 task：1 = 已发起真连接
    int32_t log[WATCH_LOG_MAX]; // 调用序：1 观察者 A、2 观察者 B、3 业务回调
    int32_t *ok;
    uint64_t skid;              // 真连接的 skid；0 = 还没连上
}watch_ctx;

static void _watch_log(watch_ctx *ctx, int32_t who) {
    if (ctx->nlog < WATCH_LOG_MAX) {
        ctx->log[ctx->nlog] = who;
    }
    ctx->nlog++;
}
// 观察者拿到的 sess 须等于 skid(连接级关闭)，不等记 -1，收尾判定就过不了
static void _watch_a(task_ctx *task, sock_ctx *sk, subtype_t pktype, uint8_t client, int32_t erro,
                     uint64_t sess, void *arg) {
    (void)task;
    (void)pktype;
    (void)client;
    (void)erro;
    _watch_log((watch_ctx *)arg, sess == sk->skid ? 1 : -1);
}
static void _watch_b(task_ctx *task, sock_ctx *sk, subtype_t pktype, uint8_t client, int32_t erro,
                     uint64_t sess, void *arg) {
    (void)task;
    (void)pktype;
    (void)client;
    (void)erro;
    _watch_log((watch_ctx *)arg, sess == sk->skid ? 2 : -1);
}
// 业务回调收尾判定：只该见到真连接那一条 CLOSE，且此前恰好是 A、B 各一次，sess 都等于该连接的 skid。
// 拒连那条若被误投，nlog 会多出来或 skid 对不上
static void _watch_finish(task_ctx *task, watch_ctx *ctx, sock_ctx *sk, uint64_t sess) {
    _watch_log(ctx, 3);
    if (0 == ctx->skid
        || sk->skid != ctx->skid
        || sess != ctx->skid
        || 3 != ctx->nlog
        || 1 != ctx->log[0]
        || 2 != ctx->log[1]
        || 3 != ctx->log[2]) {
        LOG_ERROR("watch_closed %s: unexpected close order, nlog %d.", task->name, ctx->nlog);
        return;
    }
    ATOMIC_SET((atomic_t *)ctx->ok, 1);
}

// ── 普通 task：回调驱动 ────────────────────────────────────────────────────
static void _normal_closed(task_ctx *task, sock_ctx *sk, subtype_t pktype, uint8_t client, int32_t erro, uint64_t sess) {
    (void)pktype;
    (void)client;
    (void)erro;
    _watch_finish(task, (watch_ctx *)task->arg, sk, sess);
}
static void _normal_connect_real(task_ctx *task, watch_ctx *ctx) {
    sock_ctx sk;
    ctx->real = 1;
    if (ERR_OK != task_connect(task, PACK_NONE, NULL, "127.0.0.1", ctx->httpport, NETEV_NONE, NULL, 0, &sk)) {
        LOG_ERROR("watch_closed normal: connect %d failed.", (int32_t)ctx->httpport);
    }
}
// 拒连那条 CONNECT 失败时，它的合成 CLOSE 已紧跟着入队，真连接的消息必然排在它后面
static void _normal_connected(task_ctx *task, sock_ctx *sk, subtype_t pktype, int32_t erro) {
    (void)pktype;
    watch_ctx *ctx = (watch_ctx *)task->arg;
    if (0 == ctx->real) {
        if (ERR_OK == erro) {
            LOG_ERROR("watch_closed normal: 127.0.0.1:1 unexpectedly accepted.");
            ev_close(&task->loader->netev, sk);
            return;
        }
        _normal_connect_real(task, ctx);
        return;
    }
    if (ERR_OK != erro) {
        LOG_ERROR("watch_closed normal: connect %d failed.", (int32_t)ctx->httpport);
        return;
    }
    ctx->skid = sk->skid;
    ev_close(&task->loader->netev, sk);
}
static void _normal_startup(task_ctx *task) {
    watch_ctx *ctx = (watch_ctx *)task->arg;
    sock_ctx sk;
    task_watch_closed(task, _watch_a, ctx);
    task_watch_closed(task, _watch_b, ctx);
    task_closed(task, _normal_closed);
    task_connected(task, _normal_connected);
    // 同步就失败的平台没有合成 CLOSE 可验，直接连真端口
    if (ERR_OK != task_connect(task, PACK_NONE, NULL, "127.0.0.1", 1, NETEV_NONE, NULL, 0, &sk)) {
        _normal_connect_real(task, ctx);
    }
}

// ── 协程 task：同步写法 ────────────────────────────────────────────────────
static void _coro_closed(task_ctx *task, sock_ctx *sk, subtype_t pktype, uint8_t client, int32_t erro, uint64_t sess) {
    (void)pktype;
    (void)client;
    (void)erro;
    _watch_finish(task, (watch_ctx *)coro_get_arg(task), sk, sess);
}
static void _coro_startup(task_ctx *task) {
    watch_ctx *ctx = (watch_ctx *)coro_get_arg(task);
    sock_ctx sk;
    task_watch_closed(task, _watch_a, ctx);
    task_watch_closed(task, _watch_b, ctx);
    task_closed(task, _coro_closed);
    if (ERR_OK == coro_connect(task, PACK_NONE, NULL, "127.0.0.1", 1, 0, NULL, &sk)) {
        ev_close(&task->loader->netev, &sk);
        LOG_ERROR("watch_closed coro: 127.0.0.1:1 unexpectedly accepted.");
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != coro_connect(task, PACK_NONE, NULL, "127.0.0.1", ctx->httpport, 0, NULL, &sk)) {
        LOG_ERROR("watch_closed coro: connect %d failed.", (int32_t)ctx->httpport);
        return;
    }
    ctx->skid = sk.skid;
    ev_close(&task->loader->netev, &sk);
}

static watch_ctx *_watch_ctx_new(uint16_t httpport, int32_t *ok) {
    watch_ctx *ctx;
    CALLOC(ctx, 1, sizeof(watch_ctx));
    ctx->httpport = httpport;
    ctx->ok = ok;
    return ctx;
}
void task_watch_closed_start(loader_ctx *loader, uint16_t httpport, int32_t *ok_normal, int32_t *ok_coro) {
    watch_ctx *ctx = _watch_ctx_new(httpport, ok_normal);
    task_ctx *task = task_new(loader, "watch_closed_normal", 0, NULL, _free, ctx);
    if (ERR_OK != task_register(task, _normal_startup, NULL)) {
        task_free(task);
    }
    coro_task_register(loader, "watch_closed_coro", 0, _coro_startup, NULL, _free,
                       _watch_ctx_new(httpport, ok_coro));
}
