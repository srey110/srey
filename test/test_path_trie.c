#include "test_path_trie.h"
#include "lib.h"

// MQTT 风格规则:'/' 分隔、'+' 单层、'#' 多层、'$' 预留前缀
static const path_rules MQTT_RULES = {
    .sep = '/',
    .single_wildcard = '+',
    .multi_wildcard = '#',
    .validate_segment = NULL,
    .validate_path = NULL,
    .udata = NULL,
};
// 事件总线风格:'.' 分隔、'*' 单层、无多层
static const path_rules EVENT_RULES = {
    .sep = '.',
    .single_wildcard = '*',
    .multi_wildcard = 0,
    .validate_segment = NULL,
    .validate_path = NULL,
    .udata = NULL,
};
// 仅分隔符,无通配
static const path_rules PURE_RULES = {
    .sep = '/',
    .single_wildcard = 0,
    .multi_wildcard = 0,
    .validate_segment = NULL,
    .validate_path = NULL,
    .udata = NULL,
};

// 简单 _free 跟踪
static int32_t g_free_cnt;
static void _free_track(void *p) {
    g_free_cnt++;
    FREE(p);
}
// match cb:统计命中次数 + 收集 payload
typedef struct _match_ctx {
    int32_t count;
    void *last;
}_match_ctx;
static void _match_collect(void *payload, void *udata) {
    _match_ctx *c = (_match_ctx *)udata;
    c->count++;
    c->last = payload;
}

// 1. insert/get/remove 精确 path CRUD
static void test_pt_basic(CuTest *tc) {
    g_free_cnt = 0;
    path_trie *t = path_new(&MQTT_RULES, _free_track);
    CuAssertPtrNotNull(tc, t);
    CuAssertTrue(tc, 0 == path_count(t));

    int32_t *v;
    MALLOC(v, sizeof(int32_t));
    *v = 42;
    CuAssertTrue(tc, ERR_OK == path_insert(t, "a/b/c", v));
    CuAssertTrue(tc, 1 == path_count(t));
    int32_t *got = (int32_t *)path_get(t, "a/b/c");
    CuAssertPtrEquals(tc, v, got);
    CuAssertTrue(tc, 42 == *got);

    void *old = path_remove(t, "a/b/c");
    CuAssertPtrEquals(tc, v, old);
    CuAssertTrue(tc, 0 == path_count(t));
    CuAssertPtrEquals(tc, NULL, path_get(t, "a/b/c"));
    FREE(v);
    path_free(t);
    CuAssertTrue(tc, 0 == g_free_cnt);  // remove 不调 _free
}

// 2. 重复 insert,旧 payload _free 调一次
static void test_pt_overwrite(CuTest *tc) {
    g_free_cnt = 0;
    path_trie *t = path_new(&MQTT_RULES, _free_track);
    int32_t *a;
    int32_t *b;
    MALLOC(a, sizeof(int32_t));
    MALLOC(b, sizeof(int32_t));
    path_insert(t, "x/y", a);
    path_insert(t, "x/y", b);  // 旧 a 被 _free
    CuAssertTrue(tc, 1 == g_free_cnt);
    CuAssertTrue(tc, 1 == path_count(t));
    CuAssertPtrEquals(tc, b, path_get(t, "x/y"));
    path_free(t);                 // 释放 b
    CuAssertTrue(tc, 2 == g_free_cnt);
}

// 3. validate 否决:空 / a//b / 段含 sep
static void test_pt_validate_basic(CuTest *tc) {
    CuAssertTrue(tc, ERR_FAILED == path_validate(&MQTT_RULES, "", PATH_KIND_LITERAL));
    CuAssertTrue(tc, ERR_FAILED == path_validate(&MQTT_RULES, "a//b", PATH_KIND_LITERAL));
    CuAssertTrue(tc, ERR_FAILED == path_validate(&MQTT_RULES, "/a", PATH_KIND_LITERAL));
    CuAssertTrue(tc, ERR_FAILED == path_validate(&MQTT_RULES, "a/", PATH_KIND_LITERAL));
    CuAssertTrue(tc, ERR_OK == path_validate(&MQTT_RULES, "a/b/c", PATH_KIND_LITERAL));
    CuAssertTrue(tc, ERR_OK == path_validate(&MQTT_RULES, "a", PATH_KIND_LITERAL));
}

