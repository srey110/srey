#include "task_router.h"
#include "utils/router.h"

// ── server task ────────────────────────────────────────────────────────────
// server 是一个普通 (非协程) task: _net_recv 同步调 router_dispatch, handler/中间件
// 全部在 worker 线程当场跑完, 不涉及 yield。router_ctx 单例放全局, 测试启动期间
// 不会切换实例; ATOMIC_* 操作 _g_post_count 仅用于跨 task 验证 next 后置已执行,
// 单 worker 内本来就不竞争, 只为表达"读写发生在不同 task 上"的语义清晰

// 计数器: post-tag 中间件 next 返回后 +1, 客户端通过 GET /__stats 读回验证
static atomic_t     _g_post_count = 0;
static uint16_t     _g_port       = 0;

// ── handlers ───────────────────────────────────────────────────────────────
// 每个 handler 对应客户端一项断言 (见 _run_all);
// handler 内只调 router_req_text/json, 不调阻塞 API, 同步完成响应

// GET / → text "root"; 覆盖空路径 + 字面量精确匹配
static void _h_root(router_req *ctx) {
    router_req_text(ctx, 200, "root", 4);
}
// GET /user/{id} → 回写 id 原文; 覆盖 PARAM 段提取
static void _h_user(router_req *ctx) {
    size_t n;
    const char *id = router_req_param(ctx, "id", &n);
    if (NULL == id) {
        // PARAM 段必填, _match_path 走通就一定有值; 这里冗余保护以防接口契约变更
        router_req_text(ctx, 500, "no id", 5);
        return;
    }
    router_req_text(ctx, 200, id, n);
}
// GET /file/{path?} → 有 path 回原文, 无则 "none"; 覆盖 OPT 段可选提取
static void _h_file(router_req *ctx) {
    size_t n;
    const char *p = router_req_param(ctx, "path", &n);
    if (NULL == p) {
        router_req_text(ctx, 200, "none", 4);
    } else {
        router_req_text(ctx, 200, p, n);
    }
}
// GET /static/* → 固定回 "static-ok"; 覆盖 WILD 末尾通配 (任意后续段都匹配)
static void _h_static(router_req *ctx) {
    router_req_text(ctx, 200, "static-ok", 9);
}
// GET /query?a=X&b=Y → 回 "a=X b=Y"; 覆盖 URL query 解析 (url_parse 已 url_decode)
static void _h_query(router_req *ctx) {
    size_t alen, blen;
    const char *a = router_req_query(ctx, "a", &alen);
    const char *b = router_req_query(ctx, "b", &blen);
    char buf[128];
    int32_t k = SNPRINTF(buf, sizeof(buf), "a=%.*s b=%.*s",
                         (int32_t)(NULL == a ? 0 : alen), NULL == a ? "" : a,
                         (int32_t)(NULL == b ? 0 : blen), NULL == b ? "" : b);
    router_req_text(ctx, 200, buf, (size_t)k);
}
// GET /qexist?a=... → 区分 a 键不存在(NULL→"missing")/值空(非NULL+len0→"empty")/有值("value");
// 验证 router_req_query 对 ?a= 返非 NULL 零长指针, 与 Lua query 子表 "" 对齐
static void _h_qexist(router_req *ctx) {
    size_t alen;
    const char *a = router_req_query(ctx, "a", &alen);
    const char *r = (NULL == a) ? "missing" : (0 == alen ? "empty" : "value");
    router_req_text(ctx, 200, r, strlen(r));
}
// POST /admin/stats → JSON; 覆盖 router_req_json (Content-Type 自动写)
static void _h_admin_stats(router_req *ctx) {
    const char *json = "{\"ok\":true}";
    router_req_json(ctx, 200, json, strlen(json));
}
// GET /forget → 故意不写响应, 让 router_dispatch 末尾 !responded 兜底 500
static void _h_forget(router_req *ctx) {
    (void)ctx;
}
// POST /only-post → text "post-ok"; 配合 GET /only-post → 404 测试方法位掩码隔离
static void _h_only_post(router_req *ctx) {
    router_req_text(ctx, 200, "post-ok", 7);
}
// GET /needauth → text "authed"; 仅在 auth 中间件放行后调到 (token 错误时被截断)
static void _h_needauth(router_req *ctx) {
    router_req_text(ctx, 200, "authed", 6);
}
// GET /post-mw → text "tagged"; 配合 post-tag 中间件验证 next 后置 (handler 先跑完,
// post-tag 在 router_next 返回后 ATOMIC_ADD 计数器)
static void _h_post_mw(router_req *ctx) {
    router_req_text(ctx, 200, "tagged", 6);
}
// GET /__stats → 当前 _g_post_count 字符串值; 客户端打完 N 次 /post-mw 后读出验证
static void _h_stats(router_req *ctx) {
    char buf[32];
    int32_t cnt = (int32_t)ATOMIC_GET(&_g_post_count);
    int32_t k = SNPRINTF(buf, sizeof(buf), "%d", cnt);
    router_req_text(ctx, 200, buf, (size_t)k);
}
// GET /a/{x?}/b → OPT 中置前瞻: 有值返 "x=<val>", 无值(前瞻跳过)返 "x=none"
static void _h_opt_mid(router_req *ctx) {
    size_t n;
    const char *x = router_req_param(ctx, "x", &n);
    if (NULL == x) {
        router_req_text(ctx, 200, "x=none", 6);
    } else {
        char buf[64];
        int32_t k = SNPRINTF(buf, sizeof(buf), "x=%.*s", (int32_t)n, x);
        router_req_text(ctx, 200, buf, (size_t)k);
    }
}
// GET /g1/g2/deep → "deep=11"; 嵌套 group 终点 handler:
// g1mw 中间件先 ctx->user += 1, g2mw 中间件再 += 10, 累加值 11 由 handler 写出
// 验证: (a) 嵌套 group 中间件按父→子顺序入链 (b) ctx->user 跨中间件传值
static void _h_deep(router_req *ctx) {
    intptr_t accum = (intptr_t)ctx->user;
    char buf[32];
    int32_t k = SNPRINTF(buf, sizeof(buf), "deep=%ld", (long)accum);
    router_req_text(ctx, 200, buf, (size_t)k);
}

