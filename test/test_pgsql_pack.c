#include "test_pgsql_pack.h"
#include "lib.h"
#include "protocol/pgsql/pgsql_pack.h"
#include "protocol/pgsql/pgsql_bind.h"

/* 大端读 16 位整数（参数/格式码计数字段） */
static uint16_t _rd_be16(const char *p) {
    return (uint16_t)(((uint32_t)(uint8_t)p[0] << 8) | (uint32_t)(uint8_t)p[1]);
}
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
    const char *sql = "SELECT $1 + 1";
    char *pack = pgsql_pack_stmt_prepare("stmt1", sql, 2, oids, &size);
    CuAssertPtrNotNull(tc, pack);
    /* Parse：'P' Int32(len) String(name) String(sql) Int16(nparam) Int32(oid)... */
    CuAssertTrue(tc, 'P' == pack[0]);
    /* 参数类型块也要验：只判首尾字节的话，oids 漏写或写成小端都发现不了 */
    size_t off = 5 + strlen("stmt1") + 1 + strlen(sql) + 1;
    CuAssertIntEquals(tc, 2, _rd_be16(pack + off));
    CuAssertIntEquals(tc, INT4OID, (int32_t)_rd_be32(pack + off + 2));
    CuAssertIntEquals(tc, TEXTOID, (int32_t)_rd_be32(pack + off + 6));
    /* 末尾应是 'S' + 长度 4（Sync）*/
    CuAssertTrue(tc, 'S' == pack[size - 5]);
    CuAssertTrue(tc, 4 == (int)_rd_be32(pack + size - 4));
    FREE(pack);

    /* nparam <= 0 走 else：仍要写出一个为 0 的 Int16，否则服务端按下一段解会整体错位 */
    size = 0;
    pack = pgsql_pack_stmt_prepare("stmt0", sql, 0, NULL, &size);
    CuAssertPtrNotNull(tc, pack);
    off = 5 + strlen("stmt0") + 1 + strlen(sql) + 1;
    CuAssertIntEquals(tc, 0, _rd_be16(pack + off));
    CuAssertIntEquals(tc, (int32_t)off + 2 + 5, (int32_t)size);/* P 段 + Sync(5) */
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

/* 回归：name 为 NULL(匿名语句)时 String 字段仍须写一个 NUL。
   binary_set_string 对 NULL 一个字节都不写，缺了终止符后端读不到 String 边界，
   Close 只剩 1 字节消息体、Bind 之后所有 Int16/Int32 字段整体错位一字节 */
static void test_pgsql_null_name(CuTest *tc) {
    /* Close：'C' + len(4) + 'S' + name'\0' → 消息体 4+1+1=6，总长 7 + Sync 5 */
    size_t size = 0;
    char *pack = pgsql_pack_stmt_close(NULL, &size);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, 'C' == pack[0]);
    CuAssertIntEquals(tc, 6, (int)_rd_be32(pack + 1));
    CuAssertTrue(tc, 'S' == pack[5]);
    CuAssert(tc, "NULL name must still emit its terminating NUL", 0 == pack[6]);
    CuAssertTrue(tc, 'S' == pack[size - 5]);

    /* 与空串等价：两者应逐字节相同。比完再释放——FREE 会把指针置空 */
    size_t esize = 0;
    char *epack = pgsql_pack_stmt_close("", &esize);
    CuAssertPtrNotNull(tc, epack);
    CuAssertIntEquals(tc, (int)size, (int)esize);
    CuAssertTrue(tc, 0 == memcmp(pack, epack, size));
    FREE(epack);
    FREE(pack);

    /* Bind：'B' + len + portal'\0' + name'\0' + ... 两个 String 各占 1 字节 */
    size = 0;
    pack = pgsql_pack_stmt_execute(NULL, NULL, FORMAT_TEXT, &size);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, 'B' == pack[0]);
    CuAssert(tc, "empty portal name keeps its NUL", 0 == pack[5]);
    CuAssert(tc, "NULL statement name keeps its NUL", 0 == pack[6]);
    FREE(pack);

    /* Query / CopyFail 的 String 同理 */
    size = 0;
    pack = pgsql_pack_query(NULL, &size);
    CuAssertPtrNotNull(tc, pack);
    CuAssertIntEquals(tc, 5, (int)_rd_be32(pack + 1));
    CuAssertTrue(tc, 0 == pack[5]);
    FREE(pack);
    size = 0;
    pack = pgsql_pack_copy_fail(NULL, &size);
    CuAssertPtrNotNull(tc, pack);
    CuAssertIntEquals(tc, 5, (int)_rd_be32(pack + 1));
    CuAssertTrue(tc, 0 == pack[5]);
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