// 4. validate 否决:通配位置 / + # 非独占段
static void test_pt_validate_wildcard(CuTest *tc) {
    CuAssertTrue(tc, ERR_FAILED == path_validate(&MQTT_RULES, "a+b", PATH_KIND_WILDCARD));
    CuAssertTrue(tc, ERR_FAILED == path_validate(&MQTT_RULES, "a#", PATH_KIND_WILDCARD));
    CuAssertTrue(tc, ERR_FAILED == path_validate(&MQTT_RULES, "#a", PATH_KIND_WILDCARD));
    CuAssertTrue(tc, ERR_FAILED == path_validate(&MQTT_RULES, "a/#/b", PATH_KIND_WILDCARD));  // # 必须末尾
    CuAssertTrue(tc, ERR_OK == path_validate(&MQTT_RULES, "a/+/c", PATH_KIND_WILDCARD));
    CuAssertTrue(tc, ERR_OK == path_validate(&MQTT_RULES, "a/#", PATH_KIND_WILDCARD));
    CuAssertTrue(tc, ERR_OK == path_validate(&MQTT_RULES, "#", PATH_KIND_WILDCARD));
    CuAssertTrue(tc, ERR_OK == path_validate(&MQTT_RULES, "+/+/+", PATH_KIND_WILDCARD));
    // LITERAL 不允许通配
    CuAssertTrue(tc, ERR_FAILED == path_validate(&MQTT_RULES, "a/+/c", PATH_KIND_LITERAL));
    CuAssertTrue(tc, ERR_FAILED == path_validate(&MQTT_RULES, "#", PATH_KIND_LITERAL));
}

// 5. 条件启用:无通配字符配置时,"+" "#" 段当作普通字符
static void test_pt_no_wildcard(CuTest *tc) {
    // PURE_RULES 无通配,a+b 段合法
    CuAssertTrue(tc, ERR_OK == path_validate(&PURE_RULES, "a+b", PATH_KIND_LITERAL));
    CuAssertTrue(tc, ERR_OK == path_validate(&PURE_RULES, "a#", PATH_KIND_LITERAL));
    // 但 EVENT_RULES 启用 '*',a*b 段含通配但非独占,非法
    CuAssertTrue(tc, ERR_FAILED == path_validate(&EVENT_RULES, "a*b", PATH_KIND_WILDCARD));
    CuAssertTrue(tc, ERR_OK == path_validate(&EVENT_RULES, "a.*.c", PATH_KIND_WILDCARD));
}

// 6. publish 通配匹配 - 多种通配命中
static void test_pt_match_wildcard(CuTest *tc) {
    path_trie *t = path_new(&MQTT_RULES, NULL);
    int32_t v1 = 1, v2 = 2, v3 = 3, v4 = 4, v5 = 5, v6 = 6;
    path_insert(t, "a/b/c", &v1);
    path_insert(t, "a/+/c", &v2);
    path_insert(t, "a/+/+", &v3);
    path_insert(t, "a/#",   &v4);
    path_insert(t, "#",     &v5);
    path_insert(t, "+/+/+", &v6);
    _match_ctx ctx = {0, NULL};
    path_match(t, "a/b/c", _match_collect, &ctx);
    CuAssertTrue(tc, 6 == ctx.count);     // 全部命中
    path_free(t);
}

