#include "test_advance.h"
#include "lib.h"

/* router_match_index 会就地压掉 ctx->url 的空段。npath 收了而 pathlens 没收的话，
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
    router_seg_type t;
    const char *str;
    uint32_t slen;
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

    /* 根路径拆出 0 段，第 0 段就越界 */
    idx = router_add_index(r, "GET", 3, "/", 1);
    CuAssertTrue(tc, idx >= 0);
    CuAssertIntEquals(tc, ERR_FAILED, router_seg_index(r, idx, 0, &t, &str, &slen));

    router_free(r);
}

void test_advance(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_router_url_normalize);
    SUITE_ADD_TEST(suite, test_router_shadow_mask);
    SUITE_ADD_TEST(suite, test_router_seg_index);
}
