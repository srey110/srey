#include "test_advance.h"
#include "lib.h"
#include "advance/router_tree.h"

#define RT_LIT(s)   { (uint32_t)(sizeof(s) - 1), ROUTER_SEG_LIT,   (char *)(s) }
#define RT_PARAM(s) { (uint32_t)(sizeof(s) - 1), ROUTER_SEG_PARAM, (char *)(s) }
#define RT_OPT(s)   { (uint32_t)(sizeof(s) - 1), ROUTER_SEG_OPT,   (char *)(s) }
#define RT_WILD     { 0, ROUTER_SEG_WILD, NULL }

// 注册辅助：统一清零 res，调用方无论成功失败都 free。
static int32_t _rt_add(rtree_ctx *t, router_route_id id, router_method mask,
                       const router_seg *segs, int32_t n,
                       rtree_add_result *res) {
    ZERO(res, sizeof(*res));
    return rtree_add(t, segs, n, mask, id, res);
}

// 匹配辅助：把字符串段转成 buf_ctx，n=0 时传 NULL。
static int32_t _rt_match(rtree_ctx *t, const char *const *parts, int32_t n,
                         router_method mask, char *scratch, size_t cap,
                         rtree_match_result *out) {
    buf_ctx segs[URL_MAX_PATH_DEPTH];
    for (int32_t i = 0; i < n; i++) {
        segs[i].data = (void *)parts[i];
        segs[i].lens = strlen(parts[i]);
    }
    ZERO(out, sizeof(*out));
    return rtree_match(t, n > 0 ? segs : NULL, n, (uint8_t *)scratch, cap,
                       mask, out);
}

// 比较第 i 个参数值与字符串，长度必须一致。
static int32_t _rt_param_eq(const rtree_match_result *m, int32_t i,
                            const char *want) {
    size_t n = strlen(want);
    return i < m->params_n && m->params[i].len == n
        && 0 == memcmp(m->params[i].val, want, n);
}

static void test_rt_static_param_wild(CuTest *tc) {
    rtree_ctx *t = rtree_new();
    rtree_add_result res;
    rtree_match_result m;
    char scratch[128];
    const char *user[] = { "user" };
    const char *uid[] = { "user", "42" };
    const char *stat0[] = { "static" };
    const char *stat2[] = { "static", "a", "b" };
    const char *asset[] = { "asset", "42", "x", "y" };
    const char *nope[] = { "nope" };

    router_seg root[] = { { 0, ROUTER_SEG_LIT, (char *)"" } };
    router_seg r_user[] = { RT_LIT("user") };
    router_seg r_uid[] = { RT_LIT("user"), RT_PARAM("id") };
    router_seg r_stat[] = { RT_LIT("static"), RT_WILD };
    router_seg r_asset[] = { RT_LIT("asset"), RT_PARAM("id"), RT_WILD };

    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 0, ROUTER_M_GET, root, 0, &res));
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 1, ROUTER_M_GET, r_user, 1, &res));
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 2, ROUTER_M_GET, r_uid, 2, &res));
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 3, ROUTER_M_GET, r_stat, 2, &res));
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 4, ROUTER_M_GET, r_asset, 3, &res));
    rtree_add_result_free(&res);

    CuAssertIntEquals(tc, 0, _rt_match(t, NULL, 0, ROUTER_M_GET, scratch,
                                       sizeof(scratch), &m));
    CuAssertIntEquals(tc, 1, _rt_match(t, user, 1, ROUTER_M_GET, scratch,
                                       sizeof(scratch), &m));
    CuAssertIntEquals(tc, 2, _rt_match(t, uid, 2, ROUTER_M_GET, scratch,
                                       sizeof(scratch), &m));
    CuAssertTrue(tc, _rt_param_eq(&m, 0, "42"));
    CuAssertIntEquals(tc, 3, _rt_match(t, stat0, 1, ROUTER_M_GET, scratch,
                                       sizeof(scratch), &m));
    CuAssertIntEquals(tc, 0, m.params_n);
    CuAssertIntEquals(tc, 3, _rt_match(t, stat2, 3, ROUTER_M_GET, scratch,
                                       sizeof(scratch), &m));
    CuAssertTrue(tc, _rt_param_eq(&m, 0, "a/b"));
    CuAssertIntEquals(tc, 4, _rt_match(t, asset, 4, ROUTER_M_GET, scratch,
                                       sizeof(scratch), &m));
    CuAssertTrue(tc, _rt_param_eq(&m, 0, "42"));
    CuAssertTrue(tc, _rt_param_eq(&m, 1, "x/y"));

    CuAssertIntEquals(tc, -1, _rt_match(t, nope, 1, ROUTER_M_GET, scratch,
                                        sizeof(scratch), &m));
    CuAssertIntEquals(tc, -3, _rt_match(t, user, 1, (router_method)0, scratch,
                                        sizeof(scratch), &m));
    CuAssertIntEquals(tc, -4, _rt_match(t, user, 1, ROUTER_M_POST, scratch,
                                        sizeof(scratch), &m));
    CuAssertTrue(tc, 0 != (m.allow & ROUTER_M_GET));

    CuAssertIntEquals(tc, ERR_OK, rtree_check(t));
    CuAssertTrue(tc, rtree_node_count(t) > 0);
    CuAssertIntEquals(tc, 5, (int32_t)rtree_leaf_count(t));
    CuAssertTrue(tc, rtree_mem(t) > 0);
    rtree_free(t);
}