// 构造 nseg 段 "/s" 重复路径写入 buf,返回 buf;server 注册与 client 请求共用,测段数超限拒绝
static const char *_segpath(char *buf, size_t cap, int32_t nseg) {
    size_t pos = 0;
    int32_t i;
    for (i = 0; i < nseg && pos + 2 < cap; i++) {
        buf[pos++] = '/';
        buf[pos++] = 's';
    }
    buf[pos] = '\0';
    return buf;
}
// GET 64 段 "/s/.../s" → "s64"; 覆盖 URL_MAX_PATH_DEPTH 段精确路由,验证 >64 段请求不误命中
static void _h_seg64(router_req *ctx) {
    router_req_text(ctx, 200, "s64", 3);
}

// ── middlewares ────────────────────────────────────────────────────────────

// auth: 检查 X-Token: secret, 不匹配则直接写 401 并不调 router_next → 截断,
// _h_needauth 不会被执行; 验证中间件截断语义
static void _mw_auth(router_req *ctx) {
    size_t lens = 0;
    char *token = router_req_header(ctx, "X-Token", &lens);
    if (NULL == token || 6 != lens || 0 != memcmp(token, "secret", 6)) {
        router_req_text(ctx, 401, "no", 2);
        return;
    }
    router_next(ctx);
}
// post-tag: 先 router_next 让 handler 跑完, 返回后再 ATOMIC_ADD 计数器
// 验证 next 后置处理 (Express/Laravel 风格的洋葱模型); 单纯返回值无法证明这点,
// 因为 handler 写完响应客户端已经收到, 所以借助跨请求的全局计数器观察
static void _mw_post_tag(router_req *ctx) {
    router_next(ctx);
    ATOMIC_ADD(&_g_post_count, 1);
}
// g1mw / g2mw: 给 ctx->user 加不同数值后 router_next, _h_deep 读累加值
// 不同步长 (+1 / +10) 用来区分两个中间件都执行 vs 只执行其中一个
static void _mw_g1(router_req *ctx) {
    ctx->user = (void *)(intptr_t)(((intptr_t)ctx->user) + 1);
    router_next(ctx);
}
static void _mw_g2(router_req *ctx) {
    ctx->user = (void *)(intptr_t)(((intptr_t)ctx->user) + 10);
    router_next(ctx);
}

