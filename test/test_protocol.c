
#include "test_protocol.h"
#include "lib.h"

/* =======================================================================
 * 公共辅助 —— 将字符串字面量追加到 buffer_ctx
 * ======================================================================= */
static void _bput(buffer_ctx *b, const char *s) {
    buffer_append(b, (void *)s, strlen(s));
}

/* =======================================================================
 * HTTP —— 解包与组包验证
 * ======================================================================= */

/* 解析一个完整的 HTTP 200 响应，验证状态行、头部、消息体 */
static void test_http_response(CuTest *tc) {
    buffer_ctx buf;
    buffer_init(&buf);
    _bput(&buf, "HTTP/1.1 200 OK\r\n");
    _bput(&buf, "Content-Type: application/json\r\n");
    _bput(&buf, "Content-Length: 6\r\n");
    _bput(&buf, "\r\n");
    _bput(&buf, "{\"ok\"}");

    ud_cxt ud;
    ZERO(&ud, sizeof(ud_cxt));
    int32_t status = PROT_INIT;

    struct http_pack_ctx *pack = http_unpack(&buf, &ud, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));

    /* 验证响应状态行: version / code / reason */
    buf_ctx *st = http_status(pack);
    CuAssertTrue(tc, buf_compare(&st[0], "HTTP/1.1", 8));
    CuAssertTrue(tc, buf_compare(&st[1], "200", 3));
    CuAssertTrue(tc, buf_compare(&st[2], "OK", 2));

    /* 验证头部数量（Content-Type + Content-Length = 2） */
    CuAssertTrue(tc, 2 == http_nheader(pack));

    /* 验证 Content-Type 值 */
    size_t hlen = 0;
    char *ct = http_header(pack, "content-type", &hlen);
    CuAssertPtrNotNull(tc, ct);
    CuAssertTrue(tc, hlen == strlen("application/json"));
    CuAssertTrue(tc, 0 == memcmp(ct, "application/json", hlen));

    /* 验证消息体 */
    size_t dlen = 0;
    void *data = http_data(pack, &dlen);
    CuAssertPtrNotNull(tc, data);
    CuAssertTrue(tc, 6 == dlen);
    CuAssertTrue(tc, 0 == memcmp(data, "{\"ok\"}", 6));

    _http_pkfree(pack);
    _http_udfree(&ud);
    buffer_free(&buf);
}

/* 组包（POST 请求）后再解包，验证往返一致性 */
static void test_http_pack_req(CuTest *tc) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 256);
    http_pack_req(&bw, "POST", "/test");
    http_pack_head(&bw, "Host", "localhost");
    /* http_pack_content 写入 Content-Length: N\r\n\r\n + 消息体 */
    http_pack_content(&bw, "hello", 5);

    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, bw.data, bw.offset);
    FREE(bw.data);

    ud_cxt ud;
    ZERO(&ud, sizeof(ud_cxt));
    int32_t status = PROT_INIT;

    struct http_pack_ctx *pack = http_unpack(&buf, &ud, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));

    /* 验证请求行: method / path / version */
    buf_ctx *st = http_status(pack);
    CuAssertTrue(tc, buf_compare(&st[0], "POST", 4));
    CuAssertTrue(tc, buf_compare(&st[1], "/test", 5));
    CuAssertTrue(tc, buf_compare(&st[2], "HTTP/1.1", 8));

    /* Host + Content-Length = 2 个头部 */
    CuAssertTrue(tc, 2 == http_nheader(pack));

    /* 验证 Host 头 */
    size_t hlen = 0;
    char *host = http_header(pack, "host", &hlen);
    CuAssertPtrNotNull(tc, host);
    CuAssertTrue(tc, hlen == strlen("localhost"));
    CuAssertTrue(tc, 0 == memcmp(host, "localhost", hlen));

    /* 验证消息体 */
    size_t dlen = 0;
    void *data = http_data(pack, &dlen);
    CuAssertPtrNotNull(tc, data);
    CuAssertTrue(tc, 5 == dlen);
    CuAssertTrue(tc, 0 == memcmp(data, "hello", 5));

    _http_pkfree(pack);
    _http_udfree(&ud);
    buffer_free(&buf);
}

