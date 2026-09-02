#include "test_mysql_parse.h"
#include "lib.h"
#include "protocol/mysql/mysql.h"
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
            reader->fields[i].name.data = (void *)names[i];
            reader->fields[i].name.lens = strlen(names[i]);
            reader->fields[i].type = types[i];
        }
    }
    array_init(&reader->arr_rows, sizeof(mpack_row *), 16);
    return reader;
}

// 往 buf 追加一个完整 MySQL 包：3 字节小端长度 + 1 字节 sequence_id + payload
static void _push_packet(buffer_ctx *buf, const void *payload, size_t lens, uint8_t seq) {
    char head[4];
    pack_integer(head, (uint64_t)lens, 3, 1);
    head[3] = (char)seq;
    buffer_append(buf, head, sizeof(head));
    if (lens > 0) {
        buffer_append(buf, (void *)payload, lens);
    }
}
// 把 reader 挂进 mysql_ctx，摆成"结果集读到一半"。rst 传 1(RST_FIELD) 或 2(RST_ROW)。
// 错误路径下 _mpack_parser 会自己回收 mysql->mpack(含 reader)，调用方不得再释放；
// 成功路径 mpack 仍在(等后续包)，由调用方 _mysql_pkfree
static void _mysql_stage(mysql_ctx *mysql, mysql_reader_ctx *reader, int32_t rst, mpack_type restype) {
    ZERO(mysql, sizeof(*mysql));
    mysql->cur_cmd = (MPACK_STMT_EXECUTE == restype) ? MYSQL_EXECUTE : MYSQL_QUERY;
    mysql->parse_status = rst;
    mpack_ctx *mpack;
    CALLOC(mpack, 1, sizeof(mpack_ctx));
    mpack->pack_type = restype;
    mpack->pack = reader;
    mpack->_free_mpack = _mpack_reader_free;
    mysql->mpack = mpack;
}
// _mpack_ok / _mpack_parse_field / _mpack_parse_binary_row 都是库内 static，改从公开入口
// mysql_unpack 喂整包间接测：这里补 4 字节包头(3 字节小端长度 + 序号)并把 ud 摆到 COMMAND 阶段。
// 结果集的字段/行阶段还要调用方先摆好 mysql->mpack 与 parse_status。
// payload 由 _mysql_payload 拷进独立块，故返回值不依赖这里的临时 buffer
static void *_mysql_feed(mysql_ctx *mysql, const void *payload, size_t plens, int32_t *status) {
    buffer_ctx buf;
    buffer_init(&buf);
    _push_packet(&buf, payload, plens, 1);// 序号解析侧只存不校验
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = 3;// COMMAND
    ud.context = mysql;
    *status = PROT_INIT;
    void *out = mysql_unpack(NULL, INVALID_SOCK, 0, 0, &buf, &ud, NULL, status);
    buffer_free(&buf);
    return out;
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

// mysql_reader_integer 文本协议边界：空值 / 溢出 / 负数 / INT64_MIN / 前导空白 / 裸负号。
// 修复前走 strtoll + "end - tmp == lens" 校验，空串(end 不动、lens 也是 0)与溢出(钳到
// LLONG_MAX 但 end 走到串尾)两种都能骗过它，被当成功报给调用方；且同一个空值
// uinteger 报 ERR_FAILED、integer 报 ERR_OK，同一行同一语义两个 API 结论相反
static void test_mysql_reader_integer_text_bounds(CuTest *tc) {
    char names[6][64] = { "empty", "over", "neg", "min", "space", "onlyminus" };
    uint8_t types[6] = { MYSQL_TYPE_LONGLONG, MYSQL_TYPE_LONGLONG, MYSQL_TYPE_LONGLONG,
                         MYSQL_TYPE_LONGLONG, MYSQL_TYPE_LONGLONG, MYSQL_TYPE_LONGLONG };
    mysql_reader_ctx *r = _reader_new(MPACK_QUERY, 6, names, types);

    // 单块 payload 里排布六个取值，各字段 buf_ctx 按偏移切片
    const char *src = "99999999999999999999" "-42" "-9223372036854775808" " 4" "-";
    char *p;
    MALLOC(p, 64);
    memcpy(p, src, strlen(src));
    buf_ctx cols[6] = {
        { .data = p,      .lens = 0  },// empty：非 NULL 但零长，nil 走的是另一条路(err=1)
        { .data = p,      .lens = 20 },// over：20 个 9，超 INT64_MAX
        { .data = p + 20, .lens = 3  },// neg："-42"
        { .data = p + 23, .lens = 20 },// min："-9223372036854775808"，绝对值恰好越界须单独处理
        { .data = p + 43, .lens = 2  },// space：" 4"，strtoll 会跳空白当合法
        { .data = p + 45, .lens = 1  },// onlyminus：只有符号没有数字
    };
    _reader_push_row(r, p, cols, NULL);

    int32_t err;
    int64_t v = mysql_reader_integer(r, "empty", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    CuAssertTrue(tc, 0 == v);
    // 同一个空值两个 API 必须给出同一结论
    uint64_t uv = mysql_reader_uinteger(r, "empty", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    CuAssertTrue(tc, 0 == uv);

    v = mysql_reader_integer(r, "over", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    CuAssertTrue(tc, 0 == v);

    v = mysql_reader_integer(r, "neg", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, -42 == v);

    v = mysql_reader_integer(r, "min", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, INT64_MIN == v);

    v = mysql_reader_integer(r, "space", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    CuAssertTrue(tc, 0 == v);

    v = mysql_reader_integer(r, "onlyminus", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    CuAssertTrue(tc, 0 == v);

    mysql_reader_free(r);
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

// 文本协议浮点的两个"骗过 end-tmp 校验"的输入：空值与溢出。
// 空值时 strtod 一个字符都不消耗，end-tmp 与 lens 同为 0，那道相等判定反而放行；
// 上溢时 strtod 钳到 ±HUGE_VAL 但 end 照样走到串尾，只有 errno 认得出来。
// 与 mysql_reader_integer 改用 str2u64 挡掉的是同一类。
// 反过来下溢也置 ERANGE，但那时返回的是正确的次正规数，必须放行（用例 4）
static void test_mysql_reader_float_text_bounds(CuTest *tc) {
    char names[2][64] = { "f", "d" };
    uint8_t types[2] = { MYSQL_TYPE_FLOAT, MYSQL_TYPE_DOUBLE };
    int32_t err;

    // 1) 空值（lens=0，非 NULL 列）
    mysql_reader_ctx *r = _reader_new(MPACK_QUERY, 2, names, types);
    char *p1;
    MALLOC(p1, 8);
    buf_ctx empty[2] = { { .data = p1, .lens = 0 }, { .data = p1, .lens = 0 } };
    _reader_push_row(r, p1, empty, NULL);
    CuAssertTrue(tc, 0.0f == mysql_reader_float(r, "f", &err));
    CuAssertIntEquals(tc, ERR_FAILED, err);
    CuAssertTrue(tc, 0.0 == mysql_reader_double(r, "d", &err));
    CuAssertIntEquals(tc, ERR_FAILED, err);
    mysql_reader_free(r);

    // 2) 溢出（超 double 表示范围）
    r = _reader_new(MPACK_QUERY, 2, names, types);
    char *p2;
    MALLOC(p2, 32);
    memcpy(p2, "1e400", 5);
    memcpy(p2 + 5, "-1e400", 6);
    buf_ctx ovf[2] = { { .data = p2, .lens = 5 }, { .data = p2 + 5, .lens = 6 } };
    _reader_push_row(r, p2, ovf, NULL);
    CuAssertTrue(tc, 0.0f == mysql_reader_float(r, "f", &err));
    CuAssertIntEquals(tc, ERR_FAILED, err);
    CuAssertTrue(tc, 0.0 == mysql_reader_double(r, "d", &err));
    CuAssertIntEquals(tc, ERR_FAILED, err);
    mysql_reader_free(r);

    // 3) 合法值不受影响
    r = _reader_new(MPACK_QUERY, 2, names, types);
    char *p3;
    MALLOC(p3, 16);
    memcpy(p3, "1.5", 3);
    memcpy(p3 + 3, "-2.25", 5);
    buf_ctx ok[2] = { { .data = p3, .lens = 3 }, { .data = p3 + 3, .lens = 5 } };
    _reader_push_row(r, p3, ok, NULL);
    CuAssertTrue(tc, 1.5f == mysql_reader_float(r, "f", &err));
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, -2.25 == mysql_reader_double(r, "d", &err));
    CuAssertIntEquals(tc, ERR_OK, err);
    mysql_reader_free(r);

    // 4) 下溢不算失败：ERANGE 是上溢下溢共用的，但下溢时 strtod 返回的是正确的次正规数，
    //    而 DOUBLE 列里 1e-320 这种值是服务端的常规输出（实测 MySQL 8.4 文本协议原样回 "1e-320"）
    r = _reader_new(MPACK_QUERY, 2, names, types);
    char *p4;
    MALLOC(p4, 16);
    memcpy(p4, "1e-320", 6);
    buf_ctx sub[2] = { { .data = p4, .lens = 6 }, { .data = p4, .lens = 6 } };
    _reader_push_row(r, p4, sub, NULL);
    double dv = mysql_reader_double(r, "d", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, dv > 0.0 && dv < 1e-300);
    // float 列同样按 double 解析后再窄化，窄化到 0.0f 是 IEEE 的正常结果，不是解析失败
    (void)mysql_reader_float(r, "f", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
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

// 造一条文本 DATETIME 行并读出，用例多，抽出来省掉重复的 reader 搭建
static int64_t _my_text_dt(const char *s, int32_t *err) {
    char names[1][64] = { "dt" };
    uint8_t types[1] = { MYSQL_TYPE_DATETIME };
    mysql_reader_ctx *r = _reader_new(MPACK_QUERY, 1, names, types);
    size_t n = strlen(s);
    char *p;
    MALLOC(p, n);
    memcpy(p, s, n);
    buf_ctx c[1] = { { .data = p, .lens = n } };
    _reader_push_row(r, p, c, NULL);
    int64_t v = mysql_reader_datetime(r, "dt", err);
    mysql_reader_free(r);
    return v;
}

// 文本 DATETIME 改走 _strptime 后的边界：逐字段量程由 _conv_num 校验。
// 只断言 err 与微秒余数，不断言绝对时间戳——mktime 依赖本地时区/DST
static void test_mysql_reader_datetime_text_range(CuTest *tc) {
    int32_t err;

    // 1) 完整 "日期 时间.小数秒"：小数秒仍由 parse_usec_frac 从解析终点接着取
    int64_t ts = _my_text_dt("2024-05-21 13:45:30.123456", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, ts > 0);
    CuAssertTrue(tc, 123456 == ts % 1000000);

    // 2) DATE 列只有日期，第二段不解析
    ts = _my_text_dt("2024-05-21", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, ts > 0);
    CuAssertTrue(tc, 0 == ts % 1000000);

    // 2b) 日期后面有东西就必须是完整时间：残缺时间不能当成合法 DATE 收下、把时间悄悄丢掉
    (void)_my_text_dt("2024-05-21 13:45", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);

    // 3) 零日期不在本用例范围：它是非严格 SQL 模式下的合法值，两条协议路径都返 0 + ERR_OK，
    //    见 test_mysql_reader_datetime_zero_date

    // 4) 月 / 日 / 时 / 分越界一律拒绝
    (void)_my_text_dt("2024-13-01 00:00:00", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    (void)_my_text_dt("2024-05-32 00:00:00", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    (void)_my_text_dt("2024-05-21 24:00:00", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    (void)_my_text_dt("2024-05-21 13:60:00", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);

    // 5) 分隔符不对 / 整串非日期：第一段就匹配不上
    (void)_my_text_dt("2024/05/21 13:45:30", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    (void)_my_text_dt("not-a-date", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
}

// 零日期 0000-00-00[ 00:00:00] 在文本与二进制两条路径上必须给出相同结果：
// 二进制用长度前缀 0 表示它，一直是 err=ERR_OK + 返回 0；文本侧改走 _strptime 后
// %m 的 1..12 会把它判成解析失败，两边就对不上了，故在解析前单独认掉
static void test_mysql_reader_datetime_zero_date(CuTest *tc) {
    char names[1][64] = { "dt" };
    uint8_t types[1] = { MYSQL_TYPE_DATETIME };
    int32_t err;

    // 文本协议：完整零日期时间
    int64_t ts = _my_text_dt("0000-00-00 00:00:00", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, 0 == ts);

    // 文本协议：DATE 列的零日期（无时间段）
    ts = _my_text_dt("0000-00-00", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, 0 == ts);

    // 二进制协议：长度前缀 0，同样是 ERR_OK + 0
    mysql_reader_ctx *r = _reader_new(MPACK_STMT_EXECUTE, 1, names, types);
    char *p;
    MALLOC(p, 4);
    buf_ctx c[1] = { { .data = p, .lens = 0 } };
    _reader_push_row(r, p, c, NULL);
    ts = mysql_reader_datetime(r, "dt", &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, 0 == ts);
    mysql_reader_free(r);

    // 只有年份是零的部分零日期不在此列，仍按解析失败处理（mktime 也表示不了）
    (void)_my_text_dt("2024-00-15 00:00:00", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
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

// _mpack_ok 解析 OK 包：affected_rows + last_insert_id + status_flags（warnings 只跳过不留）
static void test_mpack_ok_parse(CuTest *tc) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    binary_set_uint8(&bw, MYSQL_OK);// 包类型标志，_mpack_resultset_response 据它分派到 OK 分支
    // lenenc 5（affected_rows）
    _mysql_set_lenenc(&bw, 5);
    // lenenc 17（last_insert_id）
    _mysql_set_lenenc(&bw, 17);
    // status_flags 0x0002，warnings 3（后者本库跳过，写进来是为了让读位置对齐）
    binary_set_integer(&bw, 0x0002, 2, 1);
    binary_set_integer(&bw, 3, 2, 1);
    // 末尾随意附加（_mpack_ok 末尾会 skip 剩余）
    binary_set_int8(&bw, 0xab);
    binary_set_int8(&bw, 0xcd);

    mysql_ctx mysql;
    ZERO(&mysql, sizeof(mysql));
    mysql.cur_cmd = MYSQL_QUERY;// parse_status 留 0：这是响应首包
    int32_t status;
    mpack_ctx *mpack = (mpack_ctx *)_mysql_feed(&mysql, bw.data, bw.offset, &status);
    CuAssertTrue(tc, NULL != mpack);
    CuAssertIntEquals(tc, MPACK_OK, (int)mpack->pack_type);
    mpack_ok *ok = (mpack_ok *)mpack->pack;
    CuAssertIntEquals(tc, 5, (int)ok->affected_rows);
    CuAssertIntEquals(tc, 17, (int)ok->last_insert_id);
    CuAssertIntEquals(tc, 2, (int)ok->status_flags);
    // mysql_ctx 也被同步
    CuAssertIntEquals(tc, 5, (int)mysql.affected_rows);
    CuAssertIntEquals(tc, 17, (int)mysql.last_id);
    _mysql_pkfree(mpack);
    binary_free(&bw);
}

// _mysql_get_lenenc 遇缓冲不足须返 ERR_FAILED，不能落到 binary_get_* 的 ASSERTAB 上把进程 abort。
// 报文长度由对端决定，一个截断的包只该判失败
static void test_mysql_lenenc_truncated(CuTest *tc) {
    int32_t rtn;
    binary_ctx br;
    // 1) 空缓冲：连 flag 字节都没有。须传非 NULL 的 buf 走"外部托管"，
    //    binary_init 对 NULL 是自己 MALLOC 一块可扩容缓冲，那样 size 就不是 0 了
    char empty[1] = { 0 };
    binary_init(&br, empty, 0, 0);
    _mysql_get_lenenc(&br, &rtn);
    CuAssertIntEquals(tc, ERR_FAILED, rtn);

    // 2) flag 说后面有 2/3/8 字节，实际都差一个
    unsigned char t2[2] = { 0xfc, 0x01 };
    binary_init(&br, (char *)t2, sizeof(t2), 0);
    _mysql_get_lenenc(&br, &rtn);
    CuAssert(tc, "0xfc with 1 trailing byte must fail, not abort", ERR_FAILED == rtn);
    unsigned char t3[3] = { 0xfd, 0x01, 0x02 };
    binary_init(&br, (char *)t3, sizeof(t3), 0);
    _mysql_get_lenenc(&br, &rtn);
    CuAssertIntEquals(tc, ERR_FAILED, rtn);
    unsigned char t8[8] = { 0xfe, 1, 2, 3, 4, 5, 6, 7 };
    binary_init(&br, (char *)t8, sizeof(t8), 0);
    _mysql_get_lenenc(&br, &rtn);
    CuAssertIntEquals(tc, ERR_FAILED, rtn);

    // 3) 未知 flag 仍是失败（原有行为不变）
    unsigned char bad[2] = { 0xfb, 0x00 };
    binary_init(&br, (char *)bad, sizeof(bad), 0);
    _mysql_get_lenenc(&br, &rtn);
    CuAssertIntEquals(tc, ERR_FAILED, rtn);

    // 4) 合法值照常读出，别把正常路径一起挡了
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    _mysql_set_lenenc(&bw, 0x0a);
    _mysql_set_lenenc(&bw, 0x1234);
    _mysql_set_lenenc(&bw, 0x123456);
    // 超 0xffffff 走 8 字节编码；取值须在 32 位 size_t 内，否则 m32 构建上写入即截断
    _mysql_set_lenenc(&bw, 0x12345678);
    binary_init(&br, bw.data, bw.offset, 0);
    CuAssertTrue(tc, 0x0a == _mysql_get_lenenc(&br, &rtn) && ERR_OK == rtn);
    CuAssertTrue(tc, 0x1234 == _mysql_get_lenenc(&br, &rtn) && ERR_OK == rtn);
    CuAssertTrue(tc, 0x123456 == _mysql_get_lenenc(&br, &rtn) && ERR_OK == rtn);
    CuAssertTrue(tc, 0x12345678 == _mysql_get_lenenc(&br, &rtn) && ERR_OK == rtn);
    binary_free(&bw);
}

// 回归：OK 包置了 SERVER_SESSION_STATE_CHANGED 却不带尾部数据。
// 修复前 _mpack_ok_track 入口的 lenenc 直接读空缓冲 → ASSERTAB abort 整个进程（release 同样）；
// 现在这段可选信息按"没带"静默放弃，OK 包本身照常解析成功
static void test_mpack_ok_track_truncated(CuTest *tc) {
    binary_ctx bw;
    mysql_ctx mysql;
    int32_t status;
    mpack_ctx *mpack;

    // 1) 尾部一个字节都没有
    binary_init(&bw, NULL, 0, 0);
    binary_set_uint8(&bw, MYSQL_OK);
    _mysql_set_lenenc(&bw, 1);
    _mysql_set_lenenc(&bw, 0);
    binary_set_integer(&bw, SERVER_SESSION_STATE_CHANGED, 2, 1);
    binary_set_integer(&bw, 0, 2, 1);
    ZERO(&mysql, sizeof(mysql));
    mysql.cur_cmd = MYSQL_QUERY;
    mpack = (mpack_ctx *)_mysql_feed(&mysql, bw.data, bw.offset, &status);
    CuAssertTrue(tc, NULL != mpack);
    CuAssertIntEquals(tc, 1, (int)((mpack_ok *)mpack->pack)->affected_rows);
    CuAssert(tc, "truncated session-track must not corrupt the database name",
        '\0' == mysql.client.database[0]);
    _mysql_pkfree(mpack);
    binary_free(&bw);

    // 2) info 段长度自洽但紧跟其后的 session_state 长度字节缺失
    binary_init(&bw, NULL, 0, 0);
    binary_set_uint8(&bw, MYSQL_OK);
    _mysql_set_lenenc(&bw, 1);
    _mysql_set_lenenc(&bw, 0);
    binary_set_integer(&bw, SERVER_SESSION_STATE_CHANGED, 2, 1);
    binary_set_integer(&bw, 0, 2, 1);
    _mysql_set_lenenc(&bw, 2);// info 长度
    binary_set_int8(&bw, 'h');
    binary_set_int8(&bw, 'i');// info 到此结束，后面没有 session_state 长度字节
    ZERO(&mysql, sizeof(mysql));
    mysql.cur_cmd = MYSQL_QUERY;
    mpack = (mpack_ctx *)_mysql_feed(&mysql, bw.data, bw.offset, &status);
    CuAssertTrue(tc, NULL != mpack);
    CuAssert(tc, "truncated session-state length must not abort",
        '\0' == mysql.client.database[0]);
    _mysql_pkfree(mpack);
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
    _mpack_err(&mysql, &br);
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
    _mpack_err(&mysql, &br);
    CuAssertIntEquals(tc, 1045, mysql.error_code);
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
        binary_set_uint8(&bw, 0x00);// 二进制协议行包的前导字节，_mpack_reader_rows 校验后 skip 掉
        binary_set_uint8(&bw, 0x00);//NULL 位图：field 0 不为 NULL
        binary_set_uint8(&bw, 9);//DATETIME 长度前缀：9（非法）
        mysql_ctx mysql;
        _mysql_stage(&mysql, r, 2, MPACK_STMT_EXECUTE);
        int32_t status;
        CuAssertTrue(tc, NULL == _mysql_feed(&mysql, bw.data, bw.offset, &status));
        CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));// 错误收尾已回收 mpack 与 reader
        binary_free(&bw);
    }
    // TIME 非法长度 5 → ERR_FAILED（合法：0/8/12）
    {
        uint8_t types[1] = { MYSQL_TYPE_TIME };
        mysql_reader_ctx *r = _reader_new(MPACK_STMT_EXECUTE, 1, names, types);
        binary_ctx bw;
        binary_init(&bw, NULL, 0, 0);
        binary_set_uint8(&bw, 0x00);// 二进制协议行包的前导字节，_mpack_reader_rows 校验后 skip 掉
        binary_set_uint8(&bw, 0x00);//NULL 位图
        binary_set_uint8(&bw, 5);//TIME 长度前缀：5（非法）
        mysql_ctx mysql;
        _mysql_stage(&mysql, r, 2, MPACK_STMT_EXECUTE);
        int32_t status;
        CuAssertTrue(tc, NULL == _mysql_feed(&mysql, bw.data, bw.offset, &status));
        CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));// 错误收尾已回收 mpack 与 reader
        binary_free(&bw);
    }
    // DATE 合法长度 0（零值）→ ERR_OK
    {
        uint8_t types[1] = { MYSQL_TYPE_DATE };
        mysql_reader_ctx *r = _reader_new(MPACK_STMT_EXECUTE, 1, names, types);
        // row[0] 是二进制协议行包的前导字节，_mpack_reader_rows 校验后 skip 掉
        char row[3];
        row[0] = 0x00;
        row[1] = 0x00;//NULL 位图
        row[2] = 0;//DATE 长度前缀：0（零值，合法）
        mysql_ctx mysql;
        _mysql_stage(&mysql, r, 2, MPACK_STMT_EXECUTE);
        int32_t status;
        // 这一行解析成功后结果集还要等 EOF，故返 NULL 并置 MOREDATA；关键是不置 PROT_ERROR
        CuAssertTrue(tc, NULL == _mysql_feed(&mysql, row, sizeof(row), &status));
        CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR) && BIT_CHECK(status, PROT_MOREDATA));
        _mysql_pkfree(mysql.mpack);
    }
}

// 回归：报文里读出的长度拿去 binary_get_* 之前必须与剩余字节比过。
// 修复前这些截断报文会落到 binary_get_binary/skip 的 ASSERTAB 上 abort 整个进程（release 同样）
static void test_mysql_truncated_row(CuTest *tc) {
    // 1) NULL 位图长度来自上一个包声明的 field_count，与本包长度无关：
    //    声明 16 列需要 3 字节位图，行包只给 2 字节
    {
        char names[16][64];
        uint8_t types[16];
        for (int32_t i = 0; i < 16; i++) {
            SNPRINTF(names[i], sizeof(names[i]), "c%d", i);
            types[i] = MYSQL_TYPE_LONG;
        }
        mysql_reader_ctx *r = _reader_new(MPACK_STMT_EXECUTE, 16, names, types);
        binary_ctx bw;
        binary_init(&bw, NULL, 0, 0);
        binary_set_uint8(&bw, 0x00);// 二进制协议行包的前导字节，_mpack_reader_rows 校验后 skip 掉
        binary_set_uint8(&bw, 0x00);
        binary_set_uint8(&bw, 0x00);// 只有 2 字节，位图要 3 字节
        mysql_ctx mysql;
        _mysql_stage(&mysql, r, 2, MPACK_STMT_EXECUTE);
        int32_t status;
        CuAssert(tc, "short NULL bitmap must fail, not abort",
            NULL == _mysql_feed(&mysql, bw.data, bw.offset, &status) && BIT_CHECK(status, PROT_ERROR));
        binary_free(&bw);
    }
    // 2) 二进制协议 lenenc 字符串：长度说 10，实际只剩 3 字节
    {
        char names[1][64] = { "v" };
        uint8_t types[1] = { MYSQL_TYPE_STRING };
        mysql_reader_ctx *r = _reader_new(MPACK_STMT_EXECUTE, 1, names, types);
        binary_ctx bw;
        binary_init(&bw, NULL, 0, 0);
        binary_set_uint8(&bw, 0x00);// 二进制协议行包的前导字节，_mpack_reader_rows 校验后 skip 掉
        binary_set_uint8(&bw, 0x00);// NULL 位图
        _mysql_set_lenenc(&bw, 10);// 声明 10 字节
        binary_set_uint8(&bw, 'a');
        binary_set_uint8(&bw, 'b');
        binary_set_uint8(&bw, 'c');// 只给 3 字节
        mysql_ctx mysql;
        _mysql_stage(&mysql, r, 2, MPACK_STMT_EXECUTE);
        int32_t status;
        CuAssert(tc, "lenenc value longer than the packet must fail, not abort",
            NULL == _mysql_feed(&mysql, bw.data, bw.offset, &status) && BIT_CHECK(status, PROT_ERROR));
        binary_free(&bw);
    }
    // 3) 定长类型同样受这道判定保护：LONG 要 4 字节，位图之后一个都不剩
    {
        char names[1][64] = { "v" };
        uint8_t types[1] = { MYSQL_TYPE_LONG };
        mysql_reader_ctx *r = _reader_new(MPACK_STMT_EXECUTE, 1, names, types);
        binary_ctx bw;
        binary_init(&bw, NULL, 0, 0);
        binary_set_uint8(&bw, 0x00);// 二进制协议行包的前导字节，_mpack_reader_rows 校验后 skip 掉
        binary_set_uint8(&bw, 0x00);// 只有 NULL 位图
        mysql_ctx mysql;
        _mysql_stage(&mysql, r, 2, MPACK_STMT_EXECUTE);
        int32_t status;
        CuAssert(tc, "fixed-width value with no bytes left must fail, not abort",
            NULL == _mysql_feed(&mysql, bw.data, bw.offset, &status) && BIT_CHECK(status, PROT_ERROR));
        binary_free(&bw);
    }
    // 4) 列定义包的 catalog 长度说 10，实际只剩 2 字节
    {
        binary_ctx bw;
        binary_init(&bw, NULL, 0, 0);
        _mysql_set_lenenc(&bw, 10);
        binary_set_uint8(&bw, 'd');
        binary_set_uint8(&bw, 'e');
        char names[1][64] = { "c" };
        uint8_t types[1] = { MYSQL_TYPE_LONG };
        mysql_reader_ctx *r = _reader_new(MPACK_QUERY, 1, names, types);
        mysql_ctx mysql;
        _mysql_stage(&mysql, r, 1, MPACK_QUERY);// RST_FIELD
        int32_t status;
        CuAssert(tc, "truncated column definition must fail, not abort",
            NULL == _mysql_feed(&mysql, bw.data, bw.offset, &status) && BIT_CHECK(status, PROT_ERROR));
        binary_free(&bw);
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
// 分配 cap 字节全 '1' 的 payload，作为唯一字段的行数据压入 reader，用于构造 lens==cap 越界场景
static void _push_oversized_field(mysql_reader_ctx *reader, size_t cap) {
    char *p;
    MALLOC(p, cap);
    memset(p, '1', cap);
    buf_ctx c[1] = { { .data = p, .lens = cap } };
    _reader_push_row(reader, p, c, NULL);
}
// 超长文本字段一律拒绝，不截断也不越界。integer/uinteger 已改走 str2u64 按 lens 解析，
// 靠上界判定挡下；float/double(cap=128) 与 datetime/time(cap=48) 仍走 copy_bounded
// strict=1 (utils.c)，lens >= cap 直接返回 ERR_FAILED 且不写入目标缓冲
static void test_mysql_reader_copy_field_boundary(CuTest *tc) {
    char names[1][64] = { "n" };

    // 64 字节全 '1' 的数字串：远超 int64/uint64 量程，两个 API 都应报溢出
    uint8_t itypes[1] = { MYSQL_TYPE_LONGLONG };
    mysql_reader_ctx *r = _reader_new(MPACK_QUERY, 1, names, itypes);
    _push_oversized_field(r, 64);
    int32_t err;
    int64_t iv = mysql_reader_integer(r, "n", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    CuAssertTrue(tc, 0 == iv);
    uint64_t uv = mysql_reader_uinteger(r, "n", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    CuAssertTrue(tc, 0 == uv);
    mysql_reader_free(r);

    // float/double 共用 cap=128
    uint8_t ftypes[1] = { MYSQL_TYPE_DOUBLE };
    mysql_reader_ctx *rf = _reader_new(MPACK_QUERY, 1, names, ftypes);
    _push_oversized_field(rf, 128);
    double dv = mysql_reader_double(rf, "n", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    CuAssertTrue(tc, 0.0 == dv);
    mysql_reader_free(rf);

    // datetime/time 共用 cap=48
    uint8_t dtypes[1] = { MYSQL_TYPE_DATETIME };
    mysql_reader_ctx *rd = _reader_new(MPACK_QUERY, 1, names, dtypes);
    _push_oversized_field(rd, 48);
    int64_t ts = mysql_reader_datetime(rd, "n", &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    CuAssertTrue(tc, 0 == ts);
    mysql_reader_free(rd);

    uint8_t ttypes[1] = { MYSQL_TYPE_TIME };
    mysql_reader_ctx *rt = _reader_new(MPACK_QUERY, 1, names, ttypes);
    _push_oversized_field(rt, 48);
    struct tm tmv;
    uint32_t usec;
    (void)mysql_reader_time(rt, "n", &tmv, &usec, &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    mysql_reader_free(rt);
}
// _mpack_parse_lenenc_field：名字改为 buf_ctx 指向 payload 后，任意长度都完整保留不再截断；
// 同时验证 _mpack_parse_field 成功时把 breader->data 的所有权转给 field->payload
static void test_mpack_parse_field_long_name(CuTest *tc) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    // catalog（任意值，_mpack_parse_field 内部只 skip）
    _mysql_set_lenenc(&bw, 3);
    binary_set_binary(&bw, "def", 3);
    // schema：70 字节全 'a'，旧实现会截断为 63 字节，改用 buf_ctx 后须完整保留
    _mysql_set_lenenc(&bw, 70);
    char oversized[70];
    memset(oversized, 'a', sizeof(oversized));
    binary_set_binary(&bw, oversized, sizeof(oversized));
    // table / org_table / name / org_name：正常长度，验证长名字段之后的解析未受影响
    _mysql_set_lenenc(&bw, 4);
    binary_set_binary(&bw, "tbl1", 4);
    _mysql_set_lenenc(&bw, 4);
    binary_set_binary(&bw, "tbl1", 4);
    _mysql_set_lenenc(&bw, 3);
    binary_set_binary(&bw, "col", 3);
    _mysql_set_lenenc(&bw, 3);
    binary_set_binary(&bw, "col", 3);
    // length of fixed length fields（固定为 0x0c，仅被 skip 验证）
    _mysql_set_lenenc(&bw, 0x0c);
    // character(2) + field_lens(4) + type(1) + flags(2) + decimals(1)
    binary_set_integer(&bw, 0x21, 2, 1);
    binary_set_integer(&bw, 100, 4, 1);
    binary_set_uint8(&bw, MYSQL_TYPE_VARCHAR);
    binary_set_uinteger(&bw, 0, 2, 1);
    binary_set_uint8(&bw, 0);

    char names[1][64] = { "c" };
    uint8_t types[1] = { MYSQL_TYPE_LONG };
    mysql_reader_ctx *r = _reader_new(MPACK_QUERY, 1, names, types);
    mysql_ctx mysql;
    _mysql_stage(&mysql, r, 1, MPACK_QUERY);// RST_FIELD
    int32_t status;
    // 唯一那列解析完 index 追上 field_count，字段阶段结束转入行阶段，再等下一包 → 返 NULL 不报错
    CuAssertTrue(tc, NULL == _mysql_feed(&mysql, bw.data, bw.offset, &status));
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    mpack_field field = r->fields[0];// 下面读的 buf_ctx 都指向 payload，须在 pkfree 之前读完
    CuAssert(tc, "70-byte schema must be kept intact, not truncated", 70 == field.schema.lens);
    for (int i = 0; i < 70; i++) {
        CuAssertTrue(tc, 'a' == ((const char *)field.schema.data)[i]);
    }
    // 长名字段之后的其余字段未受影响
    CuAssert(tc, "table", 4 == field.table.lens && 0 == memcmp(field.table.data, "tbl1", 4));
    CuAssert(tc, "org_table", 4 == field.org_table.lens && 0 == memcmp(field.org_table.data, "tbl1", 4));
    CuAssert(tc, "name", 3 == field.name.lens && 0 == memcmp(field.name.data, "col", 3));
    CuAssert(tc, "org_name", 3 == field.org_name.lens && 0 == memcmp(field.org_name.data, "col", 3));
    CuAssertIntEquals(tc, MYSQL_TYPE_VARCHAR, field.type);
    // 原来断言 field.payload 就是测试自己那块内存；改走公开入口后 payload 由 _mysql_payload
    // 内部分配，指针相等测不到，只能断言确实接过了一块，漏释放交给 MEMORY_CHECK 兜
    CuAssertTrue(tc, NULL != field.payload);
    _mysql_pkfree(mysql.mpack);
    binary_free(&bw);
}
// 一条合法的列定义包体（catalog/schema/table/org_table/name/org_name + 定长段）
static void _build_field_packet(binary_ctx *bw) {
    binary_init(bw, NULL, 0, 0);
    _mysql_set_lenenc(bw, 3);
    binary_set_binary(bw, "def", 3);
    _mysql_set_lenenc(bw, 2);
    binary_set_binary(bw, "db", 2);
    _mysql_set_lenenc(bw, 4);
    binary_set_binary(bw, "tbl1", 4);
    _mysql_set_lenenc(bw, 4);
    binary_set_binary(bw, "tbl1", 4);
    _mysql_set_lenenc(bw, 3);
    binary_set_binary(bw, "col", 3);
    _mysql_set_lenenc(bw, 3);
    binary_set_binary(bw, "col", 3);
    // 以下是 ColumnDefinition41 的定长区，lenenc 先声明它一共 0x0c 字节
    _mysql_set_lenenc(bw, 0x0c);
    binary_set_integer(bw, 0x21, 2, 1);// character_set，0x21 = utf8_general_ci
    binary_set_integer(bw, 100, 4, 1);// column_length，列的最大显示宽度
    binary_set_uint8(bw, MYSQL_TYPE_VARCHAR);
    binary_set_uinteger(bw, 0, 2, 1);// flags，本用例不关心列属性，给 0
    binary_set_uint8(bw, 0);// decimals，小数位数
}
// 字段阶段的列定义条数少于声明的列数：服务端声明 2 列却只送 1 条列定义就发 EOF。
// 没收到的那列还是 CALLOC 的全零，type 0 恰好是合法枚举 MYSQL_TYPE_DECIMAL、name 长度为 0，
// 行解析会照着这份假元数据把整行拆错位，故须在转入行阶段前判定为协议错
static void test_mpack_fields_short_of_count(CuTest *tc) {
    // 首包 payload 已由调用方读出放进 breader（对齐 _mpack_resultset_response 的形状）：列数 = 2
    char *first;
    MALLOC(first, 1);
    first[0] = 2;
    binary_ctx breader;
    binary_init(&breader, first, 1, 0);

    // 后续包排进 buf：只有 1 条列定义，紧接着就是 EOF
    binary_ctx fw;
    _build_field_packet(&fw);
    buffer_ctx buf;
    buffer_init(&buf);
    _push_packet(&buf, fw.data, fw.offset, 1);
    binary_free(&fw);
    // EOF 包：首字节 0xfe + warnings(2 字节) + status_flags(2 字节)
    char eof[5] = { (char)MYSQL_EOF, 0, 0, 0, 0 };
    _push_packet(&buf, eof, sizeof(eof), 2);

    mysql_ctx mysql;
    ZERO(&mysql, sizeof(mysql));
    mysql.cur_cmd = MYSQL_QUERY;
    int32_t status = PROT_INIT;
    mpack_ctx *mpack = _mpack_parser(&mysql, &buf, &breader, &status);
    CuAssertTrue(tc, NULL == mpack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
    // 报错路径把半成品 reader 一起回收，不留给下一个包接着用
    CuAssertTrue(tc, NULL == mysql.mpack);
    CuAssertIntEquals(tc, 0, (int)mysql.parse_status);
    buffer_free(&buf);
}
// 同上的对照组：列定义条数与声明一致时正常转入行阶段，EOF 之后返回完整 mpack
static void test_mpack_fields_match_count(CuTest *tc) {
    char *first;
    MALLOC(first, 1);
    first[0] = 1;// 声明 1 列
    binary_ctx breader;
    binary_init(&breader, first, 1, 0);

    binary_ctx fw;
    _build_field_packet(&fw);
    buffer_ctx buf;
    buffer_init(&buf);
    _push_packet(&buf, fw.data, fw.offset, 1);
    binary_free(&fw);
    char eof[5] = { (char)MYSQL_EOF, 0, 0, 0, 0 };
    _push_packet(&buf, eof, sizeof(eof), 2);// 字段阶段 EOF
    _push_packet(&buf, eof, sizeof(eof), 3);// 行阶段 EOF：空结果集

    mysql_ctx mysql;
    ZERO(&mysql, sizeof(mysql));
    mysql.cur_cmd = MYSQL_QUERY;
    int32_t status = PROT_INIT;
    mpack_ctx *mpack = _mpack_parser(&mysql, &buf, &breader, &status);
    CuAssertPtrNotNull(tc, mpack);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    mysql_reader_ctx *reader = mpack->pack;
    CuAssertIntEquals(tc, 1, reader->field_count);
    _mysql_pkfree(mpack);
    buffer_free(&buf);
}
// _mysql_udfree 必须把整组解析状态一起复位。mysql_ctx 会活过连接(还有别的持有者引用着),
// 只清 mpack 而留下 parse_status/cur_cmd 的话,重连后一个非请求包就会带着上一代的
// "还在读结果集行"状态走进 reader,去解引用已经是 NULL 的 mpack
static void test_mysql_udfree_reset(CuTest *tc) {
    mysql_ctx mysql;
    ZERO(&mysql, sizeof(mysql));
    // ref=0 表示"C 借用",PROT_REF_RELEASE 会短路,不会去 FREE 这个栈上对象
    mysql.client.sk.fd = (SOCKET)7;
    mysql.parse_status = 3;// 任意非 0:代表结果集读到一半
    mysql.cur_cmd = MYSQL_QUERY;

    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.context = &mysql;
    _mysql_udfree(&ud);

    CuAssertTrue(tc, NULL == ud.context);
    CuAssertTrue(tc, INVALID_SOCK == mysql.client.sk.fd);
    CuAssertTrue(tc, NULL == mysql.mpack);
    CuAssertIntEquals(tc, 0, (int)mysql.parse_status);
    CuAssertIntEquals(tc, 0, (int)mysql.cur_cmd);
    // 重复调用安全(context 已置空直接返回)
    _mysql_udfree(&ud);
}
// 回归:行阶段收到截断的 EOF 包必须判协议错。
// 修复前 _mpack_check_final 对"包被截断"和"还有更多结果集"返回同一个 ERR_FAILED,
// 调用方按后者处理:把残缺结果集当成功交出去并置 more=1,调用方随后死等一个永不到来的结果集
static void test_mpack_row_eof_truncated(CuTest *tc) {
    static const char names[1][64] = { "id" };
    static const uint8_t types[1] = { MYSQL_TYPE_LONG };
    mysql_ctx mysql;
    ZERO(&mysql, sizeof(mysql));
    mysql.cur_cmd = MYSQL_QUERY;
    mysql.parse_status = 2;// RST_ROW:结果集已进入行阶段
    mpack_ctx *mpack;
    CALLOC(mpack, 1, sizeof(mpack_ctx));
    mpack->pack_type = MPACK_QUERY;
    mpack->pack = _reader_new(MPACK_QUERY, 1, names, types);
    mpack->_free_mpack = _mpack_reader_free;
    mysql.mpack = mpack;

    // 头 4 字节(长度 1 + 序号 5) + payload 只有一个 0xFE:EOF 标志在,后面的 4 字节没了
    unsigned char raw[] = { 0x01, 0x00, 0x00, 0x05, MYSQL_EOF };
    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, raw, sizeof(raw));
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = 3;// COMMAND
    ud.context = &mysql;
    int32_t status = PROT_INIT;
    void *out = mysql_unpack(NULL, INVALID_SOCK, 0, 0, &buf, &ud, NULL, &status);
    CuAssert(tc, "truncated EOF in the row phase must be a protocol error",
        NULL == out && BIT_CHECK(status, PROT_ERROR));
    CuAssert(tc, "must not be reported as MOREDATA", !BIT_CHECK(status, PROT_MOREDATA));
    // _mpack_parser 的错误收尾负责回收半截结果集并复位状态
    CuAssertTrue(tc, NULL == mysql.mpack);
    CuAssertIntEquals(tc, 0, (int)mysql.parse_status);
    CuAssertIntEquals(tc, 0, (int)mysql.cur_cmd);
    buffer_free(&buf);
}
// 回归:lenenc 长度必须先与剩余字节比过再收窄成 size_t。
// 反过来的话 32 位构建上 0x1_0000_0001 截成 1 就能骗过判定,再按 1 字节读 —— 整包静默错位
static void test_mpack_lenenc_no_narrow_first(CuTest *tc) {
    // 0xfe + 8 字节长度 0x0000000100000001,后面只跟 2 字节实际数据
    unsigned char raw[] = { 0xfe, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 'a', 'b' };
    char names[1][64] = { "c" };
    uint8_t types[1] = { MYSQL_TYPE_LONG };
    mysql_reader_ctx *r = _reader_new(MPACK_QUERY, 1, names, types);
    mysql_ctx mysql;
    _mysql_stage(&mysql, r, 1, MPACK_QUERY);// RST_FIELD
    int32_t status;
    // raw 有 11 字节，而 _mysql_is_eof_packet 要求 size < 9，故首字节 0xfe 不会被当成 EOF 包
    CuAssert(tc, "a 4GiB+1 lenenc length must fail regardless of size_t width",
        NULL == _mysql_feed(&mysql, raw, sizeof(raw), &status) && BIT_CHECK(status, PROT_ERROR));
}
void test_mysql_parse(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_mysql_reader_init);
    SUITE_ADD_TEST(suite, test_mysql_reader_cursor);
    SUITE_ADD_TEST(suite, test_mysql_reader_integer_text);
    SUITE_ADD_TEST(suite, test_mysql_reader_integer_text_bounds);
    SUITE_ADD_TEST(suite, test_mysql_reader_integer_binary);
    SUITE_ADD_TEST(suite, test_mysql_reader_uinteger);
    SUITE_ADD_TEST(suite, test_mysql_reader_float_double_text);
    SUITE_ADD_TEST(suite, test_mysql_reader_float_text_bounds);
    SUITE_ADD_TEST(suite, test_mysql_reader_string);
    SUITE_ADD_TEST(suite, test_mysql_reader_datetime_binary);
    SUITE_ADD_TEST(suite, test_mysql_reader_datetime_text_range);
    SUITE_ADD_TEST(suite, test_mysql_reader_datetime_zero_date);
    SUITE_ADD_TEST(suite, test_mysql_reader_time);
    SUITE_ADD_TEST(suite, test_mysql_reader_binary_lens);
    SUITE_ADD_TEST(suite, test_mpack_ok_parse);
    SUITE_ADD_TEST(suite, test_mysql_lenenc_truncated);
    SUITE_ADD_TEST(suite, test_mysql_truncated_row);
    SUITE_ADD_TEST(suite, test_mpack_ok_track_truncated);
    SUITE_ADD_TEST(suite, test_mpack_err_parse);
    SUITE_ADD_TEST(suite, test_mpack_err_empty_msg);
    SUITE_ADD_TEST(suite, test_mysql_payload);
    SUITE_ADD_TEST(suite, test_mysql_stmt_init);
    SUITE_ADD_TEST(suite, test_mysql_binary_row_temporal_invalid_len);
    SUITE_ADD_TEST(suite, test_mysql_reader_datetime_text);
    SUITE_ADD_TEST(suite, test_mysql_reader_datetime2_types);
    SUITE_ADD_TEST(suite, test_mysql_reader_copy_field_boundary);
    SUITE_ADD_TEST(suite, test_mpack_parse_field_long_name);
    SUITE_ADD_TEST(suite, test_mpack_fields_short_of_count);
    SUITE_ADD_TEST(suite, test_mpack_fields_match_count);
    SUITE_ADD_TEST(suite, test_mysql_udfree_reset);
    SUITE_ADD_TEST(suite, test_mpack_row_eof_truncated);
    SUITE_ADD_TEST(suite, test_mpack_lenenc_no_narrow_first);
}
