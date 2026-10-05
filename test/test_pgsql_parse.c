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

// 解包桩共用的"无连接"标识: 取代旧的 (INVALID_SOCK, 0) 实参对
static sock_ctx _t_nosk = { INVALID_SOCK, INVALID_INDEX, 0 };
// 解包入口的 ev 与连接标识在测试里恒为空：只喂缓冲，不发包也不认连接。
// 三个恒定实参收进薄封装，签名再变时只改这里，不必逐个改调用点。
// 解包侧直接写 *size，调用点传 NULL 时换成局部变量
static void *_t_pgsql_unpack(int32_t client, buffer_ctx *buf, ud_cxt *ud,
    size_t *size, int32_t *status) {
    size_t sink;
    return pgsql_unpack(NULL, &_t_nosk, client, buf, ud, (NULL != size) ? size : &sink, status);
}

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
            r->fields[i].nlens = (uint8_t)strlen(r->fields[i].name);
        }
    }
    pgrow_arr_init(&r->arr_rows, 0);
    return r;
}

// 给 reader 添加一行；cols[i].lens 设为 -1 表示该列 NULL。
// 同解析侧：payload 与行数组拼成一段、从 reader 的块链切，reader 释放时整链一起还。
// 调用方交出 *payload 且列值都指向它内部：这里把用到的那段搬进块链、列值按偏移改指新位置，
// 释放原 payload 后把 *payload 改指新位置，调用方之后照旧能用它读(reader 释放前有效)
static void _pg_reader_push_row(pgsql_reader_ctx *r, char **ppayload,
                                const pgpack_row *cols) {
    char *payload = *ppayload;
    size_t used = 0, end, off;
    uint16_t i;
    for (i = 0; i < r->field_count; i++) {
        if (NULL != cols[i].val) {
            end = (size_t)(cols[i].val - payload) + (cols[i].lens > 0 ? (size_t)cols[i].lens : 0);
            used = end > used ? end : used;
        }
    }
    off = ROUND_UP(used, 8);
    char *block = mem_arena_alloc(&r->arena, off + sizeof(pgpack_row) * r->field_count);
    memcpy(block, payload, used);
    pgpack_row *row = (pgpack_row *)(block + off);
    ZERO(row, sizeof(pgpack_row) * r->field_count);
    for (i = 0; i < r->field_count; i++) {
        row[i].lens = cols[i].lens;
        row[i].val = (NULL == cols[i].val) ? NULL : block + (cols[i].val - payload);
    }
    FREE(payload);
    *ppayload = block;
    pgrow_arr_push_back(&r->arr_rows, &row);
}

// 造一个"单列单行、值为给定字节串"的 reader。payload 由 reader 释放
static pgsql_reader_ctx *_pg_reader_one(const int32_t *oids, char (*names)[64],
    pgpack_format fmt, const char *val, int32_t lens) {
    pgsql_reader_ctx *r = _pg_reader_new(1, oids, names);
    r->format = fmt;
    char *p;
    MALLOC(p, (size_t)lens + 1);
    memcpy(p, val, (size_t)lens);
    p[lens] = '\0';
    pgpack_row cols[1] = { { lens, p } };
    _pg_reader_push_row(r, &p, cols);
    return r;
}

// 往 pgpack 的结果数组里追加一个结果（模拟解析侧 CommandComplete 的提交动作）
static void _pg_result_push(pgpack_ctx *pg, pgsql_reader_ctx *reader, const char *complete) {
    if (NULL == pg->results.ptr) {// 与 _pgpack_complete 同一判据(pgres_arr_free 会把 ptr 置空)
        pgres_arr_init(&pg->results, 2);
    }
    pgsql_result res;
    ZERO(&res, sizeof(res));
    res.reader = reader;
    if (NULL != complete) {
        safe_fill_str(res.complete, sizeof(res.complete), complete);
    }
    pgres_arr_push_back(&pg->results, &res);
}
// 与 _pg_result_push 成对的清理。库里那份 _pgpack_results_clear 是 static,
// 这里照它的逻辑自建一份,不为测试把它提成公共函数
static void _pg_results_clear(pgpack_ctx *pgpack) {
    pgsql_result *res;
    for (uint32_t i = 0; i < pgres_arr_size(&pgpack->results); i++) {
        res = pgres_arr_at(&pgpack->results, (int32_t)i);
        if (NULL != res->reader) {
            pgsql_reader_free(res->reader);
        }
    }
    pgres_arr_free(&pgpack->results);
    pgpack->iter_cursor = 0;
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
    _pg_results_clear(&pg);

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
    _pg_results_clear(&pg);

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
    _pg_results_clear(&it);

    // 没被取走的结果集由 _pgpack_free 回收：漏掉这段回收，收尾的 memory check 会报未释放。
    // 堆上分配 pgpack——_pgpack_free 连外壳一起 FREE，栈变量喂不得
    pgpack_ctx *heap;
    CALLOC(heap, 1, sizeof(pgpack_ctx));
    heap->type = PGPACK_OK;
    pgsql_reader_ctx *keep = _pg_reader_new(1, oids, names);
    char *payload;
    MALLOC(payload, 8);
    pgpack_row cols[1] = { { 1, payload } };
    _pg_reader_push_row(keep, &payload, cols);// 带行数据，漏回收时泄漏的不止 reader 本身
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
    _pg_results_clear(&lead);
}