// 7. publish 'a' 命中 'a/#'(空尾)、不命中 'a/+'
static void test_pt_match_hash_empty_tail(CuTest *tc) {
    path_trie *t = path_new(&MQTT_RULES, NULL);
    int32_t v1 = 1, v2 = 2;
    path_insert(t, "a/#", &v1);
    path_insert(t, "a/+", &v2);
    _match_ctx ctx = {0, NULL};
    path_match(t, "a", _match_collect, &ctx);
    CuAssertTrue(tc, 1 == ctx.count);     // 仅 a/# 命中(空尾)
    CuAssertPtrEquals(tc, &v1, ctx.last);
    path_free(t);
}

// 8. publish 'a/b/c' 不命中 'a/b'(段数严格)
static void test_pt_match_strict_segments(CuTest *tc) {
    path_trie *t = path_new(&MQTT_RULES, NULL);
    int32_t v1 = 1;
    path_insert(t, "a/b", &v1);
    _match_ctx ctx = {0, NULL};
    path_match(t, "a/b/c", _match_collect, &ctx);
    CuAssertTrue(tc, 0 == ctx.count);
    path_free(t);
}

// 9. LITERAL 输入含通配拒绝
static void test_pt_literal_no_wildcard_input(CuTest *tc) {
    path_trie *t = path_new(&MQTT_RULES, NULL);
    int32_t v = 1;
    path_insert(t, "a/+/c", &v);
    _match_ctx ctx = {0, NULL};
    path_match(t, "a/+/c", _match_collect, &ctx);  // publish topic 含 + → 拒绝
    CuAssertTrue(tc, 0 == ctx.count);
    path_free(t);
}

// 10. insert N → remove N,count=0(ASan)
static void test_pt_insert_remove_cycle(CuTest *tc) {
    path_trie *t = path_new(&MQTT_RULES, NULL);
    char topic[32];
    int32_t i;
    for (i = 0; i < 1000; i++) {
        snprintf(topic, sizeof(topic), "ns/sub%d/item%d", i / 10, i);
        path_insert(t, topic, (void *)(intptr_t)(i + 1));
    }
    CuAssertTrue(tc, 1000 == path_count(t));
    for (i = 0; i < 1000; i++) {
        snprintf(topic, sizeof(topic), "ns/sub%d/item%d", i / 10, i);
        void *old = path_remove(t, topic);
        CuAssertPtrEquals(tc, (void *)(intptr_t)(i + 1), old);
    }
    CuAssertTrue(tc, 0 == path_count(t));
    path_free(t);
}

// 11. get_or_create 占位 + 二次返同一指针
static void test_pt_get_or_create(CuTest *tc) {
    g_free_cnt = 0;
    path_trie *t = path_new(&MQTT_RULES, _free_track);
    int32_t *a;
    int32_t *b;
    MALLOC(a, sizeof(int32_t));
    MALLOC(b, sizeof(int32_t));
    void *r1 = path_get_or_create(t, "x/y", a);
    CuAssertPtrEquals(tc, a, r1);
    void *r2 = path_get_or_create(t, "x/y", b);
    CuAssertPtrEquals(tc, a, r2);   // 返回现有,b 不被使用
    CuAssertTrue(tc, 1 == path_count(t));
    FREE(b);                         // 业务自行释放未用 init
    // init=NULL 仅查询
    CuAssertPtrEquals(tc, a, path_get_or_create(t, "x/y", NULL));
    CuAssertPtrEquals(tc, NULL, path_get_or_create(t, "x/z", NULL));
    path_free(t);
    CuAssertTrue(tc, 1 == g_free_cnt);
}

// 12. rules.validate_* 自定义回调可拒绝
static int32_t _custom_seg_validate(const char *seg, size_t len, path_kind kind, void *ud) {
    (void)kind; (void)ud;
    if (len == 1 && seg[0] == 'x') {
        return ERR_FAILED;
    }
    return ERR_OK;
}
static int32_t _custom_path_validate(const char *path, path_kind kind, void *ud) {
    (void)kind; (void)ud;
    if (path[0] == '$') {
        return ERR_FAILED;
    }
    return ERR_OK;
}
// 12. rules.validate_segment / validate_path 自定义回调可拒绝
static void test_pt_custom_callbacks(CuTest *tc) {
    path_rules custom = MQTT_RULES;
    custom.validate_segment = _custom_seg_validate;
    custom.validate_path = _custom_path_validate;
    CuAssertTrue(tc, ERR_FAILED == path_validate(&custom, "$sys/a", PATH_KIND_WILDCARD));
    CuAssertTrue(tc, ERR_FAILED == path_validate(&custom, "a/x/b", PATH_KIND_LITERAL));
    CuAssertTrue(tc, ERR_OK == path_validate(&custom, "a/y/b", PATH_KIND_LITERAL));
}

