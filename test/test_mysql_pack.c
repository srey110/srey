#include "test_mysql_pack.h"
#include "lib.h"
#include "protocol/mysql/mysql_utils.h"
#include "protocol/mysql/mysql_bind.h"
#include "protocol/mysql/mysql_macro.h"

/* =======================================================================
 * _mysql_set_lenenc / _mysql_get_lenenc —— lenenc 整数编解码
 * MySQL lenenc 编码规则：
 *  - val <= 0xfa：单字节直接存值
 *  - val <= 0xffff：0xfc + 2 字节小端
 *  - val <= 0xffffff：0xfd + 3 字节小端
 *  - val <= 0xffffffffffffffff：0xfe + 8 字节小端
 * ======================================================================= */

static void _lenenc_roundtrip(CuTest *tc, uint64_t value, size_t expected_bytes) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 32);
    _mysql_set_lenenc(&bw, (size_t)value);
    CuAssertTrue(tc, expected_bytes == bw.offset);

    binary_ctx br;
    binary_init(&br, bw.data, bw.offset, 0);
    int32_t err = ERR_FAILED;
    uint64_t got = _mysql_get_lenenc(&br, &err);
    CuAssertIntEquals(tc, ERR_OK, err);
    CuAssertTrue(tc, value == got);
    /* 读完全部字节 */
    CuAssertTrue(tc, br.offset == br.size);

    binary_free(&bw);
}

static void test_mysql_lenenc(CuTest *tc) {
    /* 单字节边界：0/1/0xfa */
    _lenenc_roundtrip(tc, 0, 1);
    _lenenc_roundtrip(tc, 1, 1);
    _lenenc_roundtrip(tc, 0xfa, 1);

    /* 0xfb~0xffff：1 标志 + 2 字节 = 3 字节 */
    _lenenc_roundtrip(tc, 0xfb,   3);
    _lenenc_roundtrip(tc, 0xff,   3);
    _lenenc_roundtrip(tc, 0xffff, 3);

    /* 0x10000~0xffffff：1 标志 + 3 字节 = 4 字节 */
    _lenenc_roundtrip(tc, 0x10000,    4);
    _lenenc_roundtrip(tc, INT3_MAX,   4);

    /* > 0xffffff：1 标志 + 8 字节 = 9 字节 */
    _lenenc_roundtrip(tc, 0x01000000ULL, 9);
    _lenenc_roundtrip(tc, 0xFFFFFFFFUL,  9);/* 32 位 size_t 最大值，仍触发 8 字节编码 */
#if SIZE_MAX > 0xFFFFFFFFUL
    _lenenc_roundtrip(tc, 0x123456789ABCDEFULL, 9);/* 仅 64 位平台 size_t 能容纳此值 */
#endif

    /* 异常 flag：0xff 在 _mysql_get_lenenc 内未定义 */
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 8);
    binary_set_uint8(&bw, 0xff);
    binary_ctx br;
    binary_init(&br, bw.data, bw.offset, 0);
    int32_t err = ERR_OK;
    _mysql_get_lenenc(&br, &err);
    CuAssertIntEquals(tc, ERR_FAILED, err);
    binary_free(&bw);
}

/* =======================================================================
 * _mysql_set_payload_lens —— 回填 payload 长度到包头 0-2 字节
 * ======================================================================= */
static void test_mysql_set_payload_lens(CuTest *tc) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 32);
    /* MySQL 包头 4 字节：3 字节长度 + 1 字节 sequence id */
    binary_set_skip(&bw, 4);
    /* 写 7 字节 payload */
    binary_set_binary(&bw, "PAYLOAD", 7);

    _mysql_set_payload_lens(&bw);

    /* 头 3 字节小端长度 = 7 */
    CuAssertTrue(tc, 7 == (uint8_t)bw.data[0]);
    CuAssertTrue(tc, 0 == (uint8_t)bw.data[1]);
    CuAssertTrue(tc, 0 == (uint8_t)bw.data[2]);

    binary_free(&bw);
}

/* =======================================================================
 * mysql_bind_init / clear / nil / string / integer / uinteger / float /
 * double / datetime / time —— 写入计数与缓冲区扩展验证
 * ======================================================================= */
