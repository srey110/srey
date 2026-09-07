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

    CuAssertIntEquals(tc, ERR_OK, _mysql_set_payload_lens(&bw));

    /* 头 3 字节小端长度 = 7 */
    CuAssertTrue(tc, 7 == (uint8_t)bw.data[0]);
    CuAssertTrue(tc, 0 == (uint8_t)bw.data[1]);
    CuAssertTrue(tc, 0 == (uint8_t)bw.data[2]);
    /* 回填不得改动 offset：改了的话后续 _mysql_pack_finish 取到的包长就是错的 */
    CuAssertIntEquals(tc, 4 + 7, (int)bw.offset);

    binary_free(&bw);

    /* payload 恰好 16MB 必须拒绝：3 字节长度字段装不下，硬写下去是个截断的长度，
       服务端照它切包，整条连接从此错位。返回值原来没人接，这条守卫删掉也没人知道 */
    binary_init(&bw, NULL, 0, 0);
    binary_set_skip(&bw, MYSQL_HEAD_LENS + INT3_MAX);
    CuAssertIntEquals(tc, ERR_FAILED, _mysql_set_payload_lens(&bw));
    binary_free(&bw);

    /* 差一个字节则放行，且长度字段三个字节全满 */
    binary_init(&bw, NULL, 0, 0);
    binary_set_skip(&bw, MYSQL_HEAD_LENS + INT3_MAX - 1);
    CuAssertIntEquals(tc, ERR_OK, _mysql_set_payload_lens(&bw));
    CuAssertIntEquals(tc, 0xFE, (uint8_t)bw.data[0]);/* 0xFFFFFE 小端：低位在前 */
    CuAssertIntEquals(tc, 0xFF, (uint8_t)bw.data[1]);
    CuAssertIntEquals(tc, 0xFF, (uint8_t)bw.data[2]);
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

    /* TIME 的三种编码各测一次，逐字节比 value 段：
       只判 count 与 offset > 0 的话，正负号写错位、days 写成 2 字节都发现不了。
       datetime 的绝对值随本机时区变，不逐字节比，另测它的长度前缀 */
    mysql_bind_time(&mb, "t1", 0, 1, 12, 30, 45);/* +1d 12:30:45 → 8 字节包体 */
    CuAssertIntEquals(tc, 9, (int32_t)mb.value.offset);
    const uint8_t *v = (const uint8_t *)mb.value.data;
    CuAssertIntEquals(tc, 8, v[0]);/* 长度前缀 */
    CuAssertIntEquals(tc, 0, v[1]);/* is_negative */
    CuAssertIntEquals(tc, 1, v[2]);/* days 小端 4 字节 */
    CuAssertIntEquals(tc, 0, v[3] | v[4] | v[5]);
    CuAssertIntEquals(tc, 12, v[6]);
    CuAssertIntEquals(tc, 30, v[7]);
    CuAssertIntEquals(tc, 45, v[8]);

    size_t off = mb.value.offset;
    mysql_bind_time(&mb, "t2", 1, 2, 4, 5, 6);/* -2d 04:05:06 */
    v = (const uint8_t *)mb.value.data + off;
    CuAssertIntEquals(tc, 8, v[0]);
    CuAssertIntEquals(tc, 1, v[1]);/* is_negative 置位 */
    CuAssertIntEquals(tc, 2, v[2]);
    CuAssertIntEquals(tc, 4, v[6]);
    CuAssertIntEquals(tc, 5, v[7]);
    CuAssertIntEquals(tc, 6, v[8]);

    off = mb.value.offset;
    mysql_bind_time(&mb, "t0", 0, 0, 0, 0, 0);/* 全零 → 只写一个长度 0 */
    CuAssertIntEquals(tc, (int32_t)off + 1, (int32_t)mb.value.offset);
    CuAssertIntEquals(tc, 0, ((const uint8_t *)mb.value.data)[off]);

    off = mb.value.offset;
    mysql_bind_datetime(&mb, "dt", 1716000000);/* 2024-05-18 UTC；本机时区决定是 4 还是 7 字节 */
    v = (const uint8_t *)mb.value.data + off;
    CuAssertTrue(tc, 4 == v[0] || 7 == v[0]);
    CuAssertIntEquals(tc, (int32_t)off + 1 + v[0], (int32_t)mb.value.offset);

    CuAssertIntEquals(tc, 4, mb.count);
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