/* 实际绑定个数与 pgsql_bind_init 声明的 nparam 不符：组包侧直接拒绝。
   format/values 两个头部的计数在 init 时就按 nparam 写死了，个数不符时服务端会把多出来的
   格式码当成"参数值数量"、把值长度字段当成值，从计数字段起整条 Bind 错位成另一条语义无关的
   报文，既不报协议错也发不出去正确的查询 */
static void test_pgsql_stmt_execute_bind_mismatch(CuTest *tc) {
    pgsql_bind_ctx bind;
    size_t few_size = 1;
    size_t ok_size = 0;
    size_t many_size = 1;
    char *few;
    char *ok;
    char *many;

    pgsql_bind_init(&bind, 2);
    pgsql_bind_int32(&bind, 1);
    few = pgsql_pack_stmt_execute("stmt1", &bind, FORMAT_BINARY, &few_size);/* 少绑一个 */
    pgsql_bind_int32(&bind, 2);
    ok = pgsql_pack_stmt_execute("stmt1", &bind, FORMAT_BINARY, &ok_size);/* 正好 */
    pgsql_bind_int32(&bind, 3);
    many = pgsql_pack_stmt_execute("stmt1", &bind, FORMAT_BINARY, &many_size);/* 多绑一个 */
    pgsql_bind_free(&bind);
    /* 守卫一旦回归，few/many 就是活缓冲，三块都得收；判定先攒进局部量，
       FREE 会把指针置 NULL，收完再断言指针就永远成立了 */
    int32_t few_null = (NULL == few);
    int32_t many_null = (NULL == many);
    FREE(few);
    FREE(ok);
    FREE(many);

    /* 断言排在收拾之后：CuAssert 走 longjmp，夹在 alloc/free 中间会漏释放并报出假泄漏 */
    CuAssertTrue(tc, 0 != few_null);
    CuAssertTrue(tc, 0 == few_size);
    CuAssertTrue(tc, 0 != many_null);
    CuAssertTrue(tc, 0 == many_size);
    CuAssertTrue(tc, ok_size > 0);
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
    binary_set_binary(&bw, "BODY", 4);
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
    binary_set_binary(&bw, "AA", 2);
    pgsql_pack_end(&bw);

    size_t off = pgsql_pack_append_start(&bw, 'B');
    binary_set_binary(&bw, "BBB", 3);
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
 * pgsql_bind_free —— 可重复调用；释放后 nparam 归零，后续绑定被本文件既有的
 * "0 == nparam 即早退"统一挡住，不会拿着旧 size/offset 往空指针上写
 * ======================================================================= */
static void test_pgsql_bind_free_reuse(CuTest *tc) {
    pgsql_bind_ctx bind;
    pgsql_bind_init(&bind, 2);
    pgsql_bind_int32(&bind, 42);
    pgsql_bind_text(&bind, "hello", 5);
    CuAssertTrue(tc, bind.values.offset > 2);

    pgsql_bind_free(&bind);
    CuAssertIntEquals(tc, 0, bind.nparam);
    CuAssertPtrEquals(tc, NULL, bind.format.data);
    CuAssertPtrEquals(tc, NULL, bind.values.data);

    /* 重复 free 不炸 */
    pgsql_bind_free(&bind);
    CuAssertPtrEquals(tc, NULL, bind.values.data);

    /* free 后再绑定全部静默无视，缓冲不会被重新写出来 */
    pgsql_bind_int32(&bind, 7);
    pgsql_bind_null(&bind);
    pgsql_bind_text(&bind, "x", 1);
    CuAssertPtrEquals(tc, NULL, bind.format.data);
    CuAssertPtrEquals(tc, NULL, bind.values.data);

    /* 组包侧同样按 nparam 早退，不会发出半截 Bind 消息。
       Bind 布局：'B' Int32(len) String(portal="") String(stmt) Int16(格式码数)
       Int16(参数值数) Int16(结果格式码数=1) Int16(结果格式码)。
       只判 pack[0]=='B' 的话，两个零计数漏写、Describe/Execute/Sync 整段不发都看不出来 */
    size_t size = 0;
    char *pack = pgsql_pack_stmt_execute("stmt1", &bind, FORMAT_BINARY, &size);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, 'B' == pack[0]);
    /* Bind 自报长度含自身、不含类型字节，且必须落在总包内 */
    uint32_t blen = _rd_be32(pack + 1);
    CuAssertTrue(tc, blen + 1 <= size);
    CuAssertIntEquals(tc, '\0', pack[5]);/* portal 为空串 */
    CuAssertTrue(tc, 0 == memcmp(pack + 6, "stmt1", 6));/* 含结尾 NUL */
    /* nparam=0 时两个计数都得写出来 */
    CuAssertIntEquals(tc, 0, _rd_be16(pack + 12));
    CuAssertIntEquals(tc, 0, _rd_be16(pack + 14));
    CuAssertIntEquals(tc, 1, _rd_be16(pack + 16));/* 结果列格式码数固定 1 */
    CuAssertIntEquals(tc, FORMAT_BINARY, _rd_be16(pack + 18));
    /* Bind 之后依次是 Describe / Execute / Sync，最后一条 Sync 是 'S' + Int32(4) */
    CuAssertIntEquals(tc, 'D', pack[blen + 1]);
    CuAssertIntEquals(tc, 'S', pack[size - 5]);
    CuAssertIntEquals(tc, 4, (int32_t)_rd_be32(pack + size - 4));
    FREE(pack);

    pgsql_bind_clear(&bind);
    pgsql_bind_free(&bind);
}