// 13. scan 全遍历:节点数对、路径重建对
typedef struct _scan_ctx {
    int32_t count;
    char paths[8][64];
}_scan_ctx;
static void _scan_collect(const char *path, void *payload, void *udata) {
    (void)payload;
    _scan_ctx *c = (_scan_ctx *)udata;
    if (c->count < 8) {
        snprintf(c->paths[c->count], sizeof(c->paths[0]), "%s", path);
    }
    c->count++;
}
// 13. scan 全遍历(节点数 + 路径字符串重建)
static void test_pt_scan(CuTest *tc) {
    path_trie *t = path_new(&MQTT_RULES, NULL);
    path_insert(t, "a", (void *)1);
    path_insert(t, "a/b", (void *)2);
    path_insert(t, "a/+/c", (void *)3);
    path_insert(t, "x/#", (void *)4);
    _scan_ctx ctx;
    ctx.count = 0;
    path_scan(t, _scan_collect, &ctx);
    CuAssertTrue(tc, 4 == ctx.count);
    // 验证 path 字符串至少出现一次正确格式(顺序不保证)
    int32_t has_ab = 0, has_apc = 0;
    int32_t i;
    for (i = 0; i < ctx.count; i++) {
        if (0 == strcmp(ctx.paths[i], "a/b")) {
            has_ab = 1;
        }
        if (0 == strcmp(ctx.paths[i], "a/+/c")) {
            has_apc = 1;
        }
    }
    CuAssertTrue(tc, has_ab);
    CuAssertTrue(tc, has_apc);
    path_free(t);
}

// 14. 嵌套深层 path,匹配正确不栈溢出
static void test_pt_deep_nested(CuTest *tc) {
    path_trie *t = path_new(&MQTT_RULES, NULL);
    // 构造 50 层 path: l1/l2/l3/.../l50
    char path[512];
    size_t pos = 0;
    int32_t i;
    for (i = 1; i <= 50; i++) {
        int32_t w = snprintf(path + pos, sizeof(path) - pos, (1 == i) ? "l%d" : "/l%d", i);
        pos += (size_t)w;
    }
    int32_t v = 99;
    CuAssertTrue(tc, ERR_OK == path_insert(t, path, &v));
    _match_ctx ctx = {0, NULL};
    path_match(t, path, _match_collect, &ctx);
    CuAssertTrue(tc, 1 == ctx.count);
    CuAssertPtrEquals(tc, &v, ctx.last);
    path_free(t);
}