// server _net_recv: HTTP 完整包到达 (slice == 0) 时分发, 分片态忽略
// (任务路由不处理 chunked 请求体, 简化假设)
static void _server_net_recv(task_ctx *task, SOCKET fd, uint64_t skid,
                             subtype_t pktype, uint8_t client, uint8_t slice,
                             void *data, size_t size) {
    (void)pktype; (void)client; (void)size;
    if (0 != slice) {
        return;
    }
    router_dispatch((router_ctx *)task->arg, task, fd, skid, (struct http_pack_ctx *)data);
}
// 用户数据释放(argfree, task_free 时调): router_free 一并释放所有 entry/segs/mws/named 字符串
static void _router_free(void *arg) {
    router_free((router_ctx *)arg);
}
// 启动时注册具名中间件 + 路由 + 嵌套 group + listen(router 已在 start 建好,存 task->arg)
// 路由表覆盖: 字面量 / PARAM / OPT / WILD / query / 多方法 / 路由级中间件 / 嵌套 group
static void _server_startup(task_ctx *task) {
    router_ctx *r = (router_ctx *)task->arg;
    task_recved(task, _server_net_recv);

    // 4 个具名中间件先注册, 后续 router_get/post 的 mws 数组按名引用
    router_define(r, "auth",     _mw_auth);
    router_define(r, "post-tag", _mw_post_tag);
    router_define(r, "g1mw",     _mw_g1);
    router_define(r, "g2mw",     _mw_g2);

    // 9 条平铺路由 (无中间件): 覆盖各种 path 模板和方法位掩码
    router_get(r, NULL, "/",             _h_root,        NULL, 0);
    router_get(r, NULL, "/user/{id}",    _h_user,        NULL, 0);
    router_get(r, NULL, "/file/{path?}", _h_file,        NULL, 0);
    router_get(r, NULL, "/static/*",     _h_static,      NULL, 0);
    // 参数段 + 末尾通配: 命中后通配前的 {id} 必须仍对 handler 可见
    router_get(r, NULL, "/asset/{id}/*", _h_user,        NULL, 0);
    router_get(r, NULL, "/query",        _h_query,       NULL, 0);
    router_get(r, NULL, "/qexist",       _h_qexist,      NULL, 0);
    // {a?b} 参数名含内部 ?, 按 B2 文法当字面量段(对齐 Lua); 故 /litq/xyz 不命中参数 → 404
    router_get(r, NULL, "/litq/{a?b}",   _h_root,        NULL, 0);
    router_get(r, NULL, "/a/{x?}/b",    _h_opt_mid,     NULL, 0);
    router_post(r, NULL, "/admin/stats",  _h_admin_stats, NULL, 0);
    router_get(r, NULL, "/forget",       _h_forget,      NULL, 0);
    router_post(r, NULL, "/only-post",    _h_only_post,   NULL, 0);
    router_get(r, NULL, "/__stats",      _h_stats,       NULL, 0);

    // 路由级中间件: auth 截断验证 + post-tag 后置验证
    const char *auth_mws[] = { "auth" };
    router_get(r, NULL, "/needauth", _h_needauth, auth_mws, 1);
    const char *tag_mws[] = { "post-tag" };
    router_get(r, NULL, "/post-mw", _h_post_mw, tag_mws, 1);

    // 嵌套 group: /g1 + g1mw → /g2 + g2mw → /deep
    // 注册时 router_add 沿父链拼接 prefix 得到 "/g1/g2/deep", 同时把 g1mw + g2mw
    // 按父→子顺序合并进 entry->mws, dispatch 时一并入 chain
    const char *g1_names[] = { "g1mw" };
    router_group g1;
    router_group_root(r, &g1, "/g1", g1_names, 1);
    const char *g2_names[] = { "g2mw" };
    router_group g2;
    router_group_nest(&g1, &g2, "/g2", g2_names, 1);
    router_get(r, &g2, "/deep", _h_deep, NULL, 0);

    // URL_MAX_PATH_DEPTH(64) 段精确路由:测 >64 段请求被拒(400)不误命中(64 须与 urlparse.h URL_MAX_PATH_DEPTH 同步)
    char seg64_path[160];
    router_get(r, NULL, _segpath(seg64_path, sizeof(seg64_path), 64), _h_seg64, NULL, 0);

    uint64_t id;
    if (ERR_OK != task_listen(task, PACK_HTTP, NULL, "0.0.0.0", _g_port, &id, 0)) {
        LOG_WARN("task_router_server task_listen %d error.", _g_port);
    }
}

