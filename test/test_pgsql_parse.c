#include "test_pgsql_parse.h"
#include "lib.h"
#include "protocol/pgsql/pgsql_parse.h"
#include "protocol/pgsql/pgsql_reader.h"
#include "protocol/pgsql/pgsql_struct.h"
#include "protocol/pgsql/pgsql_macro.h"

#ifdef _WIN32
#pragma warning(disable:4312)
#endif

// 构造一个 pgsql_reader_ctx：fields 数量 + 类型 + 名称
static pgsql_reader_ctx *_pg_reader_new(uint16_t field_count, const int32_t *type_oids,
                                        const char (*names)[64]) {
    pgsql_reader_ctx *r;
    CALLOC(r, 1, sizeof(*r));
    r->field_count = field_count;
    if (field_count > 0) {
        CALLOC(r->fields, 1, sizeof(pgpack_field) * field_count);
        for (uint16_t i = 0; i < field_count; i++) {
            r->fields[i].type_oid = type_oids[i];
            safe_fill_str(r->fields[i].name, sizeof(r->fields[i].name), names[i]);
        }
    }
    array_init(&r->arr_rows, sizeof(pgpack_row *), 0);
    return r;
}

// 给 reader 添加一行；payload 由首列持有，cols 每项的 lens/val 直接拷入 row
// cols[i].lens 设为 -1 表示该列 NULL
static void _pg_reader_push_row(pgsql_reader_ctx *r, char *payload,
                                const pgpack_row *cols) {
    pgpack_row *row;
    CALLOC(row, 1, sizeof(pgpack_row) * r->field_count);
    row[0].payload = payload;
    for (uint16_t i = 0; i < r->field_count; i++) {
        row[i].lens = cols[i].lens;
        row[i].val = cols[i].val;
    }
    void *p = row;
    array_push_back(&r->arr_rows, &p);
}

// pgsql_reader_init：仅 PGPACK_OK 且 pack 非 NULL 才返回 reader，并转移所有权
static void test_pgsql_reader_init(CuTest *tc) {
    pgpack_ctx pg;
    ZERO(&pg, sizeof(pg));
    pg.type = PGPACK_ERR;
    pg.pack = (void *)1;
    CuAssertTrue(tc, NULL == pgsql_reader_init(&pg, FORMAT_TEXT));

    pg.type = PGPACK_NOTIFICATION;
    CuAssertTrue(tc, NULL == pgsql_reader_init(&pg, FORMAT_TEXT));

    pg.type = PGPACK_OK;
    pg.pack = NULL;
    CuAssertTrue(tc, NULL == pgsql_reader_init(&pg, FORMAT_TEXT));

    // 正常路径：转移所有权
    int32_t oids[1] = { INT4OID };
    char names[1][64] = { "id" };
    pgsql_reader_ctx *r = _pg_reader_new(1, oids, names);
    pg.type = PGPACK_OK;
    pg.pack = r;
    pg._free_pgpack = (void (*)(void *))0xdeadbeef; // 毒值:本路径不应触发此 free,误调即崩
    pgsql_reader_ctx *out = pgsql_reader_init(&pg, FORMAT_TEXT);
    CuAssertTrue(tc, out == r);
    CuAssertIntEquals(tc, FORMAT_TEXT, (int)out->format);
    CuAssertTrue(tc, NULL == pg.pack);
    CuAssertTrue(tc, NULL == pg._free_pgpack);
    pgsql_reader_free(out);
}

// pgsql_reader_size/seek/eof/next 游标语义
static void test_pgsql_reader_cursor(CuTest *tc) {
    int32_t oids[1] = { INT4OID };
    char names[1][64] = { "id" };
    pgsql_reader_ctx *r = _pg_reader_new(1, oids, names);

    CuAssertIntEquals(tc, 0, (int)pgsql_reader_size(r));
    CuAssertIntEquals(tc, 1, pgsql_reader_eof(r));

    // 3 行
    for (int i = 0; i < 3; i++) {
        char *p;
        MALLOC(p, 4);
        p[0] = (char)('1' + i);
        pgpack_row cols[1] = { { 1, p, NULL } };
        _pg_reader_push_row(r, p, cols);
    }
    CuAssertIntEquals(tc, 3, (int)pgsql_reader_size(r));
    CuAssertIntEquals(tc, 0, pgsql_reader_eof(r));

    pgsql_reader_next(r);
    pgsql_reader_next(r);
    pgsql_reader_next(r);
    CuAssertIntEquals(tc, 1, pgsql_reader_eof(r));
    // 越界 next 不递增
    pgsql_reader_next(r);
    CuAssertIntEquals(tc, 3, r->index);
    // seek 越界忽略
    pgsql_reader_seek(r, 99);
    CuAssertIntEquals(tc, 3, r->index);
    // seek 合法位置
    pgsql_reader_seek(r, 0);
    CuAssertIntEquals(tc, 0, r->index);
    CuAssertIntEquals(tc, 0, pgsql_reader_eof(r));

    pgsql_reader_free(r);
}