// 16. path_rules_mqtt:'$' 前缀拒绝(订阅 / 发布)
static void test_pt_mqtt_dollar_reject(CuTest *tc) {
    path_rules mqtt;
    path_rules_mqtt(&mqtt);
    // 订阅模式禁 '$' 前缀
    CuAssertTrue(tc, ERR_FAILED == path_validate(&mqtt, "$SYS/broker/uptime", PATH_KIND_WILDCARD));
    CuAssertTrue(tc, ERR_FAILED == path_validate(&mqtt, "$share/grp/jobs", PATH_KIND_WILDCARD));
    // 发布精确也拒(_mqtt_validate_path 不区分 kind)
    CuAssertTrue(tc, ERR_FAILED == path_validate(&mqtt, "$SYS/broker/uptime", PATH_KIND_LITERAL));
    // 普通 topic 不受影响
    CuAssertTrue(tc, ERR_OK == path_validate(&mqtt, "a/b/c", PATH_KIND_LITERAL));
    CuAssertTrue(tc, ERR_OK == path_validate(&mqtt, "a/+/c", PATH_KIND_WILDCARD));
}
// 17. path_rules_mqtt:继承 path_trie 内置规则(# 末尾、+ 独占段、段非空)
static void test_pt_mqtt_inherited(CuTest *tc) {
    path_rules mqtt;
    path_rules_mqtt(&mqtt);
    // # 必须末尾
    CuAssertTrue(tc, ERR_FAILED == path_validate(&mqtt, "a/#/b", PATH_KIND_WILDCARD));
    // + 必须独占段
    CuAssertTrue(tc, ERR_FAILED == path_validate(&mqtt, "a+/b", PATH_KIND_WILDCARD));
    // 段非空
    CuAssertTrue(tc, ERR_FAILED == path_validate(&mqtt, "a//b", PATH_KIND_LITERAL));
    // LITERAL 不允许通配
    CuAssertTrue(tc, ERR_FAILED == path_validate(&mqtt, "a/+/c", PATH_KIND_LITERAL));
    // 合法路径
    CuAssertTrue(tc, ERR_OK == path_validate(&mqtt, "sport/tennis/score", PATH_KIND_LITERAL));
    CuAssertTrue(tc, ERR_OK == path_validate(&mqtt, "sport/+/score", PATH_KIND_WILDCARD));
    CuAssertTrue(tc, ERR_OK == path_validate(&mqtt, "sport/#", PATH_KIND_WILDCARD));
    CuAssertTrue(tc, ERR_OK == path_validate(&mqtt, "#", PATH_KIND_WILDCARD));
}
// 18. path_rules_mqtt:整合到 trie 后插入/匹配端到端
// 注:path_new 只存 rules 指针不拷贝,本测试用栈变量需保证函数返回前不被 path_free 后使用
static void test_pt_mqtt_end_to_end(CuTest *tc) {
    path_rules mqtt;
    path_rules_mqtt(&mqtt);
    path_trie *t = path_new(&mqtt, NULL);
    CuAssertPtrNotNull(tc, t);
    int32_t v1 = 1, v2 = 2, v3 = 3;
    // 合法订阅插入成功
    CuAssertTrue(tc, ERR_OK == path_insert(t, "sport/tennis/score", &v1));
    CuAssertTrue(tc, ERR_OK == path_insert(t, "sport/+/score", &v2));
    CuAssertTrue(tc, ERR_OK == path_insert(t, "sport/#", &v3));
    // $ 前缀订阅被拒
    int32_t v4 = 4;
    CuAssertTrue(tc, ERR_FAILED == path_insert(t, "$SYS/admin", &v4));
    // 普通 publish 匹配
    _match_ctx ctx = {0, NULL};
    path_match(t, "sport/tennis/score", _match_collect, &ctx);
    CuAssertTrue(tc, 3 == ctx.count);   // 命中 v1, v2, v3
    // $ 前缀 publish 被拒
    ctx.count = 0;
    path_match(t, "$SYS/broker/uptime", _match_collect, &ctx);
    CuAssertTrue(tc, 0 == ctx.count);
    // path_matches_pattern 反向匹配
    CuAssertTrue(tc, ERR_OK == path_matches_pattern(&mqtt, "sport/tennis/score", "sport/+/score"));
    CuAssertTrue(tc, ERR_OK == path_matches_pattern(&mqtt, "sport/tennis/score", "sport/#"));
    path_free(t);
}