static void test_rt_priority(CuTest *tc) {
    rtree_ctx *t = rtree_new();
    rtree_add_result res;
    rtree_match_result m;
    char scratch[128];
    const char *user_new[] = { "user", "new" };
    const char *user_old[] = { "user", "old" };
    const char *a_b_c[] = { "a", "b", "c" };
    const char *a_b_d[] = { "a", "b", "d" };
    const char *a_x_c[] = { "a", "x", "c" };
    const char *m_static[] = { "m", "static" };
    const char *w_one[] = { "w", "a" };
    const char *w_two[] = { "w", "a", "b" };
    const char *s_css[] = { "s", "css" };
    const char *s_other[] = { "s", "other" };

    router_seg r_un[] = { RT_LIT("user"), RT_LIT("new") };
    router_seg r_uid[] = { RT_LIT("user"), RT_PARAM("id") };
    router_seg r_abc[] = { RT_LIT("a"), RT_LIT("b"), RT_LIT("c") };
    router_seg r_axc[] = { RT_LIT("a"), RT_PARAM("x"), RT_LIT("c") };
    router_seg r_ms[] = { RT_LIT("m"), RT_LIT("static") };
    router_seg r_mp[] = { RT_LIT("m"), RT_PARAM("id") };
    router_seg r_wp[] = { RT_LIT("w"), RT_PARAM("x") };
    router_seg r_ww[] = { RT_LIT("w"), RT_WILD };
    router_seg r_scss[] = { RT_LIT("s"), RT_LIT("css") };
    router_seg r_sw[] = { RT_LIT("s"), RT_WILD };

    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 10, ROUTER_M_GET, r_un, 2, &res));
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 11, ROUTER_M_GET, r_uid, 2, &res));
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 12, ROUTER_M_GET, r_abc, 3, &res));
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 13, ROUTER_M_GET, r_axc, 3, &res));
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 14, ROUTER_M_GET, r_ms, 2, &res));
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 15, ROUTER_M_POST, r_mp, 2, &res));
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 16, ROUTER_M_GET, r_wp, 2, &res));
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 17, ROUTER_M_GET, r_ww, 2, &res));
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 18, ROUTER_M_GET, r_scss, 2, &res));
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 19, ROUTER_M_GET, r_sw, 2, &res));
    rtree_add_result_free(&res);

    CuAssertIntEquals(tc, 10, _rt_match(t, user_new, 2, ROUTER_M_GET, scratch,
                                        sizeof(scratch), &m));
    CuAssertIntEquals(tc, 11, _rt_match(t, user_old, 2, ROUTER_M_GET, scratch,
                                        sizeof(scratch), &m));
    CuAssertIntEquals(tc, 12, _rt_match(t, a_b_c, 3, ROUTER_M_GET, scratch,
                                        sizeof(scratch), &m));
    CuAssertIntEquals(tc, -1, _rt_match(t, a_b_d, 3, ROUTER_M_GET, scratch,
                                        sizeof(scratch), &m));
    CuAssertIntEquals(tc, 13, _rt_match(t, a_x_c, 3, ROUTER_M_GET, scratch,
                                        sizeof(scratch), &m));
    CuAssertIntEquals(tc, 15, _rt_match(t, m_static, 2, ROUTER_M_POST, scratch,
                                        sizeof(scratch), &m));
    CuAssertIntEquals(tc, 16, _rt_match(t, w_one, 2, ROUTER_M_GET, scratch,
                                        sizeof(scratch), &m));
    CuAssertIntEquals(tc, 17, _rt_match(t, w_two, 3, ROUTER_M_GET, scratch,
                                        sizeof(scratch), &m));
    CuAssertIntEquals(tc, 18, _rt_match(t, s_css, 2, ROUTER_M_GET, scratch,
                                        sizeof(scratch), &m));
    CuAssertIntEquals(tc, 19, _rt_match(t, s_other, 2, ROUTER_M_GET, scratch,
                                        sizeof(scratch), &m));
    CuAssertIntEquals(tc, ERR_OK, rtree_check(t));
    rtree_free(t);
}