/* bind 两个缓冲的线格式逐字节验证。以前只断言过 format.offset == 18 与 values.offset > 2，
 * 于是 NULL 的 -1 长度、nparam 头与格式码的大端序、格式码本身全都没人钉；而唯一另外的
 * 调用方 task_pgsql 在 main.c 的 optional 白名单里，断言失败会被吞成 "- (network error)"。
 * 最要紧的是 NULL 那条：PostgreSQL 里长度 -1 才是 NULL、0 是零长值，写成 0 会让
 * SQL NULL 静默变成空字符串写进库 */
static void test_pgsql_bind_wire(CuTest *tc) {
    pgsql_bind_ctx b;
    const uint8_t *f;
    const uint8_t *v;

    /* nparam 头是大端 int16：写成小端时这两个字节会对调 */
    pgsql_bind_init(&b, 0x0102);
    CuAssertTrue(tc, 2 == b.format.offset);
    CuAssertTrue(tc, 2 == b.values.offset);
    f = (const uint8_t *)b.format.data;
    v = (const uint8_t *)b.values.data;
    CuAssertIntEquals(tc, 0x01, f[0]);
    CuAssertIntEquals(tc, 0x02, f[1]);
    CuAssertIntEquals(tc, 0x01, v[0]);
    CuAssertIntEquals(tc, 0x02, v[1]);
    pgsql_bind_free(&b);

    /* NULL：格式码写 FORMAT_TEXT，长度字段写 -1 即 0xFFFFFFFF，不跟值字节 */
    pgsql_bind_init(&b, 4);
    pgsql_bind_null(&b);
    CuAssertTrue(tc, 4 == b.format.offset);
    CuAssertTrue(tc, 6 == b.values.offset);
    f = (const uint8_t *)b.format.data;
    v = (const uint8_t *)b.values.data;
    CuAssertIntEquals(tc, 0x00, f[2]);
    CuAssertIntEquals(tc, (uint8_t)FORMAT_TEXT, f[3]);
    CuAssertIntEquals(tc, 0xFF, v[2]);
    CuAssertIntEquals(tc, 0xFF, v[3]);
    CuAssertIntEquals(tc, 0xFF, v[4]);
    CuAssertIntEquals(tc, 0xFF, v[5]);

    /* int32：格式码 FORMAT_BINARY，长度 4（大端 int32），值本体也是大端 */
    pgsql_bind_clear(&b);
    pgsql_bind_int32(&b, 0x12345678);
    CuAssertTrue(tc, 4 == b.format.offset);
    CuAssertTrue(tc, 10 == b.values.offset);
    f = (const uint8_t *)b.format.data;
    v = (const uint8_t *)b.values.data;
    CuAssertIntEquals(tc, 0x00, f[2]);
    CuAssertIntEquals(tc, (uint8_t)FORMAT_BINARY, f[3]);
    CuAssertIntEquals(tc, 0x00, v[2]);
    CuAssertIntEquals(tc, 0x00, v[3]);
    CuAssertIntEquals(tc, 0x00, v[4]);
    CuAssertIntEquals(tc, 0x04, v[5]);
    CuAssertIntEquals(tc, 0x12, v[6]);
    CuAssertIntEquals(tc, 0x34, v[7]);
    CuAssertIntEquals(tc, 0x56, v[8]);
    CuAssertIntEquals(tc, 0x78, v[9]);

    /* 文本与二进制两条路的格式码必须不同：都写成同一个值时下面两条有一条会红 */
    pgsql_bind_clear(&b);
    pgsql_bind_text(&b, "xy", 2);
    pgsql_bind_bytea(&b, "zw", 2);
    f = (const uint8_t *)b.format.data;
    CuAssertIntEquals(tc, (uint8_t)FORMAT_TEXT, f[3]);
    CuAssertIntEquals(tc, (uint8_t)FORMAT_BINARY, f[5]);
    /* 零长值只写长度不写体 */
    pgsql_bind_clear(&b);
    pgsql_bind_text(&b, "", 0);
    CuAssertTrue(tc, 6 == b.values.offset);
    v = (const uint8_t *)b.values.data;
    CuAssertIntEquals(tc, 0x00, v[2]);
    CuAssertIntEquals(tc, 0x00, v[5]);

    /* clear 只回退到 nparam 头之后，头部两字节必须留着 */
    pgsql_bind_clear(&b);
    CuAssertTrue(tc, 2 == b.format.offset);
    CuAssertTrue(tc, 2 == b.values.offset);
    CuAssertIntEquals(tc, 0x00, ((const uint8_t *)b.values.data)[0]);
    CuAssertIntEquals(tc, 0x04, ((const uint8_t *)b.values.data)[1]);
    pgsql_bind_free(&b);
}

/* =======================================================================
 * 测试套件注册
 * ======================================================================= */
void test_pgsql_pack(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_pgsql_bind_wire);
    SUITE_ADD_TEST(suite, test_pgsql_query);
    SUITE_ADD_TEST(suite, test_pgsql_terminate);
    SUITE_ADD_TEST(suite, test_pgsql_copy);
    SUITE_ADD_TEST(suite, test_pgsql_cancel);
    SUITE_ADD_TEST(suite, test_pgsql_stmt_prepare);
    SUITE_ADD_TEST(suite, test_pgsql_stmt_close);
    SUITE_ADD_TEST(suite, test_pgsql_null_name);
    SUITE_ADD_TEST(suite, test_pgsql_stmt_execute);
    SUITE_ADD_TEST(suite, test_pgsql_stmt_execute_bind_mismatch);
    SUITE_ADD_TEST(suite, test_pgsql_bind_basic);
    SUITE_ADD_TEST(suite, test_pgsql_bind_extra_types);
    SUITE_ADD_TEST(suite, test_pgsql_bind_free_reuse);
    SUITE_ADD_TEST(suite, test_pgsql_pack_helpers);
}