// 19. path_matches_pattern 全组合
static void test_pt_matches_pattern(CuTest *tc) {
    CuAssertTrue(tc, ERR_OK == path_matches_pattern(&MQTT_RULES, "a/b/c", "a/b/c"));
    CuAssertTrue(tc, ERR_OK == path_matches_pattern(&MQTT_RULES, "a/b/c", "a/+/c"));
    CuAssertTrue(tc, ERR_OK == path_matches_pattern(&MQTT_RULES, "a/b/c", "+/+/+"));
    CuAssertTrue(tc, ERR_OK == path_matches_pattern(&MQTT_RULES, "a/b/c", "a/#"));
    CuAssertTrue(tc, ERR_OK == path_matches_pattern(&MQTT_RULES, "a/b/c", "#"));
    CuAssertTrue(tc, ERR_OK == path_matches_pattern(&MQTT_RULES, "a/b", "a/+"));
    // 不匹配
    CuAssertTrue(tc, ERR_FAILED == path_matches_pattern(&MQTT_RULES, "a/b/c", "a/b"));     // 段数不等
    CuAssertTrue(tc, ERR_FAILED == path_matches_pattern(&MQTT_RULES, "a/b/c", "a/x/c"));   // 字符不等
    CuAssertTrue(tc, ERR_FAILED == path_matches_pattern(&MQTT_RULES, "a/b/c", "a/+"));     // 段数不等
    // 边界:literal 段数不够
    CuAssertTrue(tc, ERR_OK == path_matches_pattern(&MQTT_RULES, "a", "a/#"));        // # 匹配空尾
    CuAssertTrue(tc, ERR_FAILED == path_matches_pattern(&MQTT_RULES, "a", "a/+"));    // + 必须有段
}

// 20. payload 不可为 NULL:insert NULL 返 ERR_FAILED,不增 count、不留节点、不误调 _free
static void test_pt_reject_null_payload(CuTest *tc) {
    g_free_cnt = 0;
    path_trie *t = path_new(&MQTT_RULES, _free_track);
    CuAssertTrue(tc, ERR_FAILED == path_insert(t, "a/b", NULL));
    CuAssertTrue(tc, 0 == path_count(t));
    CuAssertPtrEquals(tc, NULL, path_get(t, "a/b"));
    int32_t v = 7;
    CuAssertTrue(tc, ERR_OK == path_insert(t, "a/b", &v));
    CuAssertTrue(tc, 1 == path_count(t));
    // 已有 payload 时再 insert NULL 仍被拒,既有 payload 与 count 不受影响
    CuAssertTrue(tc, ERR_FAILED == path_insert(t, "a/b", NULL));
    CuAssertTrue(tc, 1 == path_count(t));
    CuAssertPtrEquals(tc, &v, path_get(t, "a/b"));
    CuAssertPtrEquals(tc, &v, path_remove(t, "a/b"));
    CuAssertTrue(tc, 0 == path_count(t));
    path_free(t);
    CuAssertTrue(tc, 0 == g_free_cnt);
}

// 注册套件
void test_path_trie(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_pt_basic);
    SUITE_ADD_TEST(suite, test_pt_overwrite);
    SUITE_ADD_TEST(suite, test_pt_validate_basic);
    SUITE_ADD_TEST(suite, test_pt_validate_wildcard);
    SUITE_ADD_TEST(suite, test_pt_no_wildcard);
    SUITE_ADD_TEST(suite, test_pt_match_wildcard);
    SUITE_ADD_TEST(suite, test_pt_match_hash_empty_tail);
    SUITE_ADD_TEST(suite, test_pt_match_strict_segments);
    SUITE_ADD_TEST(suite, test_pt_literal_no_wildcard_input);
    SUITE_ADD_TEST(suite, test_pt_insert_remove_cycle);
    SUITE_ADD_TEST(suite, test_pt_get_or_create);
    SUITE_ADD_TEST(suite, test_pt_custom_callbacks);
    SUITE_ADD_TEST(suite, test_pt_scan);
    SUITE_ADD_TEST(suite, test_pt_deep_nested);
    SUITE_ADD_TEST(suite, test_pt_mqtt_dollar_reject);
    SUITE_ADD_TEST(suite, test_pt_mqtt_inherited);
    SUITE_ADD_TEST(suite, test_pt_mqtt_end_to_end);
    SUITE_ADD_TEST(suite, test_pt_matches_pattern);
    SUITE_ADD_TEST(suite, test_pt_reject_null_payload);
}
