#include "test_advance.h"
#include "lib.h"

/* router_match_index 带 URL_SKIP_EMPTY 解析，空段在 url_parse 里就丢掉。npath 少了而 pathlens 没跟着少的话，
 * url_ctx 声明的 pathlens == Σ(segs[i].lens + 1) 就不成立，下游按它预分配的缓冲会偏大；
 * "/" 这种全空段的请求压完 npath 归零，也要一并盯住 */
static void test_router_url_normalize(CuTest *tc) {
    router_ctx *r = router_new();
    url_ctx url;
    router_req ctx;
    size_t want;
    int32_t i;
    char buf[64];

    CuAssertPtrNotNull(tc, r);
    CuAssertTrue(tc, router_add_index(r, "GET", 3, "/a/b", 4) >= 0);

    /* /a//b 压成两段 */
    ZERO(&ctx, sizeof(ctx));
    ctx.url = &url;
    CuAssertTrue(tc, router_match_index(r, "GET", 3, "/a//b", 5, &ctx) >= 0);
    CuAssertIntEquals(tc, 2, url.npath);
    want = 0;
    for (i = 0; i < url.npath; i++) {
        want += (url.segs[i].lens + 1);
    }
    CuAssertTrue(tc, want == url.pathlens);
    /* pathlens + 1 必须是重组的精确容量 */
    CuAssertTrue(tc, url.pathlens == url_reorg_path(&url, buf, url.pathlens + 1));
    CuAssertStrEquals(tc, "/a/b", buf);

    /* 段本来就没空的，压缩前后一致 */
    ZERO(&ctx, sizeof(ctx));
    ctx.url = &url;
    CuAssertTrue(tc, router_match_index(r, "GET", 3, "/a/b", 4, &ctx) >= 0);
    CuAssertIntEquals(tc, 2, url.npath);
    CuAssertTrue(tc, 4 == url.pathlens);

    /* %2f 解出来的斜杠留在段内、不产生新段：url_parse 是先按裸 '/' 拆段、再逐段 decode。
       谁把 decode 提到拆段之前，/a%2fb 就会变成两段并命中 /a/b，这条随即变红 */
    ZERO(&ctx, sizeof(ctx));
    ctx.url = &url;
    CuAssertTrue(tc, router_match_index(r, "GET", 3, "/a%2fb", 6, &ctx) < 0);

    /* "." 与 ".." 是普通文本段，不做父目录归一（Laravel/Symfony 也是这个语义）。
       谁给 _router_find 加了折叠，/a/../b 就会命中 /a/b，这两条随即变红 */
    ZERO(&ctx, sizeof(ctx));
    ctx.url = &url;
    CuAssertTrue(tc, router_match_index(r, "GET", 3, "/a/../b", 7, &ctx) < 0);
    ZERO(&ctx, sizeof(ctx));
    ctx.url = &url;
    CuAssertTrue(tc, router_match_index(r, "GET", 3, "/a/%2e%2e/b", 11, &ctx) < 0);
    /* 上面几条 decode 开关都不影响结果（编码与未编码同样不命中），单独钉一条：
       参数值里的 %2D 必须被解成 '-'，decode 整个关掉这条就变红 */
    ZERO(&ctx, sizeof(ctx));
    ctx.url = &url;
    CuAssertTrue(tc, router_add_index(r, "GET", 3, "/dec/{p}", 8) >= 0);
    CuAssertTrue(tc, router_match_index(r, "GET", 3, "/dec/x%2Dy", 10, &ctx) >= 0);
    CuAssertIntEquals(tc, 1, ctx.params_n);
    CuAssertIntEquals(tc, 3, (int32_t)ctx.params[0].val_len);
    CuAssertTrue(tc, 0 == memcmp(ctx.params[0].val, "x-y", 3));

    /* "." 也是普通文本段 */
    ZERO(&ctx, sizeof(ctx));
    ctx.url = &url;
    CuAssertTrue(tc, router_match_index(r, "GET", 3, "/a/./b", 6, &ctx) < 0);

    router_free(r);

    /* "/" 的段全是空段，压完 npath 与 pathlens 都归零 */
    router_ctx *root = router_new();
    CuAssertPtrNotNull(tc, root);
    CuAssertTrue(tc, router_add_index(root, "GET", 3, "/", 1) >= 0);
    ZERO(&ctx, sizeof(ctx));
    ctx.url = &url;
    CuAssertTrue(tc, router_match_index(root, "GET", 3, "/", 1, &ctx) >= 0);
    CuAssertIntEquals(tc, 0, url.npath);
    CuAssertTrue(tc, 0 == url.pathlens);
    /* 这个形状下 url_reorg_path 只吐得出空串——正是 _lrouter_push_url 要单独给 "/" 的原因，
     * 而且容量只有 pathlens + 1 == 1 字节，想在这一层补 "/" 也没地方放 */
    buf[0] = 'x';
    CuAssertTrue(tc, 0 == url_reorg_path(&url, buf, url.pathlens + 1));
    CuAssertStrEquals(tc, "", buf);
    router_free(root);
}