static void test_mysql_bind_basic(CuTest *tc) {
    mysql_bind_ctx mb;
    mysql_bind_init(&mb);
    CuAssertIntEquals(tc, 0, mb.count);

    /* 6 个参数 */
    mysql_bind_nil(&mb, "n1");
    mysql_bind_string(&mb, "s1", "hello", 5);
    mysql_bind_integer(&mb, "i1", -12345);
    mysql_bind_uinteger(&mb, "u1", 99999999ULL);
    mysql_bind_float(&mb, "f1", 3.14f);
    mysql_bind_double(&mb, "d1", 2.71828);

    CuAssertIntEquals(tc, 6, mb.count);
    /* 各缓冲均有写入 */
    CuAssertTrue(tc, mb.bitmap.offset > 0);
    CuAssertTrue(tc, mb.type.offset > 0);
    CuAssertTrue(tc, mb.type_name.offset > 0);
    CuAssertTrue(tc, mb.value.offset > 0);

    /* clear 后 count 回零，缓冲 offset 回零 */
    mysql_bind_clear(&mb);
    CuAssertIntEquals(tc, 0, mb.count);
    CuAssertTrue(tc, 0 == mb.bitmap.offset);
    CuAssertTrue(tc, 0 == mb.type.offset);
    CuAssertTrue(tc, 0 == mb.type_name.offset);
    CuAssertTrue(tc, 0 == mb.value.offset);

    mysql_bind_free(&mb);
}

static void test_mysql_bind_temporal(CuTest *tc) {
    mysql_bind_ctx mb;
    mysql_bind_init(&mb);

    /* datetime + time 各种分支 */
    mysql_bind_datetime(&mb, "dt", 1716000000); /* 2024-05-18 UTC */
    mysql_bind_time(&mb, "t1", 0, 1, 12, 30, 45);  /* +1d 12:30:45 */
    mysql_bind_time(&mb, "t2", 1, 2, 4, 5, 6);     /* -2d 04:05:06 */
    mysql_bind_time(&mb, "t0", 0, 0, 0, 0, 0);     /* 0 → 1 字节包体 */

    CuAssertIntEquals(tc, 4, mb.count);
    CuAssertTrue(tc, mb.value.offset > 0);
    CuAssertTrue(tc, mb.type.offset > 0);

    mysql_bind_free(&mb);
}

/* =======================================================================
 * mysql_pack_query —— COM_QUERY 包格式
 * 包结构: 4 字节包头(3 长度 + 1 sequence) + 1 字节 COM_QUERY + SQL
 * ======================================================================= */
static void test_mysql_pack_query_no_bind(CuTest *tc) {
    /* 由于 mysql_ctx 涉及 capabilities，pack_query 需要构造一个最小 mysql_ctx */
    mysql_ctx mysql;
    ZERO(&mysql, sizeof(mysql));

    const char *sql = "SELECT 1";
    size_t size = 0;
    void *pack = mysql_pack_query(&mysql, sql, NULL, &size);
    CuAssertPtrNotNull(tc, pack);
    /* 至少包含 4 字节包头 + 1 字节命令 + SQL */
    CuAssertTrue(tc, size >= 4 + 1 + strlen(sql));

    char *p = (char *)pack;
    /* 3 字节小端长度 + 1 字节 sequence id（首包 0） */
    uint32_t payload_len = (uint32_t)((uint8_t)p[0]
                                       | ((uint8_t)p[1] << 8)
                                       | ((uint8_t)p[2] << 16));
    CuAssertTrue(tc, payload_len + 4 == (uint32_t)size);
    /* sequence id 写在包头第 4 字节 */
    CuAssertTrue(tc, 0 == (uint8_t)p[3]);
    /* COM_QUERY = 0x03 */
    CuAssertTrue(tc, 0x03 == (uint8_t)p[4]);

    FREE(pack);
}

/* =======================================================================
 * mysql_pack_ping / quit / selectdb —— 简单命令包格式
 * ======================================================================= */
static void test_mysql_pack_simple_cmds(CuTest *tc) {
    mysql_ctx mysql;
    ZERO(&mysql, sizeof(mysql));

    /* COM_PING = 0x0e */
    size_t size = 0;
    void *pack = mysql_pack_ping(&mysql, &size);
    CuAssertPtrNotNull(tc, pack);
    char *p = (char *)pack;
    CuAssertTrue(tc, 0x0e == (uint8_t)p[4]);
    FREE(pack);

    /* COM_QUIT = 0x01 */
    pack = mysql_pack_quit(&mysql, &size);
    CuAssertPtrNotNull(tc, pack);
    p = (char *)pack;
    CuAssertTrue(tc, 0x01 == (uint8_t)p[4]);
    FREE(pack);

    /* COM_INIT_DB = 0x02 */
    pack = mysql_pack_selectdb(&mysql, "mydb", &size);
    CuAssertPtrNotNull(tc, pack);
    p = (char *)pack;
    CuAssertTrue(tc, 0x02 == (uint8_t)p[4]);
    /* payload = COM_INIT_DB + "mydb" */
    CuAssertTrue(tc, 0 == memcmp(p + 5, "mydb", 4));
    FREE(pack);
}

