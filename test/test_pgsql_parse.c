#include "test_pgsql_parse.h"
#include "lib.h"
#include "protocol/pgsql/pgsql.h"
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

// 往 pgpack 的结果数组里追加一个结果（模拟解析侧 CommandComplete 的提交动作）
static void _pg_result_push(pgpack_ctx *pg, pgsql_reader_ctx *reader, const char *complete) {
    if (NULL == pg->results.ptr) {// 与 _pgpack_complete 同一判据(array_free 会把 ptr 置空)
        array_init(&pg->results, sizeof(pgsql_result), 2);
    }
    pgsql_result res;
    ZERO(&res, sizeof(res));
    res.reader = reader;
    if (NULL != complete) {
        safe_fill_str(res.complete, sizeof(res.complete), complete);
    }
    array_push_back(&pg->results, &res);
}

// pgsql_reader_iter：仅 PGPACK_OK 且结果里有带 reader 的语句才返回，并转移所有权
static void test_pgsql_reader_iter(CuTest *tc) {
    pgpack_ctx pg;
    ZERO(&pg, sizeof(pg));
    int32_t oids[1] = { INT4OID };
    char names[1][64] = { "id" };
    // 类型不符：即使结果数组里有 reader 也不给
    _pg_result_push(&pg, _pg_reader_new(1, oids, names), "SELECT 0");
    pg.type = PGPACK_ERR;
    CuAssertTrue(tc, NULL == pgsql_reader_iter(&pg, FORMAT_TEXT));
    pg.type = PGPACK_NOTIFICATION;
    CuAssertTrue(tc, NULL == pgsql_reader_iter(&pg, FORMAT_TEXT));
    CuAssertIntEquals(tc, 0, (int)pgsql_result_count(&pg)); // 类型不符时结果数恒为 0
    // 正常路径：所有权转给调用方，槽位随之置空。这一点由下面的 reader_at 直接钉住,
    // 不靠 _free_pgpack 毒值 —— iter / at 全程不读那个字段, 毒值永远触发不了
    pg.type = PGPACK_OK;
    CuAssertIntEquals(tc, 1, (int)pgsql_result_count(&pg));
    pgsql_reader_ctx *out = pgsql_reader_iter(&pg, FORMAT_TEXT);
    CuAssertTrue(tc, NULL != out);
    CuAssertIntEquals(tc, FORMAT_TEXT, (int)out->format);
    // 同一下标只能取走一次；取走后结果个数不变（占位仍在，只是 reader 已交出）
    CuAssertTrue(tc, NULL == pgsql_reader_at(&pg, 0, FORMAT_TEXT));
    CuAssertTrue(tc, NULL == pgsql_reader_iter(&pg, FORMAT_TEXT));
    CuAssertIntEquals(tc, 1, (int)pgsql_result_count(&pg));
    pgsql_reader_free(out);
    _pgpack_results_clear(&pg);

    // 空结果数组（如 ping / prepare 那种没有 CommandComplete 的响应）
    pgpack_ctx empty;
    ZERO(&empty, sizeof(empty));
    empty.type = PGPACK_OK;
    CuAssertTrue(tc, NULL == pgsql_reader_iter(&empty, FORMAT_TEXT));
    CuAssertIntEquals(tc, 0, (int)pgsql_result_count(&empty));
    CuAssertTrue(tc, 0 == pgsql_affected_at(&empty, 0));
}