/* 遮蔽判据是"已注册项的方法掩码把新掩码整个包住"，不是"两边有交集"。
 * 按交集判会让 head() 先于 get() 注册时把 GET 那条整条吞掉——而那正是 router_head
 * 文档要求的顺序。匹配期首条命中即返回，所以部分重叠的两条并存是有意义的 */
static void test_router_shadow_mask(CuTest *tc) {
    router_ctx *r = router_new();
    url_ctx url;
    router_req ctx;
    int32_t iget;
    int32_t iany;

    CuAssertPtrNotNull(tc, r);

    /* 1. HEAD 先、GET|HEAD 后：两条都进表，各自接住自己的方法 */
    CuAssertIntEquals(tc, 0, router_add_index(r, "HEAD", 4, "/a", 2));
    CuAssertIntEquals(tc, 1, router_add_index(r, "GET|HEAD", 8, "/a", 2));
    ZERO(&ctx, sizeof(ctx));
    ctx.url = &url;
    CuAssertIntEquals(tc, 0, router_match_index(r, "HEAD", 4, "/a", 2, &ctx));
    ZERO(&ctx, sizeof(ctx));
    ctx.url = &url;
    CuAssertIntEquals(tc, 1, router_match_index(r, "GET", 3, "/a", 2, &ctx));

    /* 2. 反过来注册：GET|HEAD 已把 HEAD 全包住，后来的 HEAD 被遮蔽 */
    CuAssertTrue(tc, router_add_index(r, "GET|HEAD", 8, "/b", 2) >= 0);
    CuAssertIntEquals(tc, -2, router_add_index(r, "HEAD", 4, "/b", 2));

    /* 3. 完全重复照旧拒掉 */
    CuAssertTrue(tc, router_add_index(r, "POST", 4, "/c", 2) >= 0);
    CuAssertIntEquals(tc, -2, router_add_index(r, "POST", 4, "/c", 2));

    /* 4. ANY 在前，任何单方法组合都被它全包 */
    CuAssertTrue(tc, router_add_index(r, "ANY", 3, "/d", 2) >= 0);
    CuAssertIntEquals(tc, -2, router_add_index(r, "GET|HEAD", 8, "/d", 2));

    /* 5. ANY 在后：GET|HEAD 包不住它剩下的方法，两条并存并按注册顺序命中 */
    iget = router_add_index(r, "GET|HEAD", 8, "/e", 2);
    CuAssertTrue(tc, iget >= 0);
    iany = router_add_index(r, "ANY", 3, "/e", 2);
    CuAssertTrue(tc, iany > iget);
    ZERO(&ctx, sizeof(ctx));
    ctx.url = &url;
    CuAssertIntEquals(tc, iget, router_match_index(r, "GET", 3, "/e", 2, &ctx));
    ZERO(&ctx, sizeof(ctx));
    ctx.url = &url;
    CuAssertIntEquals(tc, iany, router_match_index(r, "POST", 4, "/e", 2, &ctx));

    router_free(r);
}