static void test_rt_optional(CuTest *tc) {
    rtree_ctx *t = rtree_new();
    rtree_add_result res;
    rtree_match_result m;
    char scratch[128];
    const char *file0[] = { "file" };
    const char *file1[] = { "file", "abc" };
    const char *a_foo[] = { "a", "foo" };
    const char *a_foo_bar[] = { "a", "foo", "bar" };
    const char *b_only[] = { "b" };
    const char *b_foo[] = { "b", "foo" };
    const char *b_foo_bar[] = { "b", "foo", "bar" };
    const char *many16[16];
    const char *many17[17];

    router_seg r_file[] = { RT_LIT("file"), RT_OPT("path") };
    router_seg r_two[] = { RT_LIT("a"), RT_OPT("x"), RT_OPT("y") };
    router_seg r_wild[] = { RT_LIT("b"), RT_OPT("x"), RT_WILD };
    router_seg r_opt9[] = {
        RT_LIT("o"), RT_OPT("a"), RT_OPT("b"), RT_OPT("c"), RT_OPT("d"),
        RT_OPT("e"), RT_OPT("f"), RT_OPT("g"), RT_OPT("h"), RT_OPT("i")
    };

    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 20, ROUTER_M_GET, r_file, 2, &res));
    CuAssertIntEquals(tc, 2, res.n);
    CuAssertIntEquals(tc, 2, res.inserted);
    CuAssertIntEquals(tc, 0, res.shadowed);
    CuAssertIntEquals(tc, 0, res.skipped);
    rtree_add_result_free(&res);

    CuAssertIntEquals(tc, 20, _rt_match(t, file0, 1, ROUTER_M_GET, scratch,
                                        sizeof(scratch), &m));
    CuAssertIntEquals(tc, 0, m.params_n);
    CuAssertIntEquals(tc, 20, _rt_match(t, file1, 2, ROUTER_M_GET, scratch,
                                        sizeof(scratch), &m));
    CuAssertTrue(tc, _rt_param_eq(&m, 0, "abc"));

    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 22, ROUTER_M_GET, r_two, 3, &res));
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, 22, _rt_match(t, a_foo, 2, ROUTER_M_GET, scratch,
                                        sizeof(scratch), &m));
    CuAssertIntEquals(tc, 1, m.params_n);
    CuAssertTrue(tc, _rt_param_eq(&m, 0, "foo"));
    CuAssertIntEquals(tc, 22, _rt_match(t, a_foo_bar, 3, ROUTER_M_GET, scratch,
                                        sizeof(scratch), &m));
    CuAssertIntEquals(tc, 2, m.params_n);
    CuAssertTrue(tc, _rt_param_eq(&m, 0, "foo"));
    CuAssertTrue(tc, _rt_param_eq(&m, 1, "bar"));

    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 23, ROUTER_M_GET, r_wild, 3, &res));
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, 23, _rt_match(t, b_only, 1, ROUTER_M_GET, scratch,
                                        sizeof(scratch), &m));
    CuAssertIntEquals(tc, 0, m.params_n);
    CuAssertIntEquals(tc, 23, _rt_match(t, b_foo, 2, ROUTER_M_GET, scratch,
                                        sizeof(scratch), &m));
    CuAssertIntEquals(tc, 1, m.params_n);
    CuAssertTrue(tc, _rt_param_eq(&m, 0, "foo"));
    CuAssertIntEquals(tc, 23, _rt_match(t, b_foo_bar, 3, ROUTER_M_GET, scratch,
                                        sizeof(scratch), &m));
    CuAssertIntEquals(tc, 2, m.params_n);
    CuAssertTrue(tc, _rt_param_eq(&m, 0, "foo"));
    CuAssertTrue(tc, _rt_param_eq(&m, 1, "bar"));

    CuAssertIntEquals(tc, -1, _rt_add(t, 24, ROUTER_M_GET, r_opt9, 10, &res));
    rtree_add_result_free(&res);

    // 16 必填 + 1 可选：present 变体有 17 个参数被跳过，absent 变体正常入树。
    router_seg r_pmax[17];
    char pnames[16][8];
    for (int32_t i = 0; i < 16; i++) {
        SNPRINTF(pnames[i], sizeof(pnames[i]), "p%d", i);
        r_pmax[i].t = ROUTER_SEG_PARAM;
        r_pmax[i].str = pnames[i];
        r_pmax[i].str_len = (uint32_t)strlen(pnames[i]);
        many16[i] = "v";
        many17[i] = "v";
    }
    r_pmax[16].t = ROUTER_SEG_OPT;
    r_pmax[16].str = "tail";
    r_pmax[16].str_len = 4;
    many17[16] = "v";
    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 25, ROUTER_M_GET, r_pmax, 17, &res));
    CuAssertIntEquals(tc, 1, res.inserted);
    CuAssertTrue(tc, res.skipped >= 1);
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, 25, _rt_match(t, many16, 16, ROUTER_M_GET, scratch,
                                        sizeof(scratch), &m));
    CuAssertIntEquals(tc, 16, m.params_n);
    CuAssertIntEquals(tc, -1, _rt_match(t, many17, 17, ROUTER_M_GET, scratch,
                                        sizeof(scratch), &m));

    CuAssertIntEquals(tc, ERR_OK, rtree_check(t));
    rtree_free(t);
}