/* 不完整的 HTTP 头（缺少 \r\n\r\n）应触发 PROT_MOREDATA */
static void test_http_moredata(CuTest *tc) {
    buffer_ctx buf;
    buffer_init(&buf);
    _bput(&buf, "HTTP/1.1 200 OK\r\n");
    _bput(&buf, "Content-Length: 3\r\n");
    /* 故意不写 \r\n 结束符 */

    ud_cxt ud;
    ZERO(&ud, sizeof(ud_cxt));
    int32_t status = PROT_INIT;

    struct http_pack_ctx *pack = http_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_MOREDATA));
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));

    _http_udfree(&ud);
    buffer_free(&buf);
}

/* =======================================================================
 * Redis RESP —— 各类型解包、组包
 * ======================================================================= */

/* 简单类型：+Simple String、-Error、:Integer、_Null */
static void test_redis_simple(CuTest *tc) {
    /* +OK\r\n */
    {
        buffer_ctx buf;
        buffer_init(&buf);
        _bput(&buf, "+OK\r\n");
        ud_cxt ud;
        ZERO(&ud, sizeof(ud_cxt));
        int32_t status = PROT_INIT;
        redis_pack_ctx *pack = redis_unpack(&buf, &ud, &status);
        CuAssertPtrNotNull(tc, pack);
        CuAssertTrue(tc, RESP_STRING == pack->prot);
        CuAssertTrue(tc, 2 == pack->len);
        CuAssertTrue(tc, 0 == memcmp(pack->data, "OK", 2));
        _redis_pkfree(pack);
        _redis_udfree(&ud);
        buffer_free(&buf);
    }
    /* -ERR some message\r\n */
    {
        buffer_ctx buf;
        buffer_init(&buf);
        _bput(&buf, "-ERR some message\r\n");
        ud_cxt ud;
        ZERO(&ud, sizeof(ud_cxt));
        int32_t status = PROT_INIT;
        redis_pack_ctx *pack = redis_unpack(&buf, &ud, &status);
        CuAssertPtrNotNull(tc, pack);
        CuAssertTrue(tc, RESP_ERROR == pack->prot);
        /* 解析内容："ERR some message"（去掉前缀 '-' 和 '\r\n'）*/
        CuAssertTrue(tc, 0 == memcmp(pack->data, "ERR some message",
                                     strlen("ERR some message")));
        _redis_pkfree(pack);
        _redis_udfree(&ud);
        buffer_free(&buf);
    }
    /* :42\r\n */
    {
        buffer_ctx buf;
        buffer_init(&buf);
        _bput(&buf, ":42\r\n");
        ud_cxt ud;
        ZERO(&ud, sizeof(ud_cxt));
        int32_t status = PROT_INIT;
        redis_pack_ctx *pack = redis_unpack(&buf, &ud, &status);
        CuAssertPtrNotNull(tc, pack);
        CuAssertTrue(tc, RESP_INTEGER == pack->prot);
        CuAssertTrue(tc, 42 == pack->ival);
        _redis_pkfree(pack);
        _redis_udfree(&ud);
        buffer_free(&buf);
    }
    /* _\r\n (RESP3 Null) */
    {
        buffer_ctx buf;
        buffer_init(&buf);
        _bput(&buf, "_\r\n");
        ud_cxt ud;
        ZERO(&ud, sizeof(ud_cxt));
        int32_t status = PROT_INIT;
        redis_pack_ctx *pack = redis_unpack(&buf, &ud, &status);
        CuAssertPtrNotNull(tc, pack);
        CuAssertTrue(tc, RESP_NIL == pack->prot);
        CuAssertTrue(tc, 0 == pack->len);
        _redis_pkfree(pack);
        _redis_udfree(&ud);
        buffer_free(&buf);
    }
}