/* router_seg_index 把注册期 _router_parse_seg 的结果交出去，反向生成 URL 的调用方
 * (bin/script/advance/router.lua 的 Router:url) 靠它回填占位符，段语法就只剩这一份。
 * 四种段类型各取一次，再验三种越界 —— 逐段遍历正是以 ERR_FAILED 收尾 */
static void test_router_seg_index(CuTest *tc) {
    router_ctx *r = router_new();
    // 三个出参只在 router_seg_index 返 ERR_OK 时才写；断言失败会 longjmp 出去走不到下面，
    // 但 gcc 看不穿 longjmp，不给初值就报 -Wmaybe-uninitialized
    router_seg_type t = ROUTER_SEG_LIT;
    const char *str = NULL;
    uint32_t slen = 0;
    int32_t idx;

    CuAssertPtrNotNull(tc, r);

    idx = router_add_index(r, "GET", 3, "/user/{id}/{tag?}/*", 19);
    CuAssertTrue(tc, idx >= 0);

    CuAssertIntEquals(tc, ERR_OK, router_seg_index(r, idx, 0, &t, &str, &slen));
    CuAssertIntEquals(tc, ROUTER_SEG_LIT, t);
    CuAssertIntEquals(tc, 4, (int32_t)slen);
    CuAssertStrEquals(tc, "user", str);

    CuAssertIntEquals(tc, ERR_OK, router_seg_index(r, idx, 1, &t, &str, &slen));
    CuAssertIntEquals(tc, ROUTER_SEG_PARAM, t);
    CuAssertIntEquals(tc, 2, (int32_t)slen);
    CuAssertStrEquals(tc, "id", str);

    CuAssertIntEquals(tc, ERR_OK, router_seg_index(r, idx, 2, &t, &str, &slen));
    CuAssertIntEquals(tc, ROUTER_SEG_OPT, t);
    CuAssertIntEquals(tc, 3, (int32_t)slen);
    CuAssertStrEquals(tc, "tag", str);

    /* 通配段不带名字 */
    CuAssertIntEquals(tc, ERR_OK, router_seg_index(r, idx, 3, &t, &str, &slen));
    CuAssertIntEquals(tc, ROUTER_SEG_WILD, t);
    CuAssertPtrEquals(tc, NULL, (void *)str);
    CuAssertIntEquals(tc, 0, (int32_t)slen);

    /* 段取完 / k 为负 / idx 越界 */
    CuAssertIntEquals(tc, ERR_FAILED, router_seg_index(r, idx, 4, &t, &str, &slen));
    CuAssertIntEquals(tc, ERR_FAILED, router_seg_index(r, idx, -1, &t, &str, &slen));
    CuAssertIntEquals(tc, ERR_FAILED, router_seg_index(r, idx + 1, 0, &t, &str, &slen));

    /* 名字为空、名字内部含 '?' 都退化为字面量，退化后交回的是含花括号的原文 */
    idx = router_add_index(r, "GET", 3, "/{}/{a?b}", 9);
    CuAssertTrue(tc, idx >= 0);
    CuAssertIntEquals(tc, ERR_OK, router_seg_index(r, idx, 0, &t, &str, &slen));
    CuAssertIntEquals(tc, ROUTER_SEG_LIT, t);
    CuAssertStrEquals(tc, "{}", str);
    CuAssertIntEquals(tc, ERR_OK, router_seg_index(r, idx, 1, &t, &str, &slen));
    CuAssertIntEquals(tc, ROUTER_SEG_LIT, t);
    CuAssertStrEquals(tc, "{a?b}", str);

    /* 名字只认 [A-Za-z0-9_]，'-' 与 '*' 都不是，整段退化为字面量。{*} 尤其不能认作
     * 占位符：它与末尾通配交回的 key 都会是 "*"，反向生成 URL 的调用方就分不开 */
    idx = router_add_index(r, "GET", 3, "/{a-b}/{*}", 10);
    CuAssertTrue(tc, idx >= 0);
    CuAssertIntEquals(tc, ERR_OK, router_seg_index(r, idx, 0, &t, &str, &slen));
    CuAssertIntEquals(tc, ROUTER_SEG_LIT, t);
    CuAssertStrEquals(tc, "{a-b}", str);
    CuAssertIntEquals(tc, ERR_OK, router_seg_index(r, idx, 1, &t, &str, &slen));
    CuAssertIntEquals(tc, ROUTER_SEG_LIT, t);
    CuAssertStrEquals(tc, "{*}", str);

    /* 下划线与数字仍是合法名字 */
    idx = router_add_index(r, "GET", 3, "/{a_1}", 6);
    CuAssertTrue(tc, idx >= 0);
    CuAssertIntEquals(tc, ERR_OK, router_seg_index(r, idx, 0, &t, &str, &slen));
    CuAssertIntEquals(tc, ROUTER_SEG_PARAM, t);
    CuAssertStrEquals(tc, "a_1", str);

    /* 根路径拆出 0 段，第 0 段就越界 */
    idx = router_add_index(r, "GET", 3, "/", 1);
    CuAssertTrue(tc, idx >= 0);
    CuAssertIntEquals(tc, ERR_FAILED, router_seg_index(r, idx, 0, &t, &str, &slen));

    router_free(r);
}