/* CLIENT_QUERY_ATTRIBUTES 置位后 COM_QUERY 在 SQL 之前多插一段：
   parameter_count + parameter_set_count(恒 1)，有参数时再跟位图 / bind_flag / 类型名 / 值。
   caps 为 0 时整段不写，而上一条用例的 mysql_ctx 是全零 —— 于是这条分支此前零覆盖，
   漏写 parameter_set_count 那句服务端会当场错位，本地组包却看不出来 */
static void test_mysql_pack_query_attrs(CuTest *tc) {
    mysql_ctx mysql;
    const char *sql = "SELECT 1";
    size_t sqllen = strlen(sql);
    size_t size = 0;
    const uint8_t *p;

    /* 无参数：只多出 parameter_count=0 与 parameter_set_count=1 两个 lenenc */
    ZERO(&mysql, sizeof(mysql));
    BIT_SET(mysql.client.caps, CLIENT_QUERY_ATTRIBUTES);
    void *pack = mysql_pack_query(&mysql, sql, NULL, &size);
    CuAssertPtrNotNull(tc, pack);
    p = (const uint8_t *)pack;
    CuAssertIntEquals(tc, 0x03, p[4]);/* COM_QUERY */
    CuAssertIntEquals(tc, 0x00, p[5]);/* parameter_count */
    CuAssertIntEquals(tc, 0x01, p[6]);/* parameter_set_count 恒 1 */
    CuAssertIntEquals(tc, (int)(4 + 3 + sqllen), (int)size);
    CuAssertTrue(tc, 0 == memcmp(p + 7, sql, sqllen));
    FREE(pack);

    /* 传了 mbind 但一个参数都没绑：count 仍是 0，后面那四段一律不写 */
    mysql_bind_ctx mb;
    mysql_bind_init(&mb);
    ZERO(&mysql, sizeof(mysql));
    BIT_SET(mysql.client.caps, CLIENT_QUERY_ATTRIBUTES);
    pack = mysql_pack_query(&mysql, sql, &mb, &size);
    CuAssertPtrNotNull(tc, pack);
    CuAssertIntEquals(tc, (int)(4 + 3 + sqllen), (int)size);
    FREE(pack);

    /* 一个整数参数：count=1、set_count=1、1 字节位图(全 0)、bind_flag=1，
       然后是 type_name 段与 value 段，SQL 永远在最末尾 */
    mysql_bind_integer(&mb, "a", 7);
    CuAssertIntEquals(tc, 1, mb.count);
    ZERO(&mysql, sizeof(mysql));
    BIT_SET(mysql.client.caps, CLIENT_QUERY_ATTRIBUTES);
    pack = mysql_pack_query(&mysql, sql, &mb, &size);
    CuAssertPtrNotNull(tc, pack);
    p = (const uint8_t *)pack;
    CuAssertIntEquals(tc, 0x01, p[5]);/* parameter_count */
    CuAssertIntEquals(tc, 0x01, p[6]);/* parameter_set_count */
    CuAssertIntEquals(tc, 0x00, p[7]);/* 位图：唯一那个参数非 NULL */
    CuAssertIntEquals(tc, 0x01, p[8]);/* new_params_bind_flag */
    CuAssertIntEquals(tc, (int)(4 + 1 + 1 + 1 + 1 + 1
                                + mb.type_name.offset + mb.value.offset + sqllen), (int)size);
    CuAssertTrue(tc, 0 == memcmp(p + size - sqllen, sql, sqllen));
    FREE(pack);
    mysql_bind_free(&mb);
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

    /* COM_QUIT = 0x01；组包不得触碰 id / cur_cmd —— Lua 侧 __gc 在工作线程调它，
       而网络线程正拿这两个字段解析来包，写一下就是无同步的跨线程写 */
    mysql.id = 7;
    mysql.cur_cmd = MYSQL_PING;
    pack = mysql_pack_quit(&size);
    CuAssertPtrNotNull(tc, pack);
    p = (char *)pack;
    CuAssertTrue(tc, 5 == size);
    CuAssertIntEquals(tc, 0, (int)(uint8_t)p[3]);
    CuAssertTrue(tc, 0x01 == (uint8_t)p[4]);
    CuAssertIntEquals(tc, 7, (int)mysql.id);
    CuAssertIntEquals(tc, MYSQL_PING, (int)mysql.cur_cmd);
    FREE(pack);

    /* COM_INIT_DB = 0x02 */
    pack = mysql_pack_selectdb(&mysql, "mydb", &size);
    CuAssertPtrNotNull(tc, pack);
    p = (char *)pack;
    CuAssertTrue(tc, 0x02 == (uint8_t)p[4]);
    /* payload = COM_INIT_DB + "mydb" */
    CuAssertTrue(tc, 0 == memcmp(p + 5, "mydb", 4));
    FREE(pack);

    char longdb[80];
    memset(longdb, 'a', sizeof(longdb));
    longdb[64] = '\0';
    size = 1;
    pack = mysql_pack_selectdb(&mysql, longdb, &size);
    CuAssert(tc, "64-byte db name must be rejected (pending_db holds only 63)", NULL == pack);
    CuAssert(tc, "rejected selectdb must set *size to 0", 0 == size);
    CuAssert(tc, "rejected selectdb must not touch pending_db", 0 == strcmp(mysql.pending_db, "mydb"));

    longdb[63] = '\0';
    pack = mysql_pack_selectdb(&mysql, longdb, &size);
    CuAssertPtrNotNull(tc, pack);
    p = (char *)pack;
    CuAssert(tc, "63-byte db name must still be packed", 0 == memcmp(p + 5, longdb, 63));
    CuAssert(tc, "successful pack must record db name into pending_db for commit on OK", 0 == strcmp(mysql.pending_db, longdb));
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
 * 参数数量与 mbind->count 不一致 → 返回 NULL（含 params_count=0 却带了绑定）
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
    CuAssertTrue(tc, 0x17 == (uint8_t)p[4]);/* COM_STMT_EXECUTE */
    CuAssertTrue(tc, 0x78 == (uint8_t)p[5]);/* stmt_id 小端：低字节 */
    CuAssertTrue(tc, 0x56 == (uint8_t)p[6]);
    CuAssertTrue(tc, 0x34 == (uint8_t)p[7]);
    CuAssertTrue(tc, 0x12 == (uint8_t)p[8]);
    CuAssertTrue(tc, 0x00 == (uint8_t)p[9]);/* flags */
    CuAssertTrue(tc, 0x01 == (uint8_t)p[10]);/* iteration_count 小端 */
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
    mysql_bind_integer(&mb, "a", 1);/* count=1 */
    pack = mysql_pack_stmt_execute(&stmt2, &mb, &size);/* 期望 2 */
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, 0 == size);

    /* mbind->count 匹配 → 成功 */
    mysql_bind_integer(&mb, "b", 2);/* count=2 */
    pack = mysql_pack_stmt_execute(&stmt2, &mb, &size);
    CuAssertPtrNotNull(tc, pack);
    p = (char *)pack;
    CuAssertTrue(tc, 0x17 == (uint8_t)p[4]);
    /* 包含参数 payload 应比无参情形大 */
    CuAssertTrue(tc, size > 14);
    FREE(pack);

    /* 声明 0 个参数却带了绑定 → 拒绝。以前守卫整条以 params_count>0 为前提，
       这一路直接漏过去，绑定被静默丢弃而服务端只看到一条无参 EXECUTE */
    pack = mysql_pack_stmt_execute(&stmt0, &mb, &size);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, 0 == size);
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
    CuAssertTrue(tc, 5 == (uint8_t)p[0]);/* payload_len = 5 */
    CuAssertTrue(tc, 0 == (uint8_t)p[1]);
    CuAssertTrue(tc, 0 == (uint8_t)p[2]);
    CuAssertTrue(tc, 0 == (uint8_t)p[3]);/* sequence */
    CuAssertTrue(tc, 0x1a == (uint8_t)p[4]);/* COM_STMT_RESET */
    CuAssertTrue(tc, 0x0d == (uint8_t)p[5]);/* stmt_id 小端 */
    CuAssertTrue(tc, 0x0c == (uint8_t)p[6]);
    CuAssertTrue(tc, 0x0b == (uint8_t)p[7]);
    CuAssertTrue(tc, 0x0a == (uint8_t)p[8]);
    FREE(pack);
}