// pgsql_reader_bool 文本协议：t/true/yes 真值 + NULL + 类型不匹配
static void test_pgsql_reader_bool(CuTest *tc) {
    int32_t oids[3] = { BOOLOID, BOOLOID, INT4OID };
    char names[3][64] = { "a", "b", "c" };
    pgsql_reader_ctx *r = _pg_reader_new(3, oids, names);
    r->format = FORMAT_TEXT;

    char *p;
    MALLOC(p, 16);
    memcpy(p, "true", 4);
    memcpy(p + 4, "no", 2);
    pgpack_row cols[3] = {
        { 4, p, NULL },        // a: "true"
        { 2, p + 4, NULL },    // b: "no"（不在真值列表，返回 0 但 err=ERR_OK）
        { -1, NULL, NULL }     // c: NULL (int4)
    };
    _pg_reader_push_row(r, p, cols);

    int32_t err;
    CuAssertIntEquals(tc, 1, pgsql_reader_bool(r, "a", &err));
    CuAssertIntEquals(tc, ERR_OK, err);

    CuAssertIntEquals(tc, 0, pgsql_reader_bool(r, "b", &err));
    CuAssertIntEquals(tc, ERR_OK, err);

    // 字段不存在 → ERR_FAILED
    pgsql_reader_bool(r, "nosuch", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);

    // 类型不匹配（INT4 不能读 bool）
    pgsql_reader_bool(r, "c", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);

    pgsql_reader_free(r);
}

// pgsql_reader_integer 文本协议 + 二进制协议 + NULL
static void test_pgsql_reader_integer(CuTest *tc) {
    int32_t oids[1] = { INT4OID };
    char names[1][64] = { "n" };
    pgsql_reader_ctx *r = _pg_reader_new(1, oids, names);
    r->format = FORMAT_TEXT;

    char *p;
    MALLOC(p, 16);
    memcpy(p, "12345", 5);
    pgpack_row cols[1] = { { 5, p, NULL } };
    _pg_reader_push_row(r, p, cols);

    int32_t err;
    CuAssertTrue(tc, 12345 == pgsql_reader_integer(r, "n", &err));
    CuAssertIntEquals(tc, ERR_OK, err);
    pgsql_reader_free(r);

    // 二进制：4 字节大端
    pgsql_reader_ctx *r2 = _pg_reader_new(1, oids, names);
    r2->format = FORMAT_BINARY;
    char *p2;
    MALLOC(p2, 4);
    pack_integer(p2, (uint64_t)0x12345678, 4, 0); // 大端
    pgpack_row cols2[1] = { { 4, p2, NULL } };
    _pg_reader_push_row(r2, p2, cols2);
    CuAssertTrue(tc, 0x12345678 == pgsql_reader_integer(r2, "n", &err));
    CuAssertIntEquals(tc, ERR_OK, err);
    pgsql_reader_free(r2);

    // NULL 字段
    pgsql_reader_ctx *r3 = _pg_reader_new(1, oids, names);
    r3->format = FORMAT_TEXT;
    char *p3;
    MALLOC(p3, 4);
    pgpack_row cols3[1] = { { -1, NULL, NULL } };
    _pg_reader_push_row(r3, p3, cols3);
    pgsql_reader_integer(r3, "n", &err);
    CuAssertIntEquals(tc, 1, err);
    pgsql_reader_free(r3);
}