/* router_next 的游标进入时推进、返回时恢复：同一层调两次 next 是把下游从下一层起重跑一遍，
 * 不会越过下游里截断的那层。共用一个游标时，第二次会直接跳到 handler，鉴权类中间件就被绕过 */
static int32_t _next_gate_n;
static int32_t _next_handler_n;
static void _next_twice(router_req *ctx, void *arg) {
    (void)arg;
    router_next(ctx);
    router_next(ctx);
}
static void _next_gate(router_req *ctx, void *arg) {
    (void)arg;
    (void)ctx;
    _next_gate_n++;// 恒截断：不调 router_next
}
static void _next_pass(router_req *ctx, void *arg) {
    (void)arg;
    router_next(ctx);
}
static void _next_handler(router_req *ctx, void *arg) {
    (void)arg;
    (void)ctx;
    _next_handler_n++;
}
static void _arg_step(router_req *ctx, void *arg) {
    *(int32_t *)ctx->user = *(int32_t *)arg;
}
static void _arg_route(router_req *ctx, void *arg) {
    *(int32_t *)ctx->user = *(int32_t *)arg;
}
static void test_router_step_arg(CuTest *tc) {
    router_req ctx;
    int32_t got = 0;
    int32_t want = 7;
    ZERO(&ctx, sizeof(ctx));
    ctx.user = &got;
    ctx.chain[0].cb = _arg_step;
    ctx.chain[0].arg = &want;
    ctx.chain_n = 1;
    router_next(&ctx);
    CuAssertIntEquals(tc, 7, got);
}
static int32_t _fallback_n;
static void _fallback_handler(router_req *ctx, void *arg) {
    _fallback_n = *(int32_t *)arg;
    ctx->responded = 1;
}
static void test_router_fallback(CuTest *tc) {
    router_ctx *r = router_new();
    router_req ctx;
    int32_t tag = 23;
    ZERO(&ctx, sizeof(ctx));
    CuAssertIntEquals(tc, ERR_OK, router_set_no_route(r, _fallback_handler, &tag));
    CuAssertIntEquals(tc, ERR_OK, router_exec(r, -1, &ctx));
    CuAssertIntEquals(tc, 23, _fallback_n);
    CuAssertIntEquals(tc, 1, ctx.responded);

    ZERO(&ctx, sizeof(ctx));
    CuAssertIntEquals(tc, ERR_OK, router_set_no_method(r, _fallback_handler, &tag));
    CuAssertIntEquals(tc, ERR_OK, router_exec(r, -4, &ctx));
    CuAssertIntEquals(tc, 23, _fallback_n);
    // -3(未知方法) 不是 -4(路径在而方法不对)，不存在的 NoMethod 也不能兜它
    CuAssertIntEquals(tc, ERR_FAILED, router_exec(r, -3, &ctx));

    CuAssertIntEquals(tc, ERR_OK, router_set_no_route(r, NULL, NULL));
    CuAssertIntEquals(tc, ERR_FAILED, router_exec(r, -1, &ctx));
    CuAssertIntEquals(tc, ERR_FAILED, router_exec(r, -5, &ctx));
    router_free(r);
}
static void _arg_mw(router_req *ctx, void *arg) {
    *(int32_t *)ctx->user += *(int32_t *)arg;
    router_next(ctx);
}
static void _arg_add(router_req *ctx, void *arg) {
    *(int32_t *)ctx->user += *(int32_t *)arg;
}
static void test_router_chain_api(CuTest *tc) {
    router_ctx *r = router_new();
    router_req ctx;
    router_step got;
    int32_t sum = 0;
    int32_t mw_arg = 3;
    int32_t h_arg = 4;
    router_step mws[1];
    mws[0].cb = _arg_mw;
    mws[0].arg = &mw_arg;
    router_entry *e = router_add_steps(r, NULL, ROUTER_M_GET, "/steps", _arg_add, &h_arg, mws, 1);
    CuAssertPtrNotNull(tc, e);
    CuAssertIntEquals(tc, 2, router_chain_count(r, 0));
    CuAssertIntEquals(tc, ERR_OK, router_chain_at(r, 0, 0, &got));
    CuAssertTrue(tc, got.cb == _arg_mw && got.arg == &mw_arg);
    CuAssertIntEquals(tc, ERR_OK, router_chain_at(r, 0, 1, &got));
    CuAssertTrue(tc, got.cb == _arg_add && got.arg == &h_arg);
    // r / out 为 NULL、下标越界都拒
    CuAssertIntEquals(tc, -1, router_chain_count(NULL, 0));
    CuAssertIntEquals(tc, ERR_FAILED, router_chain_at(NULL, 0, 0, &got));
    CuAssertIntEquals(tc, ERR_FAILED, router_chain_at(r, 0, 0, NULL));
    CuAssertIntEquals(tc, ERR_FAILED, router_chain_at(r, 0, 2, &got));
    ZERO(&ctx, sizeof(ctx));
    ctx.user = &sum;
    CuAssertIntEquals(tc, ERR_OK, router_exec(r, 0, &ctx));
    CuAssertIntEquals(tc, 7, sum);
    router_free(r);
}
static void test_router_route_arg(CuTest *tc) {
    router_ctx *r = router_new();
    router_req ctx;
    int32_t got = 0;
    int32_t want = 11;
    router_entry *e = router_add(r, NULL, ROUTER_M_GET, "/arg", _arg_route, &want, NULL, 0);
    CuAssertPtrNotNull(tc, e);
    ZERO(&ctx, sizeof(ctx));
    ctx.user = &got;
    CuAssertIntEquals(tc, ERR_OK, router_exec(r, 0, &ctx));
    CuAssertIntEquals(tc, 11, got);
    router_free(r);
}
static void test_router_next_twice(CuTest *tc) {
    router_req ctx;
    ZERO(&ctx, sizeof(ctx));
    ctx.chain[0].cb = _next_twice;
    ctx.chain[1].cb = _next_gate;
    ctx.chain[2].cb = _next_handler;
    ctx.chain_n = 3;
    _next_gate_n = 0;
    _next_handler_n = 0;
    router_next(&ctx);
    CuAssertIntEquals(tc, 2, _next_gate_n);
    CuAssertIntEquals(tc, 0, _next_handler_n);
    CuAssertIntEquals(tc, 0, ctx.chain_i);
    /* 换成放行的一层：两次 next 各把下游完整跑一遍 */
    ctx.chain[1].cb = _next_pass;
    router_next(&ctx);
    CuAssertIntEquals(tc, 2, _next_handler_n);
    CuAssertIntEquals(tc, 0, ctx.chain_i);
}