// 可选段只能在末尾：中间的注册失败；末尾可选段后跟 * 时仍是靠前的 OPT 优先取值
static void test_rt_opt_trailing(CuTest *tc) {
    rtree_ctx *t = rtree_new();
    rtree_add_result res;
    rtree_match_result m;
    char scratch[128];
    router_seg r_mid[] = { RT_LIT("a"), RT_OPT("x"), RT_LIT("b") };
    router_seg r_lead[] = { RT_OPT("lang"), RT_LIT("docs") };
    router_seg r_param[] = { RT_LIT("p"), RT_OPT("x"), RT_PARAM("y") };
    router_seg r_ow[] = { RT_LIT("w"), RT_OPT("x"), RT_OPT("y"), RT_WILD };
    CuAssertIntEquals(tc, -1, _rt_add(t, 1, ROUTER_M_GET, r_mid, 3, &res));
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, -1, _rt_add(t, 2, ROUTER_M_GET, r_lead, 2, &res));
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, -1, _rt_add(t, 3, ROUTER_M_GET, r_param, 3, &res));
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, 0, (int32_t)rtree_leaf_count(t));
    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 4, ROUTER_M_GET, r_ow, 4, &res));
    rtree_add_result_free(&res);
    const char *w1[] = { "w", "a" };
    const char *w3[] = { "w", "a", "b", "c" };
    CuAssertIntEquals(tc, 4, _rt_match(t, w1, 2, ROUTER_M_GET, scratch, sizeof(scratch), &m));
    CuAssertIntEquals(tc, 1, m.params_n);
    CuAssertTrue(tc, _rt_param_eq(&m, 0, "a"));
    CuAssertIntEquals(tc, 4, _rt_match(t, w3, 4, ROUTER_M_GET, scratch, sizeof(scratch), &m));
    CuAssertIntEquals(tc, 3, m.params_n);
    CuAssertTrue(tc, _rt_param_eq(&m, 0, "a"));
    CuAssertTrue(tc, _rt_param_eq(&m, 1, "b"));
    CuAssertTrue(tc, _rt_param_eq(&m, 2, "c"));
    CuAssertIntEquals(tc, ERR_OK, rtree_check(t));
    rtree_free(t);
}