// server task 启动入口: 普通 task; router 在 start 建好存 task->arg, 由 argfree(_router_free) 释放
void task_router_server_start(loader_ctx *loader, const char *name, uint16_t port) {
    _g_port = port;
    router_ctx *router = router_new();
    task_ctx *task = task_new(loader, name, 0, NULL, _router_free, router);
    if (ERR_OK != task_register(task, _server_startup, NULL)) {
        task_free(task);
    }
}

// ── client task ────────────────────────────────────────────────────────────
// client 必须是协程 task (用 coro_task_register): coro_connect / coro_send 都会
// yield, 同步完成 16 项断言后把结果写回 result_slot, 由 main.c 末尾汇总

typedef struct task_router_client_ctx {
    int32_t *result;   // 指向 main.c testlist[] 中 "router_test" 槽, 通过/失败写 1/0
    uint16_t port;     // server 监听端口, 与 portlist["router_sv"] 同步
    int32_t  err;
} task_router_client_ctx;

// 通用断言 helper: 单次 connect → 构造 HTTP 请求 → coro_send 取响应 → 校验 code + body
// hk/hv 非 NULL 时附加一条 header (用于发 X-Token);
// expect_body == NULL 表示只校验状态码, 不比对 body (常用于 404/500)
// 每次都建独立连接, 不复用 keep-alive, 减少跨断言干扰
static int32_t _do_req(task_ctx *task, uint16_t port,
                       const char *method, const char *url,
                       const char *hk, const char *hv,
                       int32_t expect_code, const char *expect_body) {
    SOCKET fd;
    uint64_t skid;
    if (ERR_OK != coro_connect(task, PACK_HTTP, NULL, "127.0.0.1", port, 0, NULL, &fd, &skid)) {
        LOG_WARN("router test: connect to %d failed for %s %s.", port, method, url);
        return ERR_FAILED;
    }
    // 组装请求: method url HTTP/1.1\r\nHost: ...\r\n[hk: hv\r\n]\r\n
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    http_pack_req(&bw, method, url);
    http_pack_head(&bw, "Host", "127.0.0.1");
    if (NULL != hk) {
        http_pack_head(&bw, hk, hv);
    }
    http_pack_end(&bw);
    size_t rsize;
    // coro_send 内部 yield 等响应包, 返回时框架已完成 http_unpack
    struct http_pack_ctx *resp = coro_send(task, fd, skid, bw.data, bw.offset, &rsize, 0);
    int32_t rtn = ERR_FAILED;
    if (NULL == resp) {
        LOG_WARN("router test: coro_send failed for %s %s.", method, url);
        goto done;
    }
    // status[1] 是状态码 (字符串形态, 例 "200"); 长度精确匹配避免 "20"/"200" 误判
    buf_ctx *st = http_status(resp);
    char codestr[8];
    SNPRINTF(codestr, sizeof(codestr), "%d", expect_code);
    if (!buf_compare(&st[1], codestr, strlen(codestr))) {
        LOG_WARN("router test: %s %s expected code %d, got %.*s.",
                 method, url, expect_code, (int32_t)st[1].lens, (char *)st[1].data);
        goto done;
    }
    // 响应头里 Content-Length 必须唯一 (router 曾手写一次 + http_pack_content 再写一次)
    uint32_t nheader = http_nheader(resp);
    int32_t clcnt = 0;
    uint32_t hi;
    http_header_ctx *hd;
    for (hi = 0; hi < nheader; hi++) {
        hd = http_header_at(resp, hi);
        if (buf_compare(&hd->key, "Content-Length", sizeof("Content-Length") - 1)) {
            clcnt++;
        }
    }
    if (1 != clcnt) {
        LOG_WARN("router test: %s %s expected 1 Content-Length header, got %d.", method, url, clcnt);
        goto done;
    }
    if (NULL != expect_body) {
        size_t dlen;
        void *body = http_data(resp, &dlen);
        size_t want = strlen(expect_body);
        if (NULL == body || dlen != want || 0 != memcmp(body, expect_body, dlen)) {
            LOG_WARN("router test: %s %s expected body '%s', got '%.*s'.",
                     method, url, expect_body, (int32_t)dlen, (char *)body);
            goto done;
        }
    }
    rtn = ERR_OK;
done:
    ev_close(&task->loader->netev, fd, skid, 1);
    return rtn;
}

