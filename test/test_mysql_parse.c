#include "test_mysql_parse.h"
#include "lib.h"
#include "protocol/mysql/mysql_parse.h"
#include "protocol/mysql/mysql_reader.h"
#include "protocol/mysql/mysql_utils.h"
#include "protocol/mysql/mysql_macro.h"
#include "protocol/mysql/mysql_struct.h"

#ifdef _WIN32
#pragma warning(disable:4312)
#endif

// 构造一个最小可用的 mysql_reader_ctx：指定 pack_type、field 列表，便于后续 push 行数据
static mysql_reader_ctx *_reader_new(mpack_type pktype, int32_t field_count,
                                     const char (*names)[64], const uint8_t *types) {
    mysql_reader_ctx *reader;
    CALLOC(reader, 1, sizeof(*reader));
    reader->pack_type = pktype;
    reader->field_count = field_count;
    if (field_count > 0) {
        CALLOC(reader->fields, 1, sizeof(mpack_field) * (size_t)field_count);
        for (int32_t i = 0; i < field_count; i++) {
            safe_fill_str(reader->fields[i].name, sizeof(reader->fields[i].name), names[i]);
            reader->fields[i].type = types[i];
        }
    }
    array_init(&reader->arr_rows, sizeof(mpack_row *), 16);
    return reader;
}

// 向 reader 追加一行：payload 作为整行的内存所有者，每个 row[i] 引用 payload 中的某段
// payload 由 reader 释放（_mpack_reader_free 内 FREE(rows->payload)）
static void _reader_push_row(mysql_reader_ctx *reader, char *payload,
                             const buf_ctx *cols, const int32_t *nils) {
    mpack_row *row;
    CALLOC(row, 1, sizeof(mpack_row) * (size_t)reader->field_count);
    row[0].payload = payload;
    for (int32_t i = 0; i < reader->field_count; i++) {
        row[i].nil = nils ? nils[i] : 0;
        if (!row[i].nil && cols) {
            row[i].val = cols[i];
        }
    }
    array_push_back(&reader->arr_rows, &row);
}

// mysql_reader_init: MPACK_OK/ERR/STMT_PREPARE 返回 NULL；MPACK_QUERY/STMT_EXECUTE 成功转移所有权
static void test_mysql_reader_init(CuTest *tc) {
    // pack_type 不匹配 → NULL
    mpack_ctx p;
    ZERO(&p, sizeof(p));
    p.pack_type = MPACK_OK;
    p.pack = (void *)1; // 非 NULL 也应被 pack_type 拒绝
    CuAssertTrue(tc, NULL == mysql_reader_init(&p));

    p.pack_type = MPACK_ERR;
    CuAssertTrue(tc, NULL == mysql_reader_init(&p));

    p.pack_type = MPACK_STMT_PREPARE;
    CuAssertTrue(tc, NULL == mysql_reader_init(&p));

    // MPACK_QUERY + pack=NULL → NULL
    p.pack_type = MPACK_QUERY;
    p.pack = NULL;
    CuAssertTrue(tc, NULL == mysql_reader_init(&p));

    // MPACK_QUERY + pack 有效 → 转移所有权，pack/_free_mpack 被置 NULL
    char names[1][64] = { "id" };
    uint8_t types[1] = { MYSQL_TYPE_LONGLONG };
    mysql_reader_ctx *r = _reader_new(MPACK_QUERY, 1, names, types);
    p.pack_type = MPACK_QUERY;
    p.pack = r;
    p._free_mpack = (void (*)(void *))0xdeadbeef; // 毒值:本路径不应触发此 free,误调即崩
    mysql_reader_ctx *out = mysql_reader_init(&p);
    CuAssertTrue(tc, out == r);
    CuAssertTrue(tc, NULL == p.pack);
    CuAssertTrue(tc, NULL == p._free_mpack);
    mysql_reader_free(out);
}

// mysql_reader_size/seek/eof/next 游标语义
static void test_mysql_reader_cursor(CuTest *tc) {
    char names[1][64] = { "id" };
    uint8_t types[1] = { MYSQL_TYPE_LONGLONG };
    mysql_reader_ctx *r = _reader_new(MPACK_QUERY, 1, names, types);

    // 空 reader：size=0，eof=1
    CuAssertIntEquals(tc, 0, (int)mysql_reader_size(r));
    CuAssertIntEquals(tc, 1, mysql_reader_eof(r));

    // 3 行数据（每行 payload "1"/"2"/"3"）
    for (int i = 0; i < 3; i++) {
        char *p;
        MALLOC(p, 4);
        p[0] = (char)('1' + i);
        p[1] = '\0';
        buf_ctx cols[1] = { { .data = p, .lens = 1 } };
        _reader_push_row(r, p, cols, NULL);
    }
    CuAssertIntEquals(tc, 3, (int)mysql_reader_size(r));
    CuAssertIntEquals(tc, 0, mysql_reader_eof(r));

    // next 三次到末尾
    mysql_reader_next(r);
    CuAssertIntEquals(tc, 1, r->index);
    mysql_reader_next(r);
    mysql_reader_next(r);
    CuAssertIntEquals(tc, 3, r->index);
    CuAssertIntEquals(tc, 1, mysql_reader_eof(r));

    // next 越界后继续 next 不再递增
    mysql_reader_next(r);
    CuAssertIntEquals(tc, 3, r->index);

    // seek 越界被忽略
    mysql_reader_seek(r, 99);
    CuAssertIntEquals(tc, 3, r->index);
    // seek 合法位置
    mysql_reader_seek(r, 1);
    CuAssertIntEquals(tc, 1, r->index);
    CuAssertIntEquals(tc, 0, mysql_reader_eof(r));

    mysql_reader_free(r);
}