/* 路径在而方法不对回 405 并带 Allow，OPTIONS 自动 200(同 Laravel)；路径不在照旧 404，
 * 方法名不认识照旧 405 不带 Allow。Allow 的方法先后按方法表，不按注册顺序 */
static void test_router_allow(CuTest *tc) {
    router_ctx *r = router_new();
    url_ctx url;
    router_req ctx;
    char allow[ROUTER_ALLOW_LENS];
    size_t n;

    CuAssertPtrNotNull(tc, r);
    CuAssertTrue(tc, router_add_index(r, "POST", 4, "/u/{id}", 7) >= 0);
    CuAssertTrue(tc, router_add_index(r, "GET|HEAD", 8, "/u/{id}", 7) >= 0);
    CuAssertTrue(tc, router_add_index(r, "DELETE", 6, "/other", 6) >= 0);

    ZERO(&ctx, sizeof(ctx));
    ctx.url = &url;
    CuAssertIntEquals(tc, -4, router_match_index(r, "PUT", 3, "/u/7", 4, &ctx));
    CuAssertIntEquals(tc, 405, router_match_code(-4));
    CuAssertIntEquals(tc, 0, ctx.params_n);
    n = router_allow_value(ctx.allow, allow);
    CuAssertTrue(tc, sizeof("GET, HEAD, POST") - 1 == n);
    CuAssertTrue(tc, 0 == memcmp(allow, "GET, HEAD, POST", n));

    ZERO(&ctx, sizeof(ctx));
    ctx.url = &url;
    CuAssertIntEquals(tc, -5, router_match_index(r, "OPTIONS", 7, "/u/7", 4, &ctx));
    CuAssertIntEquals(tc, 200, router_match_code(-5));
    CuAssertTrue(tc, (ROUTER_M_GET | ROUTER_M_HEAD | ROUTER_M_POST) == ctx.allow);

    ZERO(&ctx, sizeof(ctx));
    ctx.url = &url;
    CuAssertIntEquals(tc, -1, router_match_index(r, "PUT", 3, "/none", 5, &ctx));
    CuAssertTrue(tc, 0 == ctx.allow);
    ZERO(&ctx, sizeof(ctx));
    ctx.url = &url;
    ctx.allow = ROUTER_M_GET;
    ctx.params_n = 3;
    ctx.method = ROUTER_M_HEAD;
    CuAssertIntEquals(tc, -3, router_match_index(r, "FROB", 4, "/u/7", 4, &ctx));
    CuAssertTrue(tc, 0 == ctx.allow);
    CuAssertIntEquals(tc, 0, ctx.params_n);
    CuAssertIntEquals(tc, 0, (int32_t)ctx.method);
    ZERO(&ctx, sizeof(ctx));
    ctx.url = &url;
    CuAssertTrue(tc, router_match_index(r, "POST", 4, "/u/7", 4, &ctx) >= 0);
    CuAssertTrue(tc, 0 == ctx.allow);
    router_free(r);
}

