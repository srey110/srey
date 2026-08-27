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

void test_advance(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_router_url_normalize);
}