// pgsql_reader_size/seek/eof/next 游标语义
static void test_pgsql_reader_cursor(CuTest *tc) {
    int32_t oids[1] = { INT4OID };
    char names[1][64] = { "id" };
    pgsql_reader_ctx *r = _pg_reader_new(1, oids, names);

    CuAssertIntEquals(tc, 0, (int)pgsql_reader_size(r));
    CuAssertIntEquals(tc, 1, pgsql_reader_eof(r));

    // 3 行
    char *p;
    for (int i = 0; i < 3; i++) {
        MALLOC(p, 4);
        p[0] = (char)('1' + i);
        pgpack_row cols[1] = { { 1, p } };
        _pg_reader_push_row(r, &p, cols);
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
        { 4, p },// a: "true"
        { 2, p + 4 },// b: "no"（不在真值列表，返回 0 但 err=ERR_OK）
        { -1, NULL }// c: NULL (int4)
    };
    _pg_reader_push_row(r, &p, cols);

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
    pgsql_reader_ctx *r = _pg_reader_one(oids, names, FORMAT_TEXT, "12345", 5);

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
    pgpack_row cols2[1] = { { 4, p2 } };
    _pg_reader_push_row(r2, &p2, cols2);
    CuAssertTrue(tc, 0x12345678 == pgsql_reader_integer(r2, "n", &err));
    CuAssertIntEquals(tc, ERR_OK, err);
    pgsql_reader_free(r2);

    // NULL 字段
    pgsql_reader_ctx *r3 = _pg_reader_new(1, oids, names);
    r3->format = FORMAT_TEXT;
    char *p3;
    MALLOC(p3, 4);
    pgpack_row cols3[1] = { { -1, NULL } };
    _pg_reader_push_row(r3, &p3, cols3);
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
    pgpack_row cols[1] = { { lens, p } };
    _pg_reader_push_row(r, &p, cols);
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
    pgpack_row cols[1] = { { 0, p } };// lens=0：合法空字符串，但不是一个数
    _pg_reader_push_row(r, &p, cols);
    pgsql_reader_double(r, "d", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    pgsql_reader_free(r);

    pgsql_reader_ctx *r2 = _pg_reader_one(oids, names, FORMAT_TEXT, "Infinity", 8);
    double d = pgsql_reader_double(r2, "d", &err);
    CuAssertIntEquals(tc, ERR_OK, err);// PostgreSQL float8 文本格式就发这个，拒了是回归
    CuAssertTrue(tc, d > 0 && d * 2 == d);// 无穷大
    pgsql_reader_free(r2);

    // NaN 单独一行：塞在 Infinity 那块 buffer 的尾部不算测到，列长度只声明了 8 字节
    pgsql_reader_ctx *r3 = _pg_reader_one(oids, names, FORMAT_TEXT, "NaN", 3);
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
    pgsql_reader_ctx *r = _pg_reader_one(oids, names, FORMAT_TEXT, "3.14159", 7);
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
    pgpack_row cols2[1] = { { 8, p2 } };
    _pg_reader_push_row(r2, &p2, cols2);
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
    pgpack_row cols3[1] = { { 4, p3 } };
    _pg_reader_push_row(r3, &p3, cols3);
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
    pgpack_row cols4[1] = { { 4, p4 } };
    _pg_reader_push_row(r4, &p4, cols4);
    pgsql_reader_double(r4, "d", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    pgsql_reader_free(r4);
    // FLOAT4OID 给 8 字节 → 拒绝
    pgsql_reader_ctx *r5 = _pg_reader_new(1, oids3, names);
    r5->format = FORMAT_BINARY;
    char *p5;
    MALLOC(p5, 8);
    pack_double(p5, 2.0, 0);
    pgpack_row cols5[1] = { { 8, p5 } };
    _pg_reader_push_row(r5, &p5, cols5);
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
        { 5, p },
        { -1, NULL }
    };
    _pg_reader_push_row(r, &p, cols);

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
    pgpack_row cols[1] = { { 4, p } };
    _pg_reader_push_row(r, &p, cols);
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
    pgpack_row cols[1] = { { (int32_t)strlen(s), p } };
    _pg_reader_push_row(r, &p, cols);
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
    pgpack_row cols2[1] = { { (int32_t)strlen(s2), p2 } };
    _pg_reader_push_row(r2, &p2, cols2);
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
    pgpack_row cols[1] = { { (int32_t)strlen(s), p } };
    _pg_reader_push_row(r, &p, cols);
    int32_t err;
    int32_t days = pgsql_reader_date(r, "d", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertIntEquals(tc, 1, days);
    pgsql_reader_free(r);
}

// float8 文本值的严格判定与 mysql 侧共用 strtod_s：整段消费完 + 拒上溢 + 放行下溢。
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
        pgpack_row cols[1] = { { (int32_t)n, p } };
        _pg_reader_push_row(r, &p, cols);
        (void)pgsql_reader_double(r, "d", &err);
        CuAssertIntEquals(tc, ERR_FAILED, err);
        pgsql_reader_free(r);
    }

    // 下溢：ERANGE 也置位，但返回的是正确的次正规数，必须放行
    r = _pg_reader_new(1, oids, names);
    r->format = FORMAT_TEXT;
    MALLOC(p, 8);
    memcpy(p, "1e-320", 6);
    pgpack_row sub[1] = { { 6, p } };
    _pg_reader_push_row(r, &p, sub);
    double dv = pgsql_reader_double(r, "d", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, dv > 0.0 && dv < 1e-300);
    pgsql_reader_free(r);

    // 常规值不受影响
    r = _pg_reader_new(1, oids, names);
    r->format = FORMAT_TEXT;
    MALLOC(p, 8);
    memcpy(p, "-2.25", 5);
    pgpack_row ok[1] = { { 5, p } };
    _pg_reader_push_row(r, &p, ok);
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
    pgpack_row cols[1] = { { (int32_t)n, p } };
    _pg_reader_push_row(r, &p, cols);
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
    pgpack_row cols[1] = { { (int32_t)n, p } };
    _pg_reader_push_row(r, &p, cols);
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

// 时区偏移改用 parse_colon_triple 之后的收窄。原来 sscanf 的返回值被丢掉：越界段当成功用
// （"+24" 真按 24 小时算），压根不是数字的留 0 偏移静默当 UTC，位数装不下 int 的还是有符号溢出。
// 现在这三类都拒掉整个值——PG 自己吐的时区永远合法，能走到这儿的只有坏数据
static void test_pgsql_reader_timezone_reject(CuTest *tc) {
    int32_t err;

    // 1) 负偏移方向：'-05' 是比 UTC 晚 5 小时，换算回去要加（正向的 '+08' 在上一条用例）
    int64_t usec = _pg_text_ts("2000-01-01 00:00:00-05", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, 5LL * 3600 * 1000000LL == usec);

    // 2) 缺的段填 0：只给到分钟照样算成功
    usec = _pg_text_ts("2000-01-01 00:00:00+05:30", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, -(5LL * 3600 + 30 * 60) * 1000000LL == usec);

    // 3) 三段各自越界
    (void)_pg_text_ts("2000-01-01 00:00:00+24", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    (void)_pg_text_ts("2000-01-01 00:00:00+00:60", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    (void)_pg_text_ts("2000-01-01 00:00:00+00:00:60", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);

    // 4) 符号后不是数字：原来一段都没匹配上，留 0 偏移当 UTC 用
    (void)_pg_text_ts("2000-01-01 00:00:00+", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    (void)_pg_text_ts("2000-01-01 00:00:00+ab", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);

    // 5) 位数装不下 int：原来是 %d 的有符号溢出
    (void)_pg_text_ts("2000-01-01 00:00:00+99999999999", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);

    // 6) 无时区照常成功：扫描从日期时间之后起步，别把日期里那两个减号误当偏移
    usec = _pg_text_ts("2000-01-01 00:00:00", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, 0 == usec);

    // 7) 时区后面还跟着 " BC"：偏移段取到就收手，尾巴上的非数字不算错
    usec = _pg_text_ts("0044-03-15 12:00:00+05 BC", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, usec < 0);
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
    cols[0] = (pgpack_row){ 8, p };
    _pg_reader_push_row(r, &p, cols);
    usec = pgsql_reader_timestamp(r, "ts", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, 86400000000LL == usec);
    pgsql_reader_free(r);

    // timestamp 二进制长度不符（4 字节）→ ERR_FAILED
    r = _pg_reader_new(1, tsoid, tsname);
    r->format = FORMAT_BINARY;
    MALLOC(p, 4);
    pack_integer(p, 1, 4, 0);
    cols[0] = (pgpack_row){ 4, p };
    _pg_reader_push_row(r, &p, cols);
    (void)pgsql_reader_timestamp(r, "ts", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    pgsql_reader_free(r);

    // date 二进制 4 字节大端 int32（PG 纪元天数，1 → 2000-01-02）→ OK
    r = _pg_reader_new(1, doid, dname);
    r->format = FORMAT_BINARY;
    MALLOC(p, 4);
    pack_integer(p, 1, 4, 0);
    cols[0] = (pgpack_row){ 4, p };
    _pg_reader_push_row(r, &p, cols);
    days = pgsql_reader_date(r, "d", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertIntEquals(tc, 1, days);
    pgsql_reader_free(r);

    // date 二进制长度不符（8 字节）→ ERR_FAILED
    r = _pg_reader_new(1, doid, dname);
    r->format = FORMAT_BINARY;
    MALLOC(p, 8);
    pack_integer(p, 1, 8, 0);
    cols[0] = (pgpack_row){ 8, p };
    _pg_reader_push_row(r, &p, cols);
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
    pgpack_row cols[1] = { { (int32_t)strlen(s), p } };
    _pg_reader_push_row(r, &p, cols);
    char uuid[16];
    int32_t err;
    CuAssertIntEquals(tc, ERR_OK, pgsql_reader_uuid(r, "u", uuid, &err));
    CuAssertIntEquals(tc, ERR_OK, err);
    char expect[16];
    for (int i = 0; i < 16; i++) {
        expect[i] = (char)(i + 1);
    }
    CuAssertTrue(tc, 0 == memcmp(uuid, expect, 16));
    pgsql_reader_free(r);

    // 二进制：16 字节原样
    pgsql_reader_ctx *r2 = _pg_reader_new(1, oids, names);
    r2->format = FORMAT_BINARY;
    char *p2;
    MALLOC(p2, 16);
    memcpy(p2, expect, 16);
    pgpack_row cols2[1] = { { 16, p2 } };
    _pg_reader_push_row(r2, &p2, cols2);
    char uuid2[16];
    CuAssertIntEquals(tc, ERR_OK, pgsql_reader_uuid(r2, "u", uuid2, &err));
    CuAssertTrue(tc, 0 == memcmp(uuid2, expect, 16));
    pgsql_reader_free(r2);

    // 文本长度不对 → ERR_FAILED
    pgsql_reader_ctx *r3 = _pg_reader_one(oids, names, FORMAT_TEXT, "badbad", 6);
    CuAssertIntEquals(tc, ERR_FAILED, pgsql_reader_uuid(r3, "u", uuid2, &err));
    CuAssertIntEquals(tc, ERR_FAILED, err);
    pgsql_reader_free(r3);

    // 长度恰为 36、横线也是 4 条且都在字节边界上,只是位置不对(16-4-4-4-4) → ERR_FAILED。
    // 逐对读、见横线就跳的宽松解析会把它收下,必须按 8-4-4-4-12 卡位置
    const char *bad = "0102030405060708-090a-0b0c-0d0e-0f10";
    pgsql_reader_ctx *r4 = _pg_reader_one(oids, names, FORMAT_TEXT, bad, (int32_t)strlen(bad));
    CuAssertIntEquals(tc, ERR_FAILED, pgsql_reader_uuid(r4, "u", uuid2, &err));
    CuAssertIntEquals(tc, ERR_FAILED, err);
    pgsql_reader_free(r4);
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
        { 2, p },
        { 3, p + 2 }
    };
    _pg_reader_push_row(r, &p, cols);

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

// 同一列按名与按下标各取一遍，值与 err 都得一样；按列类型挑取值接口。返回按名那次的 err
static int32_t _pg_col_same(CuTest *tc, pgsql_reader_ctx *r, const char *name, int16_t col) {
    pgpack_field *field = NULL;
    int32_t e1 = ERR_FAILED, e2 = ERR_FAILED, l1 = 0, l2 = 0;
    char u1[16], u2[16];
    CuAssertPtrEquals(tc, pgsql_reader_name(r, name, &field), pgsql_reader_index(r, col, NULL));
    CuAssertPtrNotNull(tc, field);
    CuAssertIntEquals(tc, pgsql_reader_isnull(r, name), pgsql_reader_isnull_at(r, col));
    switch (field->type_oid) {
    case BOOLOID:
        CuAssertIntEquals(tc, pgsql_reader_bool(r, name, &e1), pgsql_reader_bool_at(r, col, &e2));
        break;
    case INT4OID:
        CuAssertTrue(tc, pgsql_reader_integer(r, name, &e1) == pgsql_reader_integer_at(r, col, &e2));
        break;
    case FLOAT8OID:
        CuAssertTrue(tc, pgsql_reader_double(r, name, &e1) == pgsql_reader_double_at(r, col, &e2));
        break;
    case TEXTOID:
        CuAssertPtrEquals(tc, (void *)pgsql_reader_text(r, name, &l1, &e1), (void *)pgsql_reader_text_at(r, col, &l2, &e2));
        CuAssertIntEquals(tc, l1, l2);
        break;
    case BYTEAOID:
        CuAssertPtrEquals(tc, (void *)pgsql_reader_bytea(r, name, &l1, &e1), (void *)pgsql_reader_bytea_at(r, col, &l2, &e2));
        CuAssertIntEquals(tc, l1, l2);
        break;
    case TIMESTAMPOID:
        CuAssertTrue(tc, pgsql_reader_timestamp(r, name, &e1) == pgsql_reader_timestamp_at(r, col, &e2));
        break;
    case DATEOID:
        CuAssertIntEquals(tc, pgsql_reader_date(r, name, &e1), pgsql_reader_date_at(r, col, &e2));
        break;
    case UUIDOID:
        CuAssertIntEquals(tc, pgsql_reader_uuid(r, name, u1, &e1), pgsql_reader_uuid_at(r, col, u2, &e2));
        if (ERR_OK == e1) {
            CuAssertTrue(tc, 0 == memcmp(u1, u2, sizeof(u1)));
        }
        break;
    default:
        break;
    }
    CuAssertIntEquals(tc, e1, e2);
    return e1;
}
// pgsql_reader_col + *_at：逐列与按名接口结果一致；列名按 nlens 整段比；重名列按名取第一个、
// 第二个只能按下标取；不存在的列名为 -1，-1 与越界下标交给 _at 都按列不存在处理；没有字段描述时一律查不到
static void test_pgsql_reader_col_at(CuTest *tc) {
    int32_t oids[10] = { BOOLOID, INT4OID, FLOAT8OID, TEXTOID, BYTEAOID, TIMESTAMPOID, DATEOID, UUIDOID,
                         INT4OID, INT4OID };
    char names[10][64] = { "b", "i", "f", "s", "y", "ts", "d", "u", "n", "i" };
    static const char *vals[10] = { "t", "42", "1.5", "abc", "\\x00ff", "2000-01-02 03:04:05", "2000-01-02",
                                    "123e4567-e89b-12d3-a456-426614174000", NULL, "99" };
    pgpack_row cols[10];
    char *p;
    size_t off = 0;
    int32_t i, err, lens;
    int16_t col;
    pgsql_reader_ctx *r = _pg_reader_new(10, oids, names);
    r->format = FORMAT_TEXT;
    MALLOC(p, 256);
    for (i = 0; i < 10; i++) {
        cols[i].lens = -1;
        cols[i].val = NULL;
        if (NULL != vals[i]) {
            cols[i].lens = (int32_t)strlen(vals[i]);
            memcpy(p + off, vals[i], (size_t)cols[i].lens);
            cols[i].val = p + off;
            off += (size_t)cols[i].lens;
        }
    }
    _pg_reader_push_row(r, &p, cols);
    // 前 9 个列名互不相同：查到的就是自己的下标，取值除 NULL 列(err=1)外都成功
    for (i = 0; i < 9; i++) {
        col = pgsql_reader_col(r, names[i], strlen(names[i]));
        CuAssertIntEquals(tc, i, col);
        CuAssertIntEquals(tc, (8 == i) ? 1 : ERR_OK, _pg_col_same(tc, r, names[i], col));
    }
    // 类型不符
    pgsql_reader_integer_at(r, 3, &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    // 重名列
    CuAssertIntEquals(tc, 1, pgsql_reader_col(r, "i", 1));
    CuAssertTrue(tc, 42 == pgsql_reader_integer(r, "i", &err));
    CuAssertTrue(tc, 99 == pgsql_reader_integer_at(r, 9, &err));
    CuAssertIntEquals(tc, ERR_OK, err);
    // 按 nlens 整段比："dx" 的前 1 字节是 "d"，"tsx" 的前 2 字节是 "ts"，"ts" 的前 1 字节 "t" 不存在
    CuAssertIntEquals(tc, 6, pgsql_reader_col(r, "dx", 1));
    CuAssertIntEquals(tc, 5, pgsql_reader_col(r, "tsx", 2));
    CuAssertIntEquals(tc, -1, pgsql_reader_col(r, "ts", 1));
    // 不存在的列与越界下标
    CuAssertIntEquals(tc, -1, pgsql_reader_col(r, "nosuch", 6));
    for (i = 0; i < 2; i++) {
        col = (0 == i) ? -1 : 10;
        CuAssertTrue(tc, NULL == pgsql_reader_index(r, col, NULL));
        CuAssertIntEquals(tc, 0, pgsql_reader_isnull_at(r, col));
        CuAssertTrue(tc, 0 == pgsql_reader_integer_at(r, col, &err));
        CuAssertIntEquals(tc, ERR_FAILED, err);
        CuAssertTrue(tc, NULL == pgsql_reader_text_at(r, col, &lens, &err));
        CuAssertIntEquals(tc, ERR_FAILED, err);
        CuAssertIntEquals(tc, 0, lens);
    }
    pgsql_reader_integer(r, "nosuch", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    pgsql_reader_free(r);
    // 没有字段描述(没收到 RowDescription)
    r = _pg_reader_new(0, oids, names);
    CuAssertIntEquals(tc, -1, pgsql_reader_col(r, "b", 1));
    pgsql_reader_bool_at(r, 0, &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    CuAssertIntEquals(tc, 0, pgsql_reader_isnull_at(r, 0));
    pgsql_reader_free(r);
}

// _pgpack_error_notice：将 'S'/'M'/'C' 等字段拼接为多行字符串
static void test_pgpack_error_notice(CuTest *tc) {
    // 构造一个 ErrorResponse 风格的字节流：S:ERROR\0M:bad command\0C:42601\0\0
    binary_ctx bw;
    binary_init_write(&bw, 0, 0);
    binary_set_int8(&bw, 'S');
    binary_set_string(&bw, "ERROR");
    binary_set_int8(&bw, 'M');
    binary_set_string(&bw, "bad command");
    binary_set_int8(&bw, 'C');
    binary_set_string(&bw, "42601");
    binary_set_int8(&bw, 0);

    binary_ctx br;
    binary_init_read(&br, bw.data, bw.offset);
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
    binary_init_write(&bw, 0, 0);
    binary_set_int8(&bw, 0); // 立即结束

    binary_ctx br;
    binary_init_read(&br, bw.data, bw.offset);
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
    binary_init_read(&br, raw, sizeof(raw));
    char *msg = _pgpack_error_notice(&br);
    CuAssertPtrNotNull(tc, msg);
    CuAssertIntEquals(tc, 0, (int)msg[0]); // 这一字段整个丢掉，结果是空串
    FREE(msg);
}

// 拼一条服务端消息喂给 _pgpack_parser：code + 4 字节大端长度(含自身) + body。
// 同 _pgsql_command_response：只有 'D' / 'A' 由解析器接管 raw，其余类型这里释放
static void *_pg_feed(pgsql_ctx *pg, ud_cxt *ud, char code,
    const char *body, size_t blens, int32_t *status) {
    char *raw;
    void *pack;
    binary_ctx br;
    // 同 _pgsql_payload：DataRow 在有字段描述的 reader 时从它的块链分(块尾带行数组)，否则单独分配
    raw = ('D' == code) ? _pgpack_row_alloc(pg, 5 + blens) : NULL;
    if (NULL == raw) {
        MALLOC(raw, 5 + blens);
    }
    raw[0] = code;
    pack_integer(raw + 1, (uint64_t)(4 + blens), 4, 0);
    if (blens > 0) {
        memcpy(raw + 5, body, blens);
    }
    binary_init_read(&br, raw, 5 + blens);
    *status = PROT_INIT;
    pack = _pgpack_parser(pg, &br, ud, status);
    if ('D' != code && 'A' != code) {
        FREE(raw);
    }
    return pack;
}
/* 一条完整的 T → D → C → Z 消息流走通 _pgpack_parser。原来全仓只有两处调它、都断言 NULL
 * （拒收），而 24 个 reader 取值用例全靠手搭 struct 绕过解析器，多结果集那条更是靠测试
 * 自己复刻的 _pgpack_complete —— 于是"wire 字节 → reader"整条链零测试。
 * 这一条同时钉住四个可破坏点：
 *   1) 把 `pg->pack->pack = NULL`（所有权移入结果数组）删掉 → _pgpack_free 会先释放 reader、
 *      再由 _pgpack_results_clear 对同一指针 pgsql_reader_free → double free
 *   2) 把 pgres_arr_push_back(&results) 删掉 → result_count 变 0，多结果集整段丢失
 *   3) field->type_oid(4B) 与 field->lens(2B) 读取顺序对调 → 类型 OID 白名单判错、取值失败
 *   4) `-1 == row->lens` 与 `0 == row->lens` 两支对调 → NULL 与空串互换 */
static void test_pgpack_parser_full_flow(CuTest *tc) {
    pgsql_ctx pg;
    ud_cxt ud;
    int32_t status;
    int32_t err;
    void *pack;
    char body[64];
    char *p;
    ZERO(&pg, sizeof(pg));
    ZERO(&ud, sizeof(ud));

    /* T：1 列，列名 "n"，INT4OID，format=FORMAT_TEXT */
    p = body;
    pack_integer(p, 1, 2, 0); p += 2;// field_count
    *p++ = 'n'; *p++ = '\0';// 列名 cstring
    pack_integer(p, 0, 4, 0); p += 4;// table_oid
    pack_integer(p, 1, 2, 0); p += 2;// index
    pack_integer(p, INT4OID, 4, 0); p += 4;// type_oid
    pack_integer(p, 4, 2, 0); p += 2;// lens
    pack_integer(p, (uint64_t)-1, 4, 0); p += 4;// type_modifier
    pack_integer(p, FORMAT_TEXT, 2, 0); p += 2;// format
    CuAssertTrue(tc, NULL == _pg_feed(&pg, &ud, 'T', body, (size_t)(p - body), &status));
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));

    /* D：1 列，文本 "12345" */
    p = body;
    pack_integer(p, 1, 2, 0); p += 2;// ncolumn
    pack_integer(p, 5, 4, 0); p += 4;// 列长度
    memcpy(p, "12345", 5); p += 5;
    CuAssertTrue(tc, NULL == _pg_feed(&pg, &ud, 'D', body, (size_t)(p - body), &status));
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));

    /* C：CommandComplete 触发 _pgpack_complete，把 reader 提交进结果数组 */
    CuAssertTrue(tc, NULL == _pg_feed(&pg, &ud, 'C', "SELECT 1", 9, &status));
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));

    /* Z：ReadyForQuery 才把攒好的 pgpack 交出来 */
    pack = _pg_feed(&pg, &ud, 'Z', "I", 1, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));

    CuAssertIntEquals(tc, 1, (int)pgsql_result_count((pgpack_ctx *)pack));
    pgsql_reader_ctx *reader = pgsql_reader_iter((pgpack_ctx *)pack, FORMAT_TEXT);
    CuAssertPtrNotNull(tc, reader);
    CuAssertTrue(tc, 12345 == pgsql_reader_integer(reader, "n", &err));
    CuAssertIntEquals(tc, ERR_OK, err);
    pgsql_reader_free(reader);
    _pgpack_free((pgpack_ctx *)pack);
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
    void *pack;
    int32_t status;
    for (size_t i = 0; i < sizeof(codes); i++) {
        ZERO(&pg, sizeof(pg));
        ZERO(&ud, sizeof(ud));
        MALLOC(raw, 5);
        raw[0] = codes[i];
        pack_integer(raw + 1, 4, 4, 0); // 长度字段含自身，4 即消息体为空
        binary_init_read(&br, raw, 5);
        status = PROT_INIT;
        pack = _pgpack_parser(&pg, &br, &ud, &status);
        if ('D' != codes[i] && 'A' != codes[i]) {// 同 _pgsql_command_response：其余类型由调用方释放
            FREE(raw);
        }
        CuAssertTrue(tc, NULL == pack);
        CuAssert(tc, "an empty message body must be a protocol error, not an abort",
            BIT_CHECK(status, PROT_ERROR));
        _pgpack_free(pg.pack);
    }
}

// 往 buf 追加一条完整 pgsql 消息：类型码 1 字节 + 4 字节大端长度(含自身) + 正文
static void _pg_push_msg(buffer_ctx *buf, char code, const char *body, size_t blens) {
    char head[5];
    head[0] = code;
    pack_integer(head + 1, (uint64_t)(4 + blens), 4, 0);
    buffer_append(buf, head, sizeof(head));
    if (blens > 0) {
        buffer_append(buf, (void *)body, blens);
    }
}
// _pgsql_payload 的分帧：库内 static，走 pgsql_unpack 的 COMMAND 分支间接测。
// 上面那批用例都是把单条消息直接交给 _pgpack_parser，跳过了从缓冲区切消息这一层，
// 于是"消费多少字节"没有任何测试 —— 少切一字节就整条流错位，且错在下一条消息上
static void test_pgsql_payload_framing(CuTest *tc) {
    pgsql_ctx pg;
    ud_cxt ud;
    buffer_ctx buf;
    int32_t status;
    ZERO(&pg, sizeof(pg));
    ZERO(&ud, sizeof(ud));
    ud.status = 2;// COMMAND，枚举定义在 pgsql.c 内不可见
    ud.context = &pg;

    // 1) 空缓冲 → MOREDATA
    buffer_init(&buf);
    status = PROT_INIT;
    CuAssertTrue(tc, NULL == _t_pgsql_unpack(0, &buf, &ud, NULL, &status));
    CuAssertTrue(tc, BIT_CHECK(status, PROT_MOREDATA) && !BIT_CHECK(status, PROT_ERROR));
    buffer_free(&buf);

    // 2) 只有 4 字节，消息头要 5 → MOREDATA 且一个字节都不消费
    char part[4] = { 'C', 0x00, 0x00, 0x00 };
    buffer_init(&buf);
    buffer_append(&buf, part, sizeof(part));
    status = PROT_INIT;
    CuAssertTrue(tc, NULL == _t_pgsql_unpack(0, &buf, &ud, NULL, &status));
    CuAssertTrue(tc, BIT_CHECK(status, PROT_MOREDATA) && !BIT_CHECK(status, PROT_ERROR));
    CuAssertIntEquals(tc, 4, (int)buffer_size(&buf));
    buffer_free(&buf);

    // 3) 长度字段声明 3（协议规定含自身、合法值 >= 4）→ PROT_ERROR
    char bad[5] = { 'C', 0x00, 0x00, 0x00, 0x03 };
    buffer_init(&buf);
    buffer_append(&buf, bad, sizeof(bad));
    status = PROT_INIT;
    CuAssertTrue(tc, NULL == _t_pgsql_unpack(0, &buf, &ud, NULL, &status));
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
    buffer_free(&buf);

    // 4) 头齐但正文短一截 → MOREDATA 且缓冲原封不动（等下一次收齐再切）
    buffer_init(&buf);
    _pg_push_msg(&buf, 'C', "SELECT 1", 9);
    char *whole;
    size_t wlens = buffer_size(&buf);
    MALLOC(whole, wlens);
    CuAssertIntEquals(tc, (int)wlens, (int)buffer_copyout(&buf, 0, whole, wlens));
    buffer_free(&buf);
    buffer_init(&buf);
    buffer_append(&buf, whole, wlens - 1);
    status = PROT_INIT;
    CuAssertTrue(tc, NULL == _t_pgsql_unpack(0, &buf, &ud, NULL, &status));
    CuAssertTrue(tc, BIT_CHECK(status, PROT_MOREDATA) && !BIT_CHECK(status, PROT_ERROR));
    CuAssertIntEquals(tc, (int)(wlens - 1), (int)buffer_size(&buf));
    buffer_free(&buf);

    // 5) 两条消息挤在一个缓冲里：一次调用只该吃掉第一条，剩的字节数必须正好是第二条
    buffer_init(&buf);
    _pg_push_msg(&buf, 'C', "SELECT 1", 9);
    _pg_push_msg(&buf, 'C', "SELECT 22", 10);
    status = PROT_INIT;
    CuAssertTrue(tc, NULL == _t_pgsql_unpack(0, &buf, &ud, NULL, &status));
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertIntEquals(tc, 15, (int)buffer_size(&buf));// 5 + 10
    status = PROT_INIT;
    CuAssertTrue(tc, NULL == _t_pgsql_unpack(0, &buf, &ud, NULL, &status));
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertIntEquals(tc, 0, (int)buffer_size(&buf));
    buffer_free(&buf);
    FREE(whole);
    _pgpack_free(pg.pack);
}

// COPY OUT 走切包入口：钉住三件事
//   1) H → d(含零长) → c → C → Z 整条流的累积内容与消息边界(每次调用正好吃一条)
//   2) 半条 CopyData 等下一次收齐，缓冲原封不动
//   3) 'H' 之前的 CopyData 按非法序列报错
static void test_pgsql_copy_out_framing(CuTest *tc) {
    pgsql_ctx pg;
    ud_cxt ud;
    buffer_ctx buf;
    int32_t status;
    pgpack_ctx *pack = NULL;
    pgpack_copy_out_ctx *co;
    char hbody[5] = { 0, 0, 1, 0, 0 };// format 0, 1 列, 列格式 0
    ZERO(&pg, sizeof(pg));
    ZERO(&ud, sizeof(ud));
    ud.status = 2;// COMMAND
    ud.context = &pg;
    buffer_init(&buf);
    _pg_push_msg(&buf, 'H', hbody, sizeof(hbody));
    _pg_push_msg(&buf, 'd', "a\t1\n", 4);
    _pg_push_msg(&buf, 'd', NULL, 0);
    _pg_push_msg(&buf, 'd', "b\t2\n", 4);
    _pg_push_msg(&buf, 'c', NULL, 0);
    _pg_push_msg(&buf, 'C', "COPY 2", 7);
    _pg_push_msg(&buf, 'Z', "I", 1);
    int32_t calls = 0;
    while (NULL == pack && buffer_size(&buf) > 0) {
        status = PROT_INIT;
        pack = _t_pgsql_unpack(0, &buf, &ud, NULL, &status);
        CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
        calls++;
    }
    CuAssertIntEquals(tc, 7, calls);
    CuAssertPtrNotNull(tc, pack);
    CuAssertIntEquals(tc, 0, (int)buffer_size(&buf));
    CuAssertTrue(tc, PGPACK_COPY_OUT == pack->type);
    co = pack->pack;
    CuAssertIntEquals(tc, 8, (int)co->data.offset);
    CuAssertTrue(tc, 0 == memcmp(co->data.data, "a\t1\nb\t2\n", 8));
    _pgpack_free(pack);
    buffer_free(&buf);

    // 2) 半条 CopyData：MOREDATA 且一字节不动，补齐后照常收进去
    ZERO(&pg, sizeof(pg));
    buffer_init(&buf);
    _pg_push_msg(&buf, 'H', hbody, sizeof(hbody));
    status = PROT_INIT;
    CuAssertTrue(tc, NULL == _t_pgsql_unpack(0, &buf, &ud, NULL, &status));
    char part[9] = { 'd', 0, 0, 0, 12, 'x', 'y', 'z', 'w' };// 声明正文 8 字节，只到了 4 个
    buffer_append(&buf, part, sizeof(part));
    status = PROT_INIT;
    CuAssertTrue(tc, NULL == _t_pgsql_unpack(0, &buf, &ud, NULL, &status));
    CuAssertTrue(tc, BIT_CHECK(status, PROT_MOREDATA) && !BIT_CHECK(status, PROT_ERROR));
    CuAssertIntEquals(tc, 9, (int)buffer_size(&buf));
    buffer_append(&buf, "1234", 4);
    status = PROT_INIT;
    CuAssertTrue(tc, NULL == _t_pgsql_unpack(0, &buf, &ud, NULL, &status));
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertIntEquals(tc, 0, (int)buffer_size(&buf));
    co = pg.pack->pack;
    CuAssertIntEquals(tc, 8, (int)co->data.offset);
    CuAssertTrue(tc, 0 == memcmp(co->data.data, "xyzw1234", 8));
    _pgpack_free(pg.pack);
    buffer_free(&buf);

    // 3) 没有 'H' 就来 CopyData：非法序列
    ZERO(&pg, sizeof(pg));
    buffer_init(&buf);
    _pg_push_msg(&buf, 'd', "a\t1\n", 4);
    status = PROT_INIT;
    CuAssertTrue(tc, NULL == _t_pgsql_unpack(0, &buf, &ud, NULL, &status));
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
    buffer_free(&buf);
    _pgpack_free(pg.pack);
}

// RowDescription 正文：n 列 INT4 文本列，列名依次 "c0" "c1" ...
static size_t _pg_rowdesc(char *body, uint16_t n) {
    char *p = body;
    uint16_t i;
    pack_integer(p, n, 2, 0); p += 2;
    for (i = 0; i < n; i++) {
        *p++ = 'c'; *p++ = (char)('0' + i); *p++ = '\0';
        pack_integer(p, 0, 4, 0); p += 4;// table_oid
        pack_integer(p, i + 1, 2, 0); p += 2;// index
        pack_integer(p, INT4OID, 4, 0); p += 4;
        pack_integer(p, 4, 2, 0); p += 2;// lens
        pack_integer(p, (uint64_t)-1, 4, 0); p += 4;// type_modifier
        pack_integer(p, FORMAT_TEXT, 2, 0); p += 2;
    }
    return (size_t)(p - body);
}
// DataRow 正文：n 列，第 j 列的文本值是 row * 10 + j
static size_t _pg_datarow(char *body, uint16_t n, int32_t row) {
    char *p = body;
    char num[16];
    size_t lens;
    uint16_t j;
    pack_integer(p, n, 2, 0); p += 2;
    for (j = 0; j < n; j++) {
        lens = (size_t)SNPRINTF(num, sizeof(num), "%d", row * 10 + j);
        pack_integer(p, lens, 4, 0); p += 4;
        memcpy(p, num, lens); p += lens;
    }
    return (size_t)(p - body);
}
// 经 pgsql_unpack 分帧走 T → D×3 → C → Z：DataRow 的块尾行数组(_pgsql_payload 多分的那段)要装得下、
// 行值要读得对；列数与 T 对不上、没有 T 就来 D 都得判协议错。行数组越界写时 ASan 构建会直接报
static void test_pgsql_unpack_rows(CuTest *tc) {
    pgsql_ctx pg;
    ud_cxt ud;
    buffer_ctx buf;
    pgsql_reader_ctx *reader;
    void *pack = NULL;
    char tbody[128], dbody[64];
    size_t tl, dl;
    int32_t status, err, i;
    ZERO(&pg, sizeof(pg));
    ZERO(&ud, sizeof(ud));
    ud.status = 2;// COMMAND
    ud.context = &pg;
    tl = _pg_rowdesc(tbody, 2);

    buffer_init(&buf);
    _pg_push_msg(&buf, 'T', tbody, tl);
    for (i = 0; i < 3; i++) {
        dl = _pg_datarow(dbody, 2, i);
        _pg_push_msg(&buf, 'D', dbody, dl);
    }
    _pg_push_msg(&buf, 'C', "SELECT 3", 9);
    _pg_push_msg(&buf, 'Z', "I", 1);
    for (i = 0; i < 6 && NULL == pack; i++) {
        status = PROT_INIT;
        pack = _t_pgsql_unpack(0, &buf, &ud, NULL, &status);
        CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    }
    CuAssertPtrNotNull(tc, pack);
    CuAssertIntEquals(tc, 0, (int)buffer_size(&buf));
    buffer_free(&buf);
    reader = pgsql_reader_iter((pgpack_ctx *)pack, FORMAT_TEXT);
    CuAssertPtrNotNull(tc, reader);
    CuAssertIntEquals(tc, 3, (int)pgsql_reader_size(reader));
    for (i = 0; i < 3; i++) {
        CuAssertTrue(tc, i * 10 == pgsql_reader_integer(reader, "c0", &err));
        CuAssertIntEquals(tc, ERR_OK, err);
        CuAssertTrue(tc, i * 10 + 1 == pgsql_reader_integer(reader, "c1", &err));
        CuAssertIntEquals(tc, ERR_OK, err);
        pgsql_reader_next(reader);
    }
    CuAssertTrue(tc, 0 != pgsql_reader_eof(reader));
    pgsql_reader_free(reader);
    _pgpack_free((pgpack_ctx *)pack);

    // T 声明 2 列、D 带 3 列
    buffer_init(&buf);
    _pg_push_msg(&buf, 'T', tbody, tl);
    dl = _pg_datarow(dbody, 3, 0);
    _pg_push_msg(&buf, 'D', dbody, dl);
    status = PROT_INIT;
    CuAssertTrue(tc, NULL == _t_pgsql_unpack(0, &buf, &ud, NULL, &status));
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    status = PROT_INIT;
    CuAssertTrue(tc, NULL == _t_pgsql_unpack(0, &buf, &ud, NULL, &status));
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
    buffer_free(&buf);
    _pgpack_free(pg.pack);
    pg.pack = NULL;

    // 没有 T 就来 D
    buffer_init(&buf);
    dl = _pg_datarow(dbody, 2, 0);
    _pg_push_msg(&buf, 'D', dbody, dl);
    status = PROT_INIT;
    CuAssertTrue(tc, NULL == _t_pgsql_unpack(0, &buf, &ud, NULL, &status));
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
    buffer_free(&buf);
    _pgpack_free(pg.pack);
}
// 回归：字段描述数组的归属记在 fields_inl 上。常态 T 与 reader 同一次分配(fields_inl=1)；
// 0 列 T 之后不经 C 又来 2 列 T(违规流)时第二个 T 的数组单独分配(fields_inl=0)。以前拿 fields == reader + 1 判，
// 分配器恰好把单独分配的那块紧挨着 reader 放时就当成内嵌、漏释放，收尾的内存检查会报 not free
static void test_pgsql_fields_separate_alloc(CuTest *tc) {
    pgsql_ctx pg;
    ud_cxt ud;
    buffer_ctx buf;
    pgsql_reader_ctx *reader;
    void *pack = NULL;
    char tbody[128], dbody[64];
    size_t tl, dl;
    int32_t status, err, i;
    ZERO(&pg, sizeof(pg));
    ZERO(&ud, sizeof(ud));
    ud.status = 2;// COMMAND
    ud.context = &pg;

    // 常态：reader 由 T 新建，字段描述随它一块
    buffer_init(&buf);
    tl = _pg_rowdesc(tbody, 2);
    _pg_push_msg(&buf, 'T', tbody, tl);
    status = PROT_INIT;
    CuAssertTrue(tc, NULL == _t_pgsql_unpack(0, &buf, &ud, NULL, &status));
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    reader = pg.pack->pack;
    CuAssertIntEquals(tc, 1, (int)reader->fields_inl);
    _pgpack_free(pg.pack);
    pg.pack = NULL;

    // 违规流：0 列 T 建出没有字段描述的 reader，再来的 2 列 T 只能单独分配
    tl = _pg_rowdesc(tbody, 0);
    _pg_push_msg(&buf, 'T', tbody, tl);
    status = PROT_INIT;
    CuAssertTrue(tc, NULL == _t_pgsql_unpack(0, &buf, &ud, NULL, &status));
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    tl = _pg_rowdesc(tbody, 2);
    _pg_push_msg(&buf, 'T', tbody, tl);
    status = PROT_INIT;
    CuAssertTrue(tc, NULL == _t_pgsql_unpack(0, &buf, &ud, NULL, &status));
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    reader = pg.pack->pack;
    CuAssertPtrNotNull(tc, reader->fields);
    CuAssertIntEquals(tc, 0, (int)reader->fields_inl);
    // 之后照常落行、取值，释放走单独分配那条路
    dl = _pg_datarow(dbody, 2, 7);
    _pg_push_msg(&buf, 'D', dbody, dl);
    _pg_push_msg(&buf, 'C', "SELECT 1", 9);
    _pg_push_msg(&buf, 'Z', "I", 1);
    for (i = 0; i < 4 && NULL == pack; i++) {
        status = PROT_INIT;
        pack = _t_pgsql_unpack(0, &buf, &ud, NULL, &status);
        CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    }
    buffer_free(&buf);
    CuAssertPtrNotNull(tc, pack);
    reader = pgsql_reader_iter((pgpack_ctx *)pack, FORMAT_TEXT);
    CuAssertPtrNotNull(tc, reader);
    CuAssertTrue(tc, 71 == pgsql_reader_integer(reader, "c1", &err));
    CuAssertIntEquals(tc, ERR_OK, err);
    pgsql_reader_free(reader);
    _pgpack_free((pgpack_ctx *)pack);
}
// 行指针数组前 8 槽内嵌在 reader 里：跨过 8(搬到堆上)与 32(之后照常倍增)两个边界，逐行取值都得对
static void test_pgsql_unpack_rows_spill(CuTest *tc) {
    pgsql_ctx pg;
    ud_cxt ud;
    buffer_ctx buf;
    pgsql_reader_ctx *reader;
    void *pack = NULL;
    char tbody[128], dbody[64];
    size_t tl, dl;
    int32_t status, err, i;
    const int32_t nrows = 40;
    ZERO(&pg, sizeof(pg));
    ZERO(&ud, sizeof(ud));
    ud.status = 2;// COMMAND
    ud.context = &pg;
    buffer_init(&buf);
    tl = _pg_rowdesc(tbody, 2);
    _pg_push_msg(&buf, 'T', tbody, tl);
    for (i = 0; i < nrows; i++) {
        dl = _pg_datarow(dbody, 2, i);
        _pg_push_msg(&buf, 'D', dbody, dl);
    }
    _pg_push_msg(&buf, 'C', "SELECT 40", 10);
    _pg_push_msg(&buf, 'Z', "I", 1);
    for (i = 0; i < nrows + 4 && NULL == pack; i++) {
        status = PROT_INIT;
        pack = _t_pgsql_unpack(0, &buf, &ud, NULL, &status);
        CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    }
    buffer_free(&buf);
    CuAssertPtrNotNull(tc, pack);
    reader = pgsql_reader_iter((pgpack_ctx *)pack, FORMAT_TEXT);
    CuAssertPtrNotNull(tc, reader);
    CuAssertIntEquals(tc, nrows, (int)pgsql_reader_size(reader));
    for (i = 0; i < nrows; i++) {
        CuAssertTrue(tc, i * 10 == pgsql_reader_integer(reader, "c0", &err));
        CuAssertIntEquals(tc, ERR_OK, err);
        CuAssertTrue(tc, i * 10 + 1 == pgsql_reader_integer(reader, "c1", &err));
        pgsql_reader_next(reader);
    }
    // seek 回到内嵌槽那一段也得对
    pgsql_reader_seek(reader, 3);
    CuAssertTrue(tc, 30 == pgsql_reader_integer(reader, "c0", &err));
    pgsql_reader_free(reader);
    _pgpack_free((pgpack_ctx *)pack);
}
// 追加 T → D×5 → C → Z 里的第 m 条(0 是 T，1~5 是 D，6 是 C，7 是 Z)，返回它的整包字节数
static size_t _pg_batch_msg(buffer_ctx *buf, int32_t m) {
    char body[128];
    size_t blens;
    if (0 == m) {
        blens = _pg_rowdesc(body, 2);
        _pg_push_msg(buf, 'T', body, blens);
    } else if (m <= 5) {
        blens = _pg_datarow(body, 2, m - 1);
        _pg_push_msg(buf, 'D', body, blens);
    } else if (6 == m) {
        blens = 9;
        _pg_push_msg(buf, 'C', "SELECT 5", blens);
    } else {
        blens = 1;
        _pg_push_msg(buf, 'Z', "I", blens);
    }
    return 5 + blens;
}
// 逐行核 _pg_batch_msg 那组 DataRow 的值(第 i 行是 i*10、i*10+1)，然后释放 pack
static void _pg_batch_check(CuTest *tc, pgpack_ctx *pack, int32_t nrows) {
    pgsql_reader_ctx *reader = pgsql_reader_iter(pack, FORMAT_TEXT);
    int32_t i, err;
    CuAssertPtrNotNull(tc, reader);
    CuAssertIntEquals(tc, nrows, (int)pgsql_reader_size(reader));
    for (i = 0; i < nrows; i++) {
        CuAssertTrue(tc, i * 10 == pgsql_reader_integer(reader, "c0", &err));
        CuAssertIntEquals(tc, ERR_OK, err);
        CuAssertTrue(tc, i * 10 + 1 == pgsql_reader_integer(reader, "c1", &err));
        CuAssertIntEquals(tc, ERR_OK, err);
        pgsql_reader_next(reader);
    }
    pgsql_reader_free(reader);
    _pgpack_free(pack);
}
// 连着的 DataRow 在一次 pgsql_unpack 里解完：一次喂入整条流与逐条喂入，行值与 size 记账一致；
// 下一条 DataRow 的头或正文没收全时停在它前面，补齐后接着解
static void test_pgsql_unpack_rows_batch(CuTest *tc) {
    pgsql_ctx pg;
    ud_cxt ud;
    buffer_ctx buf;
    pgpack_ctx *pack;
    pgsql_reader_ctx *reader;
    char tbody[128], dbody[64], last[80];
    size_t sum, size[2], tl, dl, lastlens, cut;
    int32_t status, m, calls, k;
    ZERO(&ud, sizeof(ud));
    ud.status = 2;// COMMAND
    // 1) 一次喂入：T、D×5、C、Z 只要 4 次调用(五条 DataRow 一次解完)
    ZERO(&pg, sizeof(pg));
    ud.context = &pg;
    buffer_init(&buf);
    sum = 0;
    for (m = 0; m < 8; m++) {
        sum += _pg_batch_msg(&buf, m);
    }
    pack = NULL;
    calls = 0;
    while (NULL == pack && buffer_size(&buf) > 0) {
        size[0] = 0;
        status = PROT_INIT;
        pack = _t_pgsql_unpack(0, &buf, &ud, &size[0], &status);
        CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
        calls++;
    }
    CuAssertIntEquals(tc, 4, calls);
    CuAssertPtrNotNull(tc, pack);
    CuAssertIntEquals(tc, 0, (int)buffer_size(&buf));
    CuAssertIntEquals(tc, (int)sum, (int)size[0]);
    CuAssertIntEquals(tc, 0, (int)pg.recvlens);
    buffer_free(&buf);
    _pg_batch_check(tc, pack, 5);
    // 2) 逐条喂入：每追加一条调一次，只有 Z 出包
    ZERO(&pg, sizeof(pg));
    ud.context = &pg;
    buffer_init(&buf);
    pack = NULL;
    for (m = 0; m < 8; m++) {
        _pg_batch_msg(&buf, m);
        size[1] = 0;
        status = PROT_INIT;
        pack = _t_pgsql_unpack(0, &buf, &ud, &size[1], &status);
        CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
        CuAssertIntEquals(tc, 0, (int)buffer_size(&buf));
        CuAssertTrue(tc, (7 == m) == (NULL != pack));
    }
    CuAssertIntEquals(tc, (int)size[0], (int)size[1]);
    buffer_free(&buf);
    _pg_batch_check(tc, pack, 5);
    // 3) 最后一条 DataRow 只到了一部分：k=0 头都不全(3 字节)，k=1 头齐正文短 3 字节
    tl = _pg_rowdesc(tbody, 2);
    dl = _pg_datarow(dbody, 2, 3);
    last[0] = 'D';
    pack_integer(last + 1, (uint64_t)(4 + dl), 4, 0);
    memcpy(last + 5, dbody, dl);
    lastlens = 5 + dl;
    for (k = 0; k < 2; k++) {
        cut = (0 == k) ? 3 : lastlens - 3;
        ZERO(&pg, sizeof(pg));
        ud.context = &pg;
        buffer_init(&buf);
        _pg_push_msg(&buf, 'T', tbody, tl);
        status = PROT_INIT;
        CuAssertPtrEquals(tc, NULL, _t_pgsql_unpack(0, &buf, &ud, NULL, &status));
        for (m = 1; m <= 3; m++) {
            _pg_batch_msg(&buf, m);
        }
        buffer_append(&buf, last, cut);
        // 一次吃掉三条完整的 DataRow，停在残条前；再调就是 MOREDATA、一字节不动
        status = PROT_INIT;
        CuAssertPtrEquals(tc, NULL, _t_pgsql_unpack(0, &buf, &ud, NULL, &status));
        CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
        CuAssertIntEquals(tc, (int)cut, (int)buffer_size(&buf));
        reader = pg.pack->pack;
        CuAssertIntEquals(tc, 3, (int)pgsql_reader_size(reader));
        status = PROT_INIT;
        CuAssertPtrEquals(tc, NULL, _t_pgsql_unpack(0, &buf, &ud, NULL, &status));
        CuAssertTrue(tc, BIT_CHECK(status, PROT_MOREDATA) && !BIT_CHECK(status, PROT_ERROR));
        CuAssertIntEquals(tc, (int)cut, (int)buffer_size(&buf));
        // 补齐后接着解到 Z
        buffer_append(&buf, last + cut, lastlens - cut);
        _pg_push_msg(&buf, 'C', "SELECT 4", 9);
        _pg_push_msg(&buf, 'Z', "I", 1);
        pack = NULL;
        calls = 0;
        while (NULL == pack && buffer_size(&buf) > 0 && calls < 8) {
            status = PROT_INIT;
            pack = _t_pgsql_unpack(0, &buf, &ud, NULL, &status);
            CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
            calls++;
        }
        CuAssertPtrNotNull(tc, pack);
        CuAssertIntEquals(tc, 0, (int)buffer_size(&buf));
        buffer_free(&buf);
        _pg_batch_check(tc, pack, 4);
    }
}
// 结果数组前 2 槽内嵌在 pgpack 里：5 条语句跨过内嵌槽(搬到堆上)后，下标、标签、有无结果集都得对得上
static void test_pgsql_unpack_results_spill(CuTest *tc) {
    pgsql_ctx pg;
    ud_cxt ud;
    buffer_ctx buf;
    pgsql_reader_ctx *reader;
    pgpack_ctx *pk;
    void *pack = NULL;
    char tbody[128], dbody[64];
    size_t tl, dl;
    int32_t status, err, i;
    ZERO(&pg, sizeof(pg));
    ZERO(&ud, sizeof(ud));
    ud.status = 2;// COMMAND
    ud.context = &pg;
    buffer_init(&buf);
    tl = _pg_rowdesc(tbody, 2);
    // 0: SELECT 1 行；1: INSERT；2: SELECT 2 行；3: UPDATE；4: SELECT 1 行
    _pg_push_msg(&buf, 'T', tbody, tl);
    dl = _pg_datarow(dbody, 2, 1);
    _pg_push_msg(&buf, 'D', dbody, dl);
    _pg_push_msg(&buf, 'C', "SELECT 1", 9);
    _pg_push_msg(&buf, 'C', "INSERT 0 3", 11);
    _pg_push_msg(&buf, 'T', tbody, tl);
    for (i = 2; i < 4; i++) {
        dl = _pg_datarow(dbody, 2, i);
        _pg_push_msg(&buf, 'D', dbody, dl);
    }
    _pg_push_msg(&buf, 'C', "SELECT 2", 9);
    _pg_push_msg(&buf, 'C', "UPDATE 5", 9);
    _pg_push_msg(&buf, 'T', tbody, tl);
    dl = _pg_datarow(dbody, 2, 9);
    _pg_push_msg(&buf, 'D', dbody, dl);
    _pg_push_msg(&buf, 'C', "SELECT 1", 9);
    _pg_push_msg(&buf, 'Z', "I", 1);
    for (i = 0; i < 20 && NULL == pack; i++) {
        status = PROT_INIT;
        pack = _t_pgsql_unpack(0, &buf, &ud, NULL, &status);
        CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    }
    buffer_free(&buf);
    CuAssertPtrNotNull(tc, pack);
    pk = pack;
    CuAssertIntEquals(tc, 5, (int)pgsql_result_count(pk));
    CuAssertTrue(tc, 1 == pgsql_affected_at(pk, 0));
    CuAssertTrue(tc, 3 == pgsql_affected_at(pk, 1));
    CuAssertTrue(tc, 2 == pgsql_affected_at(pk, 2));
    CuAssertTrue(tc, 5 == pgsql_affected_at(pk, 3));
    CuAssertTrue(tc, 1 == pgsql_affected_at(pk, 4));
    CuAssertTrue(tc, NULL == pgsql_reader_at(pk, 1, FORMAT_TEXT));
    CuAssertTrue(tc, NULL == pgsql_reader_at(pk, 3, FORMAT_TEXT));
    reader = pgsql_reader_at(pk, 2, FORMAT_TEXT);
    CuAssertPtrNotNull(tc, reader);
    CuAssertIntEquals(tc, 2, (int)pgsql_reader_size(reader));
    CuAssertTrue(tc, 20 == pgsql_reader_integer(reader, "c0", &err));
    pgsql_reader_free(reader);
    reader = pgsql_reader_at(pk, 4, FORMAT_TEXT);
    CuAssertPtrNotNull(tc, reader);
    CuAssertTrue(tc, 91 == pgsql_reader_integer(reader, "c1", &err));
    pgsql_reader_free(reader);
    _pgpack_free(pk);// 下标 0 那个结果集没取走，由 _pgpack_free 收
}
// NotificationResponse 立即独立成包：pid / 频道 / 内容原样取出，_pgsql_pkfree 一次收干净(结构体随 pgpack 分配)
static void test_pgsql_unpack_notification(CuTest *tc) {
    pgsql_ctx pg;
    ud_cxt ud;
    buffer_ctx buf;
    pgpack_ctx *pk;
    pgpack_notification *nt;
    char body[64];
    char *p = body;
    int32_t status;
    ZERO(&pg, sizeof(pg));
    ZERO(&ud, sizeof(ud));
    ud.status = 2;// COMMAND
    ud.context = &pg;
    pack_integer(p, 4242, 4, 0); p += 4;
    memcpy(p, "chan", 5); p += 5;
    memcpy(p, "hello", 6); p += 6;
    buffer_init(&buf);
    _pg_push_msg(&buf, 'A', body, (size_t)(p - body));
    status = PROT_INIT;
    pk = _t_pgsql_unpack(0, &buf, &ud, NULL, &status);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertIntEquals(tc, 0, (int)buffer_size(&buf));
    buffer_free(&buf);
    CuAssertPtrNotNull(tc, pk);
    CuAssertIntEquals(tc, PGPACK_NOTIFICATION, (int)pk->type);
    CuAssertTrue(tc, ERR_OK != _pgsql_may_resume(pk));// 通知不唤醒等待者
    nt = pk->pack;
    CuAssertIntEquals(tc, 4242, nt->pid);
    CuAssertStrEquals(tc, "chan", nt->channel);
    CuAssertStrEquals(tc, "hello", nt->notification);
    CuAssertIntEquals(tc, 0, (int)pgsql_result_count(pk));
    _pgsql_pkfree(pk);
}
// size 记包持有的线上字节(每条消息 = 类型码 1 + 长度 4 + 消息体)：T / D / C / Z 攒到 'Z' 整笔交出；
// 通知 'A'（可夹在 C 与 Z 之间）与 CopyInResponse 'G' 立即交出、只记它自己，不进累计；断连清累计
static void test_pgsql_unpack_size(CuTest *tc) {
    pgsql_ctx pg;
    ud_cxt ud;
    buffer_ctx buf;
    pgpack_ctx *pk;
    char tbody[128], dbody[64], abody[32];
    char gbody[5] = { 0, 0, 1, 0, 0 };// CopyInResponse 正文：format 0, 1 列, 列格式 0
    char *p = abody;
    size_t tl, dl, al, size, sum;
    int32_t status, i;
    ZERO(&pg, sizeof(pg));
    ZERO(&ud, sizeof(ud));
    ud.status = 2;// COMMAND
    ud.context = &pg;
    tl = _pg_rowdesc(tbody, 2);
    dl = _pg_datarow(dbody, 2, 0);
    pack_integer(p, 7, 4, 0); p += 4;
    memcpy(p, "ch", 3); p += 3;
    memcpy(p, "x", 2); p += 2;
    al = (size_t)(p - abody);
    buffer_init(&buf);
    _pg_push_msg(&buf, 'T', tbody, tl);
    _pg_push_msg(&buf, 'D', dbody, dl);
    _pg_push_msg(&buf, 'C', "SELECT 1", 9);
    _pg_push_msg(&buf, 'A', abody, al);
    _pg_push_msg(&buf, 'Z', "I", 1);
    sum = (5 + tl) + (5 + dl) + (5 + 9) + (5 + 1);
    // 1) T / D / C：只攒不交出，size 不写
    for (i = 0; i < 3; i++) {
        size = 0;
        status = PROT_INIT;
        CuAssertPtrEquals(tc, NULL, _t_pgsql_unpack(0, &buf, &ud, &size, &status));
        CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
        CuAssertIntEquals(tc, 0, (int)size);
    }
    CuAssertIntEquals(tc, (int)(sum - 6), (int)pg.recvlens);
    // 2) 通知：size 是它自己的长度，累计不动
    size = 0;
    status = PROT_INIT;
    pk = _t_pgsql_unpack(0, &buf, &ud, &size, &status);
    CuAssertPtrNotNull(tc, pk);
    CuAssertIntEquals(tc, PGPACK_NOTIFICATION, (int)pk->type);
    CuAssertIntEquals(tc, (int)(5 + al), (int)size);
    CuAssertIntEquals(tc, (int)(sum - 6), (int)pg.recvlens);
    _pgsql_pkfree(pk);
    // 3) 'Z' 交出结果：size = T + D + C + Z，累计清零
    size = 0;
    status = PROT_INIT;
    pk = _t_pgsql_unpack(0, &buf, &ud, &size, &status);
    CuAssertPtrNotNull(tc, pk);
    CuAssertIntEquals(tc, PGPACK_OK, (int)pk->type);
    CuAssertIntEquals(tc, (int)sum, (int)size);
    CuAssertIntEquals(tc, 0, (int)pg.recvlens);
    CuAssertIntEquals(tc, 0, (int)buffer_size(&buf));
    _pgsql_pkfree(pk);
    // 4) CopyInResponse 'G'：立即交出、size 只记它自己，不进累计；后面 C + Z 的 size 也不含它
    _pg_push_msg(&buf, 'G', gbody, sizeof(gbody));
    _pg_push_msg(&buf, 'C', "COPY 2", 7);
    _pg_push_msg(&buf, 'Z', "I", 1);
    size = 0;
    status = PROT_INIT;
    pk = _t_pgsql_unpack(0, &buf, &ud, &size, &status);
    CuAssertPtrNotNull(tc, pk);
    CuAssertIntEquals(tc, PGPACK_COPY_IN, (int)pk->type);
    CuAssertIntEquals(tc, (int)(5 + sizeof(gbody)), (int)size);
    CuAssertIntEquals(tc, 0, (int)pg.recvlens);
    _pgsql_pkfree(pk);
    size = 0;
    status = PROT_INIT;
    CuAssertPtrEquals(tc, NULL, _t_pgsql_unpack(0, &buf, &ud, &size, &status));
    status = PROT_INIT;
    pk = _t_pgsql_unpack(0, &buf, &ud, &size, &status);
    CuAssertPtrNotNull(tc, pk);
    CuAssertIntEquals(tc, (int)((5 + 7) + (5 + 1)), (int)size);
    CuAssertIntEquals(tc, 0, (int)buffer_size(&buf));
    _pgsql_pkfree(pk);
    // 5) 响应收到一半就断连：_pgsql_udfree 清掉累计，不能记到重连后的第一个响应上
    _pg_push_msg(&buf, 'T', tbody, tl);
    status = PROT_INIT;
    CuAssertPtrEquals(tc, NULL, _t_pgsql_unpack(0, &buf, &ud, &size, &status));
    CuAssertIntEquals(tc, (int)(5 + tl), (int)pg.recvlens);
    _pgsql_udfree(&ud);// ref=0 是 C 借用，不会去释放栈上的 pg
    CuAssertPtrEquals(tc, NULL, ud.context);
    CuAssertPtrEquals(tc, NULL, pg.pack);
    CuAssertIntEquals(tc, 0, (int)pg.recvlens);
    buffer_free(&buf);
}
// 时间文本的定宽快路径与 _strptime 落回路径得出同一个值：规范写法走快路径，
// 不补零 / 两个空白 / 没有空白这些非规范写法形状不符、落回 _strptime，两边都得收且值一样
static void test_pgsql_reader_temporal_fastpath(CuTest *tc) {
    static const char *same[] = { "2000-01-02 03:04:05.5", "2000-1-2 3:4:5.5", "2000-01-02  03:04:05.5", "2000-01-0203:04:05.5" };
    const int64_t want = (86400LL + 3 * 3600 + 4 * 60 + 5) * 1000000LL + 500000;
    int32_t err;
    size_t i;
    for (i = 0; i < sizeof(same) / sizeof(same[0]); i++) {
        CuAssertTrue(tc, want == _pg_text_ts(same[i], &err));
        CuAssertIntEquals(tc, ERR_OK, err);
    }
    // 两条路径的量程一致：秒 60/61 都收、62 都拒；0 年都收
    CuAssertTrue(tc, (86400LL + 61) * 1000000LL == _pg_text_ts("2000-01-02 00:00:61", &err));
    CuAssertIntEquals(tc, ERR_OK, err);
    (void)_pg_text_ts("2000-01-02 00:00:62", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    (void)_pg_text_ts("0000-01-01 00:00:00", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    // 定宽形状但字段越界：快路径交回 _strptime，照样拒
    (void)_pg_text_ts("2000-00-01 00:00:00", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    (void)_pg_text_ts("2000-01-00 00:00:00", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    // 日期同理
    CuAssertIntEquals(tc, 1, _pg_text_date("2000-01-02", &err));
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertIntEquals(tc, 1, _pg_text_date("2000-1-2", &err));
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertIntEquals(tc, _pg_text_date("2000-2-31", &err), _pg_text_date("2000-02-31", &err));
    CuAssertIntEquals(tc, ERR_OK, err);
    (void)_pg_text_date("2000-01-32", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
}
// 回归：RowDescription 声明的列数对得上总长，但某个列名超长把后面的列挤出报文时也须判失败
static void test_pgpack_row_description_overlong_name(CuTest *tc) {
    // 2 列 → 需 2*19=38 字节；给足 40 字节，但第一列名字就吃掉 30 字节
    binary_ctx bw;
    binary_init_write(&bw, 0, 0);
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
    binary_init_read(&br, raw, 5 + bw.offset);
    int32_t status = PROT_INIT;
    void *pack = _pgpack_parser(&pg, &br, &ud, &status);
    FREE(raw);// 同 _pgsql_command_response：'T' 由调用方释放
    CuAssertTrue(tc, NULL == pack);
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
    // COPY OUT 的包不进 results 数组，但它的 "COPY N" 标签照读
    pg.type = PGPACK_COPY_OUT;
    safe_fill_str(pg.complete, sizeof(pg.complete), "COPY 3");
    CuAssertTrue(tc, 3 == pgsql_affected_rows(&pg));
    CuAssertTrue(tc, 0 == pgsql_result_count(&pg));// 结果集数组仍只认 PGPACK_OK
    // 报错的那条留着上一条的标签，必须报 0 而不是那条已回滚语句的行数
    pg.type = PGPACK_ERR;
    CuAssertTrue(tc, 0 == pgsql_affected_rows(&pg));
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
// SSLRequest 的应答只有一个字节，而且它是在明文上收的。配了 evssl 却收到 'N'
// 必须拒收——这是唯一一道防 MITM 把 'S' 改写成 'N'、把连接降级到明文 Startup 的闸，
// 而明文 Startup 上服务端一句 AuthCleartextPassword 就能拿到口令原文
static void test_pgsql_ssl_downgrade_refused(CuTest *tc) {
    pgsql_ctx pg;
    ZERO(&pg, sizeof(pg));
    // 'N' 分支只判非空不解引用，随便给个非 NULL 顶上
    pg.evssl = (struct evssl_ctx *)&pg;
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.context = &pg;// status 归零即 pgsql.c 的 INIT：等 SSL 协商响应
    buffer_ctx buf;
    buffer_init(&buf);
    int32_t status = 0;

    // 1) 强制降级：必须报错，不能悄悄接着走明文
    buffer_append(&buf, "N", 1);
    CuAssertTrue(tc, NULL == _t_pgsql_unpack(1, &buf, &ud, NULL, &status));
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));

    // 2) 未知应答字节同样拒收
    status = 0;
    buffer_append(&buf, "X", 1);
    CuAssertTrue(tc, NULL == _t_pgsql_unpack(1, &buf, &ud, NULL, &status));
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));

    // 3) 一个字节都还没到：只能报要更多数据，不许误判成错误
    status = 0;
    CuAssertTrue(tc, NULL == _t_pgsql_unpack(1, &buf, &ud, NULL, &status));
    CuAssertTrue(tc, BIT_CHECK(status, PROT_MOREDATA));
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));

    buffer_free(&buf);
}
// 命令阶段 DataRow / 通知以外的消息，整包(类型码 + 长度 + 正文)不超过 1024 字节放栈上解析：
// ErrorResponse 整包 1024 放栈、1025 走堆，错误串都得完整。栈缓冲写穿由 ASan 构建抓，漏释放交给收尾的内存检查
static void test_pgsql_error_stack_edge(CuTest *tc) {
    static const size_t totals[] = { 1024, 1025 };
    pgsql_ctx pg;
    ud_cxt ud;
    buffer_ctx buf;
    pgpack_ctx *pack;
    char body[1100], mtext[1100], want[1200];
    size_t mlens, blens, i;
    int32_t status, j;
    for (size_t k = 0; k < ARRAY_SIZE(totals); k++) {
        // 正文 = "SERROR\0" + "CXX000\0" + 'M' + 消息 + '\0' + 结尾 '\0'，比消息多 17 字节
        mlens = totals[k] - 5 - 17;
        for (i = 0; i < mlens; i++) {
            mtext[i] = (char)('a' + (i + k) % 26);
        }
        mtext[mlens] = '\0';
        memcpy(body, "SERROR\0CXX000\0M", 15);
        memcpy(body + 15, mtext, mlens + 1);
        blens = 15 + mlens + 1;
        body[blens++] = '\0';
        SNPRINTF(want, sizeof(want), "S: ERROR\r\nC: XX000\r\nM: %s", mtext);
        ZERO(&pg, sizeof(pg));
        ZERO(&ud, sizeof(ud));
        ud.status = 2;// COMMAND
        ud.context = &pg;
        buffer_init(&buf);
        _pg_push_msg(&buf, 'E', body, blens);
        _pg_push_msg(&buf, 'Z', "I", 1);
        pack = NULL;
        for (j = 0; j < 3 && NULL == pack; j++) {
            status = PROT_INIT;
            pack = _t_pgsql_unpack(0, &buf, &ud, NULL, &status);
            CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
        }
        CuAssertIntEquals(tc, 0, (int)buffer_size(&buf));
        buffer_free(&buf);
        CuAssertPtrNotNull(tc, pack);
        CuAssertIntEquals(tc, PGPACK_ERR, (int)pack->type);
        CuAssertStrEquals(tc, want, (const char *)pack->pack);
        _pgpack_free(pack);
    }
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
    SUITE_ADD_TEST(suite, test_pgsql_reader_timezone_reject);
    SUITE_ADD_TEST(suite, test_pgsql_reader_double_text_bounds);
    SUITE_ADD_TEST(suite, test_pgsql_reader_temporal_binary);
    SUITE_ADD_TEST(suite, test_pgsql_reader_uuid);
    SUITE_ADD_TEST(suite, test_pgsql_reader_index);
    SUITE_ADD_TEST(suite, test_pgsql_reader_col_at);
    SUITE_ADD_TEST(suite, test_pgpack_error_notice);
    SUITE_ADD_TEST(suite, test_pgpack_error_notice_empty);
    SUITE_ADD_TEST(suite, test_pgpack_error_notice_unterminated);
    SUITE_ADD_TEST(suite, test_pgpack_parser_full_flow);
    SUITE_ADD_TEST(suite, test_pgpack_parser_empty_body);
    SUITE_ADD_TEST(suite, test_pgsql_payload_framing);
    SUITE_ADD_TEST(suite, test_pgsql_copy_out_framing);
    SUITE_ADD_TEST(suite, test_pgsql_unpack_rows);
    SUITE_ADD_TEST(suite, test_pgsql_fields_separate_alloc);
    SUITE_ADD_TEST(suite, test_pgsql_unpack_rows_spill);
    SUITE_ADD_TEST(suite, test_pgsql_unpack_rows_batch);
    SUITE_ADD_TEST(suite, test_pgsql_unpack_results_spill);
    SUITE_ADD_TEST(suite, test_pgsql_unpack_notification);
    SUITE_ADD_TEST(suite, test_pgsql_unpack_size);
    SUITE_ADD_TEST(suite, test_pgsql_reader_temporal_fastpath);
    SUITE_ADD_TEST(suite, test_pgpack_row_description_overlong_name);
    SUITE_ADD_TEST(suite, test_pgsql_affected_rows);
    SUITE_ADD_TEST(suite, test_pgsql_setter_atomic);
    SUITE_ADD_TEST(suite, test_pgsql_ssl_downgrade_refused);
    SUITE_ADD_TEST(suite, test_pgsql_error_stack_edge);
}