/* =======================================================================
 * mysql_pack_stmt_prepare —— COM_STMT_PREPARE = 0x16
 * ======================================================================= */
static void test_mysql_pack_stmt_prepare(CuTest *tc) {
    mysql_ctx mysql;
    ZERO(&mysql, sizeof(mysql));

    const char *sql = "SELECT * FROM t WHERE id = ?";
    size_t size = 0;
    void *pack = mysql_pack_stmt_prepare(&mysql, sql, &size);
    CuAssertPtrNotNull(tc, pack);
    char *p = (char *)pack;
    /* COM_STMT_PREPARE = 0x16 */
    CuAssertTrue(tc, 0x16 == (uint8_t)p[4]);
    /* sql 跟在命令字节后 */
    CuAssertTrue(tc, 0 == memcmp(p + 5, sql, strlen(sql)));
    FREE(pack);
}

/* =======================================================================
 * mysql_pack_stmt_execute —— COM_STMT_EXECUTE = 0x17
 * 无参数情形：固定 14 字节（4 head + 1 cmd + 4 stmt_id + 1 flags + 4 iter）
 * 参数数量与 mbind->count 不一致 → 返回 NULL
 * ======================================================================= */
static void test_mysql_pack_stmt_execute(CuTest *tc) {
    mysql_ctx mysql;
    ZERO(&mysql, sizeof(mysql));

    /* 无参数 stmt */
    mysql_stmt_ctx stmt0;
    ZERO(&stmt0, sizeof(stmt0));
    stmt0.mysql = &mysql;
    stmt0.stmt_id = 0x12345678;
    stmt0.params_count = 0;

    size_t size = 0;
    void *pack = mysql_pack_stmt_execute(&stmt0, NULL, &size);
    CuAssertPtrNotNull(tc, pack);
    char *p = (char *)pack;
    CuAssertTrue(tc, 0x17 == (uint8_t)p[4]);                            /* COM_STMT_EXECUTE */
    CuAssertTrue(tc, 0x78 == (uint8_t)p[5]);                            /* stmt_id 小端：低字节 */
    CuAssertTrue(tc, 0x56 == (uint8_t)p[6]);
    CuAssertTrue(tc, 0x34 == (uint8_t)p[7]);
    CuAssertTrue(tc, 0x12 == (uint8_t)p[8]);
    CuAssertTrue(tc, 0x00 == (uint8_t)p[9]);                            /* flags */
    CuAssertTrue(tc, 0x01 == (uint8_t)p[10]);                           /* iteration_count 小端 */
    CuAssertTrue(tc, 0x00 == (uint8_t)p[11]);
    /* payload 长度 = 1 cmd + 4 stmt_id + 1 flags + 4 iter = 10 */
    uint32_t payload_len = (uint32_t)((uint8_t)p[0] | ((uint8_t)p[1] << 8) | ((uint8_t)p[2] << 16));
    CuAssertTrue(tc, 10 == payload_len);
    FREE(pack);

    /* 有参数但 mbind=NULL → 拒绝 */
    mysql_stmt_ctx stmt2;
    ZERO(&stmt2, sizeof(stmt2));
    stmt2.mysql = &mysql;
    stmt2.stmt_id = 1;
    stmt2.params_count = 2;
    pack = mysql_pack_stmt_execute(&stmt2, NULL, &size);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, 0 == size);

    /* mbind->count 与 params_count 不一致 → 拒绝 */
    mysql_bind_ctx mb;
    mysql_bind_init(&mb);
    mysql_bind_integer(&mb, "a", 1);   /* count=1 */
    pack = mysql_pack_stmt_execute(&stmt2, &mb, &size);  /* 期望 2 */
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, 0 == size);

    /* mbind->count 匹配 → 成功 */
    mysql_bind_integer(&mb, "b", 2);   /* count=2 */
    pack = mysql_pack_stmt_execute(&stmt2, &mb, &size);
    CuAssertPtrNotNull(tc, pack);
    p = (char *)pack;
    CuAssertTrue(tc, 0x17 == (uint8_t)p[4]);
    /* 包含参数 payload 应比无参情形大 */
    CuAssertTrue(tc, size > 14);
    FREE(pack);
    mysql_bind_free(&mb);
}

