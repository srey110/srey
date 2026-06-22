
#include "test_bson.h"
#include "lib.h"
#include <string.h>

// 用写完的 bson_ctx 的原始字节创建只读迭代器（写完后 offset 在末尾，需从 data[0] 重新读）
#define BSON_ITER_FROM(bson, reader, iter) \
    bson_ctx reader; \
    bson_iter iter; \
    bson_init(&reader, BSON_DOC(&bson), BSON_DOC_LENS(&bson)); \
    bson_iter_init(&iter, &reader)

/* =======================================================================
 * 基本类型：double / utf8 / int32 / int64 / bool / null / oid / binary / date
 * ======================================================================= */
static void test_bson_primitives(CuTest *tc) {
    char oid[BSON_OID_LENS];
    char bindata[3];
    int32_t err;
    memset(oid, 0xAB, BSON_OID_LENS);
    bindata[0] = 0x01; bindata[1] = 0x02; bindata[2] = 0x03;

    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_double(&bson, "pi", 3.14);
    bson_append_utf8(&bson, "name", "hello");
    bson_append_int32(&bson, "age", 42);
    bson_append_int64(&bson, "big", 3000000000LL);
    bson_append_bool(&bson, "flag", 1);
    bson_append_null(&bson, "nothing");
    bson_append_oid(&bson, "_id", oid);
    bson_append_binary(&bson, "data", BSON_SUBTYPE_BINARY, bindata, 3);
    bson_append_date(&bson, "ts", 1700000000000LL);
    bson_append_end(&bson);

    CuAssertTrue(tc, bson_complete(&bson));

    BSON_ITER_FROM(bson, rd, iter);

    // double
    CuAssertTrue(tc, bson_iter_next(&iter));
    CuAssertIntEquals(tc, BSON_DOUBLE, iter.type);
    CuAssertStrEquals(tc, "pi", iter.key);
    CuAssertDblEquals(tc, 3.14, bson_iter_double(&iter, &err), 1e-10);
    CuAssertIntEquals(tc, ERR_OK, err);

    // utf8
    CuAssertTrue(tc, bson_iter_next(&iter));
    CuAssertIntEquals(tc, BSON_UTF8, iter.type);
    CuAssertStrEquals(tc, "name", iter.key);
    CuAssertStrEquals(tc, "hello", bson_iter_utf8(&iter, &err));
    CuAssertIntEquals(tc, ERR_OK, err);

    // int32
    CuAssertTrue(tc, bson_iter_next(&iter));
    CuAssertIntEquals(tc, BSON_INT32, iter.type);
    CuAssertStrEquals(tc, "age", iter.key);
    CuAssertIntEquals(tc, 42, bson_iter_int32(&iter, &err));
    CuAssertIntEquals(tc, ERR_OK, err);

    // int64
    CuAssertTrue(tc, bson_iter_next(&iter));
    CuAssertIntEquals(tc, BSON_INT64, iter.type);
    CuAssertStrEquals(tc, "big", iter.key);
    CuAssertTrue(tc, 3000000000LL == bson_iter_int64(&iter, &err));
    CuAssertIntEquals(tc, ERR_OK, err);

    // bool
    CuAssertTrue(tc, bson_iter_next(&iter));
    CuAssertIntEquals(tc, BSON_BOOL, iter.type);
    CuAssertStrEquals(tc, "flag", iter.key);
    CuAssertTrue(tc, bson_iter_bool(&iter, &err));
    CuAssertIntEquals(tc, ERR_OK, err);

    // null
    CuAssertTrue(tc, bson_iter_next(&iter));
    CuAssertIntEquals(tc, BSON_NULL, iter.type);
    CuAssertStrEquals(tc, "nothing", iter.key);

    // oid
    CuAssertTrue(tc, bson_iter_next(&iter));
    CuAssertIntEquals(tc, BSON_OID, iter.type);
    CuAssertStrEquals(tc, "_id", iter.key);
    char *got_oid = bson_iter_oid(&iter, &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, 0 == memcmp(got_oid, oid, BSON_OID_LENS));

    // binary
    CuAssertTrue(tc, bson_iter_next(&iter));
    CuAssertIntEquals(tc, BSON_BINARY, iter.type);
    CuAssertStrEquals(tc, "data", iter.key);
    bson_subtype subtype = BSON_SUBTYPE_BINARY;
    size_t blens = 0;
    char *bdata = bson_iter_binary(&iter, &subtype, &blens, &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertIntEquals(tc, BSON_SUBTYPE_BINARY, (int32_t)subtype);
    CuAssertTrue(tc, 3 == blens);
    CuAssertTrue(tc, 0 == memcmp(bdata, bindata, 3));

    // date
    CuAssertTrue(tc, bson_iter_next(&iter));
    CuAssertIntEquals(tc, BSON_DATE, iter.type);
    CuAssertStrEquals(tc, "ts", iter.key);
    CuAssertTrue(tc, 1700000000000LL == bson_iter_date(&iter, &err));
    CuAssertIntEquals(tc, ERR_OK, err);

    // EOD
    CuAssertTrue(tc, !bson_iter_next(&iter));

    BSON_FREE(&bson);
}

// iter_init 后未 bson_iter_next 即调 getter:哨兵 type=BSON_EOD 令类型检查失败,
// 安全返回 0/NULL(不读未初始化的 type/val)
static void test_bson_iter_no_next(CuTest *tc) {
    int32_t err = ERR_OK;
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_double(&bson, "pi", 3.14);
    bson_append_int32(&bson, "n", 42);
    bson_append_end(&bson);
    CuAssertTrue(tc, bson_complete(&bson));

    BSON_ITER_FROM(bson, rd, iter);

    // 未 next:哨兵 EOD 不匹配任何 getter 期望类型,检查失败,不读 val
    CuAssertDblEquals(tc, 0.0, bson_iter_double(&iter, &err), 1e-10);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    err = ERR_OK;
    CuAssertIntEquals(tc, 0, bson_iter_int32(&iter, &err));
    CuAssertIntEquals(tc, ERR_FAILED, err);
    err = ERR_OK;
    CuAssertTrue(tc, NULL == bson_iter_utf8(&iter, &err));
    CuAssertIntEquals(tc, ERR_FAILED, err);

    // next 后正常迭代,哨兵不影响正常路径
    CuAssertTrue(tc, bson_iter_next(&iter));
    CuAssertDblEquals(tc, 3.14, bson_iter_double(&iter, &err), 1e-10);
    CuAssertIntEquals(tc, ERR_OK, err);

    BSON_FREE(&bson);
}

/* =======================================================================
 * 嵌套：DOCUMENT 字段 + ARRAY 字段
 * ======================================================================= */
static void test_bson_nested(CuTest *tc) {
    int32_t err;
    bson_ctx bson;
    bson_init(&bson, NULL, 0);

    // 嵌套文档
    bson_append_document_begain(&bson, "meta");
    bson_append_int32(&bson, "x", 1);
    bson_append_int32(&bson, "y", 2);
    bson_append_end(&bson);

    // 嵌套数组（key 为 "0","1"）
    bson_append_array_begain(&bson, "tags");
    bson_append_utf8(&bson, "0", "alpha");
    bson_append_utf8(&bson, "1", "beta");
    bson_append_end(&bson);

    bson_append_end(&bson);
    CuAssertTrue(tc, bson_complete(&bson));

    BSON_ITER_FROM(bson, rd, iter);

    // meta → DOCUMENT
    CuAssertTrue(tc, bson_iter_next(&iter));
    CuAssertIntEquals(tc, BSON_DOCUMENT, iter.type);
    CuAssertStrEquals(tc, "meta", iter.key);
    size_t dlens = 0;
    char *ddata = bson_iter_document(&iter, &dlens, &err);
    CuAssertIntEquals(tc, ERR_OK, err);

    bson_ctx sub;
    bson_iter sub_iter;
    bson_init(&sub, ddata, dlens);
    bson_iter_init(&sub_iter, &sub);
    CuAssertTrue(tc, bson_iter_next(&sub_iter));
    CuAssertStrEquals(tc, "x", sub_iter.key);
    CuAssertIntEquals(tc, 1, bson_iter_int32(&sub_iter, &err));
    CuAssertTrue(tc, bson_iter_next(&sub_iter));
    CuAssertStrEquals(tc, "y", sub_iter.key);
    CuAssertIntEquals(tc, 2, bson_iter_int32(&sub_iter, &err));
    CuAssertTrue(tc, !bson_iter_next(&sub_iter));

    // tags → ARRAY
    CuAssertTrue(tc, bson_iter_next(&iter));
    CuAssertIntEquals(tc, BSON_ARRAY, iter.type);
    CuAssertStrEquals(tc, "tags", iter.key);
    size_t alens = 0;
    char *adata = bson_iter_array(&iter, &alens, &err);
    CuAssertIntEquals(tc, ERR_OK, err);

    bson_ctx asub;
    bson_iter a_iter;
    bson_init(&asub, adata, alens);
    bson_iter_init(&a_iter, &asub);
    CuAssertTrue(tc, bson_iter_next(&a_iter));
    CuAssertStrEquals(tc, "0", a_iter.key);
    CuAssertStrEquals(tc, "alpha", bson_iter_utf8(&a_iter, &err));
    CuAssertTrue(tc, bson_iter_next(&a_iter));
    CuAssertStrEquals(tc, "1", a_iter.key);
    CuAssertStrEquals(tc, "beta", bson_iter_utf8(&a_iter, &err));
    CuAssertTrue(tc, !bson_iter_next(&a_iter));

    // EOD
    CuAssertTrue(tc, !bson_iter_next(&iter));

    BSON_FREE(&bson);
}

/* =======================================================================
 * bson_iter_find 点分路径查找
 * ======================================================================= */
static void test_bson_find(CuTest *tc) {
    int32_t err;
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_int32(&bson, "a", 1);
    bson_append_document_begain(&bson, "b");
    bson_append_int32(&bson, "c", 42);
    bson_append_end(&bson);
    bson_append_end(&bson);

    bson_iter result;

    // 顶层查找
    BSON_ITER_FROM(bson, rd1, iter1);
    CuAssertTrue(tc, ERR_OK == bson_iter_find(&iter1, "a", &result));
    CuAssertIntEquals(tc, BSON_INT32, result.type);
    CuAssertIntEquals(tc, 1, bson_iter_int32(&result, &err));

    // 点分多级查找
    BSON_ITER_FROM(bson, rd2, iter2);
    CuAssertTrue(tc, ERR_OK == bson_iter_find(&iter2, "b.c", &result));
    CuAssertIntEquals(tc, BSON_INT32, result.type);
    CuAssertIntEquals(tc, 42, bson_iter_int32(&result, &err));

    // 未找到
    BSON_ITER_FROM(bson, rd3, iter3);
    CuAssertTrue(tc, ERR_OK != bson_iter_find(&iter3, "x", &result));

    BSON_FREE(&bson);
}

/* =======================================================================
 * bson_complete / bson_cat
 * ======================================================================= */
static void test_bson_complete_cat(CuTest *tc) {
    // 未完成时 complete=false
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_int32(&bson, "n", 1);
    CuAssertTrue(tc, !bson_complete(&bson));
    bson_append_end(&bson);
    CuAssertTrue(tc, bson_complete(&bson));

    // bson_cat：将另一个已完成文档的字段合并进来
    bson_ctx src;
    bson_init(&src, NULL, 0);
    bson_append_utf8(&src, "tag", "x");
    bson_append_end(&src);

    bson_ctx dst;
    bson_init(&dst, NULL, 0);
    bson_cat(&dst, BSON_DOC(&src));
    bson_append_end(&dst);
    CuAssertTrue(tc, bson_complete(&dst));

    // 验证合并后有 "tag" 字段
    int32_t err;
    bson_iter result;
    BSON_ITER_FROM(dst, rd, iter);
    CuAssertTrue(tc, ERR_OK == bson_iter_find(&iter, "tag", &result));
    CuAssertStrEquals(tc, "x", bson_iter_utf8(&result, &err));

    BSON_FREE(&bson);
    BSON_FREE(&src);
    BSON_FREE(&dst);
}

/* =======================================================================
 * 其他常用类型：regex / jscode / timestamp / minkey / maxkey / jscode_n
 * 这些 setter 与 iterator 配对未独立覆盖
 * ======================================================================= */
static void test_bson_extra_types(CuTest *tc) {
    int32_t err;
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_regex(&bson, "re", "^foo$", "i");
    bson_append_jscode(&bson, "js", "function() {}");
    bson_append_jscode_n(&bson, "jsn", "var x=1;\0extra", 8); // 二进制安全（截到 8 字节）
    bson_append_timestamp(&bson, "ts", 1700000000u, 7u);
    bson_append_minkey(&bson, "mn");
    bson_append_maxkey(&bson, "mx");
    bson_append_utf8_n(&bson, "raw", "abc\0def", 7); // utf8_n 二进制安全
    bson_append_end(&bson);
    CuAssertTrue(tc, bson_complete(&bson));

    BSON_ITER_FROM(bson, rd, iter);

    // regex
    CuAssertTrue(tc, bson_iter_next(&iter));
    CuAssertIntEquals(tc, BSON_REGEX, iter.type);
    CuAssertStrEquals(tc, "re", iter.key);
    char *opts = NULL;
    const char *pat = bson_iter_regex(&iter, &opts, &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertStrEquals(tc, "^foo$", pat);
    CuAssertStrEquals(tc, "i", opts);

    // jscode
    CuAssertTrue(tc, bson_iter_next(&iter));
    CuAssertIntEquals(tc, BSON_JSCODE, iter.type);
    CuAssertStrEquals(tc, "js", iter.key);
    CuAssertStrEquals(tc, "function() {}", bson_iter_jscode(&iter, &err));
    CuAssertIntEquals(tc, ERR_OK, err);

    // jscode_n（二进制安全）
    CuAssertTrue(tc, bson_iter_next(&iter));
    CuAssertIntEquals(tc, BSON_JSCODE, iter.type);
    CuAssertStrEquals(tc, "jsn", iter.key);
    /* 前 8 字节为 "var x=1;\0" 截止 NUL；bson_iter_jscode 返回 cstring 视为到 NUL 截止 */

    // timestamp
    CuAssertTrue(tc, bson_iter_next(&iter));
    CuAssertIntEquals(tc, BSON_TIMESTAMP, iter.type);
    CuAssertStrEquals(tc, "ts", iter.key);
    uint32_t inc = 0;
    uint32_t ts_val = bson_iter_timestamp(&iter, &inc, &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, 1700000000u == ts_val);
    CuAssertTrue(tc, 7u == inc);

    // minkey + maxkey 无具体读取 API，只验证 type
    CuAssertTrue(tc, bson_iter_next(&iter));
    CuAssertIntEquals(tc, BSON_MINKEY, iter.type);
    CuAssertStrEquals(tc, "mn", iter.key);

    CuAssertTrue(tc, bson_iter_next(&iter));
    CuAssertIntEquals(tc, BSON_MAXKEY, iter.type);
    CuAssertStrEquals(tc, "mx", iter.key);

    // utf8_n：iter.lens 应为 8（7 字节 + 末尾 \0）
    CuAssertTrue(tc, bson_iter_next(&iter));
    CuAssertIntEquals(tc, BSON_UTF8, iter.type);
    CuAssertStrEquals(tc, "raw", iter.key);

    CuAssertTrue(tc, !bson_iter_next(&iter));
    BSON_FREE(&bson);
}

/* =======================================================================
 * bson_check_depth —— DoS 防护（消费者递归解码前的嵌套深度预检）
 * BSON_MAX_DEPTH = 18，depth > 18 拒绝
 * ======================================================================= */
static void test_bson_check_depth(CuTest *tc) {
    /* 平坦文档：深度 0，应允许 */
    bson_ctx flat;
    bson_init(&flat, NULL, 0);
    bson_append_int32(&flat, "a", 1);
    bson_append_int32(&flat, "b", 2);
    bson_append_end(&flat);
    CuAssertIntEquals(tc, ERR_OK, bson_check_depth(BSON_DOC(&flat), BSON_DOC_LENS(&flat)));
    BSON_FREE(&flat);

    /* 接近上限：17 层嵌套（含根共 18 层）应允许 */
    bson_ctx near_max;
    bson_init(&near_max, NULL, 0);
    int i;
    for (i = 0; i < 17; i++) {
        bson_append_document_begain(&near_max, "n");
    }
    bson_append_int32(&near_max, "leaf", 1);
    for (i = 0; i < 17; i++) {
        bson_append_end(&near_max);
    }
    bson_append_end(&near_max);
    CuAssertIntEquals(tc, ERR_OK,
        bson_check_depth(BSON_DOC(&near_max), BSON_DOC_LENS(&near_max)));
    BSON_FREE(&near_max);

    /* 手工构造 20 层 BSON 触发 depth > BSON_MAX_DEPTH 的拒绝路径
     * 每层格式：[len:4 LE][0x03 type][key "x"][\0][子文档原始字节][\0 EOD]
     * 叶子：[5,0,0,0, 0]，共 5 字节 */
    char buf[20][1024];
    size_t blens[20];
    buf[0][0] = 5; buf[0][1] = 0; buf[0][2] = 0; buf[0][3] = 0; buf[0][4] = 0;
    blens[0] = 5;
    for (i = 1; i < 20; i++) {
        /* total = 4(len) + 1(type) + 2("x"+\0) + lens[i-1] + 1(EOD) */
        size_t total = 4 + 1 + 2 + blens[i-1] + 1;
        buf[i][0] = (char)(total & 0xff);
        buf[i][1] = (char)((total >> 8) & 0xff);
        buf[i][2] = (char)((total >> 16) & 0xff);
        buf[i][3] = (char)((total >> 24) & 0xff);
        buf[i][4] = 0x03;
        buf[i][5] = 'x';
        buf[i][6] = 0;
        memcpy(buf[i] + 7, buf[i-1], blens[i-1]);
        buf[i][7 + blens[i-1]] = 0;
        blens[i] = total;
    }
    /* buf[19] 最外层从 depth=0 起递归 19 次进入叶子时 depth=19 > 18 → 拒绝 */
    CuAssertIntEquals(tc, ERR_FAILED, bson_check_depth(buf[19], blens[19]));
}

/* =======================================================================
 * bson_tostring / bson_tostring2 —— 调试串化（验证非空 + 字段名出现）
 * ======================================================================= */
static void test_bson_tostring(CuTest *tc) {
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_int32(&bson, "age", 42);
    bson_append_utf8(&bson, "name", "alice");
    bson_append_bool(&bson, "ok", 1);
    bson_append_null(&bson, "nil");
    bson_append_double(&bson, "pi", 3.14);
    /* 嵌套文档：触发递归路径 */
    bson_append_document_begain(&bson, "meta");
    bson_append_int32(&bson, "x", 1);
    bson_append_end(&bson);
    /* 嵌套数组 */
    bson_append_array_begain(&bson, "tags");
    bson_append_utf8(&bson, "0", "red");
    bson_append_end(&bson);
    bson_append_int64(&bson, "big", 1234567890123LL);
    bson_append_end(&bson);

    char *s = bson_tostring(&bson);
    CuAssertPtrNotNull(tc, s);
    CuAssertTrue(tc, NULL != strstr(s, "age"));
    CuAssertTrue(tc, NULL != strstr(s, "name"));
    CuAssertTrue(tc, NULL != strstr(s, "alice"));
    CuAssertTrue(tc, NULL != strstr(s, "ok"));
    CuAssertTrue(tc, NULL != strstr(s, "true"));
    CuAssertTrue(tc, NULL != strstr(s, "meta"));
    CuAssertTrue(tc, NULL != strstr(s, "tags"));
    CuAssertTrue(tc, NULL != strstr(s, "red"));
    FREE(s);

    /* bson_tostring2 接受原始 BSON 字节 */
    char *s2 = bson_tostring2(BSON_DOC(&bson), BSON_DOC_LENS(&bson));
    CuAssertPtrNotNull(tc, s2);
    CuAssertTrue(tc, NULL != strstr(s2, "alice"));
    FREE(s2);

    BSON_FREE(&bson);
}

/* =======================================================================
 * bson_empty / bson_oid / bson_type_tostring / bson_subtype_tostring
 * ======================================================================= */
static void test_bson_misc(CuTest *tc) {
    size_t elen;
    const char *empty = bson_empty(&elen);
    CuAssertPtrNotNull(tc, empty);
    CuAssertTrue(tc, 5 == elen);                   /* 4 字节长度 + 1 EOD */
    CuAssertIntEquals(tc, 5, (uint8_t)empty[0]);   /* len = 5 */
    CuAssertIntEquals(tc, 0, (uint8_t)empty[4]);   /* EOD */

    /* bson_oid：连续生成应递增；非全零 */
    char a[BSON_OID_LENS], b[BSON_OID_LENS];
    bson_oid(a);
    bson_oid(b);
    int all_zero = 1;
    for (int i = 0; i < BSON_OID_LENS; i++) {
        if (a[i] != 0) { all_zero = 0; break; }
    }
    CuAssertTrue(tc, !all_zero);
    /* 序列号字段（最后 3 字节）应不同（或时间字段不同） */
    CuAssertTrue(tc, 0 != memcmp(a, b, BSON_OID_LENS));

    /* bson_type_tostring 覆盖主要类型分支 */
    CuAssertStrEquals(tc, "double",   bson_type_tostring(BSON_DOUBLE));
    CuAssertStrEquals(tc, "string",   bson_type_tostring(BSON_UTF8));
    CuAssertStrEquals(tc, "object",   bson_type_tostring(BSON_DOCUMENT));
    CuAssertStrEquals(tc, "array",    bson_type_tostring(BSON_ARRAY));
    CuAssertStrEquals(tc, "bool",     bson_type_tostring(BSON_BOOL));
    CuAssertStrEquals(tc, "null",     bson_type_tostring(BSON_NULL));
    CuAssertStrEquals(tc, "int",      bson_type_tostring(BSON_INT32));
    CuAssertStrEquals(tc, "long",     bson_type_tostring(BSON_INT64));

    /* bson_subtype_tostring */
    CuAssertStrEquals(tc, "uuid",     bson_subtype_tostring(BSON_SUBTYPE_UUID));
    CuAssertStrEquals(tc, "md5",      bson_subtype_tostring(BSON_SUBTYPE_MD5));
}

// 负/零/过小长度字段：doclens 归 0，bson_iter_next 立即返回 0，不无限遍历
static void test_bson_iter_neg_lens(CuTest *tc) {
    bson_ctx reader;
    bson_iter iter;

    // -1（0xFFFFFFFF）→ (size_t)(-1) = SIZE_MAX；修复前无限遍历
    char neg[5] = {(char)0xFF, (char)0xFF, (char)0xFF, (char)0xFF, 0x00};
    bson_init(&reader, neg, 5);
    bson_iter_init(&iter, &reader);
    CuAssertTrue(tc, !bson_iter_next(&iter));

    // 长度 = 3（< 5，低于合法最小值）
    char smallbuf[5] = {0x03, 0x00, 0x00, 0x00, 0x00};
    bson_init(&reader, smallbuf, 5);
    bson_iter_init(&iter, &reader);
    CuAssertTrue(tc, !bson_iter_next(&iter));

    // 长度声称 100，实际缓冲区只有 5 字节
    char oversize[5] = {0x64, 0x00, 0x00, 0x00, 0x00};
    bson_init(&reader, oversize, 5);
    bson_iter_init(&iter, &reader);
    CuAssertTrue(tc, !bson_iter_next(&iter));
}

// 深度临界：18 层（BSON_MAX_DEPTH=18）应允许，19 层拒绝
// 现有 test_bson_check_depth 已覆盖 17/20 层，本测试精确卡边界
static void test_bson_check_depth_boundary(CuTest *tc) {
    // 复用 test_bson_check_depth 同样的手工 wire 构造方式
    char buf[20][1024];
    size_t blens[20];
    // 叶子（深度 0）: 空文档 5 字节
    buf[0][0] = 5; buf[0][1] = 0; buf[0][2] = 0; buf[0][3] = 0; buf[0][4] = 0;
    blens[0] = 5;
    int i;
    for (i = 1; i < 20; i++) {
        size_t total = 4 + 1 + 2 + blens[i-1] + 1;
        buf[i][0] = (char)(total & 0xff);
        buf[i][1] = (char)((total >> 8) & 0xff);
        buf[i][2] = (char)((total >> 16) & 0xff);
        buf[i][3] = (char)((total >> 24) & 0xff);
        buf[i][4] = 0x03;
        buf[i][5] = 'x';
        buf[i][6] = 0;
        memcpy(buf[i] + 7, buf[i-1], blens[i-1]);
        buf[i][7 + blens[i-1]] = 0;
        blens[i] = total;
    }
    // buf[18] 包含 19 个嵌套层级（含叶子），递归深度走到 18 = BSON_MAX_DEPTH，允许
    CuAssertIntEquals(tc, ERR_OK, bson_check_depth(buf[18], blens[18]));
    // buf[19] 深度走到 19，超出 BSON_MAX_DEPTH 拒绝
    CuAssertIntEquals(tc, ERR_FAILED, bson_check_depth(buf[19], blens[19]));
}

// BSN-2：doc->size > doclens 时，内层字段长度检查须以 doclens 为界而非 doc->size
// 构造：声明 doc size=15，buffer size=20；UTF8 字段 lens=5 → lens+1=6 > doclens-offset=4，应拒绝
static void test_bson_iter_field_exceeds_doclens(CuTest *tc) {
    bson_ctx reader;
    bson_iter iter;
    // 手工 wire 格式：4 字节声明 doc 大小(=15) + type(0x02) + key"a\0" + int32 lens(=5) + 9 字节 padding
    char buf[20];
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x0F; buf[1] = 0x00; buf[2] = 0x00; buf[3] = 0x00;//声明 doc size = 15
    buf[4] = 0x02;//BSON_UTF8
    buf[5] = 'a'; buf[6] = 0x00;//key "a"
    buf[7] = 0x05; buf[8] = 0x00; buf[9] = 0x00; buf[10] = 0x00;//string lens = 5
    // buf[11..19]：padding（共 9 字节，使 doc->size=20 > doclens=15）
    // 检查：lens+1=6 > doclens(15)-offset(11)=4 → 拒绝
    bson_init(&reader, buf, sizeof(buf));//doc->size = 20
    bson_iter_init(&iter, &reader);//doclens = 15
    CuAssertTrue(tc, !bson_iter_next(&iter));
}

// 补全 bson_tostring 未覆盖子类型分支：
//   regex / jscode / binary / oid / timestamp / date / minkey / maxkey
// 对应 lib/serial/bson.c _bson_dump switch 各 case
static void test_bson_tostring_subtypes(CuTest *tc) {
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_regex(&bson, "re", "abc.*", "im");
    bson_append_jscode(&bson, "code", "function(){return 1;}");
    char bin[] = { 0x01, 0x02, 0x03, 0x04 };
    bson_append_binary(&bson, "bin", BSON_SUBTYPE_BINARY, bin, sizeof(bin));
    char oid[BSON_OID_LENS];
    bson_oid(oid);
    bson_append_oid(&bson, "oid", oid);
    bson_append_timestamp(&bson, "ts", 0x12345678, 42);
    bson_append_date(&bson, "date", 1700000000000LL);
    bson_append_minkey(&bson, "min");
    bson_append_maxkey(&bson, "max");
    bson_append_end(&bson);

    char *s = bson_tostring(&bson);
    CuAssertPtrNotNull(tc, s);
    // 字段名一定都出现
    CuAssertTrue(tc, NULL != strstr(s, "re"));
    CuAssertTrue(tc, NULL != strstr(s, "code"));
    CuAssertTrue(tc, NULL != strstr(s, "bin"));
    CuAssertTrue(tc, NULL != strstr(s, "oid"));
    CuAssertTrue(tc, NULL != strstr(s, "ts"));
    CuAssertTrue(tc, NULL != strstr(s, "date"));
    CuAssertTrue(tc, NULL != strstr(s, "min"));
    CuAssertTrue(tc, NULL != strstr(s, "max"));
    // 类型名（bson_type_tostring 输出）：regex/javascript/binData/objectId/timestamp/date/minKey/maxKey
    CuAssertTrue(tc, NULL != strstr(s, "regex"));
    CuAssertTrue(tc, NULL != strstr(s, "javascript"));
    CuAssertTrue(tc, NULL != strstr(s, "binData"));
    CuAssertTrue(tc, NULL != strstr(s, "objectId"));
    CuAssertTrue(tc, NULL != strstr(s, "timestamp"));
    CuAssertTrue(tc, NULL != strstr(s, "minKey"));
    CuAssertTrue(tc, NULL != strstr(s, "maxKey"));
    // jscode 内容
    CuAssertTrue(tc, NULL != strstr(s, "function()"));
    // regex pattern 与 options 应出现
    CuAssertTrue(tc, NULL != strstr(s, "abc.*"));
    CuAssertTrue(tc, NULL != strstr(s, "im"));
    // binary 转 hex：0x01020304 = "01020304"
    CuAssertTrue(tc, NULL != strstr(s, "01020304"));
    FREE(s);

    BSON_FREE(&bson);
}

// 点分路径 find 后 result.doc 须指向自身 nested_doc（非栈变量），
// find 后调 bson_iter_next 能继续迭代子文档剩余字段
static void test_bson_find_dotted_iter_continue(CuTest *tc) {
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_document_begain(&bson, "cursor");
    bson_append_int64(&bson, "id", 12345LL);
    bson_append_utf8(&bson, "ns", "test.col");
    bson_append_end(&bson);
    bson_append_end(&bson);
    CuAssertTrue(tc, bson_complete(&bson));

    BSON_ITER_FROM(bson, rd, iter);
    bson_iter result;
    int32_t err;
    CuAssertIntEquals(tc, ERR_OK, bson_iter_find(&iter, "cursor.id", &result));
    CuAssertIntEquals(tc, BSON_INT64, result.type);
    CuAssertTrue(tc, 12345LL == bson_iter_int64(&result, &err));
    // find 后 result.doc 指向 result.nested_doc（自身字段），不再悬空；
    // bson_iter_next 应能取到子文档下一字段 "ns"
    CuAssertTrue(tc, bson_iter_next(&result));
    CuAssertIntEquals(tc, BSON_UTF8, result.type);
    CuAssertStrEquals(tc, "ns", result.key);
    CuAssertStrEquals(tc, "test.col", bson_iter_utf8(&result, &err));
    CuAssertIntEquals(tc, ERR_OK, err);

    BSON_FREE(&bson);
}

void test_bson(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_bson_primitives);
    SUITE_ADD_TEST(suite, test_bson_iter_no_next);
    SUITE_ADD_TEST(suite, test_bson_nested);
    SUITE_ADD_TEST(suite, test_bson_find);
    SUITE_ADD_TEST(suite, test_bson_complete_cat);
    SUITE_ADD_TEST(suite, test_bson_extra_types);
    SUITE_ADD_TEST(suite, test_bson_check_depth);
    SUITE_ADD_TEST(suite, test_bson_iter_neg_lens);
    SUITE_ADD_TEST(suite, test_bson_iter_field_exceeds_doclens);
    SUITE_ADD_TEST(suite, test_bson_check_depth_boundary);
    SUITE_ADD_TEST(suite, test_bson_tostring);
    SUITE_ADD_TEST(suite, test_bson_tostring_subtypes);
    SUITE_ADD_TEST(suite, test_bson_misc);
    SUITE_ADD_TEST(suite, test_bson_find_dotted_iter_continue);
}