/* 批量字符串：$6\r\nfoobar\r\n */
static void test_redis_bulk(CuTest *tc) {
    buffer_ctx buf;
    buffer_init(&buf);
    _bput(&buf, "$6\r\nfoobar\r\n");

    ud_cxt ud;
    ZERO(&ud, sizeof(ud_cxt));
    int32_t status = PROT_INIT;

    redis_pack_ctx *pack = redis_unpack(&buf, &ud, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, RESP_BSTRING == pack->prot);
    CuAssertTrue(tc, 6 == pack->len);
    CuAssertTrue(tc, 0 == memcmp(pack->data, "foobar", 6));

    _redis_pkfree(pack);
    _redis_udfree(&ud);
    buffer_free(&buf);
}

/* 数组：*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n */
static void test_redis_array(CuTest *tc) {
    buffer_ctx buf;
    buffer_init(&buf);
    _bput(&buf, "*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");

    ud_cxt ud;
    ZERO(&ud, sizeof(ud_cxt));
    int32_t status = PROT_INIT;

    redis_pack_ctx *pack = redis_unpack(&buf, &ud, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, RESP_ARRAY == pack->prot);
    CuAssertTrue(tc, 2 == pack->nelem);

    /* 第一个元素："foo" */
    redis_pack_ctx *p1 = pack->next;
    CuAssertPtrNotNull(tc, p1);
    CuAssertTrue(tc, RESP_BSTRING == p1->prot);
    CuAssertTrue(tc, 3 == p1->len);
    CuAssertTrue(tc, 0 == memcmp(p1->data, "foo", 3));

    /* 第二个元素："bar" */
    redis_pack_ctx *p2 = p1->next;
    CuAssertPtrNotNull(tc, p2);
    CuAssertTrue(tc, RESP_BSTRING == p2->prot);
    CuAssertTrue(tc, 3 == p2->len);
    CuAssertTrue(tc, 0 == memcmp(p2->data, "bar", 3));

    CuAssertTrue(tc, NULL == p2->next);

    _redis_pkfree(pack);
    _redis_udfree(&ud);
    buffer_free(&buf);
}

/* redis_pack 组包，再解包验证 */
static void test_redis_pack(CuTest *tc) {
    /* "SET key value" → *3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n */
    size_t size = 0;
    char *cmd = redis_pack(&size, "SET key value");
    CuAssertPtrNotNull(tc, cmd);
    CuAssertTrue(tc, size > 0);

    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, cmd, size);
    FREE(cmd);

    ud_cxt ud;
    ZERO(&ud, sizeof(ud_cxt));
    int32_t status = PROT_INIT;

    redis_pack_ctx *pack = redis_unpack(&buf, &ud, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, RESP_ARRAY == pack->prot);
    CuAssertTrue(tc, 3 == pack->nelem);

    redis_pack_ctx *p1 = pack->next;
    CuAssertPtrNotNull(tc, p1);
    CuAssertTrue(tc, 3 == p1->len);
    CuAssertTrue(tc, 0 == memcmp(p1->data, "SET", 3));

    redis_pack_ctx *p2 = p1->next;
    CuAssertPtrNotNull(tc, p2);
    CuAssertTrue(tc, 3 == p2->len);
    CuAssertTrue(tc, 0 == memcmp(p2->data, "key", 3));

    redis_pack_ctx *p3 = p2->next;
    CuAssertPtrNotNull(tc, p3);
    CuAssertTrue(tc, 5 == p3->len);
    CuAssertTrue(tc, 0 == memcmp(p3->data, "value", 5));

    _redis_pkfree(pack);
    _redis_udfree(&ud);
    buffer_free(&buf);
}

/* 不完整帧（"+OK" 无 \r\n）应触发 PROT_MOREDATA */
static void test_redis_moredata(CuTest *tc) {
    buffer_ctx buf;
    buffer_init(&buf);
    _bput(&buf, "+OK"); /* 故意不加 \r\n */

    ud_cxt ud;
    ZERO(&ud, sizeof(ud_cxt));
    int32_t status = PROT_INIT;

    redis_pack_ctx *pack = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_MOREDATA));
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));

    _redis_udfree(&ud);
    buffer_free(&buf);
}