/* =======================================================================
 * mysql_pack_stmt_reset —— COM_STMT_RESET = 0x1a，固定 9 字节
 * ======================================================================= */
static void test_mysql_pack_stmt_reset(CuTest *tc) {
    mysql_ctx mysql;
    ZERO(&mysql, sizeof(mysql));

    mysql_stmt_ctx stmt;
    ZERO(&stmt, sizeof(stmt));
    stmt.mysql = &mysql;
    stmt.stmt_id = 0x0a0b0c0d;

    size_t size = 0;
    void *pack = mysql_pack_stmt_reset(&stmt, &size);
    CuAssertPtrNotNull(tc, pack);
    char *p = (char *)pack;
    /* 9 字节：3 长度 + 1 sequence + 1 cmd + 4 stmt_id */
    CuAssertTrue(tc, 9 == size);
    CuAssertTrue(tc, 5 == (uint8_t)p[0]);                       /* payload_len = 5 */
    CuAssertTrue(tc, 0 == (uint8_t)p[1]);
    CuAssertTrue(tc, 0 == (uint8_t)p[2]);
    CuAssertTrue(tc, 0 == (uint8_t)p[3]);                       /* sequence */
    CuAssertTrue(tc, 0x1a == (uint8_t)p[4]);                    /* COM_STMT_RESET */
    CuAssertTrue(tc, 0x0d == (uint8_t)p[5]);                    /* stmt_id 小端 */
    CuAssertTrue(tc, 0x0c == (uint8_t)p[6]);
    CuAssertTrue(tc, 0x0b == (uint8_t)p[7]);
    CuAssertTrue(tc, 0x0a == (uint8_t)p[8]);
    FREE(pack);
}

/* =======================================================================
 * mysql_pack_stmt_close —— COM_STMT_CLOSE = 0x19
 * 调用后 stmt 被 FREE，调用方不可再访问 stmt
 * ======================================================================= */
static void test_mysql_pack_stmt_close(CuTest *tc) {
    mysql_ctx mysql;
    ZERO(&mysql, sizeof(mysql));

    /* stmt 必须 CALLOC 分配（close 内部 FREE stmt） */
    mysql_stmt_ctx *stmt;
    CALLOC(stmt, 1, sizeof(*stmt));
    stmt->mysql = &mysql;
    stmt->stmt_id = 0x77665544;

    size_t size = 0;
    void *pack = mysql_pack_stmt_close(stmt, &size);
    /* stmt 至此已被释放，不可再访问 */
    CuAssertPtrNotNull(tc, pack);
    char *p = (char *)pack;
    CuAssertTrue(tc, 9 == size);
    CuAssertTrue(tc, 0x19 == (uint8_t)p[4]);                    /* COM_STMT_CLOSE */
    CuAssertTrue(tc, 0x44 == (uint8_t)p[5]);                    /* stmt_id 小端 */
    CuAssertTrue(tc, 0x55 == (uint8_t)p[6]);
    CuAssertTrue(tc, 0x66 == (uint8_t)p[7]);
    CuAssertTrue(tc, 0x77 == (uint8_t)p[8]);
    FREE(pack);
}

/* ======================================================================= */

void test_mysql_pack(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_mysql_lenenc);
    SUITE_ADD_TEST(suite, test_mysql_set_payload_lens);
    SUITE_ADD_TEST(suite, test_mysql_bind_basic);
    SUITE_ADD_TEST(suite, test_mysql_bind_temporal);
    SUITE_ADD_TEST(suite, test_mysql_pack_query_no_bind);
    SUITE_ADD_TEST(suite, test_mysql_pack_simple_cmds);
    SUITE_ADD_TEST(suite, test_mysql_pack_stmt_prepare);
    SUITE_ADD_TEST(suite, test_mysql_pack_stmt_execute);
    SUITE_ADD_TEST(suite, test_mysql_pack_stmt_reset);
    SUITE_ADD_TEST(suite, test_mysql_pack_stmt_close);
}
