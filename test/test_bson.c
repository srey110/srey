
#include "test_bson.h"
#include "lib.h"
#include <string.h>

// 用写完的 bson_ctx 的原始字节创建只读迭代器（写完后 offset 在末尾，需从 data[0] 重新读）
#define BSON_ITER_FROM(bson, reader, iter) \
    bson_ctx reader; \
    bson_iter iter; \
    bson_init(&reader, BSON_DOC(&bson), BSON_DOC_LENS(&bson)); \
    bson_iter_init(&iter, &reader)
// key 长度用例覆盖 0..BSON_T_KEYMAX
#define BSON_T_KEYMAX 40

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

// bson_iter_next 解析失败后迭代器必须回到哨兵态。否则 type 停在畸形元素的类型而 val 为 NULL,
// _bson_iter_check 只认 type 会放行,getter 随即解引用 NULL。
// 两份畸形样本:截断的 DOUBLE(val 保持 NULL)、未 NUL 结尾的 UTF8(val 被显式置 NULL 但 lens>0)
static void test_bson_iter_malformed_poison(CuTest *tc) {
    char trunc[] = { 0x0A, 0x00, 0x00, 0x00, 0x01, 'd', 0x00, 0x01, 0x02, 0x00 };
    char badutf8[] = { 0x10, 0x00, 0x00, 0x00, 0x02, 's', 0x00,
                       0x04, 0x00, 0x00, 0x00, 'a', 'b', 'c', 'd', 0x00 };
    bson_ctx bson;
    bson_iter iter, result;
    int32_t err;

    bson_init(&bson, trunc, sizeof(trunc));
    bson_iter_init(&iter, &bson);
    CuAssertTrue(tc, !bson_iter_next(&iter));
    CuAssertIntEquals(tc, BSON_EOD, iter.type);
    CuAssertTrue(tc, NULL == iter.val);
    err = ERR_OK;
    CuAssertDblEquals(tc, 0.0, bson_iter_double(&iter, &err), 1e-10);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    err = ERR_OK;
    CuAssertIntEquals(tc, 0, bson_iter_bool(&iter, &err));
    CuAssertIntEquals(tc, ERR_FAILED, err);
    err = ERR_OK;
    CuAssertTrue(tc, NULL == bson_iter_oid(&iter, &err));
    CuAssertIntEquals(tc, ERR_FAILED, err);

    bson_init(&bson, badutf8, sizeof(badutf8));
    bson_iter_init(&iter, &bson);
    CuAssertTrue(tc, !bson_iter_next(&iter));
    CuAssertIntEquals(tc, BSON_EOD, iter.type);
    CuAssertTrue(tc, 0 == iter.lens);
    err = ERR_OK;
    CuAssertTrue(tc, NULL == bson_iter_utf8(&iter, &err));
    CuAssertIntEquals(tc, ERR_FAILED, err);

    bson_init(&bson, trunc, sizeof(trunc));
    bson_iter_init(&iter, &bson);
    CuAssertIntEquals(tc, ERR_FAILED, bson_iter_find(&iter, "nope", &result));
    CuAssertIntEquals(tc, BSON_EOD, iter.type);
    CuAssertTrue(tc, 0 != bson_iter_error(&iter));
    err = ERR_OK;
    CuAssertDblEquals(tc, 0.0, bson_iter_double(&iter, &err), 1e-10);
    CuAssertIntEquals(tc, ERR_FAILED, err);
}

// bson_iter_error 区分 bson_iter_next 返回 0 的两种含义:读到 EOD 正常结束 vs 中途读不下去。
// 毒化后两者的 type 都是 BSON_EOD,单看 type 分不出来。
// "读不下去"含两类成因:文档结构非法,以及类型字节本实现不认识——BSON 元素不自带长度,
// 认不出类型就算不出边界,后面一律读不到,故共用一个标志位
static void test_bson_iter_error_flag(CuTest *tc) {
    char trunc[] = { 0x0A, 0x00, 0x00, 0x00, 0x01, 'd', 0x00, 0x01, 0x02, 0x00 };
    // 结构完好的文档:int32 a=1,后跟一个废弃的 symbol(0x0E) 字段 s="x"
    char sym[] = {
        0x15, 0x00, 0x00, 0x00,
        0x10, 'a', 0x00, 0x01, 0x00, 0x00, 0x00,
        0x0E, 's', 0x00, 0x02, 0x00, 0x00, 0x00, 'x', 0x00,
        0x00
    };
    bson_ctx bson, rd;
    bson_iter iter;

    // 合法文档整轮遍历完:err 保持 0
    bson_init(&bson, NULL, 0);
    bson_append_int32(&bson, "a", 1);
    bson_append_utf8(&bson, "b", "x");
    bson_append_end(&bson);
    bson_init(&rd, BSON_DOC(&bson), BSON_DOC_LENS(&bson));
    bson_iter_init(&iter, &rd);
    CuAssertTrue(tc, 0 == bson_iter_error(&iter));
    while (bson_iter_next(&iter)) {
        CuAssertTrue(tc, 0 == bson_iter_error(&iter));
    }
    CuAssertIntEquals(tc, BSON_EOD, iter.type);
    CuAssertTrue(tc, 0 == bson_iter_error(&iter));
    // reset 后可重新干净遍历
    bson_iter_reset(&iter);
    CuAssertTrue(tc, 0 == bson_iter_error(&iter));
    CuAssertTrue(tc, bson_iter_next(&iter));
    BSON_FREE(&bson);

    // 畸形文档:next 同样返 0、type 同样是 BSON_EOD,但 err 置位
    bson_init(&rd, trunc, sizeof(trunc));
    bson_iter_init(&iter, &rd);
    CuAssertTrue(tc, 0 == bson_iter_error(&iter));
    CuAssertTrue(tc, !bson_iter_next(&iter));
    CuAssertIntEquals(tc, BSON_EOD, iter.type);
    CuAssertTrue(tc, 0 != bson_iter_error(&iter));

    // 长度字段本身非法(声明 10 但只给 9 字节)
    bson_init(&rd, trunc, sizeof(trunc) - 1);
    bson_iter_init(&iter, &rd);
    CuAssertTrue(tc, 0 != bson_iter_error(&iter));

    // 结构完好、只是撞上不认识的类型字节:它之前的元素照常读到,到它这里 next 返 0 且
    // err 置位——与畸形文档共用同一个标志位,所以 bson.decode 对两者一样报错
    bson_init(&rd, sym, sizeof(sym));
    bson_iter_init(&iter, &rd);
    CuAssertTrue(tc, bson_iter_next(&iter));
    CuAssertIntEquals(tc, BSON_INT32, iter.type);
    CuAssertTrue(tc, 0 == bson_iter_error(&iter));
    CuAssertTrue(tc, !bson_iter_next(&iter));
    CuAssertIntEquals(tc, BSON_EOD, iter.type);
    CuAssertTrue(tc, 0 != bson_iter_error(&iter));
}