// 多结果集：按下标取 reader / affected，越界与无结果集语句的取值
static void test_pgsql_result_multi(CuTest *tc) {
    int32_t oids[1] = { INT4OID };
    char names[1][64] = { "id" };
    pgpack_ctx pg;
    ZERO(&pg, sizeof(pg));
    pg.type = PGPACK_OK;
    // 三条语句：SELECT 有结果集、INSERT 无结果集、SELECT 有结果集
    pgsql_reader_ctx *r0 = _pg_reader_new(1, oids, names);
    pgsql_reader_ctx *r2 = _pg_reader_new(1, oids, names);
    _pg_result_push(&pg, r0, "SELECT 1");
    _pg_result_push(&pg, NULL, "INSERT 0 7");
    _pg_result_push(&pg, r2, "SELECT 2");
    safe_fill_str(pg.complete, sizeof(pg.complete), "SELECT 2"); // 整包标签＝最后一条
    CuAssertIntEquals(tc, 3, (int)pgsql_result_count(&pg));
    // 逐条 affected：中间那条无结果集，行数照样取得到
    CuAssertTrue(tc, 1 == pgsql_affected_at(&pg, 0));
    CuAssertTrue(tc, 7 == pgsql_affected_at(&pg, 1));
    CuAssertTrue(tc, 2 == pgsql_affected_at(&pg, 2));
    CuAssertTrue(tc, 2 == pgsql_affected_rows(&pg)); // 单结果 API 仍是最后一条
    CuAssertTrue(tc, 0 == pgsql_affected_at(&pg, 3)); // 越界
    // 按下标取 reader：下标 1 无结果集给 NULL，下标 3 越界给 NULL
    CuAssertTrue(tc, NULL == pgsql_reader_at(&pg, 1, FORMAT_TEXT));
    CuAssertTrue(tc, NULL == pgsql_reader_at(&pg, 3, FORMAT_TEXT));
    // 乱序取走不串扰
    pgsql_reader_ctx *o2 = pgsql_reader_at(&pg, 2, FORMAT_BINARY);
    CuAssertTrue(tc, o2 == r2);
    CuAssertIntEquals(tc, FORMAT_BINARY, (int)o2->format);
    pgsql_reader_ctx *o0 = pgsql_reader_at(&pg, 0, FORMAT_TEXT);
    CuAssertTrue(tc, o0 == r0);
    CuAssertIntEquals(tc, FORMAT_TEXT, (int)o0->format);
    CuAssertTrue(tc, NULL == pgsql_reader_at(&pg, 2, FORMAT_TEXT)); // 已取走
    pgsql_reader_free(o0);
    pgsql_reader_free(o2);
    _pgpack_results_clear(&pg);

    // reader_iter 是迭代器：连调给的是不同的结果，取完返回 NULL
    pgpack_ctx it;
    ZERO(&it, sizeof(it));
    it.type = PGPACK_OK;
    pgsql_reader_ctx *ia = _pg_reader_new(1, oids, names);
    pgsql_reader_ctx *ib = _pg_reader_new(1, oids, names);
    _pg_result_push(&it, ia, "SELECT 1");
    _pg_result_push(&it, NULL, "INSERT 0 1");// 中间夹一条无结果集的，须被跳过
    _pg_result_push(&it, ib, "SELECT 2");
    pgsql_reader_ctx *first = pgsql_reader_iter(&it, FORMAT_TEXT);
    pgsql_reader_ctx *second = pgsql_reader_iter(&it, FORMAT_TEXT);
    CuAssertTrue(tc, first == ia);
    CuAssertTrue(tc, second == ib);
    CuAssertTrue(tc, NULL == pgsql_reader_iter(&it, FORMAT_TEXT));
    pgsql_reader_free(first);
    pgsql_reader_free(second);
    _pgpack_results_clear(&it);

    // 没被取走的结果集由 _pgpack_free 回收：漏掉这段回收，收尾的 memory check 会报未释放。
    // 堆上分配 pgpack——_pgpack_free 连外壳一起 FREE，栈变量喂不得
    pgpack_ctx *heap;
    CALLOC(heap, 1, sizeof(pgpack_ctx));
    heap->type = PGPACK_OK;
    pgsql_reader_ctx *keep = _pg_reader_new(1, oids, names);
    char *payload;
    MALLOC(payload, 8);
    pgpack_row cols[1] = { { 1, payload, NULL } };
    _pg_reader_push_row(keep, payload, cols);// 带行数据，漏回收时泄漏的不止 reader 本身
    _pg_result_push(heap, keep, "SELECT 1");
    _pg_result_push(heap, _pg_reader_new(1, oids, names), "SELECT 2");
    _pgpack_free(heap);

    // 回归：首条语句无结果集（BEGIN; SELECT / SET; SELECT）时，reader_iter 仍须给出后面那条的行，
    // 不能因为下标 0 是空的就报"无结果集"
    pgpack_ctx lead;
    ZERO(&lead, sizeof(lead));
    lead.type = PGPACK_OK;
    pgsql_reader_ctx *rsel = _pg_reader_new(1, oids, names);
    _pg_result_push(&lead, NULL, "BEGIN");
    _pg_result_push(&lead, rsel, "SELECT 1");
    pgsql_reader_ctx *lout = pgsql_reader_iter(&lead, FORMAT_TEXT);
    CuAssertTrue(tc, lout == rsel);
    pgsql_reader_free(lout);
    _pgpack_results_clear(&lead);
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

// 文本协议下取一个 int8 字段，val/lens 由用例给；返回值经 out 带出，err 直接返回
static int32_t _pg_int_text(const char *val, int32_t lens, int64_t *out) {
    int32_t oids[1] = { INT8OID };
    char names[1][64] = { "n" };
    pgsql_reader_ctx *r = _pg_reader_new(1, oids, names);
    r->format = FORMAT_TEXT;
    char *p;
    MALLOC(p, (lens > 0) ? (size_t)lens : 1);
    if (lens > 0) {
        memcpy(p, val, (size_t)lens);
    }
    pgpack_row cols[1] = { { lens, p, NULL } };
    _pg_reader_push_row(r, p, cols);
    int32_t err;
    int64_t v = pgsql_reader_integer(r, "n", &err);
    SET_PTR(out, v);
    pgsql_reader_free(r);
    return err;
}

// 文本整数的边界：空串与溢出原先都骗得过 end-tmp 判等被当成解析成功
static void test_pgsql_reader_integer_bounds(CuTest *tc) {
    int64_t v = -1;
    // 空串是合法线格式(lens=0，与 -1 的 NULL 不同)，但不是一个数 → 必须失败
    CuAssertIntEquals(tc, ERR_FAILED, _pg_int_text("", 0, &v));
    // 溢出：strtoll 会钳到 LLONG_MAX 且 end 走到串尾，旧判据放行
    CuAssertIntEquals(tc, ERR_FAILED, _pg_int_text("99999999999999999999", 20, &v));
    CuAssertIntEquals(tc, ERR_FAILED, _pg_int_text("9223372036854775808", 19, &v));// INT64_MAX + 1
    // strtoll 静默接受的前导空白 / '+' / 尾随垃圾，一律拒
    CuAssertIntEquals(tc, ERR_FAILED, _pg_int_text(" 1", 2, &v));
    CuAssertIntEquals(tc, ERR_FAILED, _pg_int_text("+1", 2, &v));
    CuAssertIntEquals(tc, ERR_FAILED, _pg_int_text("1x", 2, &v));
    CuAssertIntEquals(tc, ERR_FAILED, _pg_int_text("-", 1, &v));
    // 合法边界值照常
    CuAssertIntEquals(tc, ERR_OK, _pg_int_text("9223372036854775807", 19, &v));
    CuAssertTrue(tc, INT64_MAX == v);
    CuAssertIntEquals(tc, ERR_OK, _pg_int_text("-9223372036854775808", 20, &v));
    CuAssertTrue(tc, INT64_MIN == v);
    CuAssertIntEquals(tc, ERR_OK, _pg_int_text("-42", 3, &v));
    CuAssertTrue(tc, -42 == v);
    CuAssertIntEquals(tc, ERR_OK, _pg_int_text("0", 1, &v));
    CuAssertTrue(tc, 0 == v);
}

// 文本浮点：空串同样骗得过判等；但 Infinity / NaN 是 PostgreSQL 真会发的值，必须放行
static void test_pgsql_reader_double_bounds(CuTest *tc) {
    int32_t oids[1] = { FLOAT8OID };
    char names[1][64] = { "d" };
    int32_t err;

    pgsql_reader_ctx *r = _pg_reader_new(1, oids, names);
    r->format = FORMAT_TEXT;
    char *p;
    MALLOC(p, 1);
    pgpack_row cols[1] = { { 0, p, NULL } };// lens=0：合法空字符串，但不是一个数
    _pg_reader_push_row(r, p, cols);
    pgsql_reader_double(r, "d", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    pgsql_reader_free(r);

    pgsql_reader_ctx *r2 = _pg_reader_new(1, oids, names);
    r2->format = FORMAT_TEXT;
    char *p2;
    MALLOC(p2, 8);
    memcpy(p2, "Infinity", 8);
    pgpack_row cols2[1] = { { 8, p2, NULL } };
    _pg_reader_push_row(r2, p2, cols2);
    double d = pgsql_reader_double(r2, "d", &err);
    CuAssertIntEquals(tc, ERR_OK, err);// PostgreSQL float8 文本格式就发这个，拒了是回归
    CuAssertTrue(tc, d > 0 && d * 2 == d);// 无穷大
    pgsql_reader_free(r2);

    // NaN 单独一行：塞在 Infinity 那块 buffer 的尾部不算测到，列长度只声明了 8 字节
    pgsql_reader_ctx *r3 = _pg_reader_new(1, oids, names);
    r3->format = FORMAT_TEXT;
    char *p3;
    MALLOC(p3, 3);
    memcpy(p3, "NaN", 3);
    pgpack_row cols3[1] = { { 3, p3, NULL } };
    _pg_reader_push_row(r3, p3, cols3);
    double dn = pgsql_reader_double(r3, "d", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, dn != dn);// NaN 是唯一不等于自己的值
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

// float8 文本值的严格判定与 mysql 侧共用 parse_double_strict：整段消费完 + 拒上溢 + 放行下溢。
// 改造前这里只查"消费长度相符"，"1e400" 会带着 inf 和 ERR_OK 交出去，同一个值在 mysql 侧却被拒
static void test_pgsql_reader_double_text_bounds(CuTest *tc) {
    int32_t oids[1] = { FLOAT8OID };
    char names[1][64] = { "d" };
    int32_t err;
    const char *cases[] = { "1e400", "-1e400", "", "1.5x" };
    pgsql_reader_ctx *r;
    char *p;
    size_t n;

    // 上溢 / 空串 / 有残留字符：一律拒
    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        r = _pg_reader_new(1, oids, names);
        r->format = FORMAT_TEXT;
        n = strlen(cases[i]);
        MALLOC(p, n + 1);
        memcpy(p, cases[i], n);
        pgpack_row cols[1] = { { (int32_t)n, p, NULL } };
        _pg_reader_push_row(r, p, cols);
        (void)pgsql_reader_double(r, "d", &err);
        CuAssertIntEquals(tc, ERR_FAILED, err);
        pgsql_reader_free(r);
    }

    // 下溢：ERANGE 也置位，但返回的是正确的次正规数，必须放行
    r = _pg_reader_new(1, oids, names);
    r->format = FORMAT_TEXT;
    MALLOC(p, 8);
    memcpy(p, "1e-320", 6);
    pgpack_row sub[1] = { { 6, p, NULL } };
    _pg_reader_push_row(r, p, sub);
    double dv = pgsql_reader_double(r, "d", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, dv > 0.0 && dv < 1e-300);
    pgsql_reader_free(r);

    // 常规值不受影响
    r = _pg_reader_new(1, oids, names);
    r->format = FORMAT_TEXT;
    MALLOC(p, 8);
    memcpy(p, "-2.25", 5);
    pgpack_row ok[1] = { { 5, p, NULL } };
    _pg_reader_push_row(r, p, ok);
    CuAssertTrue(tc, -2.25 == pgsql_reader_double(r, "d", &err));
    CuAssertIntEquals(tc, ERR_OK, err);
    pgsql_reader_free(r);
}

// 造一条文本行读 timestamp，返回值经 err 判定；用例多，抽出来省掉重复的 reader 搭建
static int64_t _pg_text_ts(const char *s, int32_t *err) {
    int32_t oids[1] = { TIMESTAMPOID };
    char names[1][64] = { "ts" };
    pgsql_reader_ctx *r = _pg_reader_new(1, oids, names);
    r->format = FORMAT_TEXT;
    size_t n = strlen(s);
    char *p;
    MALLOC(p, n);
    memcpy(p, s, n);
    pgpack_row cols[1] = { { (int32_t)n, p, NULL } };
    _pg_reader_push_row(r, p, cols);
    int64_t v = pgsql_reader_timestamp(r, "ts", err);
    pgsql_reader_free(r);
    return v;
}
// 同上，读 date
static int32_t _pg_text_date(const char *s, int32_t *err) {
    int32_t oids[1] = { DATEOID };
    char names[1][64] = { "d" };
    pgsql_reader_ctx *r = _pg_reader_new(1, oids, names);
    r->format = FORMAT_TEXT;
    size_t n = strlen(s);
    char *p;
    MALLOC(p, n);
    memcpy(p, s, n);
    pgpack_row cols[1] = { { (int32_t)n, p, NULL } };
    _pg_reader_push_row(r, p, cols);
    int32_t v = pgsql_reader_date(r, "d", err);
    pgsql_reader_free(r);
    return v;
}

// 文本时间戳/日期改走 _strptime 后的边界：逐字段量程由 _conv_num 校验，
// 原来的 sscanf("%d-%d-%d ...") 什么都收，越界值会被 _pgsql_date_to_days 算成垃圾天数
static void test_pgsql_reader_temporal_text_range(CuTest *tc) {
    int32_t err;

    // 1) 时区偏移：从日期时间之后起扫，不再按固定下标 11 起跳
    //    PG 纪元 2000-01-01 00:00:00+08 → UTC 侧早 8 小时
    int64_t usec = _pg_text_ts("2000-01-01 00:00:00+08", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, -8LL * 3600 * 1000000LL == usec);

    // 2) 带秒的历史 LMT 偏移 "+05:30:00"（%z 认不了，仍走手写扫描）
    usec = _pg_text_ts("2000-01-01 00:00:00+05:30:00", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, -(5LL * 3600 + 30 * 60) * 1000000LL == usec);

    // 3) 小数秒 + 时区同时出现，两段各取各的
    usec = _pg_text_ts("2000-01-01 00:00:00.000500+01", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, 500LL - 3600LL * 1000000LL == usec);

    // 4) 月 / 日 / 时越界一律拒绝（改造前 sscanf 全收，mktime / date_to_days 算出别的日期）
    (void)_pg_text_ts("2000-13-01 00:00:00", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    (void)_pg_text_ts("2000-01-32 00:00:00", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    (void)_pg_text_ts("2000-01-01 24:00:00", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    (void)_pg_text_ts("2000-01-01 00:60:00", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);

    // 5) 日期侧同样校验；BC 后缀不受影响，仍取补数年份（公元前 44 年 → 负天数）
    int32_t days = _pg_text_date("0044-03-15 BC", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, days < 0);
    (void)_pg_text_date("2000-13-01", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);

    // 6) 已知功能收窄：%Y 量程 0..9999，PG 支持到 5874897 AD 的年份现在被拒
    //    换来的是上面那组越界值不再被静默接受，取舍见 pgsql_reader.c 注释
    (void)_pg_text_date("10000-01-01", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
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

// 回归：ErrorResponse 的字段值没有 NUL 结尾时，取串不得撞断言把进程 abort
static void test_pgpack_error_notice_unterminated(CuTest *tc) {
    char raw[] = { 'S', 'E', 'R', 'R' }; // 标志 'S' 后跟 3 字节且不带 NUL
    binary_ctx br;
    binary_init(&br, raw, sizeof(raw), 0);
    char *msg = _pgpack_error_notice(&br);
    CuAssertPtrNotNull(tc, msg);
    CuAssertIntEquals(tc, 0, (int)msg[0]); // 这一字段整个丢掉，结果是空串
    FREE(msg);
}

// 回归：5 字节报文（长度字段声明 4，即消息体为空）不得让 binary_get_* 的断言 abort 整个进程。
// _pgsql_payload 只拒 lens < 4，lens == 4 合法 → 类型码 1 字节 + 长度 4 字节读完就到头，
// 下面这些分支的首个读全都落在零剩余上
static void test_pgpack_parser_empty_body(CuTest *tc) {
    static const char codes[] = { 'A', 'T', 'D', 'G', 'H', 'C', 'Z' };
    pgsql_ctx pg;
    ud_cxt ud;
    binary_ctx br;
    char *raw;
    int32_t status;
    for (size_t i = 0; i < sizeof(codes); i++) {
        ZERO(&pg, sizeof(pg));
        ZERO(&ud, sizeof(ud));
        MALLOC(raw, 5);
        raw[0] = codes[i];
        pack_integer(raw + 1, 4, 4, 0); // 长度字段含自身，4 即消息体为空
        binary_init(&br, raw, 5, 0);
        status = PROT_INIT;
        CuAssertTrue(tc, NULL == _pgpack_parser(&pg, &br, &ud, &status));
        CuAssert(tc, "an empty message body must be a protocol error, not an abort",
            BIT_CHECK(status, PROT_ERROR));
        _pgpack_free(pg.pack);
    }
}

// 回归：RowDescription 声明的列数对得上总长，但某个列名超长把后面的列挤出报文时也须判失败
static void test_pgpack_row_description_overlong_name(CuTest *tc) {
    // 2 列 → 需 2*19=38 字节；给足 40 字节，但第一列名字就吃掉 30 字节
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    binary_set_integer(&bw, 2, 2, 0);
    binary_set_string(&bw, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaa"); // 29 字符 + NUL
    for (int32_t i = 0; i < 12; i++) {
        binary_set_int8(&bw, 0); // 只补 12 字节，第一列的 18 字节定长段都不够
    }
    pgsql_ctx pg;
    ZERO(&pg, sizeof(pg));
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    char *raw;
    MALLOC(raw, 5 + bw.offset);
    raw[0] = 'T';
    pack_integer(raw + 1, 4 + bw.offset, 4, 0);
    memcpy(raw + 5, bw.data, bw.offset);
    binary_ctx br;
    binary_init(&br, raw, 5 + bw.offset, 0);
    int32_t status = PROT_INIT;
    CuAssertTrue(tc, NULL == _pgpack_parser(&pg, &br, &ud, &status));
    CuAssert(tc, "a field name that crowds out later columns must fail",
        BIT_CHECK(status, PROT_ERROR));
    _pgpack_free(pg.pack);
    binary_free(&bw);
}

// pgsql_affected_rows：从 CommandComplete 标签末尾取行数；>2^31 须用 int64 不回绕成负数
static void test_pgsql_affected_rows(CuTest *tc) {
    pgpack_ctx pg;
    ZERO(&pg, sizeof(pg));
    // 常规标签取末尾数字（UPDATE rows / INSERT oid rows）
    safe_fill_str(pg.complete, sizeof(pg.complete), "UPDATE 5");
    CuAssertTrue(tc, 5 == pgsql_affected_rows(&pg));
    safe_fill_str(pg.complete, sizeof(pg.complete), "INSERT 0 3");
    CuAssertTrue(tc, 3 == pgsql_affected_rows(&pg));
    // 空标签 → 0
    pg.complete[0] = '\0';
    CuAssertTrue(tc, 0 == pgsql_affected_rows(&pg));
    // 回归：>2^31 行不得截断为负数（曾 (int32_t)strtol 回绕成 -1294967296）
    safe_fill_str(pg.complete, sizeof(pg.complete), "UPDATE 3000000000");
    CuAssertTrue(tc, 3000000000LL == pgsql_affected_rows(&pg));
}

// pgsql_set_userpwd 的契约是"任一项超长则两个字段都保持原值"。这条不能靠 safe_fill_str
// 边填边判来实现：函数里 secure_zero 排在两次填充之前，一旦折掉前置校验，密码超长的那次
// 调用会留下"新用户名 + 空密码"。单字段的 set_db 没这个问题，靠 safe_fill_str 自身即可
static void test_pgsql_setter_atomic(CuTest *tc) {
    char toolong[128];
    memset(toolong, 'x', sizeof(toolong) - 1);
    toolong[sizeof(toolong) - 1] = '\0';

    pgsql_ctx pg;
    CuAssertIntEquals(tc, ERR_OK, pgsql_init(&pg, "127.0.0.1", 5432, NULL, "u1", "p1", "db1"));
    CuAssertStrEquals(tc, "u1", pg.user);
    CuAssertStrEquals(tc, "p1", pg.password);

    // 用户名超长：两个字段都不动
    CuAssertIntEquals(tc, ERR_FAILED, pgsql_set_userpwd(&pg, toolong, "p2"));
    CuAssertStrEquals(tc, "u1", pg.user);
    CuAssertStrEquals(tc, "p1", pg.password);
    // 密码超长：同样两个都不动（折掉前置校验后这里会变成 u2 + 空串）
    CuAssertIntEquals(tc, ERR_FAILED, pgsql_set_userpwd(&pg, "u2", toolong));
    CuAssertStrEquals(tc, "u1", pg.user);
    CuAssertStrEquals(tc, "p1", pg.password);
    // 合法则两个一起换
    CuAssertIntEquals(tc, ERR_OK, pgsql_set_userpwd(&pg, "u2", "p2"));
    CuAssertStrEquals(tc, "u2", pg.user);
    CuAssertStrEquals(tc, "p2", pg.password);

    // 单字段 setter：超长不改动原值，由 safe_fill_str "装不下就不写" 保证
    CuAssertIntEquals(tc, ERR_FAILED, pgsql_set_db(&pg, toolong));
    CuAssertStrEquals(tc, "db1", pg.database);
    CuAssertIntEquals(tc, ERR_OK, pgsql_set_db(&pg, "db2"));
    CuAssertStrEquals(tc, "db2", pg.database);
}
void test_pgsql_parse(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_pgsql_reader_iter);
    SUITE_ADD_TEST(suite, test_pgsql_result_multi);
    SUITE_ADD_TEST(suite, test_pgsql_reader_cursor);
    SUITE_ADD_TEST(suite, test_pgsql_reader_bool);
    SUITE_ADD_TEST(suite, test_pgsql_reader_integer);
    SUITE_ADD_TEST(suite, test_pgsql_reader_integer_bounds);
    SUITE_ADD_TEST(suite, test_pgsql_reader_double);
    SUITE_ADD_TEST(suite, test_pgsql_reader_double_bounds);
    SUITE_ADD_TEST(suite, test_pgsql_reader_isnull_text);
    SUITE_ADD_TEST(suite, test_pgsql_reader_bytea);
    SUITE_ADD_TEST(suite, test_pgsql_reader_timestamp_text);
    SUITE_ADD_TEST(suite, test_pgsql_reader_date);
    SUITE_ADD_TEST(suite, test_pgsql_reader_temporal_text_range);
    SUITE_ADD_TEST(suite, test_pgsql_reader_double_text_bounds);
    SUITE_ADD_TEST(suite, test_pgsql_reader_temporal_binary);
    SUITE_ADD_TEST(suite, test_pgsql_reader_uuid);
    SUITE_ADD_TEST(suite, test_pgsql_reader_index);
    SUITE_ADD_TEST(suite, test_pgpack_error_notice);
    SUITE_ADD_TEST(suite, test_pgpack_error_notice_empty);
    SUITE_ADD_TEST(suite, test_pgpack_error_notice_unterminated);
    SUITE_ADD_TEST(suite, test_pgpack_parser_empty_body);
    SUITE_ADD_TEST(suite, test_pgpack_row_description_overlong_name);
    SUITE_ADD_TEST(suite, test_pgsql_affected_rows);
    SUITE_ADD_TEST(suite, test_pgsql_setter_atomic);
}