/* =======================================================================
 * mysql_pack_stmt_close —— COM_STMT_CLOSE = 0x19
 * 只组包，stmt 仍然有效，由调用方另行 mysql_stmt_free（契约见 mysql_pack.h）
 * ======================================================================= */
static void test_mysql_pack_stmt_close(CuTest *tc) {
    mysql_ctx mysql;
    ZERO(&mysql, sizeof(mysql));

    /* pack_stmt_close 只组包，stmt 由本用例自己释放 */
    mysql_stmt_ctx *stmt;
    CALLOC(stmt, 1, sizeof(*stmt));
    stmt->mysql = &mysql;
    stmt->stmt_id = 0x77665544;

    size_t size = 0;
    void *pack = mysql_pack_stmt_close(stmt, &size);
    CuAssertPtrNotNull(tc, pack);
    /* 只组包不销毁：stmt 及其字段仍然可读 */
    CuAssertTrue(tc, 0x77665544 == stmt->stmt_id);
    CuAssertTrue(tc, &mysql == stmt->mysql);
    char *p = (char *)pack;
    CuAssertTrue(tc, 9 == size);
    CuAssertTrue(tc, 0x19 == (uint8_t)p[4]);/* COM_STMT_CLOSE */
    CuAssertTrue(tc, 0x44 == (uint8_t)p[5]);/* stmt_id 小端 */
    CuAssertTrue(tc, 0x55 == (uint8_t)p[6]);
    CuAssertTrue(tc, 0x66 == (uint8_t)p[7]);
    CuAssertTrue(tc, 0x77 == (uint8_t)p[8]);
    FREE(pack);
    mysql_stmt_free(stmt);
}

