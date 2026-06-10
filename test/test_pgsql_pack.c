#include "test_pgsql_pack.h"
#include "lib.h"
#include "protocol/pgsql/pgsql_pack.h"
#include "protocol/pgsql/pgsql_bind.h"

/* 大端读 32 位整数（pgsql 消息长度字段统一大端格式） */
static uint32_t _rd_be32(const char *p) {
    return ((uint32_t)(uint8_t)p[0] << 24)
         | ((uint32_t)(uint8_t)p[1] << 16)
         | ((uint32_t)(uint8_t)p[2] << 8)
         |  (uint32_t)(uint8_t)p[3];
}

/* =======================================================================
 * pgsql_pack_query —— 简单查询 'Q'
 * ======================================================================= */
static void test_pgsql_query(CuTest *tc) {
    const char *sql = "SELECT 1";
    size_t size = 0;
    char *pack = pgsql_pack_query(sql, &size);
    CuAssertPtrNotNull(tc, pack);

    /* 'Q' + len(4) + sql + '\0' */
    CuAssertTrue(tc, 'Q' == pack[0]);
    /* length 字段含 4 字节本身但不含类型字节 */
    uint32_t mlen = _rd_be32(pack + 1);
    CuAssertTrue(tc, mlen == 4 + strlen(sql) + 1);
    /* size = 1 (type) + 4 (len) + payload */
    CuAssertTrue(tc, size == 1 + mlen);
    CuAssertTrue(tc, 0 == memcmp(pack + 5, sql, strlen(sql)));
    CuAssertTrue(tc, 0 == pack[size - 1]);

    FREE(pack);
}

/* =======================================================================
 * pgsql_pack_terminate —— 'X' + 4 字节长度（=4）
 * ======================================================================= */
static void test_pgsql_terminate(CuTest *tc) {
    size_t size = 0;
    char *pack = pgsql_pack_terminate(&size);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, 5 == (int)size);
    CuAssertTrue(tc, 'X' == pack[0]);
    CuAssertTrue(tc, 4 == _rd_be32(pack + 1));
    FREE(pack);
}

/* =======================================================================
 * pgsql_pack_copy_data / copy_done / copy_fail
 * ======================================================================= */
static void test_pgsql_copy(CuTest *tc) {
    /* copy_data ：'d' + len(4) + data */
    const char *data = "abc";
    size_t size = 0;
    char *pack = pgsql_pack_copy_data(data, 3, &size);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, 'd' == pack[0]);
    CuAssertTrue(tc, 4 + 3 == (int)_rd_be32(pack + 1));
    CuAssertTrue(tc, 0 == memcmp(pack + 5, data, 3));
    FREE(pack);

    /* copy_done ：'c' + len(4)=4 */
    pack = pgsql_pack_copy_done(&size);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, 5 == (int)size);
    CuAssertTrue(tc, 'c' == pack[0]);
    CuAssertTrue(tc, 4 == _rd_be32(pack + 1));
    FREE(pack);

    /* copy_fail ：'f' + len + msg + \0 */
    pack = pgsql_pack_copy_fail("bad data", &size);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, 'f' == pack[0]);
    uint32_t mlen = _rd_be32(pack + 1);
    CuAssertTrue(tc, mlen == 4 + strlen("bad data") + 1);
    CuAssertTrue(tc, 0 == memcmp(pack + 5, "bad data", 8));
    FREE(pack);
}

/* =======================================================================
 * pgsql_pack_cancel —— 16 字节 CancelRequest（无类型码）
 * ======================================================================= */
static void test_pgsql_cancel(CuTest *tc) {
    char buf[16];
    pgsql_pack_cancel(buf, 0x11223344, 0x55667788u);
    /* length = 16 */
    CuAssertTrue(tc, 16 == (int)_rd_be32(buf));
    /* protocol version (取消请求魔术值) = 80877102 = 1234<<16 | 5678 */
    CuAssertTrue(tc, 80877102 == (int)_rd_be32(buf + 4));
    CuAssertTrue(tc, 0x11223344u == _rd_be32(buf + 8));
    CuAssertTrue(tc, 0x55667788u == _rd_be32(buf + 12));
}

/* =======================================================================
 * pgsql_pack_stmt_prepare —— 'P' + Sync('S')
 * ======================================================================= */
static void test_pgsql_stmt_prepare(CuTest *tc) {
    uint32_t oids[2] = { INT4OID, TEXTOID };
    size_t size = 0;
    char *pack = pgsql_pack_stmt_prepare("stmt1", "SELECT $1 + 1", 2, oids, &size);
    CuAssertPtrNotNull(tc, pack);
    /* 至少包含 P 消息和末尾 S 消息 */
    CuAssertTrue(tc, 'P' == pack[0]);
    /* 末尾应是 'S' + 长度 4（Sync）*/
    CuAssertTrue(tc, 'S' == pack[size - 5]);
    CuAssertTrue(tc, 4 == (int)_rd_be32(pack + size - 4));
    FREE(pack);
}

/* =======================================================================
 * pgsql_pack_stmt_close —— 'C' + Sync('S')
 * ======================================================================= */
static void test_pgsql_stmt_close(CuTest *tc) {
    size_t size = 0;
    char *pack = pgsql_pack_stmt_close("stmt1", &size);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, 'C' == pack[0]);
    /* 末尾 5 字节是 Sync */
    CuAssertTrue(tc, 'S' == pack[size - 5]);
    FREE(pack);
}

/* =======================================================================
 * pgsql_pack_stmt_execute —— 含 Bind/Describe/Execute/Sync
 * ======================================================================= */