// binary 的 subtype 是 wire 上的无符号字节,读取时必须窄化为 (uint8_t)。
// 直接把 binary_get_int8 的 int8_t 赋给枚举,0x80-0xFF 的用户自定义子类型会变成
// 4294967168 之类的垃圾值(枚举底层类型无符号),既匹配不上 BSON_SUBTYPE_USER 也无法往返。
// 既有用例只用 BSON_SUBTYPE_BINARY(0x00),整个高半区从未被覆盖
static void test_bson_binary_subtype_high(CuTest *tc) {
    const uint8_t subs[] = { 0x00, 0x04, 0x7F, 0x80, 0xFF };
    char payload[] = { 'a', 'b', 'c' };
    bson_ctx bson, rd;
    bson_iter iter;
    bson_subtype got;
    size_t i, blens;
    int32_t err;
    char *bdata;

    for (i = 0; i < sizeof(subs) / sizeof(subs[0]); i++) {
        bson_init(&bson, NULL, 0);
        bson_append_binary(&bson, "b", (bson_subtype)subs[i], payload, sizeof(payload));
        bson_append_end(&bson);

        bson_init(&rd, BSON_DOC(&bson), BSON_DOC_LENS(&bson));
        bson_iter_init(&iter, &rd);
        CuAssertTrue(tc, bson_iter_next(&iter));
        CuAssertIntEquals(tc, BSON_BINARY, iter.type);
        got = BSON_SUBTYPE_BINARY;
        blens = 0;
        err = ERR_FAILED;
        bdata = bson_iter_binary(&iter, &got, &blens, &err);
        CuAssertIntEquals(tc, ERR_OK, err);
        CuAssertTrue(tc, (bson_subtype)subs[i] == got);
        CuAssertTrue(tc, sizeof(payload) == blens);
        CuAssertTrue(tc, 0 == memcmp(payload, bdata, blens));
        BSON_FREE(&bson);
    }

    bson_init(&bson, NULL, 0);
    bson_append_binary(&bson, "u", BSON_SUBTYPE_USER, payload, sizeof(payload));
    bson_append_end(&bson);
    bson_init(&rd, BSON_DOC(&bson), BSON_DOC_LENS(&bson));
    bson_iter_init(&iter, &rd);
    CuAssertTrue(tc, bson_iter_next(&iter));
    got = BSON_SUBTYPE_BINARY;
    (void)bson_iter_binary(&iter, &got, &blens, &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, BSON_SUBTYPE_USER == got);
    CuAssertStrEquals(tc, "user", bson_subtype_tostring(got));
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

// bson_iter_find 全程走自己栈上的 bson_ctx，末层未命中时 result 不能带着那个已死的栈地址返回
static void test_bson_iter_find_deep_miss(CuTest *tc) {
    int32_t err;
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_document_begain(&bson, "a");
    bson_append_document_begain(&bson, "b");
    bson_append_int32(&bson, "c", 42);
    bson_append_end(&bson);
    bson_append_end(&bson);
    bson_append_end(&bson);

    bson_iter result;

    // 末层缺失。每个子用例前都重新 ZERO：失败路径压根不写 result，不重置的话后面两条
    // 分不清"没被写"和"被上一条清空过"，等于白跑
    ZERO(&result, sizeof(result));
    BSON_ITER_FROM(bson, rd1, iter1);
    CuAssertTrue(tc, ERR_OK != bson_iter_find(&iter1, "a.b.x", &result));
    CuAssertPtrEquals(tc, NULL, result.doc);
    CuAssertIntEquals(tc, BSON_EOD, result.type);

    // 中间层类型不对（a.b.c 是 int32，还想往下钻一层）
    ZERO(&result, sizeof(result));
    BSON_ITER_FROM(bson, rd2, iter2);
    CuAssertTrue(tc, ERR_OK != bson_iter_find(&iter2, "a.b.c.d", &result));
    CuAssertPtrEquals(tc, NULL, result.doc);

    // 成功路径照旧：doc 指向 result 自己的 nested_doc
    ZERO(&result, sizeof(result));
    BSON_ITER_FROM(bson, rd3, iter3);
    CuAssertTrue(tc, ERR_OK == bson_iter_find(&iter3, "a.b.c", &result));
    CuAssertTrue(tc, &result.nested_doc == result.doc);
    CuAssertIntEquals(tc, BSON_INT32, result.type);
    CuAssertIntEquals(tc, 42, bson_iter_int32(&result, &err));
    CuAssertIntEquals(tc, ERR_OK, err);

    BSON_FREE(&bson);
}

// find 未命中不能毁掉 iter 的当前元素与文档偏移:扫描全程走独立游标,不碰调用方的 doc
static void test_bson_iter_find_miss_keeps_iter(CuTest *tc) {
    int32_t err;
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_int32(&bson, "a", 11);
    bson_append_int32(&bson, "b", 22);
    bson_append_end(&bson);

    bson_iter result;
    BSON_ITER_FROM(bson, rd, iter);
    CuAssertTrue(tc, 0 != bson_iter_next(&iter));// 停在 "a"
    CuAssertIntEquals(tc, BSON_INT32, iter.type);

    ZERO(&result, sizeof(result));
    CuAssertTrue(tc, ERR_OK != bson_iter_find(&iter, "zz", &result));
    // 当前元素原封不动：type / key / 取值都还是 "a"
    CuAssertIntEquals(tc, BSON_INT32, iter.type);
    CuAssertIntEquals(tc, 1, (int32_t)iter.keylens);
    CuAssertTrue(tc, 'a' == iter.key[0]);
    CuAssertIntEquals(tc, 11, bson_iter_int32(&iter, &err));
    CuAssertIntEquals(tc, ERR_OK, err);
    // 偏移也还原了，接着 next 拿到的是 "b" 而不是重吐 "a"
    CuAssertTrue(tc, 0 != bson_iter_next(&iter));
    CuAssertIntEquals(tc, 22, bson_iter_int32(&iter, &err));

    BSON_FREE(&bson);
}
static void test_bson_iter_find_self_alias(CuTest *tc) {
    int32_t err;
    char pad[256];
    memset(pad, 'p', sizeof(pad) - 1);
    pad[sizeof(pad) - 1] = '\0';

    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_utf8(&bson, "pad", pad);
    bson_append_document_begain(&bson, "a");
    bson_append_int32(&bson, "b", 42);
    bson_append_end(&bson);
    bson_append_int32(&bson, "z", 7);
    bson_append_int32(&bson, "w", 9);
    bson_append_end(&bson);

    // 点分路径:先吃掉 pad 把偏移推到子文档长度之上,再自别名查找
    BSON_ITER_FROM(bson, rd1, iter1);
    CuAssertTrue(tc, bson_iter_next(&iter1));
    CuAssertTrue(tc, ERR_OK == bson_iter_find(&iter1, "a.b", &iter1));
    CuAssertIntEquals(tc, BSON_INT32, iter1.type);
    CuAssertIntEquals(tc, 42, bson_iter_int32(&iter1, &err));
    CuAssertIntEquals(tc, ERR_OK, err);

    // 无点号路径:result 绑在原文档那一层,继续 next 应吐出同级的下一个字段
    BSON_ITER_FROM(bson, rd2, iter2);
    CuAssertTrue(tc, ERR_OK == bson_iter_find(&iter2, "a", &iter2));
    CuAssertIntEquals(tc, BSON_DOCUMENT, iter2.type);
    CuAssertTrue(tc, bson_iter_next(&iter2));
    CuAssertIntEquals(tc, BSON_INT32, iter2.type);
    CuAssertIntEquals(tc, 7, bson_iter_int32(&iter2, &err));
    CuAssertIntEquals(tc, ERR_OK, err);

    // 链式原地收窄:第一次 find 后 iter 已绑到 a 那一层,第二次 find 在该层内定位 z,
    // 继续 next 要吐出同级的下一个字段
    BSON_ITER_FROM(bson, rd3, iter3);
    CuAssertTrue(tc, ERR_OK == bson_iter_find(&iter3, "a", &iter3));
    CuAssertTrue(tc, ERR_OK == bson_iter_find(&iter3, "z", &iter3));
    CuAssertIntEquals(tc, BSON_INT32, iter3.type);
    CuAssertIntEquals(tc, 7, bson_iter_int32(&iter3, &err));
    CuAssertTrue(tc, bson_iter_next(&iter3));
    CuAssertIntEquals(tc, BSON_INT32, iter3.type);
    CuAssertIntEquals(tc, 9, bson_iter_int32(&iter3, &err));
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, !bson_iter_next(&iter3));

    // 链式原地收窄 + 点分叠加:先吃掉 pad 让偏移(约 270)超过子文档 a 的长度,
    // 再点分自别名。修复前这里是先装 nested_doc 后还原,binary_offset 断言直接 abort
    BSON_ITER_FROM(bson, rd4, iter4);
    CuAssertTrue(tc, ERR_OK == bson_iter_find(&iter4, "pad", &iter4));
    CuAssertIntEquals(tc, BSON_UTF8, iter4.type);
    CuAssertTrue(tc, ERR_OK == bson_iter_find(&iter4, "a.b", &iter4));
    CuAssertIntEquals(tc, BSON_INT32, iter4.type);
    CuAssertIntEquals(tc, 42, bson_iter_int32(&iter4, &err));
    CuAssertIntEquals(tc, ERR_OK, err);

    BSON_FREE(&bson);
}