/* 请求目标只收 "/..." 与绝对形式 "http://host/..."：别的形状第一段会被当成 host 吃掉，
 * "x/a/b" 就命中了 /a/b。方法串非法(空段、首尾竖线、不认识的名字)注册一律 -1 */
static void test_router_target_form(CuTest *tc) {
    router_ctx *r = router_new();
    url_ctx url;
    router_req ctx;

    CuAssertPtrNotNull(tc, r);
    CuAssertTrue(tc, router_add_index(r, "GET", 3, "/a/b", 4) >= 0);
    ZERO(&ctx, sizeof(ctx));
    ctx.url = &url;
    CuAssertIntEquals(tc, -2, router_match_index(r, "GET", 3, "x/a/b", 5, &ctx));
    CuAssertIntEquals(tc, 400, router_match_code(-2));
    ZERO(&ctx, sizeof(ctx));
    ctx.url = &url;
    CuAssertIntEquals(tc, -2, router_match_index(r, "GET", 3, "a/b", 3, &ctx));
    ZERO(&ctx, sizeof(ctx));
    ctx.url = &url;
    CuAssertIntEquals(tc, -2, router_match_index(r, "OPTIONS", 7, "*", 1, &ctx));
    ZERO(&ctx, sizeof(ctx));
    ctx.url = &url;
    CuAssertTrue(tc, router_match_index(r, "GET", 3, "http://h/a/b", 12, &ctx) >= 0);

    CuAssertIntEquals(tc, -1, router_add_index(r, "GET||HEAD", 9, "/m", 2));
    CuAssertIntEquals(tc, -1, router_add_index(r, "|GET", 4, "/m", 2));
    CuAssertIntEquals(tc, -1, router_add_index(r, "GET|", 4, "/m", 2));
    CuAssertIntEquals(tc, -1, router_add_index(r, "FROB", 4, "/m", 2));
    CuAssertIntEquals(tc, -1, router_add_index(r, "get", 3, "/m", 2));
    router_free(r);
}