// pgsql_reader_double 文本 + 二进制（float4=4 字节 / float8=8 字节）
static void test_pgsql_reader_double(CuTest *tc) {
    int32_t oids[1] = { FLOAT8OID };
    char names[1][64] = { "d" };
    // 文本
    pgsql_reader_ctx *r = _pg_reader_new(1, oids, names);
    r->format = FORMAT_TEXT;
    char *p;
    MALLOC(p, 16);
    memcpy(p, "3.14159", 7);
    pgpack_row cols[1] = { { 7, p, NULL } };
    _pg_reader_push_row(r, p, cols);
    int32_t err;
    double d = pgsql_reader_double(r, "d", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, d > 3.14 && d < 3.15);
    pgsql_reader_free(r);

    // 二进制 float8（8 字节）
    pgsql_reader_ctx *r2 = _pg_reader_new(1, oids, names);
    r2->format = FORMAT_BINARY;
    char *p2;
    MALLOC(p2, 8);
    pack_double(p2, 2.71828, 0);
    pgpack_row cols2[1] = { { 8, p2, NULL } };
    _pg_reader_push_row(r2, p2, cols2);
    d = pgsql_reader_double(r2, "d", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, d > 2.71 && d < 2.72);
    pgsql_reader_free(r2);

    // 二进制 float4（4 字节）：oid 改为 FLOAT4OID
    int32_t oids3[1] = { FLOAT4OID };
    pgsql_reader_ctx *r3 = _pg_reader_new(1, oids3, names);
    r3->format = FORMAT_BINARY;
    char *p3;
    MALLOC(p3, 4);
    pack_float(p3, 1.5f, 0);
    pgpack_row cols3[1] = { { 4, p3, NULL } };
    _pg_reader_push_row(r3, p3, cols3);
    d = pgsql_reader_double(r3, "d", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, d > 1.49 && d < 1.51);
    pgsql_reader_free(r3);

    // 类型↔长度不符回归（修复前 FLOAT8OID+4 字节会被误当 float4 解码不报错）
    // FLOAT8OID 只给 4 字节 → 拒绝
    pgsql_reader_ctx *r4 = _pg_reader_new(1, oids, names);
    r4->format = FORMAT_BINARY;
    char *p4;
    MALLOC(p4, 4);
    pack_float(p4, 1.5f, 0);
    pgpack_row cols4[1] = { { 4, p4, NULL } };
    _pg_reader_push_row(r4, p4, cols4);
    pgsql_reader_double(r4, "d", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    pgsql_reader_free(r4);
    // FLOAT4OID 给 8 字节 → 拒绝
    pgsql_reader_ctx *r5 = _pg_reader_new(1, oids3, names);
    r5->format = FORMAT_BINARY;
    char *p5;
    MALLOC(p5, 8);
    pack_double(p5, 2.0, 0);
    pgpack_row cols5[1] = { { 8, p5, NULL } };
    _pg_reader_push_row(r5, p5, cols5);
    pgsql_reader_double(r5, "d", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    pgsql_reader_free(r5);
}

// pgsql_reader_isnull + pgsql_reader_text
static void test_pgsql_reader_isnull_text(CuTest *tc) {
    int32_t oids[2] = { TEXTOID, TEXTOID };
    char names[2][64] = { "s1", "s2" };
    pgsql_reader_ctx *r = _pg_reader_new(2, oids, names);
    r->format = FORMAT_TEXT;
    char *p;
    MALLOC(p, 32);
    memcpy(p, "hello", 5);
    pgpack_row cols[2] = {
        { 5, p, NULL },
        { -1, NULL, NULL }
    };
    _pg_reader_push_row(r, p, cols);

    CuAssertIntEquals(tc, 0, pgsql_reader_isnull(r, "s1"));
    CuAssertIntEquals(tc, 1, pgsql_reader_isnull(r, "s2"));
    // 不存在的字段 → 视为非 NULL（返回 0）
    CuAssertIntEquals(tc, 0, pgsql_reader_isnull(r, "nosuch"));

    int32_t lens = 0, err;
    const char *t = pgsql_reader_text(r, "s1", &lens, &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertIntEquals(tc, 5, lens);
    CuAssertTrue(tc, 0 == memcmp(t, "hello", 5));

    // NULL 字段 → err=1
    t = pgsql_reader_text(r, "s2", &lens, &err);
    CuAssertIntEquals(tc, 1, err);
    CuAssertTrue(tc, NULL == t);

    pgsql_reader_free(r);
}

// pgsql_reader_bytea：二进制格式
static void test_pgsql_reader_bytea(CuTest *tc) {
    int32_t oids[1] = { BYTEAOID };
    char names[1][64] = { "b" };
    pgsql_reader_ctx *r = _pg_reader_new(1, oids, names);
    r->format = FORMAT_BINARY;
    char *p;
    MALLOC(p, 4);
    p[0] = 0x01; p[1] = 0x02; p[2] = 0x03; p[3] = 0x04;
    pgpack_row cols[1] = { { 4, p, NULL } };
    _pg_reader_push_row(r, p, cols);
    int32_t lens = 0, err;
    const char *b = pgsql_reader_bytea(r, "b", &lens, &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertIntEquals(tc, 4, lens);
    CuAssertTrue(tc, 0 == memcmp(b, p, 4));
    pgsql_reader_free(r);
}

// pgsql_reader_timestamp 文本协议：解析 "YYYY-MM-DD HH:MM:SS[.us]" 为相对 PG 纪元微秒数
static void test_pgsql_reader_timestamp_text(CuTest *tc) {
    int32_t oids[1] = { TIMESTAMPOID };
    char names[1][64] = { "ts" };
    pgsql_reader_ctx *r = _pg_reader_new(1, oids, names);
    r->format = FORMAT_TEXT;
    char *p;
    MALLOC(p, 64);
    // PG 纪元是 2000-01-01；2000-01-02 00:00:00 应为 86400 * 1e6 微秒
    const char *s = "2000-01-02 00:00:00";
    memcpy(p, s, strlen(s));
    pgpack_row cols[1] = { { (int32_t)strlen(s), p, NULL } };
    _pg_reader_push_row(r, p, cols);
    int32_t err;
    int64_t usec = pgsql_reader_timestamp(r, "ts", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, 86400LL * 1000000LL == usec);
    pgsql_reader_free(r);

    // 带 6 位小数秒
    pgsql_reader_ctx *r2 = _pg_reader_new(1, oids, names);
    r2->format = FORMAT_TEXT;
    char *p2;
    MALLOC(p2, 64);
    const char *s2 = "2000-01-01 00:00:01.234567";
    memcpy(p2, s2, strlen(s2));
    pgpack_row cols2[1] = { { (int32_t)strlen(s2), p2, NULL } };
    _pg_reader_push_row(r2, p2, cols2);
    usec = pgsql_reader_timestamp(r2, "ts", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, 1234567LL == usec);
    pgsql_reader_free(r2);
}

// pgsql_reader_date：文本 "YYYY-MM-DD" 解析为相对 PG 纪元天数
static void test_pgsql_reader_date(CuTest *tc) {
    int32_t oids[1] = { DATEOID };
    char names[1][64] = { "d" };
    pgsql_reader_ctx *r = _pg_reader_new(1, oids, names);
    r->format = FORMAT_TEXT;
    char *p;
    MALLOC(p, 16);
    const char *s = "2000-01-02";
    memcpy(p, s, strlen(s));
    pgpack_row cols[1] = { { (int32_t)strlen(s), p, NULL } };
    _pg_reader_push_row(r, p, cols);
    int32_t err;
    int32_t days = pgsql_reader_date(r, "d", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertIntEquals(tc, 1, days);
    pgsql_reader_free(r);
}

// pgsql_reader_timestamp/date 二进制协议：大端定长（timestamp=8 / date=4），长度不符拒绝
// 回归：此前仅文本路径有覆盖，二进制路径与长度校验无单测
static void test_pgsql_reader_temporal_binary(CuTest *tc) {
    int32_t tsoid[1] = { TIMESTAMPOID };
    int32_t doid[1] = { DATEOID };
    char tsname[1][64] = { "ts" };
    char dname[1][64] = { "d" };
    int32_t err;
    char *p;
    pgsql_reader_ctx *r;
    pgpack_row cols[1];
    int64_t usec;
    int32_t days;

    // timestamp 二进制 8 字节大端 int64（PG 纪元微秒）→ OK
    r = _pg_reader_new(1, tsoid, tsname);
    r->format = FORMAT_BINARY;
    MALLOC(p, 8);
    pack_integer(p, (uint64_t)86400000000LL, 8, 0);
    cols[0] = (pgpack_row){ 8, p, NULL };
    _pg_reader_push_row(r, p, cols);
    usec = pgsql_reader_timestamp(r, "ts", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, 86400000000LL == usec);
    pgsql_reader_free(r);

    // timestamp 二进制长度不符（4 字节）→ ERR_FAILED
    r = _pg_reader_new(1, tsoid, tsname);
    r->format = FORMAT_BINARY;
    MALLOC(p, 4);
    pack_integer(p, 1, 4, 0);
    cols[0] = (pgpack_row){ 4, p, NULL };
    _pg_reader_push_row(r, p, cols);
    (void)pgsql_reader_timestamp(r, "ts", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    pgsql_reader_free(r);

    // date 二进制 4 字节大端 int32（PG 纪元天数，1 → 2000-01-02）→ OK
    r = _pg_reader_new(1, doid, dname);
    r->format = FORMAT_BINARY;
    MALLOC(p, 4);
    pack_integer(p, 1, 4, 0);
    cols[0] = (pgpack_row){ 4, p, NULL };
    _pg_reader_push_row(r, p, cols);
    days = pgsql_reader_date(r, "d", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertIntEquals(tc, 1, days);
    pgsql_reader_free(r);

    // date 二进制长度不符（8 字节）→ ERR_FAILED
    r = _pg_reader_new(1, doid, dname);
    r->format = FORMAT_BINARY;
    MALLOC(p, 8);
    pack_integer(p, 1, 8, 0);
    cols[0] = (pgpack_row){ 8, p, NULL };
    _pg_reader_push_row(r, p, cols);
    (void)pgsql_reader_date(r, "d", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    pgsql_reader_free(r);
}

// pgsql_reader_uuid：文本 + 二进制双协议
static void test_pgsql_reader_uuid(CuTest *tc) {
    int32_t oids[1] = { UUIDOID };
    char names[1][64] = { "u" };
    // 文本：36 字符
    pgsql_reader_ctx *r = _pg_reader_new(1, oids, names);
    r->format = FORMAT_TEXT;
    char *p;
    MALLOC(p, 64);
    const char *s = "01020304-0506-0708-090a-0b0c0d0e0f10";
    memcpy(p, s, strlen(s));
    pgpack_row cols[1] = { { (int32_t)strlen(s), p, NULL } };
    _pg_reader_push_row(r, p, cols);
    char uuid[16];
    int32_t err;
    CuAssertIntEquals(tc, ERR_OK, pgsql_reader_uuid(r, "u", uuid, &err));
    CuAssertIntEquals(tc, ERR_OK, err);
    char expect[16];
    for (int i = 0; i < 16; i++) expect[i] = (char)(i + 1);
    CuAssertTrue(tc, 0 == memcmp(uuid, expect, 16));
    pgsql_reader_free(r);

    // 二进制：16 字节原样
    pgsql_reader_ctx *r2 = _pg_reader_new(1, oids, names);
    r2->format = FORMAT_BINARY;
    char *p2;
    MALLOC(p2, 16);
    memcpy(p2, expect, 16);
    pgpack_row cols2[1] = { { 16, p2, NULL } };
    _pg_reader_push_row(r2, p2, cols2);
    char uuid2[16];
    CuAssertIntEquals(tc, ERR_OK, pgsql_reader_uuid(r2, "u", uuid2, &err));
    CuAssertTrue(tc, 0 == memcmp(uuid2, expect, 16));
    pgsql_reader_free(r2);

    // 文本长度不对 → ERR_FAILED
    pgsql_reader_ctx *r3 = _pg_reader_new(1, oids, names);
    r3->format = FORMAT_TEXT;
    char *p3;
    MALLOC(p3, 8);
    memcpy(p3, "badbad", 6);
    pgpack_row cols3[1] = { { 6, p3, NULL } };
    _pg_reader_push_row(r3, p3, cols3);
    CuAssertIntEquals(tc, ERR_FAILED, pgsql_reader_uuid(r3, "u", uuid2, &err));
    CuAssertIntEquals(tc, ERR_FAILED, err);
    pgsql_reader_free(r3);
}

// pgsql_reader_index 直接按列序号取数据
static void test_pgsql_reader_index(CuTest *tc) {
    int32_t oids[2] = { INT4OID, TEXTOID };
    char names[2][64] = { "n", "s" };
    pgsql_reader_ctx *r = _pg_reader_new(2, oids, names);
    r->format = FORMAT_TEXT;
    char *p;
    MALLOC(p, 16);
    memcpy(p, "42", 2);
    memcpy(p + 2, "foo", 3);
    pgpack_row cols[2] = {
        { 2, p, NULL },
        { 3, p + 2, NULL }
    };
    _pg_reader_push_row(r, p, cols);

    pgpack_field *field;
    pgpack_row *row = pgsql_reader_index(r, 0, &field);
    CuAssertPtrNotNull(tc, row);
    CuAssertIntEquals(tc, 2, row->lens);
    CuAssertIntEquals(tc, INT4OID, field->type_oid);

    row = pgsql_reader_index(r, 1, &field);
    CuAssertPtrNotNull(tc, row);
    CuAssertIntEquals(tc, TEXTOID, field->type_oid);

    // 越界
    CuAssertTrue(tc, NULL == pgsql_reader_index(r, -1, &field));
    CuAssertTrue(tc, NULL == pgsql_reader_index(r, 99, &field));

    // 行越界
    pgsql_reader_next(r);
    CuAssertTrue(tc, NULL == pgsql_reader_index(r, 0, &field));

    pgsql_reader_free(r);
}

// _pgpack_error_notice：将 'S'/'M'/'C' 等字段拼接为多行字符串
static void test_pgpack_error_notice(CuTest *tc) {
    // 构造一个 ErrorResponse 风格的字节流：S:ERROR\0M:bad command\0C:42601\0\0
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    binary_set_int8(&bw, 'S');
    binary_set_string(&bw, "ERROR");
    binary_set_int8(&bw, 'M');
    binary_set_string(&bw, "bad command");
    binary_set_int8(&bw, 'C');
    binary_set_string(&bw, "42601");
    binary_set_int8(&bw, 0);

    binary_ctx br;
    binary_init(&br, bw.data, bw.offset, 0);
    char *msg = _pgpack_error_notice(&br);
    CuAssertPtrNotNull(tc, msg);
    // 字符串应含 "S: ERROR", "M: bad command", "C: 42601"
    CuAssertTrue(tc, NULL != strstr(msg, "S: ERROR"));
    CuAssertTrue(tc, NULL != strstr(msg, "M: bad command"));
    CuAssertTrue(tc, NULL != strstr(msg, "C: 42601"));
    FREE(msg);
    binary_free(&bw);
}

// _pgpack_error_notice：空输入 → 空字符串（仅 NUL 结尾）
static void test_pgpack_error_notice_empty(CuTest *tc) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    binary_set_int8(&bw, 0); // 立即结束

    binary_ctx br;
    binary_init(&br, bw.data, bw.offset, 0);
    char *msg = _pgpack_error_notice(&br);
    CuAssertPtrNotNull(tc, msg);
    // 内容应为空（首字节即为 NUL）
    CuAssertIntEquals(tc, 0, (int)msg[0]);
    FREE(msg);
    binary_free(&bw);
}

void test_pgsql_parse(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_pgsql_reader_init);
    SUITE_ADD_TEST(suite, test_pgsql_reader_cursor);
    SUITE_ADD_TEST(suite, test_pgsql_reader_bool);
    SUITE_ADD_TEST(suite, test_pgsql_reader_integer);
    SUITE_ADD_TEST(suite, test_pgsql_reader_double);
    SUITE_ADD_TEST(suite, test_pgsql_reader_isnull_text);
    SUITE_ADD_TEST(suite, test_pgsql_reader_bytea);
    SUITE_ADD_TEST(suite, test_pgsql_reader_timestamp_text);
    SUITE_ADD_TEST(suite, test_pgsql_reader_date);
    SUITE_ADD_TEST(suite, test_pgsql_reader_temporal_binary);
    SUITE_ADD_TEST(suite, test_pgsql_reader_uuid);
    SUITE_ADD_TEST(suite, test_pgsql_reader_index);
    SUITE_ADD_TEST(suite, test_pgpack_error_notice);
    SUITE_ADD_TEST(suite, test_pgpack_error_notice_empty);
}