/* =======================================================================
 * URL —— url_parse 各字段验证
 * ======================================================================= */

static void test_url_parse(CuTest *tc) {
    /* url_parse 在原始 char 数组上就地操作，不能用 const char * */
    char url[] = "http://user:psw@host.com:8080/path/to?k1=v1&k2=v2#anchor";
    url_ctx ctx;
    url_parse(&ctx, url, strlen(url));

    CuAssertTrue(tc, buf_compare(&ctx.scheme, "http", 4));
    CuAssertTrue(tc, buf_compare(&ctx.user,   "user", 4));
    CuAssertTrue(tc, buf_compare(&ctx.psw,    "psw",  3));
    CuAssertTrue(tc, buf_compare(&ctx.host,   "host.com", 8));
    CuAssertTrue(tc, buf_compare(&ctx.port,   "8080", 4));
    CuAssertTrue(tc, buf_compare(&ctx.path,   "/path/to", 8));
    CuAssertTrue(tc, buf_compare(&ctx.anchor, "anchor", 6));

    buf_ctx *p = url_get_param(&ctx, "k1");
    CuAssertPtrNotNull(tc, p);
    CuAssertTrue(tc, buf_compare(p, "v1", 2));

    p = url_get_param(&ctx, "k2");
    CuAssertPtrNotNull(tc, p);
    CuAssertTrue(tc, buf_compare(p, "v2", 2));

    /* 不存在的参数应返回 NULL */
    p = url_get_param(&ctx, "missing");
    CuAssertTrue(tc, NULL == p);
}

/* =======================================================================
 * Custz —— 三种打包格式往返验证
 * ======================================================================= */

static void _custz_roundtrip(CuTest *tc, uint8_t pktype,
                             const char *payload, size_t plen) {
    /* 组包：header + payload */
    size_t pksize = 0;
    void *pkt = custz_pack(pktype, (void *)payload, plen, &pksize);
    CuAssertPtrNotNull(tc, pkt);
    CuAssertTrue(tc, pksize > plen); /* 包含头部开销 */

    /* 写入 buffer */
    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, pkt, pksize);
    FREE(pkt);

    /* 解包：取回 payload */
    int32_t status = PROT_INIT;
    size_t out = 0;
    void *data = custz_unpack(pktype, &buf, &out, &status);
    CuAssertPtrNotNull(tc, data);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertTrue(tc, plen == out);
    CuAssertTrue(tc, 0 == memcmp(data, payload, plen));
    /* 包已全部消费，buffer 应为空 */
    CuAssertTrue(tc, 0 == buffer_size(&buf));

    FREE(data);
    buffer_free(&buf);
}

static void test_custz(CuTest *tc) {
    const char *msg = "hello custz test data";
    size_t mlen = strlen(msg);

    _custz_roundtrip(tc, PACK_CUSTZ_FIXED, msg, mlen);
    _custz_roundtrip(tc, PACK_CUSTZ_FLAG,  msg, mlen);
    _custz_roundtrip(tc, PACK_CUSTZ_VAR,   msg, mlen);
}

/* ======================================================================= */

void test_protocol(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_http_response);
    SUITE_ADD_TEST(suite, test_http_pack_req);
    SUITE_ADD_TEST(suite, test_http_moredata);
    SUITE_ADD_TEST(suite, test_redis_simple);
    SUITE_ADD_TEST(suite, test_redis_bulk);
    SUITE_ADD_TEST(suite, test_redis_array);
    SUITE_ADD_TEST(suite, test_redis_pack);
    SUITE_ADD_TEST(suite, test_redis_moredata);
    SUITE_ADD_TEST(suite, test_url_parse);
    SUITE_ADD_TEST(suite, test_custz);
}
