#include "task_router.h"
#include "advance/router.h"

// ── server task ────────────────────────────────────────────────────────────
// server 是一个普通 (非协程) task: _net_recv 同步调 router_dispatch, handler/中间件
// 全部在 worker 线程当场跑完, 不涉及 yield。router_ctx 单例放全局, 测试启动期间
// 不会切换实例; 两个 _g_*_count 用 ATOMIC_* 仅为跨 task 验证服务端确实执行到了某处,
// 单 worker 内本来就不竞争, 只为表达"读写发生在不同 task 上"的语义清晰

// 计数器: post-tag 中间件 next 返回后 +1, 客户端通过 GET /__stats 读回验证
static atomic_t     _g_post_count = 0;
// 计数器: 流式路由收到 ROUTER_STREAM_ABORT 时 +1, 客户端通过 GET /__aborts 读回验证
static atomic_t     _g_abort_count = 0;
// 计数器: _server_startup 里每有一条路由注册失败就 +1, 客户端通过 GET /__regfail 读回。
// 空名段那两条曾让整条路由注册作废, 而"没注册"和"注册成了字面量"在线缆上都是 404, 分不开
static atomic_t     _g_regfail_count = 0;
static uint16_t     _g_port       = 0;
// ABORT 回调里回调 router_closed 用; 不走 ctx->task->arg 是因为 router_free 那条路径上 task 正在拆
static router_ctx  *_g_router     = NULL;

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
// GET /pmax/... 与 /poptovf/... → 回最后一个参数 p16。三条路由共用本 handler,
// 对应 bad 位 25(/pmax 命中) 26(/povf 溢出拒) 27(/poptovf 满 17 段拒) 28(/poptovf 16 段 OPT 跳过命中)。
// 验 ROUTER_MAX_PARAMS(16) 边界: /pmax 给 16 个参数段恰好命中且末位参数可取;
// /povf 的 17 个必填段在注册期就被拒(它无论如何都用不满 16 的上限, 留着只会每个请求
// 换一个 404 加一行 WARN) → 请求落 404;
// /poptovf 末段是 {p17?}, 走 OPT 分支的同一上限判定 —— 给满 17 段同样 404,
// 只给 16 段时 OPT 跳过仍应命中。C5 把两个分支的填参收敛到单点后, 这四个请求同时覆盖两条路径
static void _h_pmax(router_req *ctx) {
    size_t n;
    const char *v = router_req_param(ctx, "p16", &n);
    if (NULL == v) {
        router_req_text(ctx, 200, "nop16", 5);
        return;
    }
    router_req_text(ctx, 200, v, n);
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
    router_req_text(ctx, 200, buf, snprintf_lens(k, sizeof(buf)));
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
    router_req_text(ctx, 200, buf, snprintf_lens(k, sizeof(buf)));
}
// GET /nobody → 204: RFC 7230 禁止 1xx/204/304 带 Content-Length 与报文体,
// 这里故意传一个 body, 验证 router 把它连同 CL 一起丢掉
static void _h_nobody(router_req *ctx) {
    router_req_text(ctx, 204, "dropped", 7);
}
// GET /__aborts → 当前 _g_abort_count 字符串值; 客户端发一半就断开后读出验证 ABORT 已投递
static void _h_aborts(router_req *ctx) {
    char buf[32];
    int32_t cnt = (int32_t)ATOMIC_GET(&_g_abort_count);
    int32_t k = SNPRINTF(buf, sizeof(buf), "%d", cnt);
    router_req_text(ctx, 200, buf, snprintf_lens(k, sizeof(buf)));
}
// GET /__regfail → 当前 _g_regfail_count 字符串值; 客户端读出 "0" 即所有路由都注册成功
static void _h_regfail(router_req *ctx) {
    char buf[32];
    int32_t cnt = (int32_t)ATOMIC_GET(&_g_regfail_count);
    int32_t k = SNPRINTF(buf, sizeof(buf), "%d", cnt);
    router_req_text(ctx, 200, buf, snprintf_lens(k, sizeof(buf)));
}
// GET /a/{x?}/b → OPT 中置: 有值返 "x=<val>", 无值(OPT 未取到段)返 "x=none"
static void _h_opt_mid(router_req *ctx) {
    size_t n;
    const char *x = router_req_param(ctx, "x", &n);
    if (NULL == x) {
        router_req_text(ctx, 200, "x=none", 6);
    } else {
        char buf[64];
        int32_t k = SNPRINTF(buf, sizeof(buf), "x=%.*s", (int32_t)n, x);
        router_req_text(ctx, 200, buf, snprintf_lens(k, sizeof(buf)));
    }
}
// GET /optlead/{x?}/{y} → "x=<x|none>,y=<y>"; OPT 排在必填段之前,
// 只前瞻一步的贪婪匹配会让 x 吃掉唯一请求段, 使 y 无段可用而误判 404
static void _h_opt_lead(router_req *ctx) {
    size_t xn = 0;
    size_t yn = 0;
    const char *x = router_req_param(ctx, "x", &xn);
    const char *y = router_req_param(ctx, "y", &yn);
    char buf[128];
    int32_t k = SNPRINTF(buf, sizeof(buf), "x=%.*s,y=%.*s",
                         NULL == x ? 4 : (int32_t)xn, NULL == x ? "none" : x,
                         NULL == y ? 4 : (int32_t)yn, NULL == y ? "none" : y);
    router_req_text(ctx, 200, buf, snprintf_lens(k, sizeof(buf)));
}
// GET /files/{ver?}/list → "ver=<ver|none>"; 请求段与后继字面量同名时(/files/list/list)
// 前瞻会把 OPT 跳过, 末段剩余无处消耗而误判 404
static void _h_opt_ambig(router_req *ctx) {
    size_t n = 0;
    const char *v = router_req_param(ctx, "ver", &n);
    char buf[64];
    int32_t k = SNPRINTF(buf, sizeof(buf), "ver=%.*s",
                         NULL == v ? 4 : (int32_t)n, NULL == v ? "none" : v);
    router_req_text(ctx, 200, buf, snprintf_lens(k, sizeof(buf)));
}
// 自定义头值长度, 取 >255 以越过旧实现的 v[256] 栈缓冲
#define BIGHDR_LEN 300
// GET /bighdr → 回一条 BIGHDR_LEN 字节的 X-Big 头; 覆盖头值改走 http_pack_head2 后不再截断
static void _h_bighdr(router_req *ctx) {
    char val[BIGHDR_LEN];
    memset(val, 'a', sizeof(val));
    http_header_ctx extra[1];
    extra[0].key.data = (void *)"X-Big";
    extra[0].key.lens = strlen("X-Big");
    extra[0].value.data = val;
    extra[0].value.lens = sizeof(val);
    router_req_respond(ctx, 200, extra, 1, "ok", 2);
}
#define HDRSUM_LEN 1800
// GET /hdrsum → 三条各 HDRSUM_LEN 字节的头; 逐条都远在 MAX_HEADLENS(4KB) 之内,
// 累计却超。逐条判定时三条全发, 整个头部块 ~5.4KB, 对端解析器直接 PROT_ERROR、
// 客户端连响应都收不到; 累计判定应放行前两条、丢掉第三条
static void _h_hdrsum(router_req *ctx) {
    char val[HDRSUM_LEN];
    memset(val, 'b', sizeof(val));
    http_header_ctx extra[3];
    static const char *keys[3] = { "X-P1", "X-P2", "X-P3" };
    int32_t i;
    for (i = 0; i < 3; i++) {
        extra[i].key.data = (void *)keys[i];
        extra[i].key.lens = strlen(keys[i]);
        extra[i].value.data = val;
        extra[i].value.lens = sizeof(val);
    }
    router_req_respond(ctx, 200, extra, 3, "ok", 2);
}
// GET /framing → handler 故意从 extra 塞 Content-Length 与 Transfer-Encoding。
// 两者归 http_pack_content 独占, 放行就成了两条 Content-Length(或 TE 叠 CL)——
// 典型的请求走私形态, 严格对端会拒收。应整条丢弃, 线缆上仍只有一条 Content-Length
static void _h_framing(router_req *ctx) {
    http_header_ctx extra[3];
    // 大小写故意写乱: HTTP 头名大小写无关, 判定不能只认标准写法
    extra[0].key.data = (void *)"content-LENGTH";
    extra[0].key.lens = strlen("content-LENGTH");
    extra[0].value.data = (void *)"999";
    extra[0].value.lens = 3;
    extra[1].key.data = (void *)"Transfer-Encoding";
    extra[1].key.lens = strlen("Transfer-Encoding");
    extra[1].value.data = (void *)"chunked";
    extra[1].value.lens = 7;
    // 正常头: 证明前两条被丢不是整批拒掉
    extra[2].key.data = (void *)"X-Keep";
    extra[2].key.lens = strlen("X-Keep");
    extra[2].value.data = (void *)"1";
    extra[2].value.lens = 1;
    router_req_respond(ctx, 200, extra, 3, "ok", 2);
}
// GET /nullhdr → extra 里塞一条值为 NULL 的头和一条正常头。router.h 的契约是
// "头值为 NULL 整条丢弃", 而校验曾被 lens > 0 短路掉, {NULL, 0} 会照常发出 "X-Null: "
static void _h_nullhdr(router_req *ctx) {
    http_header_ctx extra[2];
    extra[0].key.data = (void *)"X-Null";
    extra[0].key.lens = strlen("X-Null");
    extra[0].value.data = NULL;
    extra[0].value.lens = 0;
    // 正常头: 证明前一条被丢不是整批拒掉
    extra[1].key.data = (void *)"X-Keep";
    extra[1].key.lens = strlen("X-Keep");
    extra[1].value.data = (void *)"1";
    extra[1].value.lens = 1;
    router_req_respond(ctx, 200, extra, 2, "ok", 2);
}
// GET /g1/g2/deep → "deep=11"; 嵌套 group 终点 handler:
// g1mw 中间件先 ctx->user += 1, g2mw 中间件再 += 10, 累加值 11 由 handler 写出
// 验证: (a) 嵌套 group 中间件按父→子顺序入链 (b) ctx->user 跨中间件传值
static void _h_deep(router_req *ctx) {
    intptr_t accum = (intptr_t)ctx->user;
    char buf[32];
    int32_t k = SNPRINTF(buf, sizeof(buf), "deep=%ld", (long)accum);
    router_req_text(ctx, 200, buf, snprintf_lens(k, sizeof(buf)));
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
// silent: 既不写响应也不调 router_next。挂在流式路由上用来压"准入被拒 + 漏写响应"
// 那一格 —— 兜底 500 之后连接必须关掉
static void _mw_silent(router_req *ctx) {
    (void)ctx;
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

// 流式路由: 首帧建缓冲, 中间帧追加, 末帧回显。ctx->user 跨帧留在 router 持有的 req 里,
// 客户端据此核对分块是否按序到齐。slice == 0 是一次到齐的普通请求, 直接回显
static void _h_st_echo(router_req *ctx, uint8_t slice, void *data, size_t lens) {
    if (0 == slice) {
        router_req_text(ctx, 200, (const char *)data, lens);
        return;
    }
    binary_ctx *bw = (binary_ctx *)ctx->user;
    // 流没收齐就断了: 只清理, 不写响应
    if (ROUTER_STREAM_ABORT & slice) {
        if (NULL != bw) {
            binary_free(bw);
            FREE(bw);
            ctx->user = NULL;
        }
        ATOMIC_ADD(&_g_abort_count, 1);
        // 契约要求这里调 router_closed 是安全的 no-op: 投 ABORT 前该流已从表里摘掉。
        // 两条 ABORT 路径(连接断 / router_free)都得成立 —— router_free 若退回边遍历边回调,
        // 这一行就会让同一条流二次 ABORT + 二次 FREE, ASan 构建下当场报 double-free
        router_closed(_g_router, ctx->sk.fd, ctx->sk.skid);
        return;
    }
    if (PROT_SLICE_START & slice) {
        MALLOC(bw, sizeof(binary_ctx));
        binary_init(bw, NULL, 0, 0);
        ctx->user = bw;
        return;
    }
    if (NULL == bw) {
        return;
    }
    if (NULL != data
        && lens > 0) {
        binary_set_binary(bw, (const char *)data, lens);
    }
    if (PROT_SLICE_END & slice) {
        router_req_text(ctx, 200, (const char *)bw->data, bw->offset);
        binary_free(bw);
        FREE(bw);
        ctx->user = NULL;
    }
}

// server _net_recv: 整串转 router_net_recv, 与 harbor.c / debug_console.c 同款接法。
// 完整请求直接派发, chunked 逐帧交给流式路由
static void _server_net_recv(task_ctx *task, sk_id *sk,
                             subtype_t pktype, uint8_t client, uint8_t slice,
                             void *data, size_t size) {
    router_net_recv((router_ctx *)task->arg, task, sk, pktype, client, slice, data, size);
}
// 注册了流式路由就必须接这个, 否则连接中途断开时 router 持有的请求上下文不回收
static void _server_net_close(task_ctx *task, sk_id *sk, subtype_t pktype, uint8_t client) {
    (void)pktype;
    (void)client;
    router_closed((router_ctx *)task->arg, sk->fd, sk->skid);
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
    task_closed(task, _server_net_close);

    // 5 个具名中间件先注册, 后续 router_get/post 的 mws 数组按名引用
    router_define(r, "auth",     _mw_auth);
    router_define(r, "silent",   _mw_silent);
    router_define(r, "post-tag", _mw_post_tag);
    router_define(r, "g1mw",     _mw_g1);
    router_define(r, "g2mw",     _mw_g2);

    // 流式路由: /st 无中间件, /stauth 挂 auth 验证准入被截断时不建流
    router_post_stream(r, NULL, "/st", _h_st_echo, NULL, 0);
    const char *st_auth_mws[] = { "auth" };
    router_post_stream(r, NULL, "/stauth", _h_st_echo, st_auth_mws, 1);
    const char *st_silent_mws[] = { "silent" };
    router_post_stream(r, NULL, "/stsilent", _h_st_echo, st_silent_mws, 1);

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
    // 空名段同样当字面量: {} 与 {?} 都不是参数, 注册须成功(失败计入 _g_regfail_count),
    // 且 /litbe/xyz 与 /litqe/xyz 都不该命中参数 → 404
    if (NULL == router_get(r, NULL, "/litbe/{}",  _h_root, NULL, 0)) {
        ATOMIC_ADD(&_g_regfail_count, 1);
    }
    if (NULL == router_get(r, NULL, "/litqe/{?}", _h_root, NULL, 0)) {
        ATOMIC_ADD(&_g_regfail_count, 1);
    }
    router_get(r, NULL, "/a/{x?}/b",    _h_opt_mid,     NULL, 0);
    // OPT 精确匹配: 可选段排在必填段之前 / 取值与后继字面量同名, 两种形态贪婪前瞻都会误判 404
    router_get(r, NULL, "/optlead/{x?}/{y}",  _h_opt_lead,  NULL, 0);
    router_get(r, NULL, "/files/{ver?}/list", _h_opt_ambig, NULL, 0);
    // 自定义头值超 256 字节, 验证不被截断
    router_get(r, NULL, "/bighdr",            _h_bighdr,    NULL, 0);
    // 三条头单看合法、累计超 MAX_HEADLENS, 验证按整块判定
    router_get(r, NULL, "/hdrsum",            _h_hdrsum,    NULL, 0);
    // extra 里的帧长头须被丢弃, 验证不会发出两条 Content-Length
    router_get(r, NULL, "/framing",           _h_framing,   NULL, 0);
    router_get(r, NULL, "/nullhdr",           _h_nullhdr,   NULL, 0);
    // 9 个可选段 > ROUTER_MAX_OPT(8): 注册应失败, 该路径只能落到 404
    router_get(r, NULL, "/optovf/{a?}/{b?}/{c?}/{d?}/{e?}/{f?}/{g?}/{h?}/{i?}", _h_root, NULL, 0);
    router_post(r, NULL, "/admin/stats",  _h_admin_stats, NULL, 0);
    router_get(r, NULL, "/forget",       _h_forget,      NULL, 0);
    router_post(r, NULL, "/only-post",    _h_only_post,   NULL, 0);
    router_get(r, NULL, "/__stats",      _h_stats,       NULL, 0);
    router_get(r, NULL, "/__aborts",     _h_aborts,      NULL, 0);
    router_get(r, NULL, "/__regfail",    _h_regfail,     NULL, 0);
    router_get(r, NULL, "/nobody",       _h_nobody,      NULL, 0);
    // 只配 router_match_index 用的条目(两个回调都为 NULL)。混进派发是配置错误,
    // 普通请求与 chunked 首帧都该给 500, 不能一个 500 一个 411
    router_add_index(r, "POST", 4, "/index-only", 11);
    router_get(r, NULL, "/pmax/{p1}/{p2}/{p3}/{p4}/{p5}/{p6}/{p7}/{p8}/{p9}/{p10}/{p11}/{p12}/{p13}/{p14}/{p15}/{p16}",          _h_pmax, NULL, 0);
    router_get(r, NULL, "/povf/{p1}/{p2}/{p3}/{p4}/{p5}/{p6}/{p7}/{p8}/{p9}/{p10}/{p11}/{p12}/{p13}/{p14}/{p15}/{p16}/{p17}",    _h_pmax, NULL, 0);
    router_get(r, NULL, "/poptovf/{p1}/{p2}/{p3}/{p4}/{p5}/{p6}/{p7}/{p8}/{p9}/{p10}/{p11}/{p12}/{p13}/{p14}/{p15}/{p16}/{p17?}", _h_pmax, NULL, 0);

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
    _g_router = router;
    task_ctx *task = task_new(loader, name, 0, NULL, _router_free, router);
    if (ERR_OK != task_register(task, _server_startup, NULL)) {
        task_free(task);
    }
}

// ── 第二个 server: 专压"一条流式路由都没注册"时的 chunked 首帧 ───────────────
// 上面那个 server 注册了流式路由, chunked 首帧总能命中一条;
// 这里一条都不注册, 才压得到 _router_chunked_probe 的三种拒绝结局
static uint16_t _g_idx_port = 0;

// POST /idx-plain → 普通路由, 用来验证同一 router 上非 index 条目收 chunked 仍是 411
static void _h_idx_plain(router_req *ctx) {
    router_req_text(ctx, 200, "plain", 5);
}
// 故意不注册流式路由, 也因此不需要 task_closed: 没有流就没有跨帧上下文要回收
static void _idx_startup(task_ctx *task) {
    router_ctx *r = (router_ctx *)task->arg;
    task_recved(task, _server_net_recv);
    router_post(r, NULL, "/idx-plain", _h_idx_plain, NULL, 0);
    router_add_index(r, "POST", 4, "/idx-only", 9);
    uint64_t id;
    if (ERR_OK != task_listen(task, PACK_HTTP, NULL, "0.0.0.0", _g_idx_port, &id, 0)) {
        LOG_WARN("task_router_index_server task_listen %d error.", _g_idx_port);
    }
}
void task_router_index_server_start(loader_ctx *loader, const char *name, uint16_t port) {
    _g_idx_port = port;
    router_ctx *router = router_new();
    task_ctx *task = task_new(loader, name, 0, NULL, _router_free, router);
    if (ERR_OK != task_register(task, _idx_startup, NULL)) {
        task_free(task);
    }
}

// ── client task ────────────────────────────────────────────────────────────
// client 必须是协程 task (用 coro_task_register): coro_connect / coro_send 都会
// yield, 同步完成 16 项断言后把结果写回 result_slot, 由 main.c 末尾汇总

typedef struct task_router_client_ctx {
    int32_t *result;   // 指向 main.c testlist[] 中 "router_test" 槽, 通过/失败写 1/0
    uint16_t port;     // server 监听端口, 与 portlist["router_sv"] 同步
    uint16_t idxport;  // 第二个 server 端口, 与 portlist["router_idx_sv"] 同步
    int32_t  err;
} task_router_client_ctx;

// 通用断言 helper: 单次 connect → 构造 HTTP 请求 → coro_send 取响应 → 校验 code + body
// hk/hv 非 NULL 时附加一条 header (用于发 X-Token);
// expect_body == NULL 表示只校验状态码, 不比对 body (常用于 404/500)
// 每次都建独立连接, 不复用 keep-alive, 减少跨断言干扰
// 响应公共断言: 状态码精确匹配 + Content-Length 唯一。所有走 router 的响应都该满足这两条,
// 故收在一处, 免得某个 helper 漏掉其中一条(_do_req_hdr 原先就没查 Content-Length)
static int32_t _resp_check(struct http_pack_ctx *resp, const char *method, const char *url,
                           int32_t expect_code) {
    // status[1] 是状态码 (字符串形态, 例 "200"); 长度精确匹配避免 "20"/"200" 误判
    buf_ctx *st = http_status(resp);
    if (NULL == st) {
        // chunked 中间/结束块没有首行；这里收到的都该是完整响应
        LOG_WARN("router test: %s %s got a pack without a status line.", method, url);
        return ERR_FAILED;
    }
    char codestr[8];
    SNPRINTF(codestr, sizeof(codestr), "%d", expect_code);
    if (!buf_compare(&st[1], codestr, strlen(codestr))) {
        LOG_WARN("router test: %s %s expected code %d, got %.*s.",
                 method, url, expect_code, (int32_t)st[1].lens, (char *)st[1].data);
        return ERR_FAILED;
    }
    // 响应头里 Content-Length 必须唯一 (router 曾手写一次 + http_pack_content 再写一次);
    // 1xx/204/304 禁带, 那几个码期望 0 条
    int32_t want_cl = http_code_nobody(expect_code) ? 0 : 1;
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
    if (want_cl != clcnt) {
        LOG_WARN("router test: %s %s expected %d Content-Length header(s), got %d.",
                 method, url, want_cl, clcnt);
        return ERR_FAILED;
    }
    return ERR_OK;
}

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
    if (ERR_OK != _resp_check(resp, method, url, expect_code)) {
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
    ev_close(&task->loader->netev, fd, skid);
    return rtn;
}

// 头部断言族的共用前导: 连上去发一条无 body 的 GET, 收响应并查状态码 200 与 Content-Length 唯一。
// 无论成败都回填 fd/skid, 调用方一律在 done: 处 ev_close —— 连接失败时是 INVALID_SOCK
// (ev_props 首行即挡, 无副作用), 连上之后才失败的则是真 fd, 必须靠这次 ev_close 收掉
static struct http_pack_ctx *_do_get(task_ctx *task, uint16_t port, const char *url,
                                     SOCKET *fd, uint64_t *skid) {
    *fd = INVALID_SOCK;
    *skid = 0;
    if (ERR_OK != coro_connect(task, PACK_HTTP, NULL, "127.0.0.1", port, 0, NULL, fd, skid)) {
        LOG_WARN("router test: connect to %d failed for %s.", port, url);
        return NULL;
    }
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    http_pack_req(&bw, "GET", url);
    http_pack_head(&bw, "Host", "127.0.0.1");
    http_pack_end(&bw);
    size_t rsize;
    struct http_pack_ctx *resp = coro_send(task, *fd, *skid, bw.data, bw.offset, &rsize, 0);
    if (NULL == resp) {
        LOG_WARN("router test: %s got no response.", url);
        return NULL;
    }
    if (ERR_OK != _resp_check(resp, "GET", url, 200)) {
        return NULL;
    }
    return resp;
}
// 头值断言 helper: 发 GET, 按长度精确比对响应中 hk 这条头的完整值
// (不能用 strlen 比, 截断后的值仍是合法 C 串, 只有长度能区分)
// 单条头断言: want=NULL 断言该头不存在, 否则断言存在且值按长度精确相等
// (不能用 strlen 比, 截断后的值仍是合法 C 串, 只有长度能区分)。
// hlen 在函数内自己初始化 —— http_header 未命中时不写它, 由调用方各自记着重置的话,
// 漏一次就会让"头不存在"的告警里报出上一条头的长度
static int32_t _hdr_check(struct http_pack_ctx *resp, const char *url,
                          const char *hk, const char *want, size_t wantlen) {
    size_t hlen = 0;
    char *hv = http_header(resp, hk, &hlen);
    if (NULL == want) {
        if (NULL != hv) {
            LOG_WARN("router test: %s header %s should have been dropped, got %zu bytes.", url, hk, hlen);
            return ERR_FAILED;
        }
        return ERR_OK;
    }
    if (NULL == hv
        || hlen != wantlen
        || 0 != memcmp(hv, want, hlen)) {
        LOG_WARN("router test: %s header %s expected %zu bytes, got %zu.", url, hk, wantlen, hlen);
        return ERR_FAILED;
    }
    return ERR_OK;
}
// HEAD 请求 + 同连接紧跟一个 GET。要点有二:
//   1. HEAD 响应只发头, 但 Content-Length 要等于同一资源 GET 的报文体长度(RFC 7231 §4.3.2)
//   2. 多发的字节会被对端当成下一条响应的开头, 所以第二条 GET 能否正常收到才是真正的判据
// 客户端解包侧拿不到请求方法, 发前须 http_set_method 登记, 否则会挂在等报文体上
static int32_t _do_head_then_get(task_ctx *task, uint16_t port, const char *url,
                                 const char *get_body) {
    SOCKET fd;
    uint64_t skid;
    if (ERR_OK != coro_connect(task, PACK_HTTP, NULL, "127.0.0.1", port, 0, NULL, &fd, &skid)) {
        LOG_WARN("router test: connect to %d failed for HEAD %s.", port, url);
        return ERR_FAILED;
    }
    int32_t rtn = ERR_FAILED;
    // 两次请求各用一个 writer: coro_send 的 copy=0 已把缓冲所有权交给事件层, 复用即 UAF
    binary_ctx bw;
    binary_ctx bw2;
    size_t rsize;
    size_t dlen;
    char want[24];
    void *body;
    struct http_pack_ctx *resp;
    if (ERR_OK != http_set_method(&task->loader->netev, fd, skid, "HEAD")) {
        LOG_WARN("router test: set method HEAD failed for %s.", url);
        goto done;
    }
    binary_init(&bw, NULL, 0, 0);
    http_pack_req(&bw, "HEAD", url);
    http_pack_head(&bw, "Host", "127.0.0.1");
    http_pack_end(&bw);
    resp = coro_send(task, fd, skid, bw.data, bw.offset, &rsize, 0);
    if (NULL == resp
        || ERR_OK != _resp_check(resp, "HEAD", url, 200)) {
        goto done;
    }
    body = http_data(resp, &dlen);
    if (NULL != body && 0 != dlen) {
        LOG_WARN("router test: HEAD %s carried a body of %zu bytes.", url, dlen);
        goto done;
    }
    SNPRINTF(want, sizeof(want), "%zu", strlen(get_body));
    if (ERR_OK != _hdr_check(resp, url, "Content-Length", want, strlen(want))) {
        goto done;
    }
    // 同一条连接再发一次 GET: 上一条若多发了字节, 这里读到的就是错位的内容
    binary_init(&bw2, NULL, 0, 0);
    http_pack_req(&bw2, "GET", url);
    http_pack_head(&bw2, "Host", "127.0.0.1");
    http_pack_end(&bw2);
    resp = coro_send(task, fd, skid, bw2.data, bw2.offset, &rsize, 0);
    if (NULL == resp
        || ERR_OK != _resp_check(resp, "GET", url, 200)) {
        goto done;
    }
    body = http_data(resp, &dlen);
    if (NULL == body
        || dlen != strlen(get_body)
        || 0 != memcmp(body, get_body, dlen)) {
        LOG_WARN("router test: GET after HEAD on %s got a misaligned response.", url);
        goto done;
    }
    rtn = ERR_OK;
done:
    ev_close(&task->loader->netev, fd, skid);
    return rtn;
}
static int32_t _do_req_hdr(task_ctx *task, uint16_t port, const char *url,
                           const char *hk, const char *want, size_t wantlen) {
    SOCKET fd;
    uint64_t skid;
    struct http_pack_ctx *resp = _do_get(task, port, url, &fd, &skid);
    int32_t rtn = ERR_FAILED;
    if (NULL == resp) {
        goto done;
    }
    if (ERR_OK != _hdr_check(resp, url, hk, want, wantlen)) {
        goto done;
    }
    rtn = ERR_OK;
done:
    ev_close(&task->loader->netev, fd, skid);
    return rtn;
}

// /hdrsum 断言: 三条头逐条都在 MAX_HEADLENS 内、累计超, 应放行前两条丢掉第三条。
// 逐条判定的旧实现三条全发, 头部块 ~5.4KB 越上限, 对端解析器判 PROT_ERROR ——
// 那种情况下这里连响应都收不到, coro_send 返回 NULL
static int32_t _do_req_hdrsum(task_ctx *task, uint16_t port) {
    SOCKET fd;
    uint64_t skid;
    struct http_pack_ctx *resp = _do_get(task, port, "/hdrsum", &fd, &skid);
    int32_t rtn = ERR_FAILED;
    char want[HDRSUM_LEN];
    if (NULL == resp) {
        goto done;
    }
    // 与 _h_hdrsum 填的内容一致, 按内容精确比 —— 只比长度的话值被写坏也发现不了
    memset(want, 'b', sizeof(want));
    if (ERR_OK != _hdr_check(resp, "/hdrsum", "X-P1", want, sizeof(want))
        || ERR_OK != _hdr_check(resp, "/hdrsum", "X-P2", want, sizeof(want))
        || ERR_OK != _hdr_check(resp, "/hdrsum", "X-P3", NULL, 0)) {
        goto done;
    }
    rtn = ERR_OK;
done:
    ev_close(&task->loader->netev, fd, skid);
    return rtn;
}

// /framing 断言: extra 里的 Content-Length / Transfer-Encoding 被丢弃, 同批的普通头不受牵连。
// 放行的话线缆上会是两条 Content-Length 叠一条 TE, srey 自己的解析器判走私整包丢弃,
// 这里同样收不到响应 —— 两种失败形态都会被下面的断言拦住
static int32_t _do_req_framing(task_ctx *task, uint16_t port) {
    SOCKET fd;
    uint64_t skid;
    // Content-Length 唯一性由 _do_get 里的 _resp_check 数, 正是本用例要守的那条
    struct http_pack_ctx *resp = _do_get(task, port, "/framing", &fd, &skid);
    int32_t rtn = ERR_FAILED;
    if (NULL == resp) {
        goto done;
    }
    if (ERR_OK != _hdr_check(resp, "/framing", "Transfer-Encoding", NULL, 0)
        || ERR_OK != _hdr_check(resp, "/framing", "X-Keep", "1", 1)) {
        goto done;
    }
    rtn = ERR_OK;
done:
    ev_close(&task->loader->netev, fd, skid);
    return rtn;
}

// /nullhdr 断言: 值为 NULL 的头整条丢弃, 同批的正常头不受牵连
static int32_t _do_req_nullhdr(task_ctx *task, uint16_t port) {
    SOCKET fd;
    uint64_t skid;
    struct http_pack_ctx *resp = _do_get(task, port, "/nullhdr", &fd, &skid);
    int32_t rtn = ERR_FAILED;
    if (NULL == resp) {
        goto done;
    }
    if (ERR_OK != _hdr_check(resp, "/nullhdr", "X-Null", NULL, 0)
        || ERR_OK != _hdr_check(resp, "/nullhdr", "X-Keep", "1", 1)) {
        goto done;
    }
    rtn = ERR_OK;
done:
    ev_close(&task->loader->netev, fd, skid);
    return rtn;
}
// 25 项断言依次跑, 任一失败都 bad 置位; 全部通过返 ERR_OK
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
    // 重复 key 取最后一个, 规则在 url_get_param
    if (ERR_OK != _do_req(task, port, "GET",  "/query?a=1&b=2&a=3", NULL, NULL, 200, "a=3 b=2")) bad |= (1 << 7);
    if (task_isclosing(task)) return ERR_FAILED;
    // [30] router_get 连带接住 HEAD: 只发头 + Content-Length 等于 GET 那份的长度,
    //      同连接紧跟的 GET 不能错位(多发的字节会被当成下一条响应的开头)
    if (ERR_OK != _do_head_then_get(task, port, "/query?a=1&b=2", "a=1 b=2")) bad |= (1 << 30);
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
    // [29] 空名段 {} / {?}: 注册必须成功(修复前 {?} 让整条路由作废, _g_regfail_count 会是 1),
    // 且两者都是字面量段, 拿任意文本去打都不命中 → 404
    if (ERR_OK != _do_req(task, port, "GET", "/__regfail",  NULL, NULL, 200, "0")) {
        bad |= (1 << 29);
    }
    if (ERR_OK != _do_req(task, port, "GET", "/litbe/xyz", NULL, NULL, 404, NULL)) {
        bad |= (1 << 29);
    }
    if (ERR_OK != _do_req(task, port, "GET", "/litqe/xyz", NULL, NULL, 404, NULL)) {
        bad |= (1 << 29);
    }
    if (task_isclosing(task)) {
        return ERR_FAILED;
    }

    // [21] OPT 中置+有值: /a/42/b → param x 消耗后 LIT /b 匹配
    if (ERR_OK != _do_req(task, port, "GET", "/a/42/b", NULL, NULL, 200, "x=42")) bad |= (1 << 21);
    if (task_isclosing(task)) return ERR_FAILED;
    // [22] OPT 中置+无值: /a/b → 唯一可行解是 OPT 不取值, 由 LIT /b 吞掉当前段
    if (ERR_OK != _do_req(task, port, "GET", "/a/b",    NULL, NULL, 200, "x=none")) bad |= (1 << 22);
    if (task_isclosing(task)) return ERR_FAILED;
    // [23] OPT 中置多余段: /a/b/c → 路由段消耗完但请求段剩余 → 404
    if (ERR_OK != _do_req(task, port, "GET", "/a/b/c",  NULL, NULL, 404, NULL)) bad |= (1 << 23);
    if (task_isclosing(task)) return ERR_FAILED;
    // [24] 204 禁带 Content-Length 与报文体: handler 传了 body 也该被丢掉。
    // 0 条 Content-Length 这一项由 _resp_check 按 http_code_nobody 判, 这里只要码对
    if (ERR_OK != _do_req(task, port, "GET", "/nobody", NULL, NULL, 204, NULL)) {
        bad |= (1 << 24);
    }
    if (task_isclosing(task)) {
        return ERR_FAILED;
    }
    // 流式路由断言另见 _run_stream
    if (ERR_OK != _do_req(task, port, "GET", "/pmax/a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p", NULL, NULL, 200, "p")) {
        bad |= (1 << 25);
    }
    if (task_isclosing(task)) {
        return ERR_FAILED;
    }
    if (ERR_OK != _do_req(task, port, "GET", "/povf/a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/q", NULL, NULL, 404, NULL)) {
        bad |= (1 << 26);
    }
    if (task_isclosing(task)) {
        return ERR_FAILED;
    }
    if (ERR_OK != _do_req(task, port, "GET", "/poptovf/a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/q", NULL, NULL, 404, NULL)) {
        bad |= (1 << 27);
    }
    if (task_isclosing(task)) {
        return ERR_FAILED;
    }
    if (ERR_OK != _do_req(task, port, "GET", "/poptovf/a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p", NULL, NULL, 200, "p")) {
        bad |= (1 << 28);
    }
    return 0 == bad ? ERR_OK : ERR_FAILED;
}

// OPT 精确匹配 + 头值不截断的补充断言; 与 _run_all 分开是因为后者的 bad 位已用到 28,
// 这里 7 条塞进去要占到 bit 35, 越过 int32_t 的位宽, 故另起一个 bad 从 bit 0 重数
static int32_t _run_opt_extra(task_ctx *task, uint16_t port) {
    int32_t bad = 0;
    // [0] OPT 排在必填段之前: 唯一请求段须留给 {y}, {x?} 缺省
    if (ERR_OK != _do_req(task, port, "GET", "/optlead/b", NULL, NULL, 200, "x=none,y=b")) {
        bad |= (1 << 0);
    }
    if (task_isclosing(task)) {
        return ERR_FAILED;
    }
    // [1] 两段都在: {x?} 与 {y} 各取一段
    if (ERR_OK != _do_req(task, port, "GET", "/optlead/a/b", NULL, NULL, 200, "x=a,y=b")) {
        bad |= (1 << 1);
    }
    if (task_isclosing(task)) {
        return ERR_FAILED;
    }
    // [2] OPT 取值与后继字面量同名: ver 取 "list", 末段 list 由字面量吃
    if (ERR_OK != _do_req(task, port, "GET", "/files/list/list", NULL, NULL, 200, "ver=list")) {
        bad |= (1 << 2);
    }
    if (task_isclosing(task)) {
        return ERR_FAILED;
    }
    // [3] 同一路由 OPT 缺省的常规形态仍命中
    if (ERR_OK != _do_req(task, port, "GET", "/files/list", NULL, NULL, 200, "ver=none")) {
        bad |= (1 << 3);
    }
    if (task_isclosing(task)) {
        return ERR_FAILED;
    }
    // [4] /a/{x?}/b 的歧义形态: 前瞻实现在此误判 404, 精确匹配应取 x=b
    if (ERR_OK != _do_req(task, port, "GET", "/a/b/b", NULL, NULL, 200, "x=b")) {
        bad |= (1 << 4);
    }
    if (task_isclosing(task)) {
        return ERR_FAILED;
    }
    // [5] 可选段超 ROUTER_MAX_OPT 的路由注册失败, 请求只能落 404
    if (ERR_OK != _do_req(task, port, "GET", "/optovf/x", NULL, NULL, 404, NULL)) {
        bad |= (1 << 5);
    }
    if (task_isclosing(task)) {
        return ERR_FAILED;
    }
    // [6] 自定义头值 BIGHDR_LEN 字节完整上线缆 (旧实现截断到 255)
    char want[BIGHDR_LEN];
    memset(want, 'a', sizeof(want));
    if (ERR_OK != _do_req_hdr(task, port, "/bighdr", "X-Big", want, sizeof(want))) {
        bad |= (1 << 6);
    }
    if (task_isclosing(task)) {
        return ERR_FAILED;
    }
    // [7] 三条头累计超 MAX_HEADLENS: 前两条上线缆, 第三条丢弃
    if (ERR_OK != _do_req_hdrsum(task, port)) {
        bad |= (1 << 7);
    }
    if (task_isclosing(task)) {
        return ERR_FAILED;
    }
    // [8] extra 里的帧长头被丢弃, 线缆上仍只有一条 Content-Length
    if (ERR_OK != _do_req_framing(task, port)) {
        bad |= (1 << 8);
    }
    if (task_isclosing(task)) {
        return ERR_FAILED;
    }
    // [9] 值为 NULL 的头整条丢弃
    if (ERR_OK != _do_req_nullhdr(task, port)) {
        bad |= (1 << 9);
    }
    if (0 != bad) {
        LOG_WARN("router test: opt/header extra assertions failed, bad=0x%x.", bad);
    }
    return 0 == bad ? ERR_OK : ERR_FAILED;
}



// 分块发送断言: 按 CLAUDE.md 的写法逐段发, 末段带终止块并等响应。
// chunks 各段拼起来就是期望回显的 body; expect 为期望状态码
static int32_t _do_chunked(task_ctx *task, uint16_t port, const char *method, const char *path,
                           const char *const *chunks, int32_t n,
                           const char *token, int32_t expect, const char *want_body) {
    SOCKET fd;
    uint64_t skid;
    if (ERR_OK != coro_connect(task, PACK_HTTP, NULL, "127.0.0.1", port, 0, NULL, &fd, &skid)) {
        LOG_WARN("router test: connect to %d failed for chunked.", port);
        return ERR_FAILED;
    }
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    http_pack_req(&bw, method, path);
    http_pack_head(&bw, "Host", "127.0.0.1");
    if (NULL != token) {
        http_pack_head(&bw, "X-Token", token);
    }
    int32_t rtn = ERR_FAILED;
    int32_t i;
    // 首段的 http_pack_chunked 会自动补 Transfer-Encoding 头与空行; 之后每段都要先把
    // 写游标退回 0, 否则那个头会被重复附加 (copy=1 让 bw 可以接着复用)
    for (i = 0; i < n; i++) {
        http_pack_chunked(&bw, (void *)chunks[i], strlen(chunks[i]));
        ev_send(&task->loader->netev, fd, skid, bw.data, bw.offset, 1);
        binary_offset(&bw, 0);
    }
    http_pack_chunked(&bw, NULL, 0);// 终止块
    size_t rsize;
    // copy=0: bw.data 所有权转给框架, 后面不能再 binary_free (同 _do_req)
    struct http_pack_ctx *resp = coro_send(task, fd, skid, bw.data, bw.offset, &rsize, 0);
    if (NULL == resp) {
        LOG_WARN("router test: chunked coro_send failed.");
        goto done;
    }
    // 走公共校验: 状态码之外还查 Content-Length 唯一性, 那条断言只在这里有
    if (ERR_OK != _resp_check(resp, method, path, expect)) {
        goto done;
    }
    if (NULL != want_body) {
        size_t blen = 0;
        void *body = http_data(resp, &blen);
        // 空 body 时 body 为 NULL, memcmp(NULL, ..., 0) 是 UB, 先按长度短路
        if (blen != strlen(want_body)
            || (blen > 0 && 0 != memcmp(body, want_body, blen))) {
            LOG_WARN("router test: chunked body mismatch, got %.*s.", (int32_t)blen, (char *)body);
            goto done;
        }
    }
    rtn = ERR_OK;
done:
    ev_close(&task->loader->netev, fd, skid);
    return rtn;
}
// 发首帧 + 一块数据就断开, 不发终止块: 服务端只能靠 router_closed 收尾,
// 流式回调应收到一次 ROUTER_STREAM_ABORT。ev_close 关闭前冲一次, 这点数据一次就写进内核
static int32_t _do_chunked_abort(task_ctx *task, uint16_t port) {
    SOCKET fd;
    uint64_t skid;
    if (ERR_OK != coro_connect(task, PACK_HTTP, NULL, "127.0.0.1", port, 0, NULL, &fd, &skid)) {
        LOG_WARN("router test: connect to %d failed for chunked abort.", port);
        return ERR_FAILED;
    }
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    http_pack_req(&bw, "POST", "/st");
    http_pack_head(&bw, "Host", "127.0.0.1");
    http_pack_chunked(&bw, (void *)"half", 4);
    ev_send(&task->loader->netev, fd, skid, bw.data, bw.offset, 1);
    binary_free(&bw);
    ev_close(&task->loader->netev, fd, skid);
    return ERR_OK;
}
// 流式路由被完整(非 chunked)请求命中、准入被拒且没写响应: 兜底 500 之后连接照旧可用,
// 只有 chunked 首帧那面(_router_st_reject)才关。用同一条连接再发一次验它没被关掉
static int32_t _do_stream_plain_reject(task_ctx *task, uint16_t port) {
    SOCKET fd;
    uint64_t skid;
    if (ERR_OK != coro_connect(task, PACK_HTTP, NULL, "127.0.0.1", port, 0, NULL, &fd, &skid)) {
        LOG_WARN("router test: connect to %d failed for stream plain reject.", port);
        return ERR_FAILED;
    }
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    http_pack_req(&bw, "POST", "/stsilent");
    http_pack_head(&bw, "Host", "127.0.0.1");
    http_pack_end(&bw);
    size_t rsize;
    int32_t rtn = ERR_FAILED;
    // copy=0: bw.data 所有权转给框架, 后面不能再 binary_free (同 _do_req)
    struct http_pack_ctx *resp = coro_send(task, fd, skid, bw.data, bw.offset, &rsize, 0);
    if (NULL == resp) {
        LOG_WARN("router test: /stsilent got no response.");
        goto done;
    }
    if (ERR_OK != _resp_check(resp, "POST", "/stsilent", 500)) {
        goto done;
    }
    // 一次到齐的请求 body 已全收完, 没有残留帧要丢弃, 所以兜底 500 之后连接照旧可用
    // (chunked 首帧那面才必须关, 理由见 _router_st_reject)
    binary_init(&bw, NULL, 0, 0);
    http_pack_req(&bw, "POST", "/stsilent");
    http_pack_head(&bw, "Host", "127.0.0.1");
    http_pack_end(&bw);
    resp = coro_send(task, fd, skid, bw.data, bw.offset, &rsize, 0);
    if (NULL == resp) {
        LOG_WARN("router test: /stsilent closed after the 500 fallback.");
        goto done;
    }
    if (ERR_OK != _resp_check(resp, "POST", "/stsilent", 500)) {
        goto done;
    }
    rtn = ERR_OK;
done:
    ev_close(&task->loader->netev, fd, skid);
    return rtn;
}
// 发首帧 + 一块数据后就不管了, 连接一直留着: 这条流会挂在 r->streams 里活到进程收尾,
// 由 router_free 排空并投 ABORT。留给 ASan 盯 router_free 那条路径的重入(见 _h_st_echo)
static int32_t _do_chunked_dangling(task_ctx *task, uint16_t port) {
    SOCKET fd;
    uint64_t skid;
    if (ERR_OK != coro_connect(task, PACK_HTTP, NULL, "127.0.0.1", port, 0, NULL, &fd, &skid)) {
        LOG_WARN("router test: connect to %d failed for dangling stream.", port);
        return ERR_FAILED;
    }
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    http_pack_req(&bw, "POST", "/st");
    http_pack_head(&bw, "Host", "127.0.0.1");
    http_pack_chunked(&bw, (void *)"live", 4);
    ev_send(&task->loader->netev, fd, skid, bw.data, bw.offset, 1);
    binary_free(&bw);
    return ERR_OK;// 有意不 ev_close
}
// 流式路由相关的九条断言
static int32_t _run_stream(task_ctx *task, uint16_t port) {
    int32_t bad = 0;
    // [0] 三块按序到齐, 末帧回显的 body 与拼接结果一致
    static const char *const ok3[3] = { "aaa", "bbbb", "c" };
    if (ERR_OK != _do_chunked(task, port, "POST", "/st", ok3, 3, NULL, 200, "aaabbbbc")) {
        bad |= (1 << 0);
    }
    if (task_isclosing(task)) {
        return ERR_FAILED;
    }
    // [1] 单块也走同一条路
    static const char *const ok1[1] = { "z" };
    if (ERR_OK != _do_chunked(task, port, "POST", "/st", ok1, 1, NULL, 200, "z")) {
        bad |= (1 << 1);
    }
    if (task_isclosing(task)) {
        return ERR_FAILED;
    }
    // [2] 零数据块: 只有首帧与终止块, 回显空 body
    if (ERR_OK != _do_chunked(task, port, "POST", "/st", NULL, 0, NULL, 200, "")) {
        bad |= (1 << 2);
    }
    if (task_isclosing(task)) {
        return ERR_FAILED;
    }
    // [3] chunked 打到普通路由 → 411 并关连接 (/only-post 是 router_post 注册的非流式路由)
    if (ERR_OK != _do_chunked(task, port, "POST", "/only-post", ok1, 1, NULL, 411, NULL)) {
        bad |= (1 << 3);
    }
    if (task_isclosing(task)) {
        return ERR_FAILED;
    }
    // [4] 准入中间件截断 → 401, 不建流
    if (ERR_OK != _do_chunked(task, port, "POST", "/stauth", ok3, 3, NULL, 401, NULL)) {
        bad |= (1 << 4);
    }
    if (task_isclosing(task)) {
        return ERR_FAILED;
    }
    // [5] 带对 token 则准入放行, 照常收齐
    if (ERR_OK != _do_chunked(task, port, "POST", "/stauth", ok3, 3, "secret", 200, "aaabbbbc")) {
        bad |= (1 << 5);
    }
    if (task_isclosing(task)) {
        return ERR_FAILED;
    }
    // [6] 发一半就断开 → 流式回调收到 ROUTER_STREAM_ABORT。放最后, 计数才是确定的 1:
    // 前六条里正常收尾的走 SLICE_END, 411 / 401 那两条压根没建流。
    // ctx->user 有没有真的释放掉由收尾的 _memcheck 兜底 —— 只看它分不清"没通知"和"没释放"
    if (ERR_OK != _do_chunked_abort(task, port)) {
        bad |= (1 << 6);
    } else {
        coro_sleep(task, 100);// 等服务端处理完连接关闭事件
        if (ERR_OK != _do_req(task, port, "GET", "/__aborts", NULL, NULL, 200, "1")) {
            bad |= (1 << 6);
        }
    }
    // [7] router_add_index 条目(两个回调皆 NULL)是配置错误, 普通请求与 chunked 首帧
    // 必须给同一个码。改前 chunked 那条被当成"普通路由收到 chunked"回 411
    if (ERR_OK != _do_req(task, port, "POST", "/index-only", NULL, NULL, 500, NULL)) {
        bad |= (1 << 7);
    }
    if (task_isclosing(task)) {
        return ERR_FAILED;
    }
    if (ERR_OK != _do_chunked(task, port, "POST", "/index-only", ok1, 1, NULL, 500, NULL)) {
        bad |= (1 << 7);
    }
    if (task_isclosing(task)) {
        return ERR_FAILED;
    }
    // [8] 留一条收不齐也不断开的流到进程收尾, 让 router_free 去排空它。
    // 排在 [6] 读 /__aborts 之后, 免得把那条计数断言从 1 顶成 2
    if (ERR_OK != _do_chunked_dangling(task, port)) {
        bad |= (1 << 8);
    }
    if (task_isclosing(task)) {
        return ERR_FAILED;
    }
    // [9] 完整(非 chunked)请求命中流式路由且准入被拒: 500 之后连接不关
    if (ERR_OK != _do_stream_plain_reject(task, port)) {
        bad |= (1 << 9);
    }
    if (0 != bad) {
        LOG_WARN("router test: stream route assertions failed, bad=0x%x.", bad);
    }
    return 0 == bad ? ERR_OK : ERR_FAILED;
}

// 打第二个 server(无流式路由): 压 _router_chunked_probe 的拒绝结局。
// 主 server 注册了流式路由, 这几条在它上面会命中并开流, 压不到这里
static int32_t _run_index(task_ctx *task, uint16_t port) {
    const char *one[] = { "x" };
    int32_t bad = 0;
    // [0] chunked 命中 index 条目 → 500; 若这条分支只会回 411, 这里就红
    if (ERR_OK != _do_chunked(task, port, "POST", "/idx-only", one, 1, NULL, 500, NULL)) {
        bad |= (1 << 0);
    }
    if (task_isclosing(task)) {
        return ERR_FAILED;
    }
    // [1] chunked 命中普通路由 → 411: 411 只表示"路由在但接不住 chunked"
    if (ERR_OK != _do_chunked(task, port, "POST", "/idx-plain", one, 1, NULL, 411, NULL)) {
        bad |= (1 << 1);
    }
    if (task_isclosing(task)) {
        return ERR_FAILED;
    }
    // [2] chunked 匹配不上 → 404 而不是 411, 与一次到齐的同一请求同码
    if (ERR_OK != _do_chunked(task, port, "POST", "/idx-nope", one, 1, NULL, 404, NULL)) {
        bad |= (1 << 2);
    }
    if (task_isclosing(task)) {
        return ERR_FAILED;
    }
    // [3] 路径对上了但方法掩码不交 → 404 (不是 405): /idx-plain 只注册了 POST
    if (ERR_OK != _do_chunked(task, port, "PUT", "/idx-plain", one, 1, NULL, 404, NULL)) {
        bad |= (1 << 3);
    }
    if (task_isclosing(task)) {
        return ERR_FAILED;
    }
    // [4] 方法名压根不认识 → 405。405 只由这一种情形产生, 与 [3] 成对锁住两者的区别
    if (ERR_OK != _do_chunked(task, port, "FROB", "/idx-plain", one, 1, NULL, 405, NULL)) {
        bad |= (1 << 4);
    }
    if (task_isclosing(task)) {
        return ERR_FAILED;
    }
    // [5] 一次到齐的请求打同一条 index 条目也是 500, 与 [0] 同码
    if (ERR_OK != _do_req(task, port, "POST", "/idx-only", NULL, NULL, 500, NULL)) {
        bad |= (1 << 5);
    }
    if (0 != bad) {
        LOG_WARN("router test: nostream chunked assertions failed, bad=0x%x.", bad);
    }
    return 0 == bad ? ERR_OK : ERR_FAILED;
}

// timeout 回调 (协程上下文中执行): 跑完一轮断言, 把 1/0 写入 result_slot
static void _client_timeout(task_ctx *task, uint64_t sess) {
    (void)sess;
    task_router_client_ctx *ctx = coro_get_arg(task);
    if (ERR_OK != _run_all(task, ctx->port)) {
        ctx->err = 1;
    }
    if (ERR_OK != _run_opt_extra(task, ctx->port)) {
        ctx->err = 1;
    }
    if (ERR_OK != _run_stream(task, ctx->port)) {
        ctx->err = 1;
    }
    if (ERR_OK != _run_index(task, ctx->idxport)) {
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
void task_router_client_start(loader_ctx *loader, const char *name, uint16_t port,
                              uint16_t idxport, int32_t *result_slot) {
    task_router_client_ctx *ctx;
    CALLOC(ctx, 1, sizeof(task_router_client_ctx));
    ctx->port = port;
    ctx->idxport = idxport;
    ctx->result = result_slot;
    coro_task_register(loader, name, 0, _client_startup, _client_closing, _client_free, ctx);
}