static void test_pgsql_stmt_execute(CuTest *tc) {
    pgsql_bind_ctx bind;
    pgsql_bind_init(&bind, 2);
    pgsql_bind_int32(&bind, 42);
    pgsql_bind_text(&bind, "hello", 5);

    size_t size = 0;
    char *pack = pgsql_pack_stmt_execute("stmt1", &bind, FORMAT_BINARY, &size);
    CuAssertPtrNotNull(tc, pack);
    /* 首字节为 'B' (Bind) */
    CuAssertTrue(tc, 'B' == pack[0]);
    /* 末尾 5 字节是 Sync */
    CuAssertTrue(tc, 'S' == pack[size - 5]);
    FREE(pack);

    pgsql_bind_free(&bind);
}

/* =======================================================================
 * pgsql_bind_* —— 绑定接口写入计数与格式
 * ======================================================================= */
static void test_pgsql_bind_basic(CuTest *tc) {
    pgsql_bind_ctx bind;
    pgsql_bind_init(&bind, 8);
    CuAssertIntEquals(tc, 8, bind.nparam);

    /* bool / int16 / int32 / int64 */
    pgsql_bind_bool(&bind, 1);
    pgsql_bind_int16(&bind, 12345);
    pgsql_bind_int32(&bind, -98765);
    pgsql_bind_int64(&bind, 0x1122334455667788LL);
    /* float / double / null / text */
    pgsql_bind_float(&bind, 3.14f);
    pgsql_bind_double(&bind, 2.71828);
    pgsql_bind_null(&bind);
    pgsql_bind_text(&bind, "abc", 3);

    /* init 时已写入 2 字节 nparam 头，再 8 个 int16 格式码 = 2 + 16 = 18 字节 */
    CuAssertTrue(tc, 18 == (int)bind.format.offset);
    /* values 缓冲含 nparam 头 + 各参数（长度 4 字节 + 数据）*/
    CuAssertTrue(tc, bind.values.offset > 2);

    /* clear 回退到 nparam 头之后（offset=2）保留头部 */
    pgsql_bind_clear(&bind);
    CuAssertTrue(tc, 2 == bind.format.offset);
    CuAssertTrue(tc, 2 == bind.values.offset);

    pgsql_bind_free(&bind);
}

static void test_pgsql_bind_extra_types(CuTest *tc) {
    pgsql_bind_ctx bind;
    pgsql_bind_init(&bind, 5);

    /* bytea / timestamp / timestamptz / date / uuid */
    pgsql_bind_bytea(&bind, "\x01\x02\x03\x04", 4);
    pgsql_bind_timestamp(&bind, 1234567LL);
    pgsql_bind_timestamptz(&bind, 7654321LL);
    pgsql_bind_date(&bind, 1000);
    char uuid[16] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10
    };
    pgsql_bind_uuid(&bind, uuid);

    /* init 时 2 字节 nparam，5 个 int16 格式码 = 2 + 10 = 12 字节 */
    CuAssertTrue(tc, 12 == (int)bind.format.offset);
    CuAssertTrue(tc, bind.values.offset > 2);

    pgsql_bind_free(&bind);
}

/* =======================================================================
 * pgsql_pack_start / append_start / end —— 子消息封装辅助
 * ======================================================================= */
static void test_pgsql_pack_helpers(CuTest *tc) {
    binary_ctx bw;

    /* 主消息：'Q' + body */
    pgsql_pack_start(&bw, 'Q');
    /* 写入一段固定 body */
    binary_set_string(&bw, "BODY", 4);
    pgsql_pack_end(&bw);

    /* offset 0 是类型字节 */
    CuAssertTrue(tc, 'Q' == bw.data[0]);
    /* offset 1 起 4 字节大端长度，应等于 4(length) + 4(BODY) = 8 */
    uint32_t mlen = _rd_be32(bw.data + 1);
    CuAssertTrue(tc, 8 == (int)mlen);
    CuAssertTrue(tc, 0 == memcmp(bw.data + 5, "BODY", 4));
    binary_free(&bw);

    /* 子消息追加 */
    pgsql_pack_start(&bw, 'A');
    binary_set_string(&bw, "AA", 2);
    pgsql_pack_end(&bw);

    size_t off = pgsql_pack_append_start(&bw, 'B');
    binary_set_string(&bw, "BBB", 3);
    pgsql_pack_append_end(&bw, off);

    /* 第一段 'A' + len(4)=6 + AA */
    CuAssertTrue(tc, 'A' == bw.data[0]);
    CuAssertTrue(tc, 6 == (int)_rd_be32(bw.data + 1));
    /* 第二段 'B' + len(4)=7 + BBB */
    CuAssertTrue(tc, 'B' == bw.data[7]);
    CuAssertTrue(tc, 7 == (int)_rd_be32(bw.data + 8));

    binary_free(&bw);
}

/* =======================================================================
 * 测试套件注册
 * ======================================================================= */
void test_pgsql_pack(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_pgsql_query);
    SUITE_ADD_TEST(suite, test_pgsql_terminate);
    SUITE_ADD_TEST(suite, test_pgsql_copy);
    SUITE_ADD_TEST(suite, test_pgsql_cancel);
    SUITE_ADD_TEST(suite, test_pgsql_stmt_prepare);
    SUITE_ADD_TEST(suite, test_pgsql_stmt_close);
    SUITE_ADD_TEST(suite, test_pgsql_stmt_execute);
    SUITE_ADD_TEST(suite, test_pgsql_bind_basic);
    SUITE_ADD_TEST(suite, test_pgsql_bind_extra_types);
    SUITE_ADD_TEST(suite, test_pgsql_pack_helpers);
}