/* =======================================================================
 * mysql_bind_free —— 可重复调用；释放后再绑定等同刚 init 的空上下文
 * Lua 侧 __gc 与 bind:free 同一入口，脚本能显式调，故这两条必须成立
 * ======================================================================= */
static void test_mysql_bind_free_reuse(CuTest *tc) {
    mysql_bind_ctx mb;
    mysql_bind_init(&mb);
    mysql_bind_string(&mb, "s1", "hello", 5);
    mysql_bind_integer(&mb, "i1", 7);
    CuAssertIntEquals(tc, 2, mb.count);

    mysql_bind_free(&mb);
    /* 四个缓冲全部回到"未分配"状态，count 归零 */
    CuAssertIntEquals(tc, 0, mb.count);
    CuAssertPtrEquals(tc, NULL, mb.bitmap.data);
    CuAssertPtrEquals(tc, NULL, mb.type.data);
    CuAssertPtrEquals(tc, NULL, mb.type_name.data);
    CuAssertPtrEquals(tc, NULL, mb.value.data);
    CuAssertTrue(tc, 0 == mb.bitmap.size && 0 == mb.bitmap.offset);
    CuAssertTrue(tc, 0 == mb.value.size && 0 == mb.value.offset);

    /* 重复 free 不炸 */
    mysql_bind_free(&mb);
    CuAssertPtrEquals(tc, NULL, mb.value.data);

    /* free 后再绑定：旧实现留着 size=256/offset，_binary_expand 会认为"还写得下"
       从而跳过 REALLOC，data 仍是 NULL，_mysql_bind_bitmap 的 memset 直接段错误 */
    mysql_bind_string(&mb, "s2", "world", 5);
    CuAssertIntEquals(tc, 1, mb.count);
    CuAssertPtrNotNull(tc, mb.bitmap.data);
    CuAssertPtrNotNull(tc, mb.value.data);
    CuAssertTrue(tc, mb.value.offset > 0);

    mysql_bind_free(&mb);
}