static void test_rt_shadow(CuTest *tc) {
    rtree_ctx *t = rtree_new();
    rtree_add_result res;
    rtree_match_result m;
    char scratch[64];
    const char *s[] = { "s" };
    const char *h[] = { "h" };
    const char *p[] = { "p" };
    const char *o[] = { "o" };
    const char *o_foo[] = { "o", "foo" };
    const char *d[] = { "d" };

    router_seg r_s[] = { RT_LIT("s") };
    router_seg r_h[] = { RT_LIT("h") };
    router_seg r_p[] = { RT_LIT("p") };
    router_seg r_o[] = { RT_LIT("o") };
    router_seg r_oo[] = { RT_LIT("o"), RT_OPT("x") };
    router_seg r_d[] = { RT_LIT("d") };

    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 30, ROUTER_M_ANY, r_s, 1, &res));
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, 30, _rt_match(t, s, 1, ROUTER_M_GET, scratch,
                                        sizeof(scratch), &m));
    CuAssertIntEquals(tc, -2, _rt_add(t, 31, ROUTER_M_GET, r_s, 1, &res));
    rtree_add_result_free(&res);

    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 32, ROUTER_M_HEAD, r_h, 1, &res));
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 33, ROUTER_M_GET | ROUTER_M_HEAD,
                                          r_h, 1, &res));
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, 32, _rt_match(t, h, 1, ROUTER_M_HEAD, scratch,
                                        sizeof(scratch), &m));
    CuAssertIntEquals(tc, 33, _rt_match(t, h, 1, ROUTER_M_GET, scratch,
                                        sizeof(scratch), &m));

    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 34, ROUTER_M_GET | ROUTER_M_HEAD,
                                          r_p, 1, &res));
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 35, ROUTER_M_POST, r_p, 1, &res));
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, 34, _rt_match(t, p, 1, ROUTER_M_GET, scratch,
                                        sizeof(scratch), &m));
    CuAssertIntEquals(tc, 35, _rt_match(t, p, 1, ROUTER_M_POST, scratch,
                                        sizeof(scratch), &m));

    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 36, ROUTER_M_GET, r_o, 1, &res));
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 37, ROUTER_M_GET, r_oo, 2, &res));
    CuAssertIntEquals(tc, 2, res.n);
    CuAssertIntEquals(tc, 1, res.inserted);
    CuAssertIntEquals(tc, 1, res.shadowed);
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, 36, _rt_match(t, o, 1, ROUTER_M_GET, scratch,
                                        sizeof(scratch), &m));
    CuAssertIntEquals(tc, 37, _rt_match(t, o_foo, 2, ROUTER_M_GET, scratch,
                                        sizeof(scratch), &m));

    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 38, ROUTER_M_GET, r_d, 1, &res));
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, -2, _rt_add(t, 39, ROUTER_M_GET, r_d, 1, &res));
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, 38, _rt_match(t, d, 1, ROUTER_M_GET, scratch,
                                        sizeof(scratch), &m));

    CuAssertIntEquals(tc, ERR_OK, rtree_check(t));
    rtree_free(t);
}