// mysql_reader_integer 文本协议：strtoll 解析 + 类型不匹配 + nil
static void test_mysql_reader_integer_text(CuTest *tc) {
    char names[3][64] = { "a", "b", "c" };
    uint8_t types[3] = { MYSQL_TYPE_LONGLONG, MYSQL_TYPE_VARCHAR, MYSQL_TYPE_LONG };
    mysql_reader_ctx *r = _reader_new(MPACK_QUERY, 3, names, types);

    // 行 1：a=42, b="x", c=NULL
    char *p1;
    MALLOC(p1, 32);
    memcpy(p1, "42", 2);
    memcpy(p1 + 2, "x", 1);
    buf_ctx c1[3] = { { .data = p1, .lens = 2 }, { .data = p1 + 2, .lens = 1 }, { .data = NULL, .lens = 0 } };
    int32_t n1[3] = { 0, 0, 1 };
    _reader_push_row(r, p1, c1, n1);

    int32_t err;
    int64_t v = mysql_reader_integer(r, "a", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, 42 == v);

    // 字段类型不匹配（VARCHAR 不能读整数）
    v = mysql_reader_integer(r, "b", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    CuAssertTrue(tc, 0 == v);

    // nil 字段 → err=1
    v = mysql_reader_integer(r, "c", &err);
    CuAssertIntEquals(tc, 1, err);
    CuAssertTrue(tc, 0 == v);

    // 不存在的字段 → err=ERR_FAILED
    v = mysql_reader_integer(r, "nosuch", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);

    mysql_reader_free(r);

    // 行尾游标越界
    mysql_reader_ctx *r2 = _reader_new(MPACK_QUERY, 1, names, types);
    // r2->index=0, arr_rows empty → 越界
    v = mysql_reader_integer(r2, "a", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    mysql_reader_free(r2);
}

// mysql_reader_integer 二进制协议（MPACK_STMT_EXECUTE）：直接 unpack_integer
static void test_mysql_reader_integer_binary(CuTest *tc) {
    char names[1][64] = { "x" };
    uint8_t types[1] = { MYSQL_TYPE_LONG };
    mysql_reader_ctx *r = _reader_new(MPACK_STMT_EXECUTE, 1, names, types);

    // 4 字节小端 int32 = -123
    char *p;
    MALLOC(p, 4);
    pack_integer(p, (uint64_t)(int64_t)-123, 4, 1);
    buf_ctx c1[1] = { { .data = p, .lens = 4 } };
    _reader_push_row(r, p, c1, NULL);

    int32_t err;
    int64_t v = mysql_reader_integer(r, "x", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, -123 == v);

    mysql_reader_free(r);

    // 单字节 TINY
    mysql_reader_ctx *r2 = _reader_new(MPACK_STMT_EXECUTE, 1, names, (uint8_t[]){ MYSQL_TYPE_TINY });
    char *p2;
    MALLOC(p2, 1);
    p2[0] = (char)127;
    buf_ctx c2[1] = { { .data = p2, .lens = 1 } };
    _reader_push_row(r2, p2, c2, NULL);
    v = mysql_reader_integer(r2, "x", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, 127 == v);
    mysql_reader_free(r2);
}

// mysql_reader_uinteger 文本路径 + 二进制 TINY 单字节路径
static void test_mysql_reader_uinteger(CuTest *tc) {
    char names[1][64] = { "n" };
    uint8_t types[1] = { MYSQL_TYPE_LONGLONG };
    mysql_reader_ctx *r = _reader_new(MPACK_QUERY, 1, names, types);
    char *p;
    MALLOC(p, 24);
    const char *s = "18446744073709551610"; // 接近 UINT64_MAX
    memcpy(p, s, strlen(s));
    buf_ctx c[1] = { { .data = p, .lens = strlen(s) } };
    _reader_push_row(r, p, c, NULL);
    int32_t err;
    uint64_t v = mysql_reader_uinteger(r, "n", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, 18446744073709551610ULL == v);
    mysql_reader_free(r);

    // 二进制 TINY：单字节 uint8 = 200
    char tinames[1][64] = { "u" };
    uint8_t titypes[1] = { MYSQL_TYPE_TINY };
    mysql_reader_ctx *r2 = _reader_new(MPACK_STMT_EXECUTE, 1, tinames, titypes);
    char *p2;
    MALLOC(p2, 1);
    p2[0] = (char)200;
    buf_ctx c2[1] = { { .data = p2, .lens = 1 } };
    _reader_push_row(r2, p2, c2, NULL);
    v = mysql_reader_uinteger(r2, "u", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, 200 == v);
    mysql_reader_free(r2);
}

// mysql_reader_float / double 文本路径
static void test_mysql_reader_float_double_text(CuTest *tc) {
    char names[2][64] = { "f", "d" };
    uint8_t types[2] = { MYSQL_TYPE_FLOAT, MYSQL_TYPE_DOUBLE };
    mysql_reader_ctx *r = _reader_new(MPACK_QUERY, 2, names, types);
    char *p;
    MALLOC(p, 64);
    memcpy(p, "3.14", 4);
    memcpy(p + 4, "2.71828", 7);
    buf_ctx c[2] = { { .data = p, .lens = 4 }, { .data = p + 4, .lens = 7 } };
    _reader_push_row(r, p, c, NULL);
    int32_t err;
    float f = mysql_reader_float(r, "f", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, f > 3.13f && f < 3.15f);
    double d = mysql_reader_double(r, "d", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, d > 2.71 && d < 2.72);
    // 类型不匹配：用 float 字段读 double
    d = mysql_reader_double(r, "f", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    mysql_reader_free(r);
}

// mysql_reader_string：返回指针 + 长度，多种合法类型
static void test_mysql_reader_string(CuTest *tc) {
    char names[2][64] = { "s", "i" };
    uint8_t types[2] = { MYSQL_TYPE_VARCHAR, MYSQL_TYPE_LONG };
    mysql_reader_ctx *r = _reader_new(MPACK_QUERY, 2, names, types);
    char *p;
    MALLOC(p, 32);
    memcpy(p, "hello", 5);
    memcpy(p + 5, "1", 1);
    buf_ctx c[2] = { { .data = p, .lens = 5 }, { .data = p + 5, .lens = 1 } };
    _reader_push_row(r, p, c, NULL);
    size_t lens = 0;
    int32_t err;
    char *s = mysql_reader_string(r, "s", &lens, &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertIntEquals(tc, 5, (int)lens);
    CuAssertTrue(tc, 0 == memcmp(s, "hello", 5));

    // 类型不匹配：LONG 字段不能读字符串
    s = mysql_reader_string(r, "i", &lens, &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    CuAssertTrue(tc, NULL == s);
    mysql_reader_free(r);
}

// mysql_reader_datetime 二进制协议：7 字节 year(2)+month(1)+day(1)+h(1)+m(1)+s(1)
// 注：文本协议依赖 strtots → mktime 行为，在不同时区/DST 下可能不稳定；
// 这里覆盖更可控的 binary 路径，文本路径的 0 长度短路在第二段验证
static void test_mysql_reader_datetime_binary(CuTest *tc) {
    char names[1][64] = { "dt" };
    uint8_t types[1] = { MYSQL_TYPE_DATETIME };
    mysql_reader_ctx *r = _reader_new(MPACK_STMT_EXECUTE, 1, names, types);
    char *p;
    MALLOC(p, 7);
    pack_integer(p, 2024, 2, 1); // year 小端 2 字节
    p[2] = 5;    // month
    p[3] = 21;   // day
    p[4] = 13;   // hour
    p[5] = 45;   // min
    p[6] = 30;   // sec
    buf_ctx c[1] = { { .data = p, .lens = 7 } };
    _reader_push_row(r, p, c, NULL);
    int32_t err;
    int64_t ts = mysql_reader_datetime(r, "dt", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, ts > 0);
    mysql_reader_free(r);

    // 0 长度数据 → err（公共短路，文本/二进制都生效）
    mysql_reader_ctx *r2 = _reader_new(MPACK_QUERY, 1, names, types);
    char *p2;
    MALLOC(p2, 4);
    buf_ctx c2[1] = { { .data = p2, .lens = 0 } };
    _reader_push_row(r2, p2, c2, NULL);
    (void)mysql_reader_datetime(r2, "dt", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    mysql_reader_free(r2);
}

// mysql_reader_time 文本协议：正/负 "HH:MM:SS" 格式 + 二进制协议 8 字节
static void test_mysql_reader_time(CuTest *tc) {
    char names[1][64] = { "t" };
    uint8_t types[1] = { MYSQL_TYPE_TIME };
    // 文本路径正值
    {
        mysql_reader_ctx *r = _reader_new(MPACK_QUERY, 1, names, types);
        char *p;
        MALLOC(p, 16);
        const char *s = "12:34:56";
        memcpy(p, s, strlen(s));
        buf_ctx c[1] = { { .data = p, .lens = strlen(s) } };
        _reader_push_row(r, p, c, NULL);
        struct tm t;
        uint32_t usec = 0;
        ZERO(&t, sizeof(t));
        int32_t err;
        int32_t neg = mysql_reader_time(r, "t", &t, &usec, &err);
        CuAssertIntEquals(tc, ERR_OK, err);
        CuAssertIntEquals(tc, 0, neg);
        CuAssertIntEquals(tc, 12, t.tm_hour);
        CuAssertIntEquals(tc, 34, t.tm_min);
        CuAssertIntEquals(tc, 56, t.tm_sec);
        CuAssertIntEquals(tc, 0, (int32_t)usec);
        mysql_reader_free(r);
    }
    // 文本路径含微秒
    {
        mysql_reader_ctx *r = _reader_new(MPACK_QUERY, 1, names, types);
        char *p;
        MALLOC(p, 24);
        const char *s = "12:34:56.123456";
        memcpy(p, s, strlen(s));
        buf_ctx c[1] = { { .data = p, .lens = strlen(s) } };
        _reader_push_row(r, p, c, NULL);
        struct tm t;
        uint32_t usec = 0;
        ZERO(&t, sizeof(t));
        int32_t err;
        int32_t neg = mysql_reader_time(r, "t", &t, &usec, &err);
        CuAssertIntEquals(tc, ERR_OK, err);
        CuAssertIntEquals(tc, 0, neg);
        CuAssertIntEquals(tc, 56, t.tm_sec);
        CuAssertIntEquals(tc, 123456, (int32_t)usec);
        mysql_reader_free(r);
    }
    // 文本路径负值
    {
        mysql_reader_ctx *r = _reader_new(MPACK_QUERY, 1, names, types);
        char *p;
        MALLOC(p, 16);
        const char *s = "-1:30:45";
        memcpy(p, s, strlen(s));
        buf_ctx c[1] = { { .data = p, .lens = strlen(s) } };
        _reader_push_row(r, p, c, NULL);
        struct tm t;
        uint32_t usec = 0;
        ZERO(&t, sizeof(t));
        int32_t err;
        int32_t neg = mysql_reader_time(r, "t", &t, &usec, &err);
        CuAssertIntEquals(tc, ERR_OK, err);
        CuAssertIntEquals(tc, 1, neg);
        mysql_reader_free(r);
    }
    // 二进制路径 8 字节：is_negative + 4 字节天 + h/m/s
    {
        mysql_reader_ctx *r = _reader_new(MPACK_STMT_EXECUTE, 1, names, types);
        // 直接手写 8 字节：is_negative(1) + days(4 字节小端) + h(1) + m(1) + s(1)
        char *p;
        MALLOC(p, 8);
        p[0] = 1;                              // negative
        pack_integer(p + 1, (uint64_t)2, 4, 1);// days=2
        p[5] = 5;                              // hour
        p[6] = 30;                             // min
        p[7] = 45;                             // sec
        buf_ctx c[1] = { { .data = p, .lens = 8 } };
        _reader_push_row(r, p, c, NULL);
        struct tm t;
        uint32_t usec = 0;
        ZERO(&t, sizeof(t));
        int32_t err;
        int32_t neg = mysql_reader_time(r, "t", &t, &usec, &err);
        CuAssertIntEquals(tc, ERR_OK, err);
        CuAssertIntEquals(tc, 1, neg);
        CuAssertIntEquals(tc, 2, t.tm_mday);
        CuAssertIntEquals(tc, 5, t.tm_hour);
        CuAssertIntEquals(tc, 30, t.tm_min);
        CuAssertIntEquals(tc, 45, t.tm_sec);
        CuAssertIntEquals(tc, 0, (int32_t)usec);
        mysql_reader_free(r);
    }
}

// 二进制 reader 长度校验回归：FLOAT 恰 4 字节 / DOUBLE 恰 8 字节 / DATETIME ∈ {4,7,11} /
// TIME ∈ {8,12}；长度不符一律返 ERR_FAILED（覆盖近期修复，防畸形 server 数据被误当合法值或越界）
static void test_mysql_reader_binary_lens(CuTest *tc) {
    char nm[1][64] = { "v" };
    char tnm[1][64] = { "t" };
    int32_t err;
    char *p;
    buf_ctx c[1];
    mysql_reader_ctx *r;
    float f;
    double dd;
    int64_t ts;
    struct tm tmv;
    uint32_t usec;
    int32_t neg;

    // FLOAT 二进制恰 4 字节 → OK
    r = _reader_new(MPACK_STMT_EXECUTE, 1, nm, (uint8_t[]){ MYSQL_TYPE_FLOAT });
    MALLOC(p, 4);
    pack_float(p, 3.5f, 1);
    c[0].data = p; c[0].lens = 4;
    _reader_push_row(r, p, c, NULL);
    f = mysql_reader_float(r, "v", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, f > 3.49f && f < 3.51f);
    mysql_reader_free(r);

    // FLOAT 列但 8 字节 → 长度不符 ERR_FAILED（不读越界）
    r = _reader_new(MPACK_STMT_EXECUTE, 1, nm, (uint8_t[]){ MYSQL_TYPE_FLOAT });
    MALLOC(p, 8);
    pack_double(p, 3.5, 1);
    c[0].data = p; c[0].lens = 8;
    _reader_push_row(r, p, c, NULL);
    (void)mysql_reader_float(r, "v", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    mysql_reader_free(r);

    // DOUBLE 二进制恰 8 字节 → OK
    r = _reader_new(MPACK_STMT_EXECUTE, 1, nm, (uint8_t[]){ MYSQL_TYPE_DOUBLE });
    MALLOC(p, 8);
    pack_double(p, 2.5, 1);
    c[0].data = p; c[0].lens = 8;
    _reader_push_row(r, p, c, NULL);
    dd = mysql_reader_double(r, "v", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, dd > 2.49 && dd < 2.51);
    mysql_reader_free(r);

    // DOUBLE 列但 4 字节 → 长度不符 ERR_FAILED
    r = _reader_new(MPACK_STMT_EXECUTE, 1, nm, (uint8_t[]){ MYSQL_TYPE_DOUBLE });
    MALLOC(p, 4);
    pack_float(p, 2.5f, 1);
    c[0].data = p; c[0].lens = 4;
    _reader_push_row(r, p, c, NULL);
    (void)mysql_reader_double(r, "v", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    mysql_reader_free(r);

    // DATETIME 二进制 4 字节（仅日期）→ OK
    r = _reader_new(MPACK_STMT_EXECUTE, 1, nm, (uint8_t[]){ MYSQL_TYPE_DATETIME });
    MALLOC(p, 4);
    pack_integer(p, 2024, 2, 1);
    p[2] = 6; p[3] = 15;
    c[0].data = p; c[0].lens = 4;
    _reader_push_row(r, p, c, NULL);
    ts = mysql_reader_datetime(r, "v", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, ts > 0);
    mysql_reader_free(r);

    // DATETIME 二进制 11 字节（含微秒）→ OK，微秒纳入返回值
    r = _reader_new(MPACK_STMT_EXECUTE, 1, nm, (uint8_t[]){ MYSQL_TYPE_DATETIME });
    MALLOC(p, 11);
    pack_integer(p, 2024, 2, 1);
    p[2] = 6; p[3] = 15; p[4] = 10; p[5] = 20; p[6] = 30;
    pack_integer(p + 7, 123456, 4, 1);
    c[0].data = p; c[0].lens = 11;
    _reader_push_row(r, p, c, NULL);
    ts = mysql_reader_datetime(r, "v", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, ts > 0);
    CuAssertTrue(tc, 123456 == (int32_t)(ts % 1000000));
    mysql_reader_free(r);

    // DATETIME 二进制畸形长度 5 → ERR_FAILED
    r = _reader_new(MPACK_STMT_EXECUTE, 1, nm, (uint8_t[]){ MYSQL_TYPE_DATETIME });
    MALLOC(p, 5);
    pack_integer(p, 2024, 2, 1);
    p[2] = 6; p[3] = 15; p[4] = 10;
    c[0].data = p; c[0].lens = 5;
    _reader_push_row(r, p, c, NULL);
    (void)mysql_reader_datetime(r, "v", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    mysql_reader_free(r);

    // TIME 二进制 12 字节（含微秒）→ OK，微秒纳入返回值
    r = _reader_new(MPACK_STMT_EXECUTE, 1, tnm, (uint8_t[]){ MYSQL_TYPE_TIME });
    MALLOC(p, 12);
    p[0] = 0;
    pack_integer(p + 1, 3, 4, 1);
    p[5] = 8; p[6] = 15; p[7] = 30;
    pack_integer(p + 8, 654321, 4, 1);
    c[0].data = p; c[0].lens = 12;
    _reader_push_row(r, p, c, NULL);
    ZERO(&tmv, sizeof(tmv));
    usec = 0;
    neg = mysql_reader_time(r, "t", &tmv, &usec, &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertIntEquals(tc, 0, neg);
    CuAssertIntEquals(tc, 3, tmv.tm_mday);
    CuAssertIntEquals(tc, 8, tmv.tm_hour);
    CuAssertIntEquals(tc, 15, tmv.tm_min);
    CuAssertIntEquals(tc, 30, tmv.tm_sec);
    CuAssertIntEquals(tc, 654321, (int32_t)usec);
    mysql_reader_free(r);

    // TIME 二进制畸形长度 7 → ERR_FAILED
    r = _reader_new(MPACK_STMT_EXECUTE, 1, tnm, (uint8_t[]){ MYSQL_TYPE_TIME });
    MALLOC(p, 7);
    p[0] = 0;
    pack_integer(p + 1, 3, 4, 1);
    p[5] = 8; p[6] = 15;
    c[0].data = p; c[0].lens = 7;
    _reader_push_row(r, p, c, NULL);
    ZERO(&tmv, sizeof(tmv));
    usec = 0;
    (void)mysql_reader_time(r, "t", &tmv, &usec, &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    mysql_reader_free(r);
}

// _mpack_ok 解析 OK 包：affected_rows + last_insert_id + status_flags + warnings
static void test_mpack_ok_parse(CuTest *tc) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    // lenenc 5（affected_rows）
    _mysql_set_lenenc(&bw, 5);
    // lenenc 17（last_insert_id）
    _mysql_set_lenenc(&bw, 17);
    // status_flags 0x0002，warnings 3
    binary_set_integer(&bw, 0x0002, 2, 1);
    binary_set_integer(&bw, 3, 2, 1);
    // 末尾随意附加（_mpack_ok 末尾会 skip 剩余）
    binary_set_int8(&bw, 0xab);
    binary_set_int8(&bw, 0xcd);

    binary_ctx br;
    binary_init(&br, bw.data, bw.offset, 0);
    mysql_ctx mysql;
    ZERO(&mysql, sizeof(mysql));
    mpack_ok ok;
    ZERO(&ok, sizeof(ok));
    int32_t rtn = _mpack_ok(&mysql, &br, &ok);
    CuAssertIntEquals(tc, ERR_OK, rtn);
    CuAssertIntEquals(tc, 5, (int)ok.affected_rows);
    CuAssertIntEquals(tc, 17, (int)ok.last_insert_id);
    CuAssertIntEquals(tc, 2, (int)ok.status_flags);
    CuAssertIntEquals(tc, 3, (int)ok.warnings);
    // mysql_ctx 也被同步
    CuAssertIntEquals(tc, 5, (int)mysql.affected_rows);
    CuAssertIntEquals(tc, 17, (int)mysql.last_id);
    binary_free(&bw);
}

// _mpack_err 解析 ERR 包：error_code + 跳过 6 字节 SQL state + 剩余字节为 msg
static void test_mpack_err_parse(CuTest *tc) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    // error_code 0x1234
    binary_set_integer(&bw, 0x1234, 2, 1);
    // sql_state_marker(1) + sql_state(5) = 6 字节
    binary_set_binary(&bw, "#HY000", 6);
    // 错误消息
    const char *msg = "syntax error near 'foo'";
    binary_set_binary(&bw, msg, strlen(msg));

    binary_ctx br;
    binary_init(&br, bw.data, bw.offset, 0);
    mysql_ctx mysql;
    ZERO(&mysql, sizeof(mysql));
    mpack_err err;
    ZERO(&err, sizeof(err));
    _mpack_err(&mysql, &br, &err);
    CuAssertIntEquals(tc, 0x1234, err.error_code);
    CuAssertIntEquals(tc, (int)strlen(msg), (int)err.error_msg.lens);
    CuAssertTrue(tc, 0 == memcmp(err.error_msg.data, msg, strlen(msg)));
    CuAssertIntEquals(tc, 0x1234, mysql.error_code);
    CuAssertStrEquals(tc, msg, mysql.error_msg);
    binary_free(&bw);
}

// _mpack_err 空错误消息：error_msg 长度 0，mysql.error_msg 为空字符串
static void test_mpack_err_empty_msg(CuTest *tc) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    binary_set_integer(&bw, 1045, 2, 1);
    binary_set_binary(&bw, "#28000", 6);
    // 无 msg

    binary_ctx br;
    binary_init(&br, bw.data, bw.offset, 0);
    mysql_ctx mysql;
    ZERO(&mysql, sizeof(mysql));
    mpack_err err;
    ZERO(&err, sizeof(err));
    _mpack_err(&mysql, &br, &err);
    CuAssertIntEquals(tc, 1045, err.error_code);
    CuAssertIntEquals(tc, 0, (int)err.error_msg.lens);
    CuAssertStrEquals(tc, "", mysql.error_msg);
    binary_free(&bw);
}

// _mysql_payload：从 buffer 取 lenpref + payload；不足时 PROT_MOREDATA
static void test_mysql_payload(CuTest *tc) {
    // 完整包：3 字节长度 + 1 字节 sequence_id + payload
    buffer_ctx buf;
    buffer_init(&buf);
    char head[4];
    head[0] = 5; head[1] = 0; head[2] = 0; head[3] = 0; // len=5, seq=0
    buffer_append(&buf, head, 4);
    buffer_append(&buf, "hello", 5);

    mysql_ctx mysql;
    ZERO(&mysql, sizeof(mysql));
    int32_t status = PROT_INIT;
    size_t plen = 0;
    char *p = _mysql_payload(&mysql, &buf, &plen, &status);
    CuAssertPtrNotNull(tc, p);
    CuAssertIntEquals(tc, 5, (int)plen);
    CuAssertTrue(tc, 0 == memcmp(p, "hello", 5));
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_MOREDATA));
    FREE(p);
    buffer_free(&buf);

    // 半包：只有 head，没有 payload → PROT_MOREDATA
    buffer_init(&buf);
    head[0] = 5; head[1] = 0; head[2] = 0; head[3] = 0;
    buffer_append(&buf, head, 4);
    status = PROT_INIT;
    p = _mysql_payload(&mysql, &buf, &plen, &status);
    CuAssertTrue(tc, NULL == p);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_MOREDATA));
    buffer_free(&buf);

    // 完全无头：PROT_MOREDATA
    buffer_init(&buf);
    status = PROT_INIT;
    p = _mysql_payload(&mysql, &buf, &plen, &status);
    CuAssertTrue(tc, NULL == p);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_MOREDATA));
    buffer_free(&buf);
}

// mysql_stmt_init：从 MPACK_STMT_PREPARE mpack 提取 mysql_stmt_ctx 并转移所有权
static void test_mysql_stmt_init(CuTest *tc) {
    // mpack=NULL → NULL
    CuAssertTrue(tc, NULL == mysql_stmt_init(NULL));

    // pack=NULL → NULL
    mpack_ctx p;
    ZERO(&p, sizeof(p));
    p.pack_type = MPACK_STMT_PREPARE;
    p.pack = NULL;
    CuAssertTrue(tc, NULL == mysql_stmt_init(&p));

    // pack_type 不匹配 → NULL（含非 STMT_PREPARE 的其他常见类型）
    p.pack_type = MPACK_OK;
    p.pack = (void *)1;
    CuAssertTrue(tc, NULL == mysql_stmt_init(&p));
    p.pack_type = MPACK_ERR;
    CuAssertTrue(tc, NULL == mysql_stmt_init(&p));
    p.pack_type = MPACK_QUERY;
    CuAssertTrue(tc, NULL == mysql_stmt_init(&p));
    p.pack_type = MPACK_STMT_EXECUTE;
    CuAssertTrue(tc, NULL == mysql_stmt_init(&p));

    // 合法路径：成功转移 stmt 所有权，pack/_free_mpack 置 NULL
    mysql_stmt_ctx *stmt;
    CALLOC(stmt, 1, sizeof(*stmt));
    stmt->stmt_id = 0xabcdef;
    stmt->params_count = 2;
    stmt->field_count = 3;

    p.pack_type = MPACK_STMT_PREPARE;
    p.pack = stmt;
    p._free_mpack = (void (*)(void *))0xdeadbeef; // 毒值:本路径不应触发此 free,误调即崩

    mysql_stmt_ctx *out = mysql_stmt_init(&p);
    CuAssertTrue(tc, out == stmt);
    CuAssertTrue(tc, NULL == p.pack);
    CuAssertTrue(tc, NULL == p._free_mpack);
    CuAssertTrue(tc, 0xabcdef == out->stmt_id);
    CuAssertTrue(tc, 2 == out->params_count);
    CuAssertTrue(tc, 3 == out->field_count);

    /* 调用方负责释放（mpack 已不再持有所有权） */
    FREE(stmt);
}

// _mpack_parse_binary_row：temporal 类型非法长度前缀 → ERR_FAILED
static void test_mysql_binary_row_temporal_invalid_len(CuTest *tc) {
    char names[1][64] = { "ts" };
    // DATETIME 非法长度 9 → ERR_FAILED（合法：0/4/7/11）
    {
        uint8_t types[1] = { MYSQL_TYPE_DATETIME };
        mysql_reader_ctx *r = _reader_new(MPACK_STMT_EXECUTE, 1, names, types);
        binary_ctx bw;
        binary_init(&bw, NULL, 0, 0);
        binary_set_uint8(&bw, 0x00);//NULL 位图：field 0 不为 NULL
        binary_set_uint8(&bw, 9);//DATETIME 长度前缀：9（非法）
        binary_ctx br;
        binary_init(&br, bw.data, bw.offset, 0);
        int32_t rtn = _mpack_parse_binary_row(r, &br);
        CuAssertIntEquals(tc, ERR_FAILED, rtn);
        mysql_reader_free(r);
        binary_free(&bw);
    }
    // TIME 非法长度 5 → ERR_FAILED（合法：0/8/12）
    {
        uint8_t types[1] = { MYSQL_TYPE_TIME };
        mysql_reader_ctx *r = _reader_new(MPACK_STMT_EXECUTE, 1, names, types);
        binary_ctx bw;
        binary_init(&bw, NULL, 0, 0);
        binary_set_uint8(&bw, 0x00);//NULL 位图
        binary_set_uint8(&bw, 5);//TIME 长度前缀：5（非法）
        binary_ctx br;
        binary_init(&br, bw.data, bw.offset, 0);
        int32_t rtn = _mpack_parse_binary_row(r, &br);
        CuAssertIntEquals(tc, ERR_FAILED, rtn);
        mysql_reader_free(r);
        binary_free(&bw);
    }
    // DATE 合法长度 0（零值）→ ERR_OK
    {
        uint8_t types[1] = { MYSQL_TYPE_DATE };
        mysql_reader_ctx *r = _reader_new(MPACK_STMT_EXECUTE, 1, names, types);
        char *buf;
        MALLOC(buf, 2);
        buf[0] = 0x00;//NULL 位图
        buf[1] = 0;//DATE 长度前缀：0（零值，合法）
        binary_ctx br;
        binary_init(&br, buf, 2, 0);//外部托管：所有权随 row->payload 转 mysql_reader_free
        int32_t rtn = _mpack_parse_binary_row(r, &br);
        CuAssertIntEquals(tc, ERR_OK, rtn);
        mysql_reader_free(r);//释放 buf（通过 row->payload）
    }
}

// mysql_reader_datetime 文本路径：DATE-only(n=3)、DATETIME(n=6)、含微秒、格式错误
static void test_mysql_reader_datetime_text(CuTest *tc) {
    char names[1][64] = { "v" };
    int32_t err;
    int64_t ts;
    mysql_reader_ctx *r;
    char *p;
    buf_ctx c[1];
    // DATE-only "YYYY-MM-DD"（n=3）→ 微秒余数为 0
    r = _reader_new(MPACK_QUERY, 1, names, (uint8_t[]){ MYSQL_TYPE_DATE });
    MALLOC(p, 16);
    memcpy(p, "2024-06-15", 10);
    c[0].data = p; c[0].lens = 10;
    _reader_push_row(r, p, c, NULL);
    ts = mysql_reader_datetime(r, "v", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, ts > 0);
    CuAssertIntEquals(tc, 0, (int32_t)(ts % 1000000));
    mysql_reader_free(r);
    // DATETIME 无微秒（n=6）→ 微秒余数为 0
    r = _reader_new(MPACK_QUERY, 1, names, (uint8_t[]){ MYSQL_TYPE_DATETIME });
    MALLOC(p, 32);
    memcpy(p, "2024-06-15 10:20:30", 19);
    c[0].data = p; c[0].lens = 19;
    _reader_push_row(r, p, c, NULL);
    ts = mysql_reader_datetime(r, "v", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, ts > 0);
    CuAssertIntEquals(tc, 0, (int32_t)(ts % 1000000));
    mysql_reader_free(r);
    // DATETIME 含微秒 → 微秒余数 = 123456
    r = _reader_new(MPACK_QUERY, 1, names, (uint8_t[]){ MYSQL_TYPE_DATETIME });
    MALLOC(p, 32);
    memcpy(p, "2024-06-15 10:20:30.123456", 26);
    c[0].data = p; c[0].lens = 26;
    _reader_push_row(r, p, c, NULL);
    ts = mysql_reader_datetime(r, "v", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, ts > 0);
    CuAssertIntEquals(tc, 123456, (int32_t)(ts % 1000000));
    mysql_reader_free(r);
    // 格式错误 → ERR_FAILED
    r = _reader_new(MPACK_QUERY, 1, names, (uint8_t[]){ MYSQL_TYPE_DATETIME });
    MALLOC(p, 16);
    memcpy(p, "not-a-date", 10);
    c[0].data = p; c[0].lens = 10;
    _reader_push_row(r, p, c, NULL);
    (void)mysql_reader_datetime(r, "v", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    mysql_reader_free(r);
}
// DATETIME2/TIMESTAMP2/TIME2 二进制路径与基类型相同解码逻辑
static void test_mysql_reader_datetime2_types(CuTest *tc) {
    char nm[1][64] = { "v" };
    char tnm[1][64] = { "t" };
    int32_t err;
    int64_t ts;
    struct tm tmv;
    uint32_t usec;
    int32_t neg;
    char *p;
    buf_ctx c[1];
    mysql_reader_ctx *r;
    // DATETIME2 二进制 7 字节 → 与 DATETIME 相同解码路径，微秒余数 0
    r = _reader_new(MPACK_STMT_EXECUTE, 1, nm, (uint8_t[]){ MYSQL_TYPE_DATETIME2 });
    MALLOC(p, 7);
    pack_integer(p, 2024, 2, 1);
    p[2] = 3; p[3] = 10; p[4] = 14; p[5] = 30; p[6] = 45;
    c[0].data = p; c[0].lens = 7;
    _reader_push_row(r, p, c, NULL);
    ts = mysql_reader_datetime(r, "v", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, ts > 0);
    CuAssertIntEquals(tc, 0, (int32_t)(ts % 1000000));
    mysql_reader_free(r);
    // TIMESTAMP2 二进制 11 字节（含微秒）→ 微秒余数 = 999999
    r = _reader_new(MPACK_STMT_EXECUTE, 1, nm, (uint8_t[]){ MYSQL_TYPE_TIMESTAMP2 });
    MALLOC(p, 11);
    pack_integer(p, 2025, 2, 1);
    p[2] = 1; p[3] = 1; p[4] = 0; p[5] = 0; p[6] = 0;
    pack_integer(p + 7, 999999, 4, 1);
    c[0].data = p; c[0].lens = 11;
    _reader_push_row(r, p, c, NULL);
    ts = mysql_reader_datetime(r, "v", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, ts > 0);
    CuAssertIntEquals(tc, 999999, (int32_t)(ts % 1000000));
    mysql_reader_free(r);
    // TIME2 二进制 8 字节 → 与 TIME 相同解码路径
    r = _reader_new(MPACK_STMT_EXECUTE, 1, tnm, (uint8_t[]){ MYSQL_TYPE_TIME2 });
    MALLOC(p, 8);
    p[0] = 0;
    pack_integer(p + 1, 1, 4, 1);
    p[5] = 2; p[6] = 3; p[7] = 4;
    c[0].data = p; c[0].lens = 8;
    _reader_push_row(r, p, c, NULL);
    ZERO(&tmv, sizeof(tmv));
    usec = 0;
    neg = mysql_reader_time(r, "t", &tmv, &usec, &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertIntEquals(tc, 0, neg);
    CuAssertIntEquals(tc, 1, tmv.tm_mday);
    CuAssertIntEquals(tc, 2, tmv.tm_hour);
    CuAssertIntEquals(tc, 3, tmv.tm_min);
    CuAssertIntEquals(tc, 4, tmv.tm_sec);
    CuAssertIntEquals(tc, 0, (int32_t)usec);
    mysql_reader_free(r);
}
void test_mysql_parse(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_mysql_reader_init);
    SUITE_ADD_TEST(suite, test_mysql_reader_cursor);
    SUITE_ADD_TEST(suite, test_mysql_reader_integer_text);
    SUITE_ADD_TEST(suite, test_mysql_reader_integer_binary);
    SUITE_ADD_TEST(suite, test_mysql_reader_uinteger);
    SUITE_ADD_TEST(suite, test_mysql_reader_float_double_text);
    SUITE_ADD_TEST(suite, test_mysql_reader_string);
    SUITE_ADD_TEST(suite, test_mysql_reader_datetime_binary);
    SUITE_ADD_TEST(suite, test_mysql_reader_time);
    SUITE_ADD_TEST(suite, test_mysql_reader_binary_lens);
    SUITE_ADD_TEST(suite, test_mpack_ok_parse);
    SUITE_ADD_TEST(suite, test_mpack_err_parse);
    SUITE_ADD_TEST(suite, test_mpack_err_empty_msg);
    SUITE_ADD_TEST(suite, test_mysql_payload);
    SUITE_ADD_TEST(suite, test_mysql_stmt_init);
    SUITE_ADD_TEST(suite, test_mysql_binary_row_temporal_invalid_len);
    SUITE_ADD_TEST(suite, test_mysql_reader_datetime_text);
    SUITE_ADD_TEST(suite, test_mysql_reader_datetime2_types);
}
