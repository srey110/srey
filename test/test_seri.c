#include "test_seri.h"
#include "lib.h"
#include "serial/yyjson/yyjson_helper.h"

// nil / true / false 三元基础往返
static void test_seri_basic_nil_bool(CuTest *tc) {
    binary_ctx bw;
    binary_init_write(&bw, 0, 0);
    seri_append_nil(&bw);
    seri_append_bool(&bw, 1);
    seri_append_bool(&bw, 0);

    seri_iter iter;
    seri_item item;
    seri_iter_init(&iter, bw.data, bw.offset);
    CuAssertIntEquals(tc, 1, seri_iter_next(&iter, &item));
    CuAssertIntEquals(tc, SERI_ITEM_NIL, item.type);
    CuAssertIntEquals(tc, 1, seri_iter_next(&iter, &item));
    CuAssertIntEquals(tc, SERI_ITEM_BOOL, item.type);
    CuAssertIntEquals(tc, 1, item.v.b);
    CuAssertIntEquals(tc, 1, seri_iter_next(&iter, &item));
    CuAssertIntEquals(tc, SERI_ITEM_BOOL, item.type);
    CuAssertIntEquals(tc, 0, item.v.b);
    CuAssertIntEquals(tc, 0, seri_iter_next(&iter, &item));

    binary_free(&bw);
}
// 整数各档边界：0 / byte / word / dword(u32) / dword(neg i32) / qword
static void test_seri_int_buckets(CuTest *tc) {
    int64_t vals[] = {
        0,
        1, 0xFF,// BYTE 边界
        0x100, 0xFFFF,// WORD 边界
        0x10000, 0xFFFFFFFF,// DWORD(u32) 边界
        -1, INT32_MIN,// DWORD(i32) 负数
        ((int64_t)INT32_MAX) + 1,// QWORD 正越界
        INT64_MIN, INT64_MAX// QWORD 端点
    };
    size_t n = sizeof(vals) / sizeof(vals[0]);
    binary_ctx bw;
    binary_init_write(&bw, 0, 0);
    size_t i;
    for (i = 0; i < n; i++) {
        seri_append_int(&bw, vals[i]);
    }

    seri_iter iter;
    seri_item item;
    seri_iter_init(&iter, bw.data, bw.offset);
    for (i = 0; i < n; i++) {
        CuAssertIntEquals(tc, 1, seri_iter_next(&iter, &item));
        CuAssertIntEquals(tc, SERI_ITEM_INT, item.type);
        CuAssertTrue(tc, vals[i] == item.v.i);
    }
    CuAssertIntEquals(tc, 0, seri_iter_next(&iter, &item));

    binary_free(&bw);
}
// 实数精度往返
static void test_seri_real(CuTest *tc) {
    binary_ctx bw;
    binary_init_write(&bw, 0, 0);
    seri_append_real(&bw, 3.14159265358979);
    seri_append_real(&bw, -1.0e-300);
    seri_append_real(&bw, 0.0);

    seri_iter iter;
    seri_item item;
    seri_iter_init(&iter, bw.data, bw.offset);
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, SERI_ITEM_REAL, item.type);
    CuAssertDblEquals(tc, 3.14159265358979, item.v.r, 1e-12);
    seri_iter_next(&iter, &item);
    CuAssertDblEquals(tc, -1.0e-300, item.v.r, 1e-310);
    seri_iter_next(&iter, &item);
    CuAssertDblEquals(tc, 0.0, item.v.r, 0.0);

    binary_free(&bw);
}
// 字符串短/长边界：长度 0 / 1 / 31(短) / 32(长 u16) 临界，含二进制 NUL
static void test_seri_string(CuTest *tc) {
    binary_ctx bw;
    binary_init_write(&bw, 0, 0);
    seri_append_string(&bw, "", 0);
    seri_append_string(&bw, "a", 1);
    char s31[31];
    memset(s31, 'x', 31);
    seri_append_string(&bw, s31, 31);
    char s32[32];
    memset(s32, 'y', 32);
    seri_append_string(&bw, s32, 32);
    // 二进制安全：含 NUL
    char bin[5] = {'\0', 'A', '\0', 'B', '\0'};
    seri_append_string(&bw, bin, 5);

    seri_iter iter;
    seri_item item;
    seri_iter_init(&iter, bw.data, bw.offset);
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, SERI_ITEM_STRING, item.type);
    CuAssertIntEquals(tc, 0, (int32_t)item.v.s.len);
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, 1, (int32_t)item.v.s.len);
    CuAssertIntEquals(tc, 'a', item.v.s.p[0]);
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, 31, (int32_t)item.v.s.len);
    CuAssertTrue(tc, 0 == memcmp(item.v.s.p, s31, 31));
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, 32, (int32_t)item.v.s.len);
    CuAssertTrue(tc, 0 == memcmp(item.v.s.p, s32, 32));
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, 5, (int32_t)item.v.s.len);
    CuAssertTrue(tc, 0 == memcmp(item.v.s.p, bin, 5));

    binary_free(&bw);
}
// userdata 指针往返
static void test_seri_userdata(CuTest *tc) {
    binary_ctx bw;
    binary_init_write(&bw, 0, 0);
    int local_var = 0;
    void *p1 = &local_var;
    void *p2 = (void *)(uintptr_t)0xCAFEBABE12345678ULL;
    seri_append_userdata(&bw, p1);
    seri_append_userdata(&bw, p2);

    seri_iter iter;
    seri_item item;
    seri_iter_init(&iter, bw.data, bw.offset);
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, SERI_ITEM_USERDATA, item.type);
    CuAssertPtrEquals(tc, p1, item.v.ud);
    seri_iter_next(&iter, &item);
    CuAssertPtrEquals(tc, p2, item.v.ud);

    binary_free(&bw);
}
// 简单 array：数组段 [10, 20]，hash 段 {"k1"=true}
static void test_seri_array_simple(CuTest *tc) {
    binary_ctx bw;
    binary_init_write(&bw, 0, 0);
    seri_append_array_start(&bw, 2);
    seri_append_int(&bw, 10);
    seri_append_int(&bw, 20);
    seri_append_string(&bw, "k1", 2);
    seri_append_bool(&bw, 1);
    seri_append_array_end(&bw);

    seri_iter iter;
    seri_item item;
    seri_iter_init(&iter, bw.data, bw.offset);
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, SERI_ITEM_ARRAY_BEGIN, item.type);
    CuAssertIntEquals(tc, 2, (int32_t)item.v.array_n);
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, SERI_ITEM_INT, item.type);
    CuAssertTrue(tc, 10 == item.v.i);
    seri_iter_next(&iter, &item);
    CuAssertTrue(tc, 20 == item.v.i);
    // hash 段
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, SERI_ITEM_STRING, item.type);
    CuAssertIntEquals(tc, 2, (int32_t)item.v.s.len);
    CuAssertTrue(tc, 0 == memcmp(item.v.s.p, "k1", 2));
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, SERI_ITEM_BOOL, item.type);
    CuAssertIntEquals(tc, 1, item.v.b);
    // hash 段结束（NIL 标记）
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, SERI_ITEM_NIL, item.type);
    CuAssertIntEquals(tc, 0, seri_iter_next(&iter, &item));

    binary_free(&bw);
}
// 长 array 转义：array_n >= 31 时 cookie=31 后跟 INT 真实长度
static void test_seri_array_long(CuTest *tc) {
    binary_ctx bw;
    binary_init_write(&bw, 0, 0);
    uint32_t n = 100;
    seri_append_array_start(&bw, n);
    uint32_t i;
    for (i = 0; i < n; i++) {
        seri_append_int(&bw, (int64_t)i);
    }
    seri_append_array_end(&bw);

    seri_iter iter;
    seri_item item;
    seri_iter_init(&iter, bw.data, bw.offset);
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, SERI_ITEM_ARRAY_BEGIN, item.type);
    CuAssertIntEquals(tc, (int32_t)n, (int32_t)item.v.array_n);
    for (i = 0; i < n; i++) {
        seri_iter_next(&iter, &item);
        CuAssertTrue(tc, (int64_t)i == item.v.i);
    }
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, SERI_ITEM_NIL, item.type);

    binary_free(&bw);
}
// 嵌套 array：外层 [42, inner_array]，inner = {3.14, "hi"}
static void test_seri_array_nested(CuTest *tc) {
    binary_ctx bw;
    binary_init_write(&bw, 0, 0);
    seri_append_array_start(&bw, 2);
    seri_append_int(&bw, 42);
    seri_append_array_start(&bw, 2);// 嵌套
    seri_append_real(&bw, 3.14);
    seri_append_string(&bw, "hi", 2);
    seri_append_array_end(&bw);// 内层 end
    seri_append_array_end(&bw);// 外层 end

    seri_iter iter;
    seri_item item;
    seri_iter_init(&iter, bw.data, bw.offset);
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, SERI_ITEM_ARRAY_BEGIN, item.type);
    CuAssertIntEquals(tc, 2, (int32_t)item.v.array_n);
    seri_iter_next(&iter, &item);
    CuAssertTrue(tc, 42 == item.v.i);
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, SERI_ITEM_ARRAY_BEGIN, item.type);
    CuAssertIntEquals(tc, 2, (int32_t)item.v.array_n);
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, SERI_ITEM_REAL, item.type);
    CuAssertDblEquals(tc, 3.14, item.v.r, 1e-9);
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, SERI_ITEM_STRING, item.type);
    CuAssertIntEquals(tc, 2, (int32_t)item.v.s.len);
    // 返回码必须判：到流尾时返回 0 且不写 *out，item 留着上一轮的值。
    // 不判的话外层 array_end 丢了也照样读到上一条留下的 NIL
    CuAssertIntEquals(tc, 1, seri_iter_next(&iter, &item));
    CuAssertIntEquals(tc, SERI_ITEM_NIL, item.type);// 内层 end
    item.type = SERI_ITEM_INT;// 打脏，逼外层 end 自己写回来
    CuAssertIntEquals(tc, 1, seri_iter_next(&iter, &item));
    CuAssertIntEquals(tc, SERI_ITEM_NIL, item.type);// 外层 end
    // 两个 end 之后流已尽：返回 0，且不再写 item
    item.type = SERI_ITEM_INT;
    CuAssertIntEquals(tc, 0, seri_iter_next(&iter, &item));
    CuAssertIntEquals(tc, SERI_ITEM_INT, item.type);

    binary_free(&bw);
}
// 错误流：截断 buffer / 非法 tag，iter_next 返回 -1
static void test_seri_invalid_stream(CuTest *tc) {
    // 构造一个完整 INT(QWORD) 然后截掉一半
    binary_ctx bw;
    binary_init_write(&bw, 0, 0);
    seri_append_int(&bw, 0x1234567890ABCDEFLL);

    seri_iter iter;
    seri_item item;
    seri_iter_init(&iter, bw.data, bw.offset - 3);// 截掉末尾 3 字节
    CuAssertIntEquals(tc, -1, seri_iter_next(&iter, &item));
    binary_free(&bw);

    // 非法 tag：低 3 位 = 7（未定义类型）
    char bad = 0x07;
    seri_iter_init(&iter, &bad, 1);
    CuAssertIntEquals(tc, -1, seri_iter_next(&iter, &item));
}