// 24 项断言依次跑, 任一失败都 bad 置位; 全部通过返 ERR_OK
// 每次 _do_req 之间插 task_isclosing 早返, SIGINT 时尽快收尾
// bad 用位掩码记录, 单次跑不会复用, 但若失败时 LOG_WARN 输出可看到哪几位出错
static int32_t _run_all(task_ctx *task, uint16_t port) {
    int32_t bad = 0;
    // 计数器是全局静态变量, 进程多次跑 test 会累加, 先清零隔离
    ATOMIC_SET(&_g_post_count, 0);
    // [0]  字面量 + 默认 200
    if (ERR_OK != _do_req(task, port, "GET",  "/",                NULL, NULL, 200, "root"))      bad |= (1 << 0);
    if (task_isclosing(task)) return ERR_FAILED;
    // [1]  路由不存在 → 404 (dispatch 兜底, 不进任何 handler)
    if (ERR_OK != _do_req(task, port, "GET",  "/nonexist",        NULL, NULL, 404, NULL))        bad |= (1 << 1);
    if (task_isclosing(task)) return ERR_FAILED;
    // [2]  PARAM 段提取
    if (ERR_OK != _do_req(task, port, "GET",  "/user/42",         NULL, NULL, 200, "42"))        bad |= (1 << 2);
    if (task_isclosing(task)) return ERR_FAILED;
    // [3]  OPT 段缺失 → handler 看到 NULL 走 "none" 分支
    if (ERR_OK != _do_req(task, port, "GET",  "/file",            NULL, NULL, 200, "none"))      bad |= (1 << 3);
    if (task_isclosing(task)) return ERR_FAILED;
    // [4]  OPT 段存在 → 原样回写
    if (ERR_OK != _do_req(task, port, "GET",  "/file/abc",        NULL, NULL, 200, "abc"))       bad |= (1 << 4);
    if (task_isclosing(task)) return ERR_FAILED;
    // [5]  WILD 单段
    if (ERR_OK != _do_req(task, port, "GET",  "/static/x",        NULL, NULL, 200, "static-ok")) bad |= (1 << 5);
    if (task_isclosing(task)) return ERR_FAILED;
    // [6]  WILD 多段都匹配, 不要求 handler 区分剩余 path
    if (ERR_OK != _do_req(task, port, "GET",  "/static/x/y/z",    NULL, NULL, 200, "static-ok")) bad |= (1 << 6);
    if (task_isclosing(task)) return ERR_FAILED;
    // [7]  query 参数解析
    if (ERR_OK != _do_req(task, port, "GET",  "/query?a=1&b=2",   NULL, NULL, 200, "a=1 b=2"))   bad |= (1 << 7);
    if (task_isclosing(task)) return ERR_FAILED;
    // [8]  auth 中间件截断: 无 X-Token → 401, handler 不应被调到
    if (ERR_OK != _do_req(task, port, "GET",  "/needauth",        NULL,        NULL, 401, "no"))     bad |= (1 << 8);
    if (task_isclosing(task)) return ERR_FAILED;
    // [9]  auth 中间件放行: token 正确 → 进 handler 返 "authed"
    if (ERR_OK != _do_req(task, port, "GET",  "/needauth",        "X-Token",   "secret", 200, "authed")) bad |= (1 << 9);
    if (task_isclosing(task)) return ERR_FAILED;
    // [10] POST + router_req_json 响应辅助
    if (ERR_OK != _do_req(task, port, "POST", "/admin/stats",     NULL, NULL, 200, "{\"ok\":true}")) bad |= (1 << 10);
    if (task_isclosing(task)) return ERR_FAILED;
    // [11] handler 漏写响应 → dispatch 末尾兜底 500 "Internal Server Error\n"
    if (ERR_OK != _do_req(task, port, "GET",  "/forget",          NULL, NULL, 500, NULL))            bad |= (1 << 11);
    if (task_isclosing(task)) return ERR_FAILED;
    // [12] POST 方法位掩码命中
    if (ERR_OK != _do_req(task, port, "POST", "/only-post",       NULL, NULL, 200, "post-ok"))       bad |= (1 << 12);
    if (task_isclosing(task)) return ERR_FAILED;
    // [13] 同 path 不同方法 → 404 (验证 method_mask 不会误命中)
    if (ERR_OK != _do_req(task, port, "GET",  "/only-post",       NULL, NULL, 404, NULL))            bad |= (1 << 13);
    if (task_isclosing(task)) return ERR_FAILED;

    // [14] post-tag 中间件 next 后置: 打 3 次 /post-mw 后 /__stats 应 = 3
    // 三个请求共用 bit14, 任一失败都把这一位染坏; 最后再发 /__stats 校验计数
    if (ERR_OK != _do_req(task, port, "GET",  "/post-mw", NULL, NULL, 200, "tagged")) bad |= (1 << 14);
    if (ERR_OK != _do_req(task, port, "GET",  "/post-mw", NULL, NULL, 200, "tagged")) bad |= (1 << 14);
    if (ERR_OK != _do_req(task, port, "GET",  "/post-mw", NULL, NULL, 200, "tagged")) bad |= (1 << 14);
    if (task_isclosing(task)) return ERR_FAILED;
    if (ERR_OK != _do_req(task, port, "GET",  "/__stats", NULL, NULL, 200, "3"))      bad |= (1 << 14);
    if (task_isclosing(task)) return ERR_FAILED;

    // [15] 嵌套 group 中间件继承: g1mw (+1) → g2mw (+10) → handler 写 "deep=11"
    // 若 g1mw 未生效会得 "deep=10", g2mw 未生效会得 "deep=1", 顺序反则结果不变
    // 但参考代码逻辑此处必须为 11; 数值不等于 11 都说明中间件链有问题
    if (ERR_OK != _do_req(task, port, "GET",  "/g1/g2/deep", NULL, NULL, 200, "deep=11")) bad |= (1 << 15);
    if (task_isclosing(task)) return ERR_FAILED;

    // [16] 正好 URL_MAX_PATH_DEPTH(64) 段精确请求命中 64 段路由
    char p64[160];
    char p65[170];
    _segpath(p64, sizeof(p64), 64);
    _segpath(p65, sizeof(p65), 65);
    if (ERR_OK != _do_req(task, port, "GET", p64, NULL, NULL, 200, "s64")) bad |= (1 << 16);
    if (task_isclosing(task)) return ERR_FAILED;
    // [17] 65 段请求:url_parse 段数超限直接失败,不误命中 64 段路由(→ 400)
    if (ERR_OK != _do_req(task, port, "GET", p65, NULL, NULL, 400, NULL)) bad |= (1 << 17);
    if (task_isclosing(task)) return ERR_FAILED;
    // [18] 参数段 + 末尾通配: /asset/{id}/* 命中后 {id} 仍可取 (通配不吞掉 params_n)
    if (ERR_OK != _do_req(task, port, "GET", "/asset/42/x", NULL, NULL, 200, "42")) bad |= (1 << 18);
    if (task_isclosing(task)) return ERR_FAILED;
    // [19] router_req_query 区分键不存在/值空/有值: ?a= 返非 NULL 零长(empty), 缺 a 返 NULL(missing)
    if (ERR_OK != _do_req(task, port, "GET", "/qexist?a=1", NULL, NULL, 200, "value"))   bad |= (1 << 19);
    if (ERR_OK != _do_req(task, port, "GET", "/qexist?a=",  NULL, NULL, 200, "empty"))   bad |= (1 << 19);
    if (ERR_OK != _do_req(task, port, "GET", "/qexist",     NULL, NULL, 200, "missing")) bad |= (1 << 19);
    if (task_isclosing(task)) return ERR_FAILED;
    // [20] B2: {a?b} 参数名含内部 ? → 当字面量段, /litq/xyz 不命中参数 → 404 (修复前当 PARAM 会返 200)
    if (ERR_OK != _do_req(task, port, "GET", "/litq/xyz", NULL, NULL, 404, NULL)) bad |= (1 << 20);
    if (task_isclosing(task)) return ERR_FAILED;

    // [21] OPT 中置+有值: /a/42/b → param x 消耗后 LIT /b 匹配
    if (ERR_OK != _do_req(task, port, "GET", "/a/42/b", NULL, NULL, 200, "x=42")) bad |= (1 << 21);
    if (task_isclosing(task)) return ERR_FAILED;
    // [22] OPT 中置+无值(前瞻跳过): /a/b → skip_opt 令 OPT 不消耗, LIT /b 吞当前段
    if (ERR_OK != _do_req(task, port, "GET", "/a/b",    NULL, NULL, 200, "x=none")) bad |= (1 << 22);
    if (task_isclosing(task)) return ERR_FAILED;
    // [23] OPT 中置多余段: /a/b/c → 路由段消耗完但请求段剩余 → 404
    if (ERR_OK != _do_req(task, port, "GET", "/a/b/c",  NULL, NULL, 404, NULL)) bad |= (1 << 23);

    return 0 == bad ? ERR_OK : ERR_FAILED;
}