static void test_rt_methods(CuTest *tc) {
    rtree_ctx *t = rtree_new();
    rtree_add_result res;
    rtree_match_result m;
    char scratch[64];
    const char *u7[] = { "u", "7" };
    const char *any[] = { "any" };

    router_seg r_u[] = { RT_LIT("u"), RT_PARAM("id") };
    router_seg r_any[] = { RT_LIT("any") };

    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 40, ROUTER_M_POST, r_u, 2, &res));
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 41, ROUTER_M_GET | ROUTER_M_HEAD,
                                          r_u, 2, &res));
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 42, ROUTER_M_ANY, r_any, 1, &res));
    rtree_add_result_free(&res);

    CuAssertIntEquals(tc, -4, _rt_match(t, u7, 2, ROUTER_M_PUT, scratch,
                                        sizeof(scratch), &m));
    CuAssertTrue(tc, (m.allow & (ROUTER_M_GET | ROUTER_M_HEAD | ROUTER_M_POST))
                     == (ROUTER_M_GET | ROUTER_M_HEAD | ROUTER_M_POST));
    CuAssertIntEquals(tc, -5, _rt_match(t, u7, 2, ROUTER_M_OPTIONS, scratch,
                                        sizeof(scratch), &m));
    CuAssertTrue(tc, 0 != (m.allow & ROUTER_M_POST));
    CuAssertIntEquals(tc, 42, _rt_match(t, any, 1, ROUTER_M_POST, scratch,
                                        sizeof(scratch), &m));
    CuAssertIntEquals(tc, 42, _rt_match(t, any, 1, ROUTER_M_PUT, scratch,
                                        sizeof(scratch), &m));
    CuAssertIntEquals(tc, ERR_OK, rtree_check(t));
    rtree_free(t);
}

static void test_rt_variant_meta(CuTest *tc) {
    rtree_ctx *t = rtree_new();
    rtree_add_result res;
    rtree_match_result m;
    char scratch[64];
    const char *v_x[] = { "v", "x" };
    const char *v_xy[] = { "v", "x", "y" };
    int32_t i0 = -1;
    int32_t i1 = -1;

    router_seg r_v[] = { RT_LIT("v"), RT_PARAM("a"), RT_OPT("b") };
    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 50, ROUTER_M_GET, r_v, 3, &res));
    CuAssertIntEquals(tc, 2, res.n);
    for (int32_t i = 0; i < res.n; i++) {
        if (0 == res.variants[i].opt_bits) {
            i0 = i;
        } else {
            i1 = i;
        }
    }
    CuAssertTrue(tc, i0 >= 0 && i1 >= 0);
    CuAssertIntEquals(tc, 1, res.variants[i0].param_n);
    CuAssertIntEquals(tc, 1, res.variants[i0].param_seg[0]);
    CuAssertIntEquals(tc, 2, res.variants[i1].param_n);
    CuAssertIntEquals(tc, 1, res.variants[i1].param_seg[0]);
    CuAssertIntEquals(tc, 2, res.variants[i1].param_seg[1]);

    CuAssertIntEquals(tc, 50, _rt_match(t, v_x, 2, ROUTER_M_GET, scratch,
                                        sizeof(scratch), &m));
    CuAssertIntEquals(tc, i0, (int32_t)m.variant_id);
    CuAssertIntEquals(tc, 1, m.params_n);
    CuAssertIntEquals(tc, 50, _rt_match(t, v_xy, 3, ROUTER_M_GET, scratch,
                                        sizeof(scratch), &m));
    CuAssertIntEquals(tc, i1, (int32_t)m.variant_id);
    CuAssertIntEquals(tc, 2, m.params_n);
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, ERR_OK, rtree_check(t));
    rtree_free(t);
}