// json_get_string：长度按 yyjson 记的真实字节数算,不是 strlen;内嵌 NUL 一律拒收。
// json_has 用来把"字段没配"与"配错了"分开——可选字段缺席不该跟着报错
static void test_json_helper_string(CuTest *tc) {
    const char *src = "{\"ok\":\"abc\",\"nul\":\"sc\\u0000ript\",\"num\":7}";
    yyjson_doc *doc = yyjson_read(src, strlen(src), 0);
    CuAssertPtrNotNull(tc, doc);
    yyjson_val *root = yyjson_doc_get_root(doc);

    char buf[8];
    memset(buf, 'x', sizeof(buf));
    CuAssertIntEquals(tc, ERR_OK, json_get_string(root, "ok", buf, sizeof(buf)));
    CuAssertStrEquals(tc, "abc", buf);

    // 真实长度 7,strlen 只看到 2:旧实现会当成装得下并悄悄截成 "sc"
    memset(buf, 'x', sizeof(buf));
    CuAssertIntEquals(tc, ERR_FAILED, json_get_string(root, "nul", buf, sizeof(buf)));
    CuAssertTrue(tc, 'x' == buf[0]);// 失败不写 dst

    // 缓冲装不下:值 3 字节 + NUL 要 4,给 3
    CuAssertIntEquals(tc, ERR_FAILED, json_get_string(root, "ok", buf, 3));
    // 字段不存在 / 类型不符
    CuAssertIntEquals(tc, ERR_FAILED, json_get_string(root, "missing", buf, sizeof(buf)));
    CuAssertIntEquals(tc, ERR_FAILED, json_get_string(root, "num", buf, sizeof(buf)));

    CuAssertTrue(tc, 0 != json_has(root, "ok"));
    CuAssertTrue(tc, 0 != json_has(root, "num"));
    CuAssertTrue(tc, 0 == json_has(root, "missing"));
    yyjson_doc_free(doc);
}
// json_get_number 过滤 NaN/Inf、json_get_num_range 卡上下界：配置里的数字最终都要 cast 成
// uint16_t/uint8_t 这类窄整型，而 C99 §6.3.1.4 规定 NaN/Inf 与超范围值转整型均为 UB。
// 两个函数是 srey/main.c 的 CFG_NUM 的唯一依赖，失败时一律不得动 *val
static void test_json_helper_number(CuTest *tc) {
    const char *src = "{\"i\":7,\"f\":2.5,\"big\":70000,\"neg\":-1,\"s\":\"x\"}";
    yyjson_doc *doc = yyjson_read(src, strlen(src), 0);
    CuAssertPtrNotNull(tc, doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    double val = -12345;
    CuAssertIntEquals(tc, ERR_OK, json_get_number(root, "i", &val));
    CuAssertDblEquals(tc, 7, val, 0);
    CuAssertIntEquals(tc, ERR_OK, json_get_number(root, "f", &val));
    CuAssertDblEquals(tc, 2.5, val, 0);
    // 字段缺席与类型不符都算失败，且不许动 *val
    val = -12345;
    CuAssertIntEquals(tc, ERR_FAILED, json_get_number(root, "missing", &val));
    CuAssertIntEquals(tc, ERR_FAILED, json_get_number(root, "s", &val));
    CuAssertDblEquals(tc, -12345, val, 0);
    // 恰在界上合法（上下界都是闭区间），界外拒收
    CuAssertIntEquals(tc, ERR_OK, json_get_num_range(root, "i", 7, 7, &val));
    CuAssertDblEquals(tc, 7, val, 0);
    val = -12345;
    CuAssertIntEquals(tc, ERR_FAILED, json_get_num_range(root, "big", 0, UINT16_MAX, &val));
    CuAssertIntEquals(tc, ERR_FAILED, json_get_num_range(root, "neg", 0, UINT16_MAX, &val));
    CuAssertDblEquals(tc, -12345, val, 0);
    yyjson_doc_free(doc);
    // NaN / Inf 得开 ALLOW_INF_AND_NAN 才解得出来。srey 读配置时不开这个 flag，所以这里验的
    // 是 json_get_number 自己那道 isnan/isinf，而不是 yyjson 的默认拒收
    const char *bad = "{\"nan\":NaN,\"inf\":Infinity,\"ninf\":-Infinity}";
    yyjson_doc *bdoc = yyjson_read(bad, strlen(bad), YYJSON_READ_ALLOW_INF_AND_NAN);
    CuAssertPtrNotNull(tc, bdoc);
    yyjson_val *broot = yyjson_doc_get_root(bdoc);
    val = -12345;
    CuAssertIntEquals(tc, ERR_FAILED, json_get_number(broot, "nan", &val));
    CuAssertIntEquals(tc, ERR_FAILED, json_get_number(broot, "inf", &val));
    CuAssertIntEquals(tc, ERR_FAILED, json_get_number(broot, "ninf", &val));
    CuAssertDblEquals(tc, -12345, val, 0);
    yyjson_doc_free(bdoc);
}
// 字符串长度三档的边界：0 / 31 走短串（tag 自带长度），32 / 65535 走 u16 长度前缀，65536 走 u32。
// 逐字节核对 tag 与长度前缀（wire 格式对外，不能只靠往返自洽），再读回比对内容。
// tag = 类型 | (cookie << 3)：短串类型 4，长串类型 5、cookie 为前缀字节数
static void test_seri_string_lens_boundary(CuTest *tc) {
    const size_t lens[] = { 0, 31, 32, 65535, 65536 };
    const size_t hdr[] = { 1, 1, 3, 3, 5 };
    const uint8_t tag[] = { 0x04, 0x04 | (31 << 3), 0x05 | (2 << 3), 0x05 | (2 << 3), 0x05 | (4 << 3) };
    binary_ctx bw;
    seri_iter iter;
    seri_item item;
    size_t i, j;
    char *src;

    MALLOC(src, 65536);
    for (j = 0; j < 65536; j++) {
        src[j] = (char)(j * 31 + 7);// 含 0 字节，顺带验二进制安全
    }
    for (i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        binary_init_write(&bw, 0, 0);
        seri_append_string(&bw, 0 == lens[i] ? NULL : src, lens[i]);// len==0 时 s 可为 NULL
        CuAssertTrue(tc, hdr[i] + lens[i] == bw.offset);
        CuAssertIntEquals(tc, tag[i], (uint8_t)bw.data[0]);
        if (hdr[i] > 1) {
            CuAssertTrue(tc, (int64_t)lens[i] == unpack_integer(bw.data + 1, (int32_t)(hdr[i] - 1), 1, 0));
        }
        CuAssertTrue(tc, 0 == memcmp(bw.data + hdr[i], src, lens[i]));

        seri_iter_init(&iter, bw.data, bw.offset);
        CuAssertIntEquals(tc, 1, seri_iter_next(&iter, &item));
        CuAssertIntEquals(tc, SERI_ITEM_STRING, item.type);
        CuAssertTrue(tc, lens[i] == item.v.s.len);
        CuAssertTrue(tc, 0 == memcmp(item.v.s.p, src, lens[i]));
        CuAssertIntEquals(tc, 0, seri_iter_next(&iter, &item));
        binary_free(&bw);
    }
    FREE(src);
}
// seri_append_string 的 s 指向 bw 自己的缓冲：先写一个源串，再把余量压到装不下下一条，
// 让自指追加必然扩容（ASan 下 realloc 必换地址）。结果须与"扩容前快照 + 独立拷贝追加"逐字节一致。
// 扩容条件是 need + offset + 1 > size，即余量 <= need
static void test_seri_string_self_alias(CuTest *tc) {
    const size_t lens[] = { 5, 31, 32, 300, 65536 };
    const size_t hdr[] = { 1, 1, 3, 3, 5 };
    binary_ctx bw, ref;
    seri_iter iter;
    seri_item item;
    size_t i, j, need, oldsize;
    char *src;
    char *copy;

    MALLOC(src, 65536);
    for (j = 0; j < 65536; j++) {
        src[j] = (char)(j * 13 + 1);
    }
    for (i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        binary_init_write(&bw, 0, 0);
        seri_append_string(&bw, src, lens[i]);
        CuAssertTrue(tc, 0 == memcmp(bw.data + hdr[i], src, lens[i]));
        need = hdr[i] + lens[i];
        while (bw.size - bw.offset > need) {
            seri_append_nil(&bw);
        }
        binary_init_write(&ref, 0, 0);
        binary_set_binary(&ref, bw.data, bw.offset);
        MALLOC(copy, lens[i]);
        memcpy(copy, bw.data + hdr[i], lens[i]);
        oldsize = bw.size;
        seri_append_string(&bw, bw.data + hdr[i], lens[i]);
        CuAssertTrue(tc, bw.size > oldsize);
        seri_append_string(&ref, copy, lens[i]);
        CuAssertTrue(tc, ref.offset == bw.offset);
        CuAssertTrue(tc, 0 == memcmp(ref.data, bw.data, bw.offset));
        // 流里首尾两个串都是源串，中间是填充的 nil
        seri_iter_init(&iter, bw.data, bw.offset);
        CuAssertIntEquals(tc, 1, seri_iter_next(&iter, &item));
        CuAssertTrue(tc, lens[i] == item.v.s.len);
        CuAssertTrue(tc, 0 == memcmp(item.v.s.p, src, lens[i]));
        do {
            CuAssertIntEquals(tc, 1, seri_iter_next(&iter, &item));
        } while (SERI_ITEM_NIL == item.type);
        CuAssertIntEquals(tc, SERI_ITEM_STRING, item.type);
        CuAssertTrue(tc, lens[i] == item.v.s.len);
        CuAssertTrue(tc, 0 == memcmp(item.v.s.p, src, lens[i]));
        CuAssertIntEquals(tc, 0, seri_iter_next(&iter, &item));
        FREE(copy);
        binary_free(&ref);
        binary_free(&bw);
    }
    // 容量足够、不扩容时自指同样正确
    binary_init_write(&bw, 1 << 16, 0);
    seri_append_string(&bw, src, 300);
    oldsize = bw.size;
    seri_append_string(&bw, bw.data + 3, 300);
    CuAssertTrue(tc, oldsize == bw.size);
    CuAssertTrue(tc, 2 * (3 + 300) == bw.offset);
    CuAssertTrue(tc, 0 == memcmp(bw.data, bw.data + 303, 303));
    binary_free(&bw);
    FREE(src);
}
// json_val_number / json_val_num_range / json_val_string：直接收已取出的值，NULL 按字段不存在处理。
// 失败一律不动出参；与按名查找的 json_get_* 同口径；值来自 yyjson_obj_iter_get（乱序查）也照常可用
static void test_json_helper_val(CuTest *tc) {
    const char *src = "{\"u\":7,\"neg\":-3,\"f\":2.5,\"big\":70000,\"s\":\"abc\",\"nul\":\"a\\u0000b\","
                      "\"b\":true,\"n\":null,\"o\":{},\"a\":[1]}";
    const char *nonnum[] = { "s", "nul", "b", "n", "o", "a" };
    const char *keys[] = { "u", "neg", "f", "big", "s", "nul", "b", "n", "o", "a", "missing" };
    const char *bad = "{\"nan\":NaN,\"inf\":Infinity,\"ninf\":-Infinity}";
    yyjson_doc *doc;
    yyjson_doc *bdoc;
    yyjson_val *root;
    yyjson_val *broot;
    yyjson_obj_iter it;
    char buf[8];
    char gbuf[8];
    double val, gval;
    size_t i;

    doc = yyjson_read(src, strlen(src), 0);
    CuAssertPtrNotNull(tc, doc);
    root = yyjson_doc_get_root(doc);

    // NULL
    val = -12345;
    CuAssertIntEquals(tc, ERR_FAILED, json_val_number(NULL, &val));
    CuAssertIntEquals(tc, ERR_FAILED, json_val_num_range(NULL, 0, 100, &val));
    CuAssertDblEquals(tc, -12345, val, 0);
    memset(buf, 'x', sizeof(buf));
    CuAssertIntEquals(tc, ERR_FAILED, json_val_string(NULL, buf, sizeof(buf)));
    CuAssertTrue(tc, 'x' == buf[0]);

    // 非数字：字符串 / 布尔 / null / 对象 / 数组
    for (i = 0; i < sizeof(nonnum) / sizeof(nonnum[0]); i++) {
        CuAssertIntEquals(tc, ERR_FAILED, json_val_number(yyjson_obj_get(root, nonnum[i]), &val));
        CuAssertIntEquals(tc, ERR_FAILED, json_val_num_range(yyjson_obj_get(root, nonnum[i]), -1e9, 1e9, &val));
    }
    CuAssertDblEquals(tc, -12345, val, 0);

    // 正常值：无符号 / 有符号 / 浮点
    CuAssertIntEquals(tc, ERR_OK, json_val_number(yyjson_obj_get(root, "u"), &val));
    CuAssertDblEquals(tc, 7, val, 0);
    CuAssertIntEquals(tc, ERR_OK, json_val_number(yyjson_obj_get(root, "neg"), &val));
    CuAssertDblEquals(tc, -3, val, 0);
    CuAssertIntEquals(tc, ERR_OK, json_val_number(yyjson_obj_get(root, "f"), &val));
    CuAssertDblEquals(tc, 2.5, val, 0);

    // 区间是闭的：恰在界上合法
    CuAssertIntEquals(tc, ERR_OK, json_val_num_range(yyjson_obj_get(root, "u"), 7, 7, &val));
    CuAssertDblEquals(tc, 7, val, 0);
    CuAssertIntEquals(tc, ERR_OK, json_val_num_range(yyjson_obj_get(root, "neg"), -3, 0, &val));
    CuAssertDblEquals(tc, -3, val, 0);
    CuAssertIntEquals(tc, ERR_OK, json_val_num_range(yyjson_obj_get(root, "big"), 0, 70000, &val));
    CuAssertDblEquals(tc, 70000, val, 0);
    // 越界：超上界 / 低于下界 / 浮点刚过界
    val = -12345;
    CuAssertIntEquals(tc, ERR_FAILED, json_val_num_range(yyjson_obj_get(root, "big"), 0, UINT16_MAX, &val));
    CuAssertIntEquals(tc, ERR_FAILED, json_val_num_range(yyjson_obj_get(root, "neg"), 0, UINT16_MAX, &val));
    CuAssertIntEquals(tc, ERR_FAILED, json_val_num_range(yyjson_obj_get(root, "f"), 0, 2.4, &val));
    CuAssertIntEquals(tc, ERR_FAILED, json_val_num_range(yyjson_obj_get(root, "f"), 2.6, 3, &val));
    CuAssertDblEquals(tc, -12345, val, 0);

    // 字符串：非字符串 / 内嵌 NUL / 装不下 / 缓冲为 0 都失败且不写 buf；恰好装下（含 NUL 共 4 字节）成功
    memset(buf, 'x', sizeof(buf));
    CuAssertIntEquals(tc, ERR_FAILED, json_val_string(yyjson_obj_get(root, "u"), buf, sizeof(buf)));
    CuAssertIntEquals(tc, ERR_FAILED, json_val_string(yyjson_obj_get(root, "n"), buf, sizeof(buf)));
    CuAssertIntEquals(tc, ERR_FAILED, json_val_string(yyjson_obj_get(root, "nul"), buf, sizeof(buf)));
    CuAssertIntEquals(tc, ERR_FAILED, json_val_string(yyjson_obj_get(root, "s"), buf, 3));
    CuAssertIntEquals(tc, ERR_FAILED, json_val_string(yyjson_obj_get(root, "s"), buf, 0));
    CuAssertTrue(tc, 'x' == buf[0]);
    CuAssertIntEquals(tc, ERR_OK, json_val_string(yyjson_obj_get(root, "s"), buf, 4));
    CuAssertStrEquals(tc, "abc", buf);

    // 与按名查找的包装版口径一致：返回码相同，成功时取到的值相同
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        val = -1;
        gval = -1;
        CuAssertIntEquals(tc, json_get_number(root, keys[i], &gval), json_val_number(yyjson_obj_get(root, keys[i]), &val));
        CuAssertDblEquals(tc, gval, val, 0);
        val = -1;
        gval = -1;
        CuAssertIntEquals(tc, json_get_num_range(root, keys[i], 0, UINT16_MAX, &gval),
                          json_val_num_range(yyjson_obj_get(root, keys[i]), 0, UINT16_MAX, &val));
        CuAssertDblEquals(tc, gval, val, 0);
        memset(buf, 'x', sizeof(buf));
        memset(gbuf, 'x', sizeof(gbuf));
        CuAssertIntEquals(tc, json_get_string(root, keys[i], gbuf, sizeof(gbuf)),
                          json_val_string(yyjson_obj_get(root, keys[i]), buf, sizeof(buf)));
        CuAssertTrue(tc, 0 == memcmp(gbuf, buf, sizeof(buf)));
    }

    // 值来自 yyjson_obj_iter_get：先查靠后的键再查靠前的（要绕回开头），缺席的键给 NULL
    CuAssertTrue(tc, yyjson_obj_iter_init(root, &it));
    CuAssertIntEquals(tc, ERR_OK, json_val_string(yyjson_obj_iter_get(&it, "s"), buf, sizeof(buf)));
    CuAssertStrEquals(tc, "abc", buf);
    CuAssertIntEquals(tc, ERR_OK, json_val_num_range(yyjson_obj_iter_get(&it, "u"), 0, UINT16_MAX, &val));
    CuAssertDblEquals(tc, 7, val, 0);
    CuAssertIntEquals(tc, ERR_OK, json_val_number(yyjson_obj_iter_get(&it, "f"), &val));
    CuAssertDblEquals(tc, 2.5, val, 0);
    CuAssertTrue(tc, NULL == yyjson_obj_iter_get(&it, "missing"));
    val = -12345;
    CuAssertIntEquals(tc, ERR_FAILED, json_val_num_range(yyjson_obj_iter_get(&it, "missing"), 0, 1, &val));
    CuAssertDblEquals(tc, -12345, val, 0);
    yyjson_doc_free(doc);

    // NaN / Inf：json_val_number 自己那道 isnan/isinf 拦下
    bdoc = yyjson_read(bad, strlen(bad), YYJSON_READ_ALLOW_INF_AND_NAN);
    CuAssertPtrNotNull(tc, bdoc);
    broot = yyjson_doc_get_root(bdoc);
    val = -12345;
    CuAssertIntEquals(tc, ERR_FAILED, json_val_number(yyjson_obj_get(broot, "nan"), &val));
    CuAssertIntEquals(tc, ERR_FAILED, json_val_number(yyjson_obj_get(broot, "inf"), &val));
    CuAssertIntEquals(tc, ERR_FAILED, json_val_number(yyjson_obj_get(broot, "ninf"), &val));
    CuAssertIntEquals(tc, ERR_FAILED, json_val_num_range(yyjson_obj_get(broot, "inf"), -INFINITY, INFINITY, &val));
    CuAssertDblEquals(tc, -12345, val, 0);
    yyjson_doc_free(bdoc);
}
void test_seri(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_seri_basic_nil_bool);
    SUITE_ADD_TEST(suite, test_seri_int_buckets);
    SUITE_ADD_TEST(suite, test_seri_real);
    SUITE_ADD_TEST(suite, test_seri_string);
    SUITE_ADD_TEST(suite, test_seri_userdata);
    SUITE_ADD_TEST(suite, test_seri_array_simple);
    SUITE_ADD_TEST(suite, test_seri_array_long);
    SUITE_ADD_TEST(suite, test_seri_array_nested);
    SUITE_ADD_TEST(suite, test_seri_invalid_stream);
    SUITE_ADD_TEST(suite, test_json_helper_string);
    SUITE_ADD_TEST(suite, test_json_helper_number);
    SUITE_ADD_TEST(suite, test_seri_string_lens_boundary);
    SUITE_ADD_TEST(suite, test_seri_string_self_alias);
    SUITE_ADD_TEST(suite, test_json_helper_val);
}