// bson_cat 的源指向目标自身时，binary_set_binary 里的 REALLOC 会把源搬走。
// Lua 侧 b:cat(b:data()) 就是这条路径。断言"追加进去的字节等于原文档去掉头和 EOD"，
// 产物本身不是合法 bson，这里只守 UAF 不守结构
static void test_bson_cat_self_alias(CuTest *tc) {
    char big[220];
    char snap[512];
    memset(big, 'v', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_utf8(&bson, "k", big);
    bson_append_end(&bson);

    size_t doclens = BSON_DOC_LENS(&bson);
    CuAssertTrue(tc, doclens > 0 && doclens <= sizeof(snap));
    memcpy(snap, BSON_DOC(&bson), doclens);

    CuAssertTrue(tc, ERR_OK == bson_cat(&bson, BSON_DOC(&bson), doclens));
    CuAssertTrue(tc, doclens + (doclens - 5) == BSON_DOC_LENS(&bson));
    CuAssertTrue(tc, 0 == memcmp(BSON_DOC(&bson), snap, doclens));
    CuAssertTrue(tc, 0 == memcmp(BSON_DOC(&bson) + doclens, snap + 4, doclens - 5));

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
    CuAssertIntEquals(tc, ERR_OK, bson_cat(&dst, BSON_DOC(&src), BSON_DOC_LENS(&src)));
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

// bson_cat 的各条早退与正常路径:NULL / lens==0、缓冲不足 5 字节、头声明长度超出缓冲、
// 末字节不是 EOD、空文档(声明 5),畸形的整篇丢弃返 ERR_FAILED,空文档 no-op 返 ERR_OK。
// 空文档与畸形必须分属不同分支——写空的 bson_ctx 与 bson_empty() 恰好都是 5 字节,
// 两者若并进同一条判断,正常入参也会被报成失败。
// 字节数上限不在这一层(见末尾那条 70000 的断言),那取决于承载协议
static void test_bson_cat_bounds(CuTest *tc) {
    size_t before;
    char *big;
    int32_t err;
    bson_iter result;
    bson_ctx dst, empty, src;

    bson_init(&empty, NULL, 0);
    bson_append_end(&empty);
    CuAssertTrue(tc, bson_complete(&empty));
    CuAssertTrue(tc, 5 == BSON_DOC_LENS(&empty));

    bson_init(&dst, NULL, 0);
    before = BSON_DOC_LENS(&dst);

    CuAssertIntEquals(tc, ERR_OK, bson_cat(&dst, NULL, 0));
    CuAssertTrue(tc, before == BSON_DOC_LENS(&dst));

    CuAssertIntEquals(tc, ERR_OK, bson_cat(&dst, BSON_DOC(&empty), BSON_DOC_LENS(&empty)));
    CuAssertTrue(tc, before == BSON_DOC_LENS(&dst));

    MALLOC(big, 70000);
    ZERO(big, 70000);
    big[0] = (char)0x70;
    big[1] = (char)0x11;
    big[2] = (char)0x01;
    // 结构合法的大文档照收:字节数上限不归 bson_cat 管(取决于承载协议,如 mongo 的
    // MONGO_MAX_PACK_LENS)。换个 ctx 拼,免得把 dst 撑大影响后面的 before 比对
    bson_ctx bigdst;
    bson_init(&bigdst, NULL, 0);
    CuAssertIntEquals(tc, ERR_OK, bson_cat(&bigdst, big, 70000));
    // before 是 bson_init 预留的 4 字节长度头;拼入量是 70000 去掉头和 EOD
    CuAssertTrue(tc, BSON_DOC_LENS(&bigdst) == before + 70000 - 5);
    BSON_FREE(&bigdst);
    // 缓冲够大但只报 5 字节:声明长度超出传入长度,拒收。旧实现只信头里的 70000,
    // 会照着它从 big 之后一路读出去
    CuAssertIntEquals(tc, ERR_FAILED, bson_cat(&dst, big, 5));
    CuAssertTrue(tc, before == BSON_DOC_LENS(&dst));
    FREE(big);

    // 非 NULL 但长度为 0:当 no-op,不去碰那 4 字节头
    CuAssertIntEquals(tc, ERR_OK, bson_cat(&dst, BSON_DOC(&empty), 0));
    CuAssertTrue(tc, before == BSON_DOC_LENS(&dst));
    // 不足最小文档(4 字节长度头 + EOD)
    CuAssertIntEquals(tc, ERR_FAILED, bson_cat(&dst, BSON_DOC(&empty), 4));
    CuAssertTrue(tc, before == BSON_DOC_LENS(&dst));

    // 声明长度 0~4:结构上不可能的文档,不能当 no-op 静默吞掉
    char tiny[8];
    ZERO(tiny, sizeof(tiny));
    CuAssertIntEquals(tc, ERR_FAILED, bson_cat(&dst, tiny, sizeof(tiny)));// 声明 0
    tiny[0] = 4;
    CuAssertIntEquals(tc, ERR_FAILED, bson_cat(&dst, tiny, sizeof(tiny)));// 声明 4
    CuAssertTrue(tc, before == BSON_DOC_LENS(&dst));

    // 最后一字节不是 EOD:不是 BSON,不能按 doclens-5 把中间那段原样拼进来。
    // 前 4 字节小端恰为自身长度 8,四道旧检查全过
    tiny[0] = 8;
    tiny[7] = 0x41;
    CuAssertIntEquals(tc, ERR_FAILED, bson_cat(&dst, tiny, sizeof(tiny)));
    CuAssertTrue(tc, before == BSON_DOC_LENS(&dst));

    bson_init(&src, NULL, 0);
    bson_append_int32(&src, "n", 7);
    bson_append_end(&src);
    CuAssertIntEquals(tc, ERR_OK, bson_cat(&dst, BSON_DOC(&src), BSON_DOC_LENS(&src)));
    CuAssertTrue(tc, before < BSON_DOC_LENS(&dst));
    bson_append_end(&dst);
    CuAssertTrue(tc, bson_complete(&dst));

    BSON_ITER_FROM(dst, rd, iter);
    CuAssertTrue(tc, ERR_OK == bson_iter_find(&iter, "n", &result));
    CuAssertIntEquals(tc, 7, bson_iter_int32(&result, &err));
    CuAssertIntEquals(tc, ERR_OK, err);

    BSON_FREE(&empty);
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

    // jscode_n（二进制安全）：写进去的是 8 字节 "var x=1;\0"，含内嵌 NUL。
    // 只判 type/key 的话，_n 变体退化成按 strlen 截断也照样过 —— 而那正是 _n 存在的理由
    CuAssertTrue(tc, bson_iter_next(&iter));
    CuAssertIntEquals(tc, BSON_JSCODE, iter.type);
    CuAssertStrEquals(tc, "jsn", iter.key);
    CuAssertIntEquals(tc, 8, (int32_t)iter.lens);
    CuAssertTrue(tc, 0 == memcmp(iter.val, "var x=1;", 8));
    /* bson_iter_jscode 返回 cstring，到第一个 NUL 截止 —— 与上面的原始 8 字节各测一遍 */
    CuAssertStrEquals(tc, "var x=1;", bson_iter_jscode(&iter, &err));
    CuAssertIntEquals(tc, ERR_OK, err);

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

    // utf8_n：写进去的是 7 字节 "abc\0def"。长度前缀含末尾 NUL 写的是 8，
    // 而 bson_iter_next 取的是 vlens-1，故 iter.lens 是 7
    CuAssertTrue(tc, bson_iter_next(&iter));
    CuAssertIntEquals(tc, BSON_UTF8, iter.type);
    CuAssertStrEquals(tc, "raw", iter.key);
    CuAssertIntEquals(tc, 7, (int32_t)iter.lens);
    CuAssertTrue(tc, 0 == memcmp(iter.val, "abc\0def", 7));

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
 * bson_tostring / bson_tostring2 —— 调试串化（验证非空 + 字段名出现）。
 * ts 字段取两个最高位已置起的 uint32：timestamp 分支原先用 %d 打无符号量，
 * 会渲染成 -2147483649 一类负数，而这份输出正是 mongo_parse 在命令失败时喂给
 * LOG_WARN 的诊断内容。打印顺序是 inc 后 ts
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
    bson_append_timestamp(&bson, "ts", 0x80000000u, 0x80000001u);
    bson_append_end(&bson);

    char *s = bson_tostring(&bson);
    CuAssertPtrNotNull(tc, s);
    CuAssertTrue(tc, NULL != strstr(s, "2147483649 2147483648"));
    CuAssertTrue(tc, NULL == strstr(s, "-2147483"));
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

    /* 内嵌 NUL 的字符串不得被截断：写入侧是长度感知的(bson_append_utf8_n)，
       串化侧原先按 strlen 取长度，会把 NUL 之后整段切掉。这条路径是 mongo 失败命令的
       诊断输出，截短等于让运维看半截错误。NUL 转义成可见的 "\0"，
       因为两个消费者(LOG_WARN 的 %s、Lua 的 :tostring)都按 NUL 结尾读 */
    bson_ctx nb;
    bson_init(&nb, NULL, 0);
    bson_append_utf8_n(&nb, "s", "x\0y", 3);
    bson_append_jscode_n(&nb, "j", "a\0b", 3);
    bson_append_end(&nb);
    char *s3 = bson_tostring(&nb);
    CuAssertPtrNotNull(tc, s3);
    CuAssertTrue(tc, NULL != strstr(s3, "x\\0y"));
    CuAssertTrue(tc, NULL != strstr(s3, "a\\0b"));
    FREE(s3);
    BSON_FREE(&nb);
}

/* =======================================================================
 * bson_empty / bson_oid / bson_type_tostring / bson_subtype_tostring
 * ======================================================================= */
static void test_bson_misc(CuTest *tc) {
    size_t elen;
    const char *empty = bson_empty(&elen);
    CuAssertPtrNotNull(tc, empty);
    CuAssertTrue(tc, 5 == elen);/* 4 字节长度 + 1 EOD */
    CuAssertIntEquals(tc, 5, (uint8_t)empty[0]);/* len = 5 */
    CuAssertIntEquals(tc, 0, (uint8_t)empty[4]);/* EOD */

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

// bson_check_depth 的契约是"深度未超限 且 文档结构合法"。此前它只能判前者：
// bson_iter_next 对"读到 EOD"和"元素非法被拒"都返回 0，循环无从区分，
// 于是结构非法的文档只被检查了坏元素之前的前缀却报 ERR_OK，按契约当校验闸门用就会放行垃圾
static void test_bson_check_depth_malformed(CuTest *tc) {
    char badtype[] = { 0x08, 0x00, 0x00, 0x00, 0x42, 'x', 0x00, 0x00 };
    char trunc[] = { 0x0A, 0x00, 0x00, 0x00, 0x01, 'd', 0x00, 0x01, 0x02, 0x00 };
    bson_ctx ok;

    // 不支持的元素类型字节
    CuAssertIntEquals(tc, ERR_FAILED, bson_check_depth(badtype, sizeof(badtype)));
    // 截断的定长值：声明 DOUBLE 却只剩 3 字节
    CuAssertIntEquals(tc, ERR_FAILED, bson_check_depth(trunc, sizeof(trunc)));
    // 长度字段本身非法：声明 10 但只给 9 字节
    CuAssertIntEquals(tc, ERR_FAILED, bson_check_depth(trunc, sizeof(trunc) - 1));

    // EOD 提前出现、声明长度还剩字节：err 不置位（读到的确实是合法 EOD），
    // 只能靠"读完 EOD 后 offset 必等于 doclens"这条判据拦下
    char early_eod[] = { 0x06, 0x00, 0x00, 0x00, 0x00, (char)0xFF };
    CuAssertIntEquals(tc, ERR_FAILED, bson_check_depth(early_eod, sizeof(early_eod)));
    // 同样构造但声明长度正好到 EOD：合法
    char exact_eod[] = { 0x05, 0x00, 0x00, 0x00, 0x00 };
    CuAssertIntEquals(tc, ERR_OK, bson_check_depth(exact_eod, sizeof(exact_eod)));

    // 对照：合法文档仍返 ERR_OK
    bson_init(&ok, NULL, 0);
    bson_append_int32(&ok, "a", 1);
    bson_append_end(&ok);
    CuAssertIntEquals(tc, ERR_OK, bson_check_depth(BSON_DOC(&ok), BSON_DOC_LENS(&ok)));
    BSON_FREE(&ok);
}

// ③ 声明长度被恰好耗尽却缺终止 EOD 字节：元素消耗完必须给 EOD 留一字节，故第一次 next
// 就要拒收并置 err——否则 bson_iter_find 这类命中即返回的调用方拿不到任何失败信号。
// noeod 声明 11 字节、含一个完整 INT32 元素、无尾部 0x00；witheod 是同一文档补上 EOD 的对照。
// ④ bson_iter_reset 须与 bson_iter_init 一样毒化当前元素：reset 后在下一次 next 之前
// 调 getter 必须失败，而不是拿到 reset 前那个元素的值
static void test_bson_truncated_and_reset(CuTest *tc) {
    char noeod[] = { 0x0B, 0x00, 0x00, 0x00, 0x10, 'a', 0x00, 0x07, 0x00, 0x00, 0x00 };
    char witheod[] = { 0x0C, 0x00, 0x00, 0x00, 0x10, 'a', 0x00, 0x07, 0x00, 0x00, 0x00, 0x00 };
    bson_ctx bson;
    bson_iter iter;
    int32_t err;
    const char *val;

    CuAssertIntEquals(tc, ERR_FAILED, bson_check_depth(noeod, sizeof(noeod)));
    CuAssertIntEquals(tc, ERR_OK, bson_check_depth(witheod, sizeof(witheod)));

    bson_init(&bson, noeod, sizeof(noeod));
    bson_iter_init(&iter, &bson);
    CuAssertIntEquals(tc, 0, bson_iter_next(&iter));
    CuAssertTrue(tc, 0 != bson_iter_error(&iter));

    bson_ctx wbson;
    bson_init(&wbson, NULL, 0);
    bson_append_utf8(&wbson, "s", "hello");
    bson_append_end(&wbson);
    BSON_ITER_FROM(wbson, rd, rditer);
    CuAssertTrue(tc, 0 != bson_iter_next(&rditer));
    err = ERR_OK;
    val = bson_iter_utf8(&rditer, &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertStrEquals(tc, "hello", val);
    bson_iter_reset(&rditer);
    CuAssertIntEquals(tc, 0, bson_iter_error(&rditer));
    err = ERR_OK;
    val = bson_iter_utf8(&rditer, &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    CuAssertTrue(tc, NULL == val);
    CuAssertTrue(tc, 0 != bson_iter_next(&rditer));
    err = ERR_FAILED;
    val = bson_iter_utf8(&rditer, &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertStrEquals(tc, "hello", val);
    BSON_FREE(&wbson);
}

// bson_iter_init 对 size < 4 的缓冲是降级处理：读不到长度字段就置 doclens=0 / err=1 后正常返回；
// bson_iter_reset 从前无守卫直接 binary_offset(doc, 4)，缓冲不足 4 字节时撞越界断言 abort。
// Lua 侧一路可达：lpub_check_lens 只挡负数、_lbson_check_complete 只查 data 非空 + depth==0，
// bson.new(ptr, 0..3) → bson.iter.new(b) → it:reset() 中间没有任何一道拦得住。
// 修复后 reset 与 init 表现一致：err 保持置位、当前元素被毒化、getter 依旧失败。
static void test_bson_iter_reset_short_buffer(CuTest *tc) {
    char buf[4] = { 0x03, 0x00, 0x00, 0x00 };
    bson_ctx rd;
    bson_iter iter;
    int32_t err;
    size_t n;

    for (n = 0; n <= 3; n++) {
        bson_init(&rd, buf, n);
        bson_iter_init(&iter, &rd);
        CuAssertTrue(tc, 0 != bson_iter_error(&iter));
        bson_iter_reset(&iter);// 改前此行 abort
        CuAssertTrue(tc, 0 != bson_iter_error(&iter));
        CuAssertIntEquals(tc, BSON_EOD, iter.type);
        CuAssertTrue(tc, !bson_iter_next(&iter));
        err = ERR_OK;
        CuAssertTrue(tc, NULL == bson_iter_utf8(&iter, &err));
        CuAssertIntEquals(tc, ERR_FAILED, err);
    }
    // 对照：长度字段合法的最小空文档，reset 仍应正常跳到 4 并可重新遍历
    char emptydoc[] = { 0x05, 0x00, 0x00, 0x00, 0x00 };
    bson_init(&rd, emptydoc, sizeof(emptydoc));
    bson_iter_init(&iter, &rd);
    CuAssertIntEquals(tc, 0, bson_iter_error(&iter));
    CuAssertTrue(tc, !bson_iter_next(&iter));
    bson_iter_reset(&iter);
    CuAssertIntEquals(tc, 0, bson_iter_error(&iter));
    CuAssertTrue(tc, !bson_iter_next(&iter));
}

// bson_iter_init 从前只挡 size >= 4，挡不住 size - offset < 4：doc.offset 停在离缓冲末尾
// 不足 4 字节处时，binary_get_integer 当场断言。归零游标收进 bson_iter_init 后，
// 写入模式的 bson 可以直接建 iter，调用方不必再自己 binary_offset(&doc, 0)
static void test_bson_iter_init_offset_reset(CuTest *tc) {
    int32_t err;
    bson_ctx bson;
    bson_iter iter;

    // 写入模式：offset 停在末尾，直接建 iter 应能正常遍历
    bson_init(&bson, NULL, 0);
    bson_append_int32(&bson, "n", 7);
    bson_append_end(&bson);
    CuAssertTrue(tc, bson.doc.offset > 0);
    bson_iter_init(&iter, &bson);
    CuAssertIntEquals(tc, 0, bson_iter_error(&iter));
    CuAssertTrue(tc, bson_iter_next(&iter));
    CuAssertIntEquals(tc, BSON_INT32, iter.type);
    CuAssertIntEquals(tc, 7, bson_iter_int32(&iter, &err));
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, !bson_iter_next(&iter));
    CuAssertIntEquals(tc, 0, bson_iter_error(&iter));
    BSON_FREE(&bson);

    // size - offset < 4：改前此处断言 abort
    char raw[] = { 0x05, 0x00, 0x00, 0x00, 0x00 };
    bson_ctx tight;
    bson_init(&tight, raw, sizeof(raw));
    binary_offset(&tight.doc, 3);
    bson_iter_init(&iter, &tight);
    CuAssertIntEquals(tc, 0, bson_iter_error(&iter));
    CuAssertTrue(tc, !bson_iter_next(&iter));
    CuAssertIntEquals(tc, 0, bson_iter_error(&iter));
}

// 两个原始数据入口传 NULL 时必须直接失败：bson_init 把 data==NULL 重载为"新建可写文档"，
// 于是会 MALLOC 一块无人持有的缓冲、再按未初始化内容遍历，既泄漏又可能把堆残渣当字段打印。
// 零长 binary 那条 Lua 路径已在 bson.c:368 堵掉（val 不再为 NULL），这里守的是其余
// 任何把 NULL 交进来的调用方。
// 本用例兼作泄漏回归——修复前每轮漏 2 * BINARY_INCREASE(256) 字节，退出时 memcheck 必报非 0
static void test_bson_null_data_entry(CuTest *tc) {
    int32_t i;
    for (i = 0; i < 64; i++) {
        CuAssertPtrEquals(tc, NULL, bson_tostring2(NULL, 0));
        CuAssertIntEquals(tc, ERR_FAILED, bson_check_depth(NULL, 0));
        CuAssertPtrEquals(tc, NULL, bson_tostring2(NULL, 32));
        CuAssertIntEquals(tc, ERR_FAILED, bson_check_depth(NULL, 32));
    }
}

// 零长 binary 是合法 BSON（MongoDB 的 SASL 应答里就有）：type 有效时 val 必须非 NULL，
// 否则按"类型检查通过即可用 val"写的消费方会直接解引用 NULL。
// 修复前 binary_get_binary(doc, 0) 返 NULL，mongo_parse 把 ok=1 的应答判成认证失败
static void test_bson_binary_zero_length(CuTest *tc) {
    bson_ctx bson, rd;
    bson_iter iter;
    bson_subtype got = BSON_SUBTYPE_USER;
    size_t blens = 1;
    int32_t err = ERR_FAILED;
    char *bdata;

    bson_init(&bson, NULL, 0);
    bson_append_binary(&bson, "payload", BSON_SUBTYPE_BINARY, NULL, 0);
    bson_append_int32(&bson, "next", 7);// 跟一个字段，验证零长没把偏移带偏
    bson_append_end(&bson);

    bson_init(&rd, BSON_DOC(&bson), BSON_DOC_LENS(&bson));
    bson_iter_init(&iter, &rd);
    CuAssertTrue(tc, bson_iter_next(&iter));
    CuAssertIntEquals(tc, BSON_BINARY, iter.type);
    CuAssertIntEquals(tc, 0, bson_iter_error(&iter));
    CuAssertTrue(tc, 0 == iter.lens);
    // 不变式：type 有效 ⟹ val 非 NULL；有没有数据由 lens 表达
    CuAssertPtrNotNull(tc, iter.val);

    bdata = bson_iter_binary(&iter, &got, &blens, &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertPtrNotNull(tc, bdata);
    CuAssertTrue(tc, 0 == blens);
    CuAssertTrue(tc, BSON_SUBTYPE_BINARY == got);

    // 后一个字段照常读到，且整篇没有置 err
    CuAssertTrue(tc, bson_iter_next(&iter));
    CuAssertIntEquals(tc, BSON_INT32, iter.type);
    CuAssertIntEquals(tc, 7, bson_iter_int32(&iter, NULL));
    CuAssertTrue(tc, !bson_iter_next(&iter));
    CuAssertIntEquals(tc, 0, bson_iter_error(&iter));
    BSON_FREE(&bson);
}

// 子文档/定长值把根文档的 EOD 吃掉：合法文档末尾必有一字节 EOD，元素消耗完必须给它留位
static void test_bson_iter_eod_reserved(CuTest *tc) {
    bson_ctx reader;
    bson_iter iter;
    char buf[13];
    // 畸形：声明 12 字节 = 4(len) + 1(type) + 2(key) + 5(子文档) —— 根文档自己没有 EOD
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x0C;//doc size = 12
    buf[4] = 0x03;//BSON_DOCUMENT
    buf[5] = 'a'; buf[6] = 0x00;//key "a"
    buf[7] = 0x05;//子文档声明长度 5 = 4(len) + 1(eod)
    // buf[11] = 0x00 即子文档的 EOD，同时是根文档的最后一字节
    bson_init(&reader, buf, 12);
    bson_iter_init(&iter, &reader);
    CuAssertTrue(tc, !bson_iter_next(&iter));
    // 断言 err：把"被拒收"与"正常读到 EOD"区分开，两者的 next 都返 0
    CuAssertTrue(tc, 0 != bson_iter_error(&iter));

    // 合法：多一字节给根文档自己的 EOD，必须照常读出
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x0D;//doc size = 13
    buf[4] = 0x03;
    buf[5] = 'a'; buf[6] = 0x00;
    buf[7] = 0x05;
    // buf[11] 子文档 EOD，buf[12] 根文档 EOD
    bson_init(&reader, buf, 13);
    bson_iter_init(&iter, &reader);
    CuAssertTrue(tc, bson_iter_next(&iter));
    CuAssertIntEquals(tc, BSON_DOCUMENT, iter.type);
    CuAssertTrue(tc, !bson_iter_next(&iter));
    CuAssertIntEquals(tc, 0, bson_iter_error(&iter));

    // 定长类型走的是另一条边界判定,同样不许吃掉 EOD:声明 11 = 4 + 1(type) + 2(key) + 4(int32)
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x0B;
    buf[4] = 0x10;//BSON_INT32
    buf[5] = 'a'; buf[6] = 0x00;
    buf[7] = 0x2A;//值 42
    bson_init(&reader, buf, 11);
    bson_iter_init(&iter, &reader);
    CuAssertTrue(tc, !bson_iter_next(&iter));
    CuAssertTrue(tc, 0 != bson_iter_error(&iter));

    // 合法:多一字节给 EOD
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x0C;
    buf[4] = 0x10;
    buf[5] = 'a'; buf[6] = 0x00;
    buf[7] = 0x2A;
    bson_init(&reader, buf, 12);
    bson_iter_init(&iter, &reader);
    CuAssertTrue(tc, bson_iter_next(&iter));
    CuAssertIntEquals(tc, 42, bson_iter_int32(&iter, NULL));
    CuAssertTrue(tc, !bson_iter_next(&iter));
    CuAssertIntEquals(tc, 0, bson_iter_error(&iter));
}
// BSN-2：doc->size > doclens 时，内层字段长度检查须以 doclens 为界而非 doc->size
// 构造：声明 doc size=15，buffer size=20；UTF8 字段 lens=5 → 5 > doclens-offset=4，应拒绝
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
    // 检查：lens=5 > doclens(15)-offset(11)=4 → 拒绝
    bson_init(&reader, buf, sizeof(buf));//doc->size = 20
    bson_iter_init(&iter, &reader);//doclens = 15
    CuAssertTrue(tc, !bson_iter_next(&iter));
}

// BINARY 的 subtype 字节在末尾 EOD 判定之前就被解引用,挡住它的只有 _bson_iter_lenprefix 里
// binary 专属的 +1 修正量。构造可读空间恰等于声明长度的畸形文档:修正量在时被拒,
// 去掉修正量则读到文档末尾之后一字节。精确分配让 ASan 的 redzone 紧贴末尾 ——
// 无 ASan 时两条路都落到末尾判定,观测结果相同,只有 sh mk.sh test asan debug 能变红
static void test_bson_iter_binary_subtype_bound(CuTest *tc) {
    bson_ctx reader;
    bson_iter iter;
    // wire: int32 doclen=11 | 0x05 BINARY | key "b\0" | int32 lens=0 | 到此为止,subtype 位缺失
    size_t dlen = 11;
    char *buf;
    MALLOC(buf, dlen);
    buf[0] = 0x0B; buf[1] = 0x00; buf[2] = 0x00; buf[3] = 0x00;// doclen = 11
    buf[4] = 0x05;// BSON_BINARY
    buf[5] = 'b'; buf[6] = 0x00;// key "b"
    buf[7] = 0x00; buf[8] = 0x00; buf[9] = 0x00; buf[10] = 0x00;// binary lens = 0

    bson_init(&reader, buf, dlen);
    bson_iter_init(&iter, &reader);// doclens = 11
    int32_t next_rtn = bson_iter_next(&iter);
    int32_t err = bson_iter_error(&iter);
    FREE(buf);
    CuAssertTrue(tc, !next_rtn);
    CuAssertTrue(tc, 0 != err);
}

// 定长类型的长度检查(_bson_iter_fixed)独立于变长那条,拿覆盖为零的 DECIMAL128 钉它:
// 声明长度装不下 16 字节值时须拒收,装得下则照常读出
static void test_bson_iter_decimal128_bound(CuTest *tc) {
    bson_ctx reader;
    bson_iter iter;
    char buf[24];

    // 畸形:doclen=20,值起点 7,可读 13 < 16
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x14;// doclen = 20
    buf[4] = 0x13;// BSON_DECIMAL128
    buf[5] = 'd'; buf[6] = 0x00;// key "d"
    bson_init(&reader, buf, 20);
    bson_iter_init(&iter, &reader);
    CuAssertTrue(tc, !bson_iter_next(&iter));
    CuAssertTrue(tc, 0 != bson_iter_error(&iter));

    // 合法:doclen=24 = 4 + 1 + 2 + 16 + 1(EOD)
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x18;// doclen = 24
    buf[4] = 0x13;
    buf[5] = 'd'; buf[6] = 0x00;
    buf[7] = 0x2A;// 值首字节,只验读得出、不解码(无 decimal128 取值接口)
    bson_init(&reader, buf, sizeof(buf));
    bson_iter_init(&iter, &reader);
    CuAssertTrue(tc, bson_iter_next(&iter));
    CuAssertIntEquals(tc, BSON_DECIMAL128, iter.type);
    CuAssertIntEquals(tc, BSON_DECIMAL128_LENS, (int32_t)iter.lens);
    CuAssertTrue(tc, !bson_iter_next(&iter));
    CuAssertIntEquals(tc, 0, bson_iter_error(&iter));
}

// UTF8/JSCODE 字段声明长度的末尾字节必须是 NUL;畸形为非 NUL 时 bson_iter_next 须拒绝,
// 否则下游 _bson_dump strlen / lua_pushstring 会越过文档末尾读堆外内存
static void test_bson_iter_utf8_no_terminator(CuTest *tc) {
    bson_ctx reader;
    bson_iter iter;
    // wire: int32 doclen=13 | 0x02 UTF8 | key "v\0" | int32 strlen=1 | 末尾位 | 文档末尾
    // strlen=1 = 0 内容字符 + 1 终止位;末尾位畸形为 'X' → iter->val 无 NUL 终止
    size_t dlen = 13;
    char *buf;
    MALLOC(buf, dlen);// 精确分配,ASan redzone 紧贴文档末尾,修复前 strlen 越界即报 heap-overflow
    buf[0] = 0x0D; buf[1] = 0x00; buf[2] = 0x00; buf[3] = 0x00;// doclen = 13
    buf[4] = 0x02;// BSON_UTF8
    buf[5] = 'v'; buf[6] = 0x00;// key "v"
    buf[7] = 0x01; buf[8] = 0x00; buf[9] = 0x00; buf[10] = 0x00;// strlen = 1
    buf[11] = 'X';// 末尾位畸形非 NUL
    buf[12] = 'Y';// 文档末尾畸形非 EOD,使 strlen 越过 buf[12] 读堆外

    bson_init(&reader, buf, dlen);
    bson_iter_init(&iter, &reader);// doclens = 13
    CuAssertTrue(tc, !bson_iter_next(&iter));// 修复后拒绝;修复前接受致下游越界

    // bson_tostring2 全程不得越界:畸形 UTF8 被拒,dump 不触发 strlen
    char *s = bson_tostring2(buf, dlen);
    CuAssertPtrNotNull(tc, s);
    FREE(s);

    FREE(buf);
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
    // 每行是 "<key>(<类型名>): <值>"，按 key(type 整体比：
    // 只查裸 key 的话 "re" 会被类型名 "regex" 顺手满足，"ts"/"min"/"max"/"bin" 同理
    CuAssertTrue(tc, NULL != strstr(s, "re(regex)"));
    CuAssertTrue(tc, NULL != strstr(s, "code(javascript)"));
    CuAssertTrue(tc, NULL != strstr(s, "bin(binData)"));
    CuAssertTrue(tc, NULL != strstr(s, "oid(objectId)"));
    CuAssertTrue(tc, NULL != strstr(s, "ts(timestamp)"));
    CuAssertTrue(tc, NULL != strstr(s, "min(minKey)"));
    CuAssertTrue(tc, NULL != strstr(s, "max(maxKey)"));
    CuAssertTrue(tc, NULL != strstr(s, "date("));
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

/* BSON 是对外互通的格式（MongoDB 服务端照 spec 解），而本文件其余用例全是 encode→decode
   的自洽往返：把 int32 写成大端、把类型字节换个数、把总长算错一位，编解码两边一起变，
   往返照样对得上。这里钉住两份逐字节的最小文档。
   spec: document ::= int32(总长，含自身与结尾 0) e_list "\x00"；
         int32 元素 ::= 0x10 cstring(键) int32(小端)；
         字符串元素 ::= 0x02 cstring(键) int32(串长含结尾 0) bytes 0x00 */
static void test_bson_wire_layout(CuTest *tc) {
    /* { "a": 1 } */
    static const uint8_t want_i32[] = {
        0x0C, 0x00, 0x00, 0x00,/* 总长 12 */
        BSON_INT32, 0x61, 0x00,/* 类型 + 键 "a" */
        0x01, 0x00, 0x00, 0x00,/* 值 1，小端 */
        0x00/* 文档结尾 */
    };
    bson_ctx b;
    bson_init(&b, NULL, 0);
    bson_append_int32(&b, "a", 1);
    bson_append_end(&b);
    CuAssertIntEquals(tc, (int)sizeof(want_i32), (int)BSON_DOC_LENS(&b));
    CuAssertTrue(tc, 0 == memcmp(BSON_DOC(&b), want_i32, sizeof(want_i32)));
    BSON_FREE(&b);

    /* { "b": "hi" } */
    static const uint8_t want_str[] = {
        0x0F, 0x00, 0x00, 0x00,/* 总长 15 */
        BSON_UTF8, 0x62, 0x00,/* 类型 + 键 "b" */
        0x03, 0x00, 0x00, 0x00,/* 串长 3 = strlen + 结尾 0 */
        0x68, 0x69, 0x00,/* "hi" + 结尾 0 */
        0x00
    };
    bson_init(&b, NULL, 0);
    bson_append_utf8(&b, "b", "hi");
    bson_append_end(&b);
    CuAssertIntEquals(tc, (int)sizeof(want_str), (int)BSON_DOC_LENS(&b));
    CuAssertTrue(tc, 0 == memcmp(BSON_DOC(&b), want_str, sizeof(want_str)));
    BSON_FREE(&b);
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

// 空键是合法的 BSON e_name，find("") 要能命中它；点分路径里的空段仍须一律拒绝
static void test_bson_iter_find_empty_key(CuTest *tc) {
    int32_t err;
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_int32(&bson, "", 42);
    bson_append_int32(&bson, "x", 7);
    // 子文档里也放一个空键:没有它的话,下面的点分路径全被"父段不是文档"那条守卫兜住,
    // 空段守卫删掉照样过,钉不住任何东西
    bson_append_document_begain(&bson, "d");
    bson_append_int32(&bson, "", 9);
    bson_append_int32(&bson, "y", 1);
    bson_append_end(&bson);
    bson_append_end(&bson);
    CuAssertTrue(tc, bson_complete(&bson));

    bson_iter result;
    {
        BSON_ITER_FROM(bson, rd, iter);
        CuAssertIntEquals(tc, ERR_OK, bson_iter_find(&iter, "", &result));
        CuAssertIntEquals(tc, 42, bson_iter_int32(&result, &err));
        CuAssertIntEquals(tc, ERR_OK, err);
    }
    // 非空键不受影响
    {
        BSON_ITER_FROM(bson, rd, iter);
        CuAssertIntEquals(tc, ERR_OK, bson_iter_find(&iter, "x", &result));
        CuAssertIntEquals(tc, 7, bson_iter_int32(&result, &err));
    }
    // 点分路径的空段:开头、结尾、中间、单个点,全部拒绝
    {
        BSON_ITER_FROM(bson, rd, iter);
        CuAssertTrue(tc, ERR_OK != bson_iter_find(&iter, ".", &result));
    }
    {
        BSON_ITER_FROM(bson, rd, iter);
        CuAssertTrue(tc, ERR_OK != bson_iter_find(&iter, "x.", &result));
    }
    {
        BSON_ITER_FROM(bson, rd, iter);
        CuAssertTrue(tc, ERR_OK != bson_iter_find(&iter, ".x", &result));
    }
    // 结尾空段:父段是真文档、子文档里那个空键确实存在,拒收只能来自空段守卫本身
    {
        BSON_ITER_FROM(bson, rd, iter);
        CuAssertTrue(tc, ERR_OK != bson_iter_find(&iter, "d.", &result));
    }
    // 中间空段
    {
        BSON_ITER_FROM(bson, rd, iter);
        CuAssertTrue(tc, ERR_OK != bson_iter_find(&iter, "d..y", &result));
    }
    // 对照:同一层的非空段照常命中,守卫没把正常路径一起挡掉
    {
        BSON_ITER_FROM(bson, rd, iter);
        CuAssertIntEquals(tc, ERR_OK, bson_iter_find(&iter, "d.y", &result));
        CuAssertIntEquals(tc, 1, bson_iter_int32(&result, &err));
    }
    BSON_FREE(&bson);
}

// 按长度 klen 生成 key（不写结尾 NUL）。混入 0x01 / 0x80 / 0xFF / 0x7F 这类最容易让
// "8 字节一起查 0"误判的字节，起点随 klen 错开，各长度的 key 内容互不相同
static void _bson_mk_key(char *key, size_t klen) {
    static const char tbl[] = { 'a', 0x01, (char)0x80, (char)0xFF, 'Z', 0x7F, (char)0x81, (char)0xFE, '.', '9' };
    size_t i;
    for (i = 0; i < klen; i++) {
        key[i] = tbl[(i + klen) % sizeof(tbl)];
    }
}
// 遍历 data：应恰好是 key 长度 0..BSON_T_KEYMAX 各一个的 int32 字段，值为 0x01010101 + klen
static void _bson_check_keys(CuTest *tc, char *data, size_t lens) {
    char key[BSON_T_KEYMAX + 1];
    bson_ctx rd;
    bson_iter iter;
    size_t k;
    int32_t err;
    bson_init(&rd, data, lens);
    bson_iter_init(&iter, &rd);
    CuAssertIntEquals(tc, 0, bson_iter_error(&iter));
    for (k = 0; k <= BSON_T_KEYMAX; k++) {
        _bson_mk_key(key, k);
        CuAssertTrue(tc, bson_iter_next(&iter));
        CuAssertIntEquals(tc, BSON_INT32, iter.type);
        CuAssertIntEquals(tc, (int32_t)k, (int32_t)iter.keylens);
        CuAssertTrue(tc, 0 == memcmp(iter.key, key, k));
        CuAssertTrue(tc, '\0' == iter.key[k]);
        err = ERR_FAILED;
        CuAssertIntEquals(tc, (int32_t)(0x01010101 + k), bson_iter_int32(&iter, &err));
        CuAssertIntEquals(tc, ERR_OK, err);
    }
    CuAssertTrue(tc, !bson_iter_next(&iter));
    CuAssertIntEquals(tc, 0, bson_iter_error(&iter));
}
// key 找结尾 NUL 的边界：长度 0..40 的 key 各追加一个 int32 后逐个读回，keylens / key 内容 / 值都要对。
// 值取 0x01010101 + klen：紧跟在 NUL 后面的全是 0x01，8 字节一起查时这类字节最容易被误当成 0。
// 同一文档再拷到偏移 1..7 的缓冲上重读一遍，缓冲末尾恰好是文档末尾（ASan 下多读一字节即报）
static void test_bson_key_lens_scan(CuTest *tc) {
    char key[BSON_T_KEYMAX + 1];
    bson_ctx bson;
    size_t k, shift, lens;
    char *raw;

    bson_init(&bson, NULL, 0);
    for (k = 0; k <= BSON_T_KEYMAX; k++) {
        _bson_mk_key(key, k);
        key[k] = '\0';
        bson_append_int32(&bson, key, (int32_t)(0x01010101 + k));
    }
    bson_append_end(&bson);
    CuAssertTrue(tc, bson_complete(&bson));
    lens = BSON_DOC_LENS(&bson);
    _bson_check_keys(tc, BSON_DOC(&bson), lens);
    CuAssertIntEquals(tc, ERR_OK, bson_check_depth(BSON_DOC(&bson), lens));

    for (shift = 1; shift <= 7; shift++) {
        MALLOC(raw, lens + shift);
        memcpy(raw + shift, BSON_DOC(&bson), lens);
        _bson_check_keys(tc, raw + shift, lens);
        CuAssertIntEquals(tc, ERR_OK, bson_check_depth(raw + shift, lens));
        FREE(raw);
    }
    BSON_FREE(&bson);
}
// key 一直延伸到声明长度末尾、中间没有 NUL：遍历必须报错，且不能到声明长度之外去找 NUL。
// 对照组是同一个 key 补上 NUL、类型换成不带值的 NULL、再补 EOD：key 的 NUL 落在倒数第二字节，照常读出
static void test_bson_key_no_nul(CuTest *tc) {
    char key[BSON_T_KEYMAX + 1];
    bson_ctx rd;
    bson_iter iter;
    size_t klen, dlen;
    char *buf;

    for (klen = 0; klen <= 24; klen++) {
        _bson_mk_key(key, klen);

        // 1) 精确分配：[len][0x10][key...] 到此为止，没有 NUL 也没有 EOD
        dlen = 5 + klen;
        MALLOC(buf, dlen);
        ZERO(buf, 4);
        buf[0] = (char)dlen;
        buf[4] = BSON_INT32;
        memcpy(buf + 5, key, klen);
        bson_init(&rd, buf, dlen);
        bson_iter_init(&iter, &rd);
        CuAssertIntEquals(tc, 0, bson_iter_error(&iter));
        CuAssertTrue(tc, !bson_iter_next(&iter));
        CuAssertTrue(tc, 0 != bson_iter_error(&iter));
        CuAssertIntEquals(tc, BSON_EOD, iter.type);
        CuAssertIntEquals(tc, ERR_FAILED, bson_check_depth(buf, dlen));
        FREE(buf);

        // 2) 缓冲比声明长度多出 16 个 0：NUL 只出现在声明长度之外，仍然不算数
        MALLOC(buf, dlen + 16);
        ZERO(buf, dlen + 16);
        buf[0] = (char)dlen;
        buf[4] = BSON_INT32;
        memcpy(buf + 5, key, klen);
        bson_init(&rd, buf, dlen + 16);
        bson_iter_init(&iter, &rd);
        CuAssertIntEquals(tc, 0, bson_iter_error(&iter));
        CuAssertTrue(tc, !bson_iter_next(&iter));
        CuAssertTrue(tc, 0 != bson_iter_error(&iter));
        FREE(buf);

        // 3) 对照：[len][0x0A][key][NUL][EOD]，精确分配
        dlen = 5 + klen + 2;
        MALLOC(buf, dlen);
        ZERO(buf, 4);
        buf[0] = (char)dlen;
        buf[4] = BSON_NULL;
        memcpy(buf + 5, key, klen);
        buf[5 + klen] = '\0';
        buf[6 + klen] = '\0';
        bson_init(&rd, buf, dlen);
        bson_iter_init(&iter, &rd);
        CuAssertTrue(tc, bson_iter_next(&iter));
        CuAssertIntEquals(tc, BSON_NULL, iter.type);
        CuAssertIntEquals(tc, (int32_t)klen, (int32_t)iter.keylens);
        CuAssertTrue(tc, 0 == memcmp(iter.key, key, klen));
        CuAssertTrue(tc, !bson_iter_next(&iter));
        CuAssertIntEquals(tc, 0, bson_iter_error(&iter));
        CuAssertIntEquals(tc, ERR_OK, bson_check_depth(buf, dlen));
        FREE(buf);
    }
}

// 把 b 的余量压到装不下 need 字节，逼下一次追加扩容；ref 跟着写同样的填充字段，两边字节保持一致。
// 扩容条件是 need + offset + 1 > size，即余量 <= need
static void _bson_squeeze(bson_ctx *b, bson_ctx *ref, size_t need) {
    while (b->doc.size - b->doc.offset > need) {
        bson_append_int32(b, "f", 1);
        bson_append_int32(ref, "f", 1);
    }
}
// 数一遍 data 里 key 为 key、且值与期望一致的字段个数
static int32_t _bson_count_alias(char *data, size_t lens, const char *key, const char *bin, const char *oid) {
    bson_ctx rd, sub;
    bson_iter iter, siter;
    bson_subtype st;
    size_t blens;
    char *p;
    int32_t err, n = 0;
    bson_init(&rd, data, lens);
    bson_iter_init(&iter, &rd);
    while (bson_iter_next(&iter)) {
        if (0 != strcmp(iter.key, key)) {
            continue;
        }
        switch (iter.type) {
        case BSON_UTF8:
            p = (char *)bson_iter_utf8(&iter, &err);
            if (ERR_OK == err && 0 == strcmp(p, "self-alias-value")) {
                n++;
            }
            break;
        case BSON_BINARY:
            p = bson_iter_binary(&iter, &st, &blens, &err);
            if (ERR_OK == err && BSON_SUBTYPE_USER == st && 32 == blens && 0 == memcmp(p, bin, 32)) {
                n++;
            }
            break;
        case BSON_OID:
            p = bson_iter_oid(&iter, &err);
            if (ERR_OK == err && 0 == memcmp(p, oid, BSON_OID_LENS)) {
                n++;
            }
            break;
        case BSON_DOCUMENT:
            p = bson_iter_document(&iter, &blens, &err);
            if (ERR_OK != err) {
                break;
            }
            bson_init(&sub, p, blens);
            bson_iter_init(&siter, &sub);
            if (bson_iter_next(&siter) && 42 == bson_iter_int32(&siter, &err) && ERR_OK == err) {
                n++;
            }
            break;
        default:
            break;
        }
    }
    return n;
}
// key 与 val 都指向本 ctx 自己的缓冲、且这次追加必然扩容（ASan 下 realloc 必换地址）：
// 结果须与用独立拷贝追加的 ref 逐字节一致，读回来的值也与原字段相同。
// 下标在写字段前按 wire 布局算好：type 1 字节之后是 key，key 的 NUL 之后是值
// bson_append_regex 的 key / pattern / options 都指向本 ctx 自己的缓冲，且这次追加必然扩容：
// 结果须与用独立拷贝追加逐字节一致，两份 regex 都读得回原值
static void test_bson_append_regex_self_alias(CuTest *tc) {
    char kcopy[16];
    char pcopy[16];
    char ocopy[16];
    bson_ctx b, ref, rd;
    bson_iter it;
    size_t i, oldsize, koff, poff, ooff;
    const char *pat;
    char *opt;
    int32_t hits = 0;

    bson_init(&b, NULL, 0);
    bson_init(&ref, NULL, 0);
    for (i = 0; i < 30; i++) {
        bson_append_int32(&b, "fill", (int32_t)i);
        bson_append_int32(&ref, "fill", (int32_t)i);
    }
    koff = b.doc.offset + 1;
    poff = koff + 3;// "re\0"
    ooff = poff + 6;// "abc.*\0"
    bson_append_regex(&b, "re", "abc.*", "im");
    bson_append_regex(&ref, "re", "abc.*", "im");
    CuAssertStrEquals(tc, "re", b.doc.data + koff);
    CuAssertStrEquals(tc, "abc.*", b.doc.data + poff);
    CuAssertStrEquals(tc, "im", b.doc.data + ooff);

    // 1 + "re\0" + "abc.*\0" + "im\0"
    _bson_squeeze(&b, &ref, 1 + 3 + 6 + 3);
    oldsize = b.doc.size;
    strcpy(kcopy, b.doc.data + koff);
    strcpy(pcopy, b.doc.data + poff);
    strcpy(ocopy, b.doc.data + ooff);
    bson_append_regex(&b, b.doc.data + koff, b.doc.data + poff, b.doc.data + ooff);
    bson_append_regex(&ref, kcopy, pcopy, ocopy);
    CuAssertTrue(tc, b.doc.size > oldsize);

    bson_append_end(&b);
    bson_append_end(&ref);
    CuAssertTrue(tc, BSON_DOC_LENS(&ref) == BSON_DOC_LENS(&b));
    CuAssertTrue(tc, 0 == memcmp(BSON_DOC(&ref), BSON_DOC(&b), BSON_DOC_LENS(&b)));
    CuAssertIntEquals(tc, ERR_OK, bson_check_depth(BSON_DOC(&b), BSON_DOC_LENS(&b)));
    bson_init(&rd, BSON_DOC(&b), BSON_DOC_LENS(&b));
    bson_iter_init(&it, &rd);
    while (bson_iter_next(&it)) {
        if (BSON_REGEX != it.type) {
            continue;
        }
        opt = NULL;
        pat = bson_iter_regex(&it, &opt, NULL);
        CuAssertStrEquals(tc, "re", it.key);
        CuAssertStrEquals(tc, "abc.*", pat);
        CuAssertStrEquals(tc, "im", opt);
        hits++;
    }
    CuAssertIntEquals(tc, 0, bson_iter_error(&it));
    CuAssertIntEquals(tc, 2, hits);
    BSON_FREE(&b);
    BSON_FREE(&ref);
}
static void test_bson_append_self_alias(CuTest *tc) {
    char bin[32];
    char oid[BSON_OID_LENS];
    char kcopy[16];
    char vcopy[64];
    bson_ctx b, ref;
    size_t i, oldsize, sublens;
    size_t koff_name, voff_name, koff_bin, voff_bin, koff_oid, voff_oid, koff_sub, voff_sub;

    for (i = 0; i < sizeof(bin); i++) {
        bin[i] = (char)(0xF0 - i);
    }
    for (i = 0; i < BSON_OID_LENS; i++) {
        oid[i] = (char)(0x30 + i);
    }
    bson_init(&b, NULL, 0);
    bson_init(&ref, NULL, 0);
    // 先写几十个字段让缓冲先长起来
    for (i = 0; i < 30; i++) {
        bson_append_int32(&b, "fill", (int32_t)i);
        bson_append_int32(&ref, "fill", (int32_t)i);
    }
    koff_name = b.doc.offset + 1;
    voff_name = koff_name + 5 + 4;// "name\0" + int32 长度
    bson_append_utf8(&b, "name", "self-alias-value");
    bson_append_utf8(&ref, "name", "self-alias-value");
    koff_bin = b.doc.offset + 1;
    voff_bin = koff_bin + 4 + 4 + 1;// "bin\0" + int32 长度 + subtype
    bson_append_binary(&b, "bin", BSON_SUBTYPE_USER, bin, sizeof(bin));
    bson_append_binary(&ref, "bin", BSON_SUBTYPE_USER, bin, sizeof(bin));
    koff_oid = b.doc.offset + 1;
    voff_oid = koff_oid + 4;// "oid\0"
    bson_append_oid(&b, "oid", oid);
    bson_append_oid(&ref, "oid", oid);
    koff_sub = b.doc.offset + 1;
    voff_sub = koff_sub + 4;// "sub\0"
    bson_append_document_begain(&b, "sub");
    bson_append_int32(&b, "x", 42);
    bson_append_utf8(&b, "y", "z");
    bson_append_end(&b);
    bson_append_document_begain(&ref, "sub");
    bson_append_int32(&ref, "x", 42);
    bson_append_utf8(&ref, "y", "z");
    bson_append_end(&ref);
    sublens = b.doc.offset - voff_sub;
    CuAssertTrue(tc, sublens <= sizeof(vcopy));

    // 下标确实指着预期的内容
    CuAssertStrEquals(tc, "name", b.doc.data + koff_name);
    CuAssertStrEquals(tc, "self-alias-value", b.doc.data + voff_name);
    CuAssertStrEquals(tc, "bin", b.doc.data + koff_bin);
    CuAssertTrue(tc, 0 == memcmp(b.doc.data + voff_bin, bin, sizeof(bin)));
    CuAssertStrEquals(tc, "oid", b.doc.data + koff_oid);
    CuAssertTrue(tc, 0 == memcmp(b.doc.data + voff_oid, oid, BSON_OID_LENS));
    CuAssertStrEquals(tc, "sub", b.doc.data + koff_sub);
    CuAssertTrue(tc, (int32_t)sublens == (int32_t)unpack_integer(b.doc.data + voff_sub, 4, 1, 1));

    // utf8：1 + "name\0" + 4 + 16 + 1
    _bson_squeeze(&b, &ref, 1 + 5 + 4 + 16 + 1);
    oldsize = b.doc.size;
    strcpy(kcopy, b.doc.data + koff_name);
    strcpy(vcopy, b.doc.data + voff_name);
    bson_append_utf8(&b, b.doc.data + koff_name, b.doc.data + voff_name);
    bson_append_utf8(&ref, kcopy, vcopy);
    CuAssertTrue(tc, b.doc.size > oldsize);

    // binary：1 + "bin\0" + 4 + 1 + 32
    _bson_squeeze(&b, &ref, 1 + 4 + 4 + 1 + sizeof(bin));
    oldsize = b.doc.size;
    strcpy(kcopy, b.doc.data + koff_bin);
    memcpy(vcopy, b.doc.data + voff_bin, sizeof(bin));
    bson_append_binary(&b, b.doc.data + koff_bin, BSON_SUBTYPE_USER, b.doc.data + voff_bin, sizeof(bin));
    bson_append_binary(&ref, kcopy, BSON_SUBTYPE_USER, vcopy, sizeof(bin));
    CuAssertTrue(tc, b.doc.size > oldsize);

    // oid：1 + "oid\0" + 12
    _bson_squeeze(&b, &ref, 1 + 4 + BSON_OID_LENS);
    oldsize = b.doc.size;
    strcpy(kcopy, b.doc.data + koff_oid);
    memcpy(vcopy, b.doc.data + voff_oid, BSON_OID_LENS);
    bson_append_oid(&b, b.doc.data + koff_oid, b.doc.data + voff_oid);
    bson_append_oid(&ref, kcopy, vcopy);
    CuAssertTrue(tc, b.doc.size > oldsize);

    // document：1 + "sub\0" + 子文档整段
    _bson_squeeze(&b, &ref, 1 + 4 + sublens);
    oldsize = b.doc.size;
    strcpy(kcopy, b.doc.data + koff_sub);
    memcpy(vcopy, b.doc.data + voff_sub, sublens);
    bson_append_document(&b, b.doc.data + koff_sub, b.doc.data + voff_sub, sublens);
    bson_append_document(&ref, kcopy, vcopy, sublens);
    CuAssertTrue(tc, b.doc.size > oldsize);

    bson_append_end(&b);
    bson_append_end(&ref);
    CuAssertTrue(tc, bson_complete(&b));
    CuAssertTrue(tc, BSON_DOC_LENS(&ref) == BSON_DOC_LENS(&b));
    CuAssertTrue(tc, 0 == memcmp(BSON_DOC(&ref), BSON_DOC(&b), BSON_DOC_LENS(&b)));
    CuAssertIntEquals(tc, ERR_OK, bson_check_depth(BSON_DOC(&b), BSON_DOC_LENS(&b)));
    // 原字段与自指追加的那一份都读得回原值：每个 key 各两份
    CuAssertIntEquals(tc, 2, _bson_count_alias(BSON_DOC(&b), BSON_DOC_LENS(&b), "name", bin, oid));
    CuAssertIntEquals(tc, 2, _bson_count_alias(BSON_DOC(&b), BSON_DOC_LENS(&b), "bin", bin, oid));
    CuAssertIntEquals(tc, 2, _bson_count_alias(BSON_DOC(&b), BSON_DOC_LENS(&b), "oid", bin, oid));
    CuAssertIntEquals(tc, 2, _bson_count_alias(BSON_DOC(&b), BSON_DOC_LENS(&b), "sub", bin, oid));
    BSON_FREE(&b);
    BSON_FREE(&ref);
}

// bson_init_prefix 用例共用的一组字段：600 字节字符串逼扩容，两层嵌套检查各层长度回填
static void _bson_prefix_fill(bson_ctx *b, const char *big) {
    bson_append_int32(b, "a", 1);
    bson_append_utf8(b, "big", big);
    bson_append_document_begain(b, "d");
    bson_append_int32(b, "x", 2);
    bson_append_array_begain(b, "arr");
    bson_append_utf8(b, "0", "p");
    bson_append_utf8(b, "1", "q");
    bson_append_end(b);
    bson_append_end(b);
    bson_append_int64(b, "z", 3);
}
// bson_init_prefix：预留的 prefix 字节在扩容后原样保留、不被改写；文档从 prefix 起，
// 与普通 bson_init 写出的逐字节相同，能从 prefix 处用读模式遍历，各层 bson_append_end 回填正确
static void test_bson_init_prefix(CuTest *tc) {
    const size_t prefixes[] = { 0, 1, 7, 21 };
    const size_t caps[] = { 0, 16, 4096 };
    // {"d": {"x": 1}}：外层 20 字节，内层 12 字节
    static const uint8_t want[] = {
        0x14, 0x00, 0x00, 0x00,
        BSON_DOCUMENT, 'd', 0x00,
        0x0C, 0x00, 0x00, 0x00,
        BSON_INT32, 'x', 0x00, 0x01, 0x00, 0x00, 0x00,
        0x00,
        0x00
    };
    char big[601];
    bson_ctx b, ref, rd, sub, arr;
    bson_iter iter, siter, aiter;
    size_t pi, ci, i, dlens, slens;
    int32_t err;
    char *p;

    memset(big, 'b', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    bson_init(&ref, NULL, 0);
    _bson_prefix_fill(&ref, big);
    bson_append_end(&ref);

    for (pi = 0; pi < sizeof(prefixes) / sizeof(prefixes[0]); pi++) {
        for (ci = 0; ci < sizeof(caps) / sizeof(caps[0]); ci++) {
            bson_init_prefix(&b, caps[ci], prefixes[pi]);
            // 刚初始化：prefix 之后只有 4 字节长度占位，文档还没闭合
            CuAssertTrue(tc, prefixes[pi] + 4 == BSON_DOC_LENS(&b));
            memset(BSON_DOC(&b), 0xA5, prefixes[pi]);
            _bson_prefix_fill(&b, big);
            bson_append_end(&b);
            CuAssertTrue(tc, bson_complete(&b));
            for (i = 0; i < prefixes[pi]; i++) {
                CuAssertIntEquals(tc, 0xA5, (uint8_t)BSON_DOC(&b)[i]);
            }
            dlens = BSON_DOC_LENS(&b) - prefixes[pi];
            CuAssertTrue(tc, dlens == BSON_DOC_LENS(&ref));
            CuAssertTrue(tc, (int64_t)dlens == unpack_integer(BSON_DOC(&b) + prefixes[pi], 4, 1, 1));
            CuAssertTrue(tc, 0 == memcmp(BSON_DOC(&b) + prefixes[pi], BSON_DOC(&ref), dlens));
            CuAssertIntEquals(tc, ERR_OK, bson_check_depth(BSON_DOC(&b) + prefixes[pi], dlens));

            bson_init(&rd, BSON_DOC(&b) + prefixes[pi], dlens);
            bson_iter_init(&iter, &rd);
            CuAssertIntEquals(tc, 0, bson_iter_error(&iter));
            CuAssertTrue(tc, bson_iter_next(&iter));
            CuAssertStrEquals(tc, "a", iter.key);
            CuAssertIntEquals(tc, 1, bson_iter_int32(&iter, &err));
            CuAssertTrue(tc, bson_iter_next(&iter));
            CuAssertStrEquals(tc, "big", iter.key);
            CuAssertStrEquals(tc, big, bson_iter_utf8(&iter, &err));
            CuAssertTrue(tc, bson_iter_next(&iter));
            CuAssertIntEquals(tc, BSON_DOCUMENT, iter.type);
            p = bson_iter_document(&iter, &slens, &err);
            CuAssertIntEquals(tc, ERR_OK, err);
            bson_init(&sub, p, slens);
            bson_iter_init(&siter, &sub);
            CuAssertTrue(tc, bson_iter_next(&siter));
            CuAssertIntEquals(tc, 2, bson_iter_int32(&siter, &err));
            CuAssertTrue(tc, bson_iter_next(&siter));
            CuAssertIntEquals(tc, BSON_ARRAY, siter.type);
            p = bson_iter_array(&siter, &slens, &err);
            CuAssertIntEquals(tc, ERR_OK, err);
            bson_init(&arr, p, slens);
            bson_iter_init(&aiter, &arr);
            CuAssertTrue(tc, bson_iter_next(&aiter));
            CuAssertStrEquals(tc, "p", bson_iter_utf8(&aiter, &err));
            CuAssertTrue(tc, bson_iter_next(&aiter));
            CuAssertStrEquals(tc, "q", bson_iter_utf8(&aiter, &err));
            CuAssertTrue(tc, !bson_iter_next(&aiter));
            CuAssertIntEquals(tc, 0, bson_iter_error(&aiter));
            CuAssertTrue(tc, !bson_iter_next(&siter));
            CuAssertIntEquals(tc, 0, bson_iter_error(&siter));
            CuAssertTrue(tc, bson_iter_next(&iter));
            CuAssertStrEquals(tc, "z", iter.key);
            CuAssertTrue(tc, 3 == bson_iter_int64(&iter, &err));
            CuAssertTrue(tc, !bson_iter_next(&iter));
            CuAssertIntEquals(tc, 0, bson_iter_error(&iter));
            BSON_FREE(&b);
        }
    }
    BSON_FREE(&ref);

    // 字节级：prefix=3 写 {"d": {"x": 1}}，外层长度从 prefix 处算起，内层从内层长度字段算起
    bson_init_prefix(&b, 0, 3);
    memset(BSON_DOC(&b), 0x5A, 3);
    bson_append_document_begain(&b, "d");
    bson_append_int32(&b, "x", 1);
    bson_append_end(&b);
    bson_append_end(&b);
    CuAssertTrue(tc, 3 + sizeof(want) == BSON_DOC_LENS(&b));
    for (i = 0; i < 3; i++) {
        CuAssertIntEquals(tc, 0x5A, (uint8_t)BSON_DOC(&b)[i]);
    }
    CuAssertTrue(tc, 0 == memcmp(BSON_DOC(&b) + 3, want, sizeof(want)));
    BSON_FREE(&b);
}

// bson_tostring 对 binary / oid / decimal128 的十六进制输出（大写、无分隔）逐字比对全文，
// 顺带覆盖零长 binary、内嵌 NUL 的转义、空串、嵌套缩进。decimal128 没有 append 接口，手工拼一篇再 bson_cat
static void test_bson_tostring_hex_exact(CuTest *tc) {
    static const char want[] =
        "{\r\n"
        "    bin(binData): (binary) 0001ABFF\r\n"
        "    empty(binData): (uuid) \r\n"
        "    _id(objectId): 000102030405060708090A0B\r\n"
        "    dec(decimal): 101112131415161718191A1B1C1D1E1F\r\n"
        "    s(string): \\0a\\0\\0b\\0\r\n"
        "    e(string): \r\n"
        "    d(object): {\r\n"
        "        x(binData): (binary) 7F\r\n"
        "    }\r\n"
        "}";
    static const char hexd[] = "0123456789ABCDEF";
    const char *line = "    k(binData): (binary) ";
    char bin[4] = { 0x00, 0x01, (char)0xAB, (char)0xFF };
    char oid[BSON_OID_LENS];
    char dec[26];// {"dec": decimal128} = 4 + 1 + "dec\0" + 16 + 1
    char one = 0x7F;
    char payload[37];
    char hex2[2];
    binary_ctx expect;
    bson_ctx b;
    size_t i, j, before;
    char *s;

    for (i = 0; i < BSON_OID_LENS; i++) {
        oid[i] = (char)i;
    }
    ZERO(dec, sizeof(dec));
    dec[0] = (char)sizeof(dec);
    dec[4] = BSON_DECIMAL128;
    memcpy(dec + 5, "dec", 4);
    for (i = 0; i < BSON_DECIMAL128_LENS; i++) {
        dec[9 + i] = (char)(0x10 + i);
    }

    bson_init(&b, NULL, 0);
    bson_append_binary(&b, "bin", BSON_SUBTYPE_BINARY, bin, sizeof(bin));
    bson_append_binary(&b, "empty", BSON_SUBTYPE_UUID, NULL, 0);
    bson_append_oid(&b, "_id", oid);
    CuAssertIntEquals(tc, ERR_OK, bson_cat(&b, dec, sizeof(dec)));
    bson_append_utf8_n(&b, "s", "\0a\0\0b\0", 6);
    bson_append_utf8(&b, "e", "");
    bson_append_document_begain(&b, "d");
    bson_append_binary(&b, "x", BSON_SUBTYPE_BINARY, &one, 1);
    bson_append_end(&b);
    bson_append_end(&b);

    before = BSON_DOC_LENS(&b);
    s = bson_tostring(&b);
    CuAssertStrEquals(tc, want, s);
    FREE(s);
    CuAssertTrue(tc, before == BSON_DOC_LENS(&b));// 串化不改写入位置
    // 同一份字节走读模式入口
    s = bson_tostring2(BSON_DOC(&b), BSON_DOC_LENS(&b));
    CuAssertStrEquals(tc, want, s);
    FREE(s);
    BSON_FREE(&b);

    // 200 个 37 字节 binary：串化文本远超输出缓冲的初始容量，中途多次扩容后十六进制仍须落在正确位置。
    // 期望文本用本地查表独立生成
    binary_init_write(&expect, 0, 0);
    binary_set_binary(&expect, "{\r\n", 3);
    bson_init(&b, NULL, 0);
    for (i = 0; i < 200; i++) {
        for (j = 0; j < sizeof(payload); j++) {
            payload[j] = (char)(i * sizeof(payload) + j);
        }
        bson_append_binary(&b, "k", BSON_SUBTYPE_BINARY, payload, sizeof(payload));
        binary_set_binary(&expect, line, strlen(line));
        for (j = 0; j < sizeof(payload); j++) {
            hex2[0] = hexd[(uint8_t)payload[j] >> 4];
            hex2[1] = hexd[(uint8_t)payload[j] & 0x0F];
            binary_set_binary(&expect, hex2, 2);
        }
        binary_set_binary(&expect, "\r\n", 2);
    }
    bson_append_end(&b);
    binary_set_binary(&expect, "}", 1);
    binary_set_int8(&expect, 0);
    s = bson_tostring2(BSON_DOC(&b), BSON_DOC_LENS(&b));
    CuAssertStrEquals(tc, expect.data, s);
    FREE(s);
    binary_free(&expect);
    BSON_FREE(&b);
}
void test_bson(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_bson_primitives);
    SUITE_ADD_TEST(suite, test_bson_iter_no_next);
    SUITE_ADD_TEST(suite, test_bson_iter_malformed_poison);
    SUITE_ADD_TEST(suite, test_bson_binary_subtype_high);
    SUITE_ADD_TEST(suite, test_bson_nested);
    SUITE_ADD_TEST(suite, test_bson_find);
    SUITE_ADD_TEST(suite, test_bson_iter_find_deep_miss);
    SUITE_ADD_TEST(suite, test_bson_iter_find_miss_keeps_iter);
    SUITE_ADD_TEST(suite, test_bson_iter_find_self_alias);
    SUITE_ADD_TEST(suite, test_bson_iter_find_empty_key);
    SUITE_ADD_TEST(suite, test_bson_cat_self_alias);
    SUITE_ADD_TEST(suite, test_bson_complete_cat);
    SUITE_ADD_TEST(suite, test_bson_cat_bounds);
    SUITE_ADD_TEST(suite, test_bson_extra_types);
    SUITE_ADD_TEST(suite, test_bson_check_depth);
    SUITE_ADD_TEST(suite, test_bson_iter_neg_lens);
    SUITE_ADD_TEST(suite, test_bson_iter_field_exceeds_doclens);
    SUITE_ADD_TEST(suite, test_bson_iter_binary_subtype_bound);
    SUITE_ADD_TEST(suite, test_bson_iter_decimal128_bound);
    SUITE_ADD_TEST(suite, test_bson_iter_eod_reserved);
    SUITE_ADD_TEST(suite, test_bson_iter_utf8_no_terminator);
    SUITE_ADD_TEST(suite, test_bson_check_depth_boundary);
    SUITE_ADD_TEST(suite, test_bson_check_depth_malformed);
    SUITE_ADD_TEST(suite, test_bson_truncated_and_reset);
    SUITE_ADD_TEST(suite, test_bson_iter_reset_short_buffer);
    SUITE_ADD_TEST(suite, test_bson_iter_init_offset_reset);
    SUITE_ADD_TEST(suite, test_bson_null_data_entry);
    SUITE_ADD_TEST(suite, test_bson_binary_zero_length);
    SUITE_ADD_TEST(suite, test_bson_iter_error_flag);
    SUITE_ADD_TEST(suite, test_bson_tostring);
    SUITE_ADD_TEST(suite, test_bson_tostring_subtypes);
    SUITE_ADD_TEST(suite, test_bson_misc);
    SUITE_ADD_TEST(suite, test_bson_find_dotted_iter_continue);
    SUITE_ADD_TEST(suite, test_bson_wire_layout);
    SUITE_ADD_TEST(suite, test_bson_key_lens_scan);
    SUITE_ADD_TEST(suite, test_bson_key_no_nul);
    SUITE_ADD_TEST(suite, test_bson_append_self_alias);
    SUITE_ADD_TEST(suite, test_bson_append_regex_self_alias);
    SUITE_ADD_TEST(suite, test_bson_init_prefix);
    SUITE_ADD_TEST(suite, test_bson_tostring_hex_exact);
}