/* 末尾通配吃下的段(已解码、空段已丢)按 '/' 拼成参数 "*"；一段没吃下就没有这个参数，
 * 通配前的占位符照常可取 */
static void test_router_wild_param(CuTest *tc) {
    router_ctx *r = router_new();
    url_ctx url;
    router_req ctx;
    const char *v;
    size_t n = 0;

    CuAssertPtrNotNull(tc, r);
    CuAssertTrue(tc, router_add_index(r, "GET", 3, "/static/*", 9) >= 0);
    CuAssertTrue(tc, router_add_index(r, "GET", 3, "/asset/{id}/*", 13) >= 0);

    ZERO(&ctx, sizeof(ctx));
    ctx.url = &url;
    CuAssertTrue(tc, router_match_index(r, "GET", 3, "/static/a//b%2Fc/d", 18, &ctx) >= 0);
    v = router_req_param(&ctx, "*", &n);
    CuAssertPtrNotNull(tc, v);
    CuAssertTrue(tc, 7 == n && 0 == memcmp(v, "a/b/c/d", 7));

    ZERO(&ctx, sizeof(ctx));
    ctx.url = &url;
    CuAssertTrue(tc, router_match_index(r, "GET", 3, "/static", 7, &ctx) >= 0);
    CuAssertPtrEquals(tc, NULL, (void *)router_req_param(&ctx, "*", &n));

    // 通配零段命中：前面的 {id} 仍要绑上，"*" 不应出现
    ZERO(&ctx, sizeof(ctx));
    ctx.url = &url;
    CuAssertTrue(tc, router_match_index(r, "GET", 3, "/asset/42", 9, &ctx) >= 0);
    v = router_req_param(&ctx, "id", &n);
    CuAssertTrue(tc, NULL != v && 2 == n && 0 == memcmp(v, "42", 2));
    CuAssertPtrEquals(tc, NULL, (void *)router_req_param(&ctx, "*", &n));

    ZERO(&ctx, sizeof(ctx));
    ctx.url = &url;
    CuAssertTrue(tc, router_match_index(r, "GET", 3, "/asset/42/x/y", 13, &ctx) >= 0);
    v = router_req_param(&ctx, "id", &n);
    CuAssertTrue(tc, NULL != v && 2 == n && 0 == memcmp(v, "42", 2));
    v = router_req_param(&ctx, "*", &n);
    CuAssertTrue(tc, NULL != v && 3 == n && 0 == memcmp(v, "x/y", 3));
    router_free(r);
}

/* 中间件登记的误用当场失败：NULL 回调、引用没 define 的名字(全局 / 路由级 / 分组)都拒，
 * 被拒的路由不进表。分组前缀与路径之间自动补 '/'("/api" + "users" 即 /api/users) */