static void test_rt_bad_input(CuTest *tc) {
    rtree_ctx *t = rtree_new();
    rtree_add_result res;
    rtree_match_result m;
    char scratch[16];
    router_seg one[] = { RT_LIT("x") };
    router_seg bad_wild[] = { RT_WILD, RT_LIT("x") };
    router_seg wild[] = { RT_WILD };
    buf_ctx many[URL_MAX_PATH_DEPTH + 1];

    CuAssertIntEquals(tc, -1, _rt_add(t, 0, (router_method)0, one, 1, &res));
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, -1, _rt_add(t, ROUTER_ROUTE_NONE, ROUTER_M_GET,
                                      one, 1, &res));
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, -1, _rt_add(t, 0, ROUTER_M_GET, NULL, 1, &res));
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, -1, _rt_add(t, 0, ROUTER_M_GET, bad_wild, 2, &res));
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 7, ROUTER_M_GET, wild, 1, &res));
    rtree_add_result_free(&res);
    CuAssertIntEquals(tc, -3, _rt_match(t, NULL, 0, (router_method)0,
                                        scratch, sizeof(scratch), &m));
    CuAssertIntEquals(tc, -1, rtree_match(t, NULL, -1, (uint8_t *)scratch,
                                          sizeof(scratch), ROUTER_M_GET, &m));
    for (int32_t i = 0; i < URL_MAX_PATH_DEPTH + 1; i++) {
        many[i].data = (void *)"x";
        many[i].lens = 1;
    }
    CuAssertIntEquals(tc, -1, rtree_match(t, many, URL_MAX_PATH_DEPTH + 1,
                                          (uint8_t *)scratch, sizeof(scratch),
                                          ROUTER_M_GET, &m));
    CuAssertIntEquals(tc, ROUTER_ROUTE_NONE, m.route_id);
    CuAssertIntEquals(tc, ERR_OK, rtree_check(t));
    rtree_free(t);
}

static void test_rt_scratch_limit(CuTest *tc) {
    rtree_ctx *t = rtree_new();
    rtree_add_result res;
    rtree_match_result m;
    char scratch[8];
    const char *ab[] = { "w", "a", "b" };

    router_seg r_w[] = { RT_LIT("w"), RT_WILD };
    CuAssertIntEquals(tc, ERR_OK, _rt_add(t, 60, ROUTER_M_GET, r_w, 2, &res));
    rtree_add_result_free(&res);

    CuAssertIntEquals(tc, 60, _rt_match(t, ab, 3, ROUTER_M_GET, scratch, 3, &m));
    CuAssertTrue(tc, _rt_param_eq(&m, 0, "a/b"));
    CuAssertIntEquals(tc, -1, _rt_match(t, ab, 3, ROUTER_M_GET, scratch, 2, &m));
    CuAssertIntEquals(tc, ERR_OK, rtree_check(t));
    rtree_free(t);
}

void test_router_tree(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_rt_static_param_wild);
    SUITE_ADD_TEST(suite, test_rt_priority);
    SUITE_ADD_TEST(suite, test_rt_optional);
    SUITE_ADD_TEST(suite, test_rt_opt_trailing);
    SUITE_ADD_TEST(suite, test_rt_shadow);
    SUITE_ADD_TEST(suite, test_rt_methods);
    SUITE_ADD_TEST(suite, test_rt_variant_meta);
    SUITE_ADD_TEST(suite, test_rt_scratch_limit);
    SUITE_ADD_TEST(suite, test_rt_bad_input);
}