// timeout 回调 (协程上下文中执行): 跑完一轮断言, 把 1/0 写入 result_slot
static void _client_timeout(task_ctx *task, uint64_t sess) {
    (void)sess;
    task_router_client_ctx *ctx = coro_get_arg(task);
    if (ERR_OK != _run_all(task, ctx->port)) {
        ctx->err = 1;
    }
    if (ctx->err) {
        *ctx->result = 0;
    } else {
        *ctx->result = 1;
    }
}

// 启动后延后 100ms 再发请求, 等 server task_listen 落地; 同步参考 task_timeout.c 取值
static void _client_startup(task_ctx *task) {
    task_timeout(task, 0, 100, _client_timeout);
}
static void _client_closing(task_ctx *task) {
    (void)task;
}
static void _client_free(void *p) {
    FREE(p);
}

// client task 启动入口: 协程 task, _client_timeout 内的 coro_connect/coro_send 需协程上下文
void task_router_client_start(loader_ctx *loader, const char *name, uint16_t port, int32_t *result_slot) {
    task_router_client_ctx *ctx;
    CALLOC(ctx, 1, sizeof(task_router_client_ctx));
    ctx->port = port;
    ctx->result = result_slot;
    coro_task_register(loader, name, 0, _client_startup, _client_closing, _client_free, ctx);
}