static void test_router_register(CuTest *tc) {
    router_ctx *r = router_new();
    url_ctx url;
    router_req ctx;
    router_group g;
    router_group sub;
    const char *undef[] = { "nosuch" };

    CuAssertPtrNotNull(tc, r);
    CuAssertIntEquals(tc, ERR_FAILED, router_define(r, "x", NULL, NULL));
    CuAssertIntEquals(tc, ERR_FAILED, router_use_fn(r, NULL, NULL));
    CuAssertIntEquals(tc, ERR_FAILED, router_use(r, "x"));
    CuAssertIntEquals(tc, ERR_OK, router_define(r, "x", _next_pass, NULL));
    CuAssertIntEquals(tc, ERR_OK, router_use(r, "x"));
    CuAssertIntEquals(tc, ERR_OK, router_use_fn(r, _next_pass, NULL));

    CuAssertPtrEquals(tc, NULL, (void *)router_get(r, NULL, "/undef", _next_handler, undef, 1));
    router_group_root(&g, "/gu", undef, 1);
    CuAssertPtrEquals(tc, NULL, (void *)router_get(r, &g, "/undef", _next_handler, NULL, 0));
    ZERO(&ctx, sizeof(ctx));
    ctx.url = &url;
    CuAssertIntEquals(tc, -1, router_match_index(r, "GET", 3, "/undef", 6, &ctx));
    ZERO(&ctx, sizeof(ctx));
    ctx.url = &url;
    CuAssertIntEquals(tc, -1, router_match_index(r, "GET", 3, "/gu/undef", 9, &ctx));

    router_group_root(&g, "/api", NULL, 0);
    CuAssertPtrNotNull(tc, router_get(r, &g, "users", _next_handler, NULL, 0));
    router_group_nest(&g, &sub, "v1", NULL, 0);
    CuAssertPtrNotNull(tc, router_get(r, &sub, "items", _next_handler, NULL, 0));
    ZERO(&ctx, sizeof(ctx));
    ctx.url = &url;
    CuAssertTrue(tc, router_match_index(r, "GET", 3, "/api/users", 10, &ctx) >= 0);
    ZERO(&ctx, sizeof(ctx));
    ctx.url = &url;
    CuAssertTrue(tc, router_match_index(r, "GET", 3, "/api/v1/items", 13, &ctx) >= 0);
    ZERO(&ctx, sizeof(ctx));
    ctx.url = &url;
    CuAssertIntEquals(tc, -1, router_match_index(r, "GET", 3, "/apiusers", 9, &ctx));
    router_free(r);
}

// harbor_pack 直接写线格式，必须与原先 snprintf + http_pack_req/http_pack_head 拼出来的逐字节相同
static void test_harbor_pack_wire(CuTest *tc) {
    static const char want_call[] = "POST /call?dst=18446744073709551615&type=65535 HTTP/1.1\r\n"
        "Connection: Keep-Alive\r\nContent-Type: application/octet-stream\r\nContent-Length: 3\r\n\r\nabc";
    static const char want_req[] = "POST /request?dst=0&type=0 HTTP/1.1\r\n"
        "Connection: Keep-Alive\r\nContent-Type: application/octet-stream\r\nContent-Length: 0\r\n\r\n";
    size_t lens;
    char *p = harbor_pack(UINT64_MAX, 1, 65535, (void *)"abc", 3, &lens);
    int32_t ok_call = (sizeof(want_call) - 1 == lens && 0 == memcmp(p, want_call, lens));
    FREE(p);
    p = harbor_pack(0, 0, 0, NULL, 0, &lens);
    int32_t ok_req = (sizeof(want_req) - 1 == lens && 0 == memcmp(p, want_req, lens));
    FREE(p);
    CuAssertTrue(tc, 0 != ok_call);
    CuAssertTrue(tc, 0 != ok_req);
}
void test_advance(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_router_url_normalize);
    SUITE_ADD_TEST(suite, test_harbor_pack_wire);
    SUITE_ADD_TEST(suite, test_router_shadow_mask);
    SUITE_ADD_TEST(suite, test_router_seg_index);
    SUITE_ADD_TEST(suite, test_router_step_arg);
    SUITE_ADD_TEST(suite, test_router_chain_api);
    SUITE_ADD_TEST(suite, test_router_route_arg);
    SUITE_ADD_TEST(suite, test_router_fallback);
    SUITE_ADD_TEST(suite, test_router_next_twice);
    SUITE_ADD_TEST(suite, test_router_allow);
    SUITE_ADD_TEST(suite, test_router_target_form);
    SUITE_ADD_TEST(suite, test_router_wild_param);
    SUITE_ADD_TEST(suite, test_router_register);
    test_router_tree(suite);
}