/* bind 四个缓冲的线格式逐字节验证。以前只断言过 count 与 offset > 0，于是 NULL 位图位序、
 * 无符号标志位、整数自动选宽、time 双布局全都没人钉；而唯一另外的调用方 task_mysql
 * 在 main.c 的 optional 白名单里，断言失败会被吞成 "- (network error)"。
 * 类型码用枚举符号而不写数字：这里要钉的是布局与字节序，不是枚举取值 */
static void test_mysql_bind_wire(CuTest *tc) {
    mysql_bind_ctx mb;
    const uint8_t *p;
    mysql_bind_init(&mb);

    /* NULL 位图：每 8 个参数共用一字节，第 i 个占 bit (i % 8)。
       位序写反成 (1 << (7 - index)) 时这两个字节会变成 0x51 / 0x80 */
    mysql_bind_integer(&mb, "a0", 1);
    mysql_bind_nil(&mb, "a1");
    mysql_bind_integer(&mb, "a2", 2);
    mysql_bind_nil(&mb, "a3");
    mysql_bind_integer(&mb, "a4", 3);
    mysql_bind_integer(&mb, "a5", 4);
    mysql_bind_integer(&mb, "a6", 5);
    mysql_bind_nil(&mb, "a7");
    mysql_bind_nil(&mb, "a8");
    CuAssertIntEquals(tc, 9, mb.count);
    CuAssertTrue(tc, 2 == mb.bitmap.offset);
    p = (const uint8_t *)mb.bitmap.data;
    CuAssertIntEquals(tc, 0x8A, p[0]);/* bit1|bit3|bit7 */
    CuAssertIntEquals(tc, 0x01, p[1]);/* 第 9 个参数落第二字节的 bit0 */

    /* 无符号标志在第 15 位。去掉 | 0x8000 之后服务端会把大 uint64 按有符号解释 */
    mysql_bind_clear(&mb);
    mysql_bind_uinteger(&mb, "u", 0xFFFFFFFFFFFFFFFFULL);
    CuAssertTrue(tc, 2 == mb.type.offset);
    p = (const uint8_t *)mb.type.data;
    CuAssertIntEquals(tc, (uint8_t)MYSQL_TYPE_LONGLONG, p[0]);/* 小端低字节 = 类型码 */
    CuAssertIntEquals(tc, 0x80, p[1]);
    /* 有符号那支同一个类型码，但高字节必须是 0 */
    mysql_bind_clear(&mb);
    mysql_bind_integer(&mb, "i", -4294967296LL);
    p = (const uint8_t *)mb.type.data;
    CuAssertIntEquals(tc, (uint8_t)MYSQL_TYPE_LONGLONG, p[0]);
    CuAssertIntEquals(tc, 0x00, p[1]);

    /* 整数按值域自动选最小类型，value 缓冲宽度随之变化 */
    mysql_bind_clear(&mb);
    mysql_bind_integer(&mb, NULL, 127);
    CuAssertIntEquals(tc, (uint8_t)MYSQL_TYPE_TINY, ((const uint8_t *)mb.type.data)[0]);
    CuAssertTrue(tc, 1 == mb.value.offset);
    mysql_bind_clear(&mb);
    mysql_bind_integer(&mb, NULL, 128);
    CuAssertIntEquals(tc, (uint8_t)MYSQL_TYPE_SHORT, ((const uint8_t *)mb.type.data)[0]);
    CuAssertTrue(tc, 2 == mb.value.offset);
    mysql_bind_clear(&mb);
    mysql_bind_integer(&mb, NULL, 0x12345678);
    CuAssertIntEquals(tc, (uint8_t)MYSQL_TYPE_LONG, ((const uint8_t *)mb.type.data)[0]);
    CuAssertTrue(tc, 4 == mb.value.offset);
    p = (const uint8_t *)mb.value.data;
    CuAssertIntEquals(tc, 0x78, p[0]);/* 值也是小端 */
    CuAssertIntEquals(tc, 0x56, p[1]);
    CuAssertIntEquals(tc, 0x34, p[2]);
    CuAssertIntEquals(tc, 0x12, p[3]);

    /* time 双布局：全零只写一个长度字节 0，否则 1 + 8 */
    mysql_bind_clear(&mb);
    mysql_bind_time(&mb, NULL, 0, 0, 0, 0, 0);
    CuAssertTrue(tc, 1 == mb.value.offset);
    CuAssertIntEquals(tc, 0, ((const uint8_t *)mb.value.data)[0]);
    mysql_bind_clear(&mb);
    mysql_bind_time(&mb, NULL, 1, 2, 3, 4, 5);
    CuAssertTrue(tc, 9 == mb.value.offset);
    p = (const uint8_t *)mb.value.data;
    CuAssertIntEquals(tc, 8, p[0]);
    CuAssertIntEquals(tc, 1, p[1]);/* is_negative */
    CuAssertIntEquals(tc, 2, p[2]);/* days 小端 4 字节 */
    CuAssertIntEquals(tc, 0, p[3]);
    CuAssertIntEquals(tc, 0, p[4]);
    CuAssertIntEquals(tc, 0, p[5]);
    CuAssertIntEquals(tc, 3, p[6]);
    CuAssertIntEquals(tc, 4, p[7]);
    CuAssertIntEquals(tc, 5, p[8]);

    /* datetime 走 4 字节还是 7 字节取决于本地时区是否恰好午夜，故只钉"长度字节与体长自洽" */
    mysql_bind_clear(&mb);
    mysql_bind_datetime(&mb, NULL, 1716000000);
    p = (const uint8_t *)mb.value.data;
    CuAssertTrue(tc, 4 == p[0] || 7 == p[0]);
    CuAssertTrue(tc, (size_t)p[0] + 1 == mb.value.offset);

    /* type_name 缓冲 = 同样 2 字节类型 + lenenc(名字长) + 名字本体 */
    mysql_bind_clear(&mb);
    mysql_bind_integer(&mb, "ab", 1);
    CuAssertTrue(tc, 5 == mb.type_name.offset);
    p = (const uint8_t *)mb.type_name.data;
    CuAssertIntEquals(tc, (uint8_t)MYSQL_TYPE_TINY, p[0]);
    CuAssertIntEquals(tc, 0x00, p[1]);
    CuAssertIntEquals(tc, 2, p[2]);/* lenenc 短档直接写长度 */
    CuAssertIntEquals(tc, 'a', p[3]);
    CuAssertIntEquals(tc, 'b', p[4]);
    /* 匿名参数写零长名字 */
    mysql_bind_clear(&mb);
    mysql_bind_integer(&mb, NULL, 1);
    CuAssertTrue(tc, 3 == mb.type_name.offset);
    CuAssertIntEquals(tc, 0, ((const uint8_t *)mb.type_name.data)[2]);

    mysql_bind_free(&mb);
}

/* ======================================================================= */

void test_mysql_pack(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_mysql_bind_wire);
    SUITE_ADD_TEST(suite, test_mysql_lenenc);
    SUITE_ADD_TEST(suite, test_mysql_set_payload_lens);
    SUITE_ADD_TEST(suite, test_mysql_bind_basic);
    SUITE_ADD_TEST(suite, test_mysql_bind_temporal);
    SUITE_ADD_TEST(suite, test_mysql_bind_free_reuse);
    SUITE_ADD_TEST(suite, test_mysql_pack_query_no_bind);
    SUITE_ADD_TEST(suite, test_mysql_pack_query_attrs);
    SUITE_ADD_TEST(suite, test_mysql_pack_simple_cmds);
    SUITE_ADD_TEST(suite, test_mysql_pack_stmt_prepare);
    SUITE_ADD_TEST(suite, test_mysql_pack_stmt_execute);
    SUITE_ADD_TEST(suite, test_mysql_pack_stmt_reset);
    SUITE_ADD_TEST(suite, test_mysql_pack_stmt_close);
}
