
#include "test_protocol.h"
#include "lib.h"
#include "protocol/custz_head.h"
#include "protocol/prots.h"

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
    binary_free(&bw);

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

// HTTP smuggling 辅助：输入 raw HTTP 字节，断言解析是否触发 PROT_ERROR
// expect_error=1 期望 _check_transfer 拒绝；0 期望成功解析
static void _http_smuggle_check(CuTest *tc, const char *raw, int32_t expect_error) {
    buffer_ctx buf;
    buffer_init(&buf);
    _bput(&buf, raw);

    ud_cxt ud;
    ZERO(&ud, sizeof(ud_cxt));
    int32_t status = PROT_INIT;

    struct http_pack_ctx *pack = http_unpack(&buf, &ud, &status);
    if (expect_error) {
        CuAssertTrue(tc, NULL == pack);
        CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
    } else {
        CuAssertPtrNotNull(tc, pack);
        CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
        _http_pkfree(pack);
    }
    _http_udfree(&ud);
    buffer_free(&buf);
}

// RFC 7230 §3.3.2 / §3.3.3 — HTTP Request Smuggling 防御
static void test_http_smuggling(CuTest *tc) {
    // 1. 重复 Content-Length 值不同（CL.CL desync）→ 拒绝
    _http_smuggle_check(tc,
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Length: 0\r\n"
        "Content-Length: 10\r\n"
        "\r\n",
        1);
    // 2. 重复 Content-Length 值相同 → 接受（RFC SHOULD 合并）
    _http_smuggle_check(tc,
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Length: 5\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello",
        0);
    // 3. TE 后 CL（TE.CL desync）→ 拒绝
    _http_smuggle_check(tc,
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Content-Length: 6\r\n"
        "\r\n",
        1);
    // 4. CL 后 TE（CL.TE desync）→ 拒绝
    _http_smuggle_check(tc,
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Length: 6\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n",
        1);
    // 5. 负数 Content-Length → 拒绝
    _http_smuggle_check(tc,
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Length: -1\r\n"
        "\r\n",
        1);
    // 6. 非数字 Content-Length → 拒绝
    _http_smuggle_check(tc,
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Length: abc\r\n"
        "\r\n",
        1);
    // 7. 合法 chunked（单个 TE，无 CL）→ 接受
    _http_smuggle_check(tc,
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "0\r\n\r\n",
        0);
    // 8. 正号 Content-Length（RFC 7230 仅 1*DIGIT）→ 拒绝
    _http_smuggle_check(tc,
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Length: +6\r\n"
        "\r\n",
        1);
}

// chunked chunk-size 走私：第一次 unpack 解析 header(chunked)，第二次 unpack 解析 chunk-size 行；断言其是否被拒
static void _chunked_size_check(CuTest *tc, const char *sizeline, int32_t expect_error) {
    buffer_ctx buf;
    buffer_init(&buf);
    _bput(&buf, "GET / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n");
    _bput(&buf, sizeline);
    ud_cxt ud;
    ZERO(&ud, sizeof(ud_cxt));
    int32_t status = PROT_INIT;
    struct http_pack_ctx *pack = http_unpack(&buf, &ud, &status);  // 1) header；chunked 分支不设 ud->context，手动释放
    CuAssertPtrNotNull(tc, pack);
    _http_pkfree(pack);
    status = PROT_INIT;
    pack = http_unpack(&buf, &ud, &status);                 // 2) chunk-size 行
    if (expect_error) {
        CuAssertTrue(tc, NULL == pack);
        CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
    } else {
        CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    }
    _http_udfree(&ud);
    buffer_free(&buf);
}
// RFC 7230 §4.1：chunk-size 首字符须为 HEXDIG；strtoul 会跳前导空白 / 吞 '+'/'-' / 空白后接受 "0x"（请求走私）
static void test_http_chunked_size_smuggle(CuTest *tc) {
    _chunked_size_check(tc, " 0x10\r\n", 1);   // 前导空白 + 0x（旧代码绕过 0x 拒绝）
    _chunked_size_check(tc, "0x10\r\n", 1);     // 无空白 0x
    _chunked_size_check(tc, "+5\r\n", 1);       // strtoul 吞 '+'
    _chunked_size_check(tc, " 5\r\n", 1);       // 前导空白
    _chunked_size_check(tc, "a\r\n", 0);        // 合法 hex，不误拒（解析成功后等 data）
}

// _http_check_keyval value 按 token 严格匹配(RFC 7230 §3.3.1)
static void test_http_check_keyval_token(CuTest *tc) {
    http_header_ctx head;
    head.key.data = "transfer-encoding";
    head.key.lens = strlen("transfer-encoding");
    // 1. 单 token 命中
    head.value.data = "chunked";
    head.value.lens = strlen("chunked");
    CuAssertIntEquals(tc, ERR_OK,
        _http_check_keyval(&head, "transfer-encoding", strlen("transfer-encoding"),
                                  "chunked", strlen("chunked")));
    // 2. token 列表中间
    head.value.data = "gzip, chunked, deflate";
    head.value.lens = strlen("gzip, chunked, deflate");
    CuAssertIntEquals(tc, ERR_OK,
        _http_check_keyval(&head, "transfer-encoding", strlen("transfer-encoding"),
                                  "chunked", strlen("chunked")));
    // 3. token 列表无空格
    head.value.data = "gzip,chunked";
    head.value.lens = strlen("gzip,chunked");
    CuAssertIntEquals(tc, ERR_OK,
        _http_check_keyval(&head, "transfer-encoding", strlen("transfer-encoding"),
                                  "chunked", strlen("chunked")));
    // 4. token 首位 + 末尾 OWS
    head.value.data = "chunked , gzip";
    head.value.lens = strlen("chunked , gzip");
    CuAssertIntEquals(tc, ERR_OK,
        _http_check_keyval(&head, "transfer-encoding", strlen("transfer-encoding"),
                                  "chunked", strlen("chunked")));
    // 5. 子串误命中阻断："chunkedfoo" 严格不匹配
    head.value.data = "chunkedfoo";
    head.value.lens = strlen("chunkedfoo");
    CuAssertIntEquals(tc, ERR_FAILED,
        _http_check_keyval(&head, "transfer-encoding", strlen("transfer-encoding"),
                                  "chunked", strlen("chunked")));
    // 6. 子串误命中阻断："xchunked"
    head.value.data = "xchunked";
    head.value.lens = strlen("xchunked");
    CuAssertIntEquals(tc, ERR_FAILED,
        _http_check_keyval(&head, "transfer-encoding", strlen("transfer-encoding"),
                                  "chunked", strlen("chunked")));
    // 7. token 列表中含非法子串："gzip, chunkedfoo"
    head.value.data = "gzip, chunkedfoo";
    head.value.lens = strlen("gzip, chunkedfoo");
    CuAssertIntEquals(tc, ERR_FAILED,
        _http_check_keyval(&head, "transfer-encoding", strlen("transfer-encoding"),
                                  "chunked", strlen("chunked")));
    // 8. 大小写不敏感
    head.value.data = "Chunked";
    head.value.lens = strlen("Chunked");
    CuAssertIntEquals(tc, ERR_OK,
        _http_check_keyval(&head, "transfer-encoding", strlen("transfer-encoding"),
                                  "chunked", strlen("chunked")));
    // 9. key 不匹配
    head.key.data = "content-type";
    head.key.lens = strlen("content-type");
    head.value.data = "chunked";
    head.value.lens = strlen("chunked");
    CuAssertIntEquals(tc, ERR_FAILED,
        _http_check_keyval(&head, "transfer-encoding", strlen("transfer-encoding"),
                                  "chunked", strlen("chunked")));
    // 10. val=NULL 仅 key 检查
    head.key.data = "content-length";
    head.key.lens = strlen("content-length");
    head.value.data = "42";
    head.value.lens = strlen("42");
    CuAssertIntEquals(tc, ERR_OK,
        _http_check_keyval(&head, "content-length", strlen("content-length"), NULL, 0));
}

// chunked trailer 超 MAX_HEADLENS=4KB 应 PROT_ERROR
static void test_http_chunked_trailer_limit(CuTest *tc) {
    buffer_ctx buf;
    buffer_init(&buf);
    _bput(&buf, "HTTP/1.1 200 OK\r\n");
    _bput(&buf, "Transfer-Encoding: chunked\r\n");
    _bput(&buf, "\r\n");
    _bput(&buf, "5\r\nhello\r\n");
    _bput(&buf, "0\r\n");  // chunked 终止行，后跟 trailer
    // 5KB trailer 数据，无 \r\n\r\n 终止
    char trailer[5000];
    memset(trailer, 'A', sizeof(trailer));
    buffer_append(&buf, trailer, sizeof(trailer));

    ud_cxt ud;
    ZERO(&ud, sizeof(ud_cxt));
    int32_t status;
    struct http_pack_ctx *pack;

    // 第 1 轮：解析 head（chunked 起始 pack）
    status = PROT_INIT;
    pack = http_unpack(&buf, &ud, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    _http_pkfree(pack);

    // 第 2 轮：解析 chunked "5\r\nhello\r\n"
    status = PROT_INIT;
    pack = http_unpack(&buf, &ud, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    _http_pkfree(pack);

    // 第 3 轮：进入终止块路径，5KB > 4KB 无 \r\n\r\n → PROT_ERROR
    status = PROT_INIT;
    pack = http_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));

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

// http_code_status 各典型状态码映射 + 未知码 fallback "Unknown"
static void test_http_code_status(CuTest *tc) {
    // 1xx
    CuAssertStrEquals(tc, "Continue",            http_code_status(100));
    CuAssertStrEquals(tc, "Switching Protocols", http_code_status(101));
    // 2xx
    CuAssertStrEquals(tc, "OK",                  http_code_status(200));
    CuAssertStrEquals(tc, "Created",             http_code_status(201));
    CuAssertStrEquals(tc, "No Content",          http_code_status(204));
    // 3xx
    CuAssertStrEquals(tc, "Moved Permanently",   http_code_status(301));
    CuAssertStrEquals(tc, "Not Modified",        http_code_status(304));
    // 4xx
    CuAssertStrEquals(tc, "Bad Request",         http_code_status(400));
    CuAssertStrEquals(tc, "Unauthorized",        http_code_status(401));
    CuAssertStrEquals(tc, "Forbidden",           http_code_status(403));
    CuAssertStrEquals(tc, "Not Found",           http_code_status(404));
    CuAssertStrEquals(tc, "Method Not Allowed",  http_code_status(405));
    CuAssertStrEquals(tc, "Conflict",            http_code_status(409));
    // 5xx
    CuAssertStrEquals(tc, "Internal Server Error", http_code_status(500));
    CuAssertStrEquals(tc, "Bad Gateway",         http_code_status(502));
    CuAssertStrEquals(tc, "Service Unavailable", http_code_status(503));
    // 未知/无效码 → "Unknown"
    CuAssertStrEquals(tc, "Unknown", http_code_status(0));
    CuAssertStrEquals(tc, "Unknown", http_code_status(999));
    CuAssertStrEquals(tc, "Unknown", http_code_status(-1));
    // http_pack_resp 应使用 http_code_status 的描述串
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    http_pack_resp(&bw, 200);
    CuAssertTrue(tc, NULL != memstr(0, bw.data, bw.offset, "HTTP/1.1 200 OK\r\n", 17));
    binary_free(&bw);
}

// http_pack_chunked 完整 round-trip：
// 1) 第一段：bw 含状态行+头部，offset>0 触发自动追加 Transfer-Encoding 头 + 终止 \r\n\r\n + 块行
// 2) 第二段：binary_offset(bw, 0) 复用，仅写块行+数据
// 3) 终止块：lens=0 写 "0\r\n\r\n"
// 4) 拼接后用 http_unpack 重放，逐块解析得到 PROT_SLICE_START / PROT_SLICE / PROT_SLICE_END
static void test_http_pack_chunked(CuTest *tc) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    // 第一段：状态行 + 头部 + chunk1
    http_pack_resp(&bw, 200);
    http_pack_head(&bw, "Content-Type", "text/plain");
    size_t off_before = bw.offset;
    CuAssertTrue(tc, off_before > 0);
    http_pack_chunked(&bw, "AAA", 3);
    // 应包含 Transfer-Encoding: Chunked 标记
    CuAssertTrue(tc, NULL != memstr(0, bw.data, bw.offset,
        "Transfer-Encoding: Chunked\r\n", sizeof("Transfer-Encoding: Chunked\r\n") - 1));
    // 应包含 chunk1 的十六进制长度 "3" + 数据 "AAA"
    CuAssertTrue(tc, NULL != memstr(0, bw.data, bw.offset, "3\r\nAAA\r\n", 8));

    // 拼接到一个缓冲区便于 unpack 重放
    buffer_ctx playback;
    buffer_init(&playback);
    buffer_append(&playback, bw.data, bw.offset);

    // 第二段：binary_offset 重置，仅写 chunk2
    binary_offset(&bw, 0);
    http_pack_chunked(&bw, "BBBB", 4);
    // 第二段不应再次写 Transfer-Encoding 头
    CuAssertTrue(tc, NULL == memstr(0, bw.data, bw.offset,
        "Transfer-Encoding", sizeof("Transfer-Encoding") - 1));
    // 内容应为 "4\r\nBBBB\r\n"：1(hex_size) + 2(crlf) + 4(data) + 2(crlf) = 9 字节
    CuAssertIntEquals(tc, 9, (int)bw.offset);
    CuAssertTrue(tc, 0 == memcmp(bw.data, "4\r\nBBBB\r\n", 9));
    buffer_append(&playback, bw.data, bw.offset);

    // 终止块：lens=0 写 "0\r\n\r\n"
    binary_offset(&bw, 0);
    http_pack_chunked(&bw, NULL, 0);
    CuAssertIntEquals(tc, 5, (int)bw.offset);
    CuAssertTrue(tc, 0 == memcmp(bw.data, "0\r\n\r\n", 5));
    buffer_append(&playback, bw.data, bw.offset);
    binary_free(&bw);

    // unpack 重放：先头部（chunked 标记），再两个数据块，再终止
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    int32_t status = PROT_INIT;
    struct http_pack_ctx *pack;

    // 1) 头部 → chunked 起始
    pack = http_unpack(&playback, &ud, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertIntEquals(tc, 1, http_chunked(pack));
    CuAssertTrue(tc, BIT_CHECK(status, PROT_SLICE_START));
    _http_pkfree(pack);

    // 2) chunk1 "AAA"
    status = PROT_INIT;
    pack = http_unpack(&playback, &ud, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertIntEquals(tc, 2, http_chunked(pack));
    size_t dlen = 0;
    void *d = http_data(pack, &dlen);
    CuAssertIntEquals(tc, 3, (int)dlen);
    CuAssertTrue(tc, 0 == memcmp(d, "AAA", 3));
    CuAssertTrue(tc, BIT_CHECK(status, PROT_SLICE));
    _http_pkfree(pack);

    // 3) chunk2 "BBBB"
    status = PROT_INIT;
    pack = http_unpack(&playback, &ud, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertIntEquals(tc, 2, http_chunked(pack));
    d = http_data(pack, &dlen);
    CuAssertIntEquals(tc, 4, (int)dlen);
    CuAssertTrue(tc, 0 == memcmp(d, "BBBB", 4));
    CuAssertTrue(tc, BIT_CHECK(status, PROT_SLICE));
    _http_pkfree(pack);

    // 4) 终止块
    status = PROT_INIT;
    pack = http_unpack(&playback, &ud, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertIntEquals(tc, 2, http_chunked(pack));
    CuAssertTrue(tc, BIT_CHECK(status, PROT_SLICE_END));
    _http_pkfree(pack);

    _http_udfree(&ud);
    buffer_free(&playback);
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

// Null Bulk String：$-1\r\n → pack->len == -1，修复前 PACK_TOO_LONG(UINT64_MAX) 永久拒绝此合法包
static void test_redis_null_bulk(CuTest *tc) {
    buffer_ctx buf;
    buffer_init(&buf);
    _bput(&buf, "$-1\r\n");

    ud_cxt ud;
    ZERO(&ud, sizeof(ud_cxt));
    int32_t status = PROT_INIT;

    redis_pack_ctx *pack = redis_unpack(&buf, &ud, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, RESP_BSTRING == pack->prot);
    CuAssertTrue(tc, -1 == pack->len);
    CuAssertTrue(tc, NULL == pack->next);

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

// _reader_line / _reader_bulk 长度行无 CRLF 持续累积超 MAX_PACK_SIZE(64KB) 应 PROT_ERROR
static void test_redis_oversize_no_crlf(CuTest *tc) {
    // 1. 单行 RESP "+aaa..." 无 CRLF 超 64KB
    {
        buffer_ctx buf;
        buffer_init(&buf);
        size_t n = 70 * 1024;
        char *huge;
        MALLOC(huge, n);
        huge[0] = '+';
        memset(huge + 1, 'a', n - 1);
        buffer_append(&buf, huge, n);
        FREE(huge);

        ud_cxt ud;
        ZERO(&ud, sizeof(ud_cxt));
        int32_t status = PROT_INIT;
        redis_pack_ctx *pack = redis_unpack(&buf, &ud, &status);
        CuAssertTrue(tc, NULL == pack);
        CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));

        _redis_udfree(&ud);
        buffer_free(&buf);
    }
    // 2. Bulk 长度行 "$1111..." 无 CRLF 超 64KB
    {
        buffer_ctx buf;
        buffer_init(&buf);
        size_t n = 70 * 1024;
        char *huge;
        MALLOC(huge, n);
        huge[0] = '$';
        memset(huge + 1, '1', n - 1);
        buffer_append(&buf, huge, n);
        FREE(huge);

        ud_cxt ud;
        ZERO(&ud, sizeof(ud_cxt));
        int32_t status = PROT_INIT;
        redis_pack_ctx *pack = redis_unpack(&buf, &ud, &status);
        CuAssertTrue(tc, NULL == pack);
        CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));

        _redis_udfree(&ud);
        buffer_free(&buf);
    }
    // 3. Aggregate 类型 "*9999..." 无 CRLF 超 64KB（_redis_reader_agg 同模式 PACK_TOO_LONG 守卫）
    {
        buffer_ctx buf;
        buffer_init(&buf);
        size_t n = 70 * 1024;
        char *huge;
        MALLOC(huge, n);
        huge[0] = '*';
        memset(huge + 1, '9', n - 1);
        buffer_append(&buf, huge, n);
        FREE(huge);

        ud_cxt ud;
        ZERO(&ud, sizeof(ud_cxt));
        int32_t status = PROT_INIT;
        redis_pack_ctx *pack = redis_unpack(&buf, &ud, &status);
        CuAssertTrue(tc, NULL == pack);
        CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));

        _redis_udfree(&ud);
        buffer_free(&buf);
    }
}

// RESP3 类型解包：BOOL/DOUBLE/BIGNUM/BERROR/VERB
// 对应 lib/protocol/redis.c:294-328 (line 类型) 与 _reader_bulk (bulk 类型)
static void test_redis_resp3_scalar(CuTest *tc) {
    // BOOL true: "#t\r\n"
    {
        buffer_ctx buf;
        buffer_init(&buf);
        _bput(&buf, "#t\r\n");
        ud_cxt ud;
        ZERO(&ud, sizeof(ud));
        int32_t status = PROT_INIT;
        redis_pack_ctx *pack = redis_unpack(&buf, &ud, &status);
        CuAssertPtrNotNull(tc, pack);
        CuAssertTrue(tc, RESP_BOOL == pack->prot);
        CuAssertTrue(tc, 1 == pack->ival);
        _redis_pkfree(pack);
        _redis_udfree(&ud);
        buffer_free(&buf);
    }
    // BOOL false: "#f\r\n"
    {
        buffer_ctx buf;
        buffer_init(&buf);
        _bput(&buf, "#f\r\n");
        ud_cxt ud;
        ZERO(&ud, sizeof(ud));
        int32_t status = PROT_INIT;
        redis_pack_ctx *pack = redis_unpack(&buf, &ud, &status);
        CuAssertPtrNotNull(tc, pack);
        CuAssertTrue(tc, RESP_BOOL == pack->prot);
        CuAssertTrue(tc, 0 == pack->ival);
        _redis_pkfree(pack);
        _redis_udfree(&ud);
        buffer_free(&buf);
    }
    // BOOL 非法字符: "#x\r\n" → PROT_ERROR
    {
        buffer_ctx buf;
        buffer_init(&buf);
        _bput(&buf, "#x\r\n");
        ud_cxt ud;
        ZERO(&ud, sizeof(ud));
        int32_t status = PROT_INIT;
        redis_pack_ctx *pack = redis_unpack(&buf, &ud, &status);
        CuAssertTrue(tc, NULL == pack);
        CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
        _redis_udfree(&ud);
        buffer_free(&buf);
    }
    // DOUBLE 普通值: ",3.14\r\n"
    {
        buffer_ctx buf;
        buffer_init(&buf);
        _bput(&buf, ",3.14\r\n");
        ud_cxt ud;
        ZERO(&ud, sizeof(ud));
        int32_t status = PROT_INIT;
        redis_pack_ctx *pack = redis_unpack(&buf, &ud, &status);
        CuAssertPtrNotNull(tc, pack);
        CuAssertTrue(tc, RESP_DOUBLE == pack->prot);
        CuAssertDblEquals(tc, 3.14, pack->dval, 1e-9);
        _redis_pkfree(pack);
        _redis_udfree(&ud);
        buffer_free(&buf);
    }
    // DOUBLE inf
    {
        buffer_ctx buf;
        buffer_init(&buf);
        _bput(&buf, ",inf\r\n");
        ud_cxt ud;
        ZERO(&ud, sizeof(ud));
        int32_t status = PROT_INIT;
        redis_pack_ctx *pack = redis_unpack(&buf, &ud, &status);
        CuAssertPtrNotNull(tc, pack);
        CuAssertTrue(tc, RESP_DOUBLE == pack->prot);
        CuAssertTrue(tc, isinf(pack->dval) && pack->dval > 0);
        _redis_pkfree(pack);
        _redis_udfree(&ud);
        buffer_free(&buf);
    }
    // DOUBLE -inf
    {
        buffer_ctx buf;
        buffer_init(&buf);
        _bput(&buf, ",-inf\r\n");
        ud_cxt ud;
        ZERO(&ud, sizeof(ud));
        int32_t status = PROT_INIT;
        redis_pack_ctx *pack = redis_unpack(&buf, &ud, &status);
        CuAssertPtrNotNull(tc, pack);
        CuAssertTrue(tc, RESP_DOUBLE == pack->prot);
        CuAssertTrue(tc, isinf(pack->dval) && pack->dval < 0);
        _redis_pkfree(pack);
        _redis_udfree(&ud);
        buffer_free(&buf);
    }
    // DOUBLE nan
    {
        buffer_ctx buf;
        buffer_init(&buf);
        _bput(&buf, ",nan\r\n");
        ud_cxt ud;
        ZERO(&ud, sizeof(ud));
        int32_t status = PROT_INIT;
        redis_pack_ctx *pack = redis_unpack(&buf, &ud, &status);
        CuAssertPtrNotNull(tc, pack);
        CuAssertTrue(tc, RESP_DOUBLE == pack->prot);
        CuAssertTrue(tc, isnan(pack->dval));
        _redis_pkfree(pack);
        _redis_udfree(&ud);
        buffer_free(&buf);
    }
    // BIGNUM 在 int64 范围内: "(9223372036854775807\r\n"
    {
        buffer_ctx buf;
        buffer_init(&buf);
        _bput(&buf, "(9223372036854775807\r\n");
        ud_cxt ud;
        ZERO(&ud, sizeof(ud));
        int32_t status = PROT_INIT;
        redis_pack_ctx *pack = redis_unpack(&buf, &ud, &status);
        CuAssertPtrNotNull(tc, pack);
        CuAssertTrue(tc, RESP_BIGNUM == pack->prot);
        CuAssertTrue(tc, INT64_MAX == pack->ival);
        _redis_pkfree(pack);
        _redis_udfree(&ud);
        buffer_free(&buf);
    }
    // BERROR: "!21\r\nSYNTAX invalid syntax\r\n"
    {
        buffer_ctx buf;
        buffer_init(&buf);
        _bput(&buf, "!21\r\nSYNTAX invalid syntax\r\n");
        ud_cxt ud;
        ZERO(&ud, sizeof(ud));
        int32_t status = PROT_INIT;
        redis_pack_ctx *pack = redis_unpack(&buf, &ud, &status);
        CuAssertPtrNotNull(tc, pack);
        CuAssertTrue(tc, RESP_BERROR == pack->prot);
        CuAssertTrue(tc, 21 == pack->len);
        CuAssertTrue(tc, 0 == memcmp(pack->data, "SYNTAX invalid syntax", 21));
        _redis_pkfree(pack);
        _redis_udfree(&ud);
        buffer_free(&buf);
    }
    // VERB: "=15\r\ntxt:Some string\r\n" → venc="txt"，data="Some string"(11 字节)
    {
        buffer_ctx buf;
        buffer_init(&buf);
        _bput(&buf, "=15\r\ntxt:Some string\r\n");
        ud_cxt ud;
        ZERO(&ud, sizeof(ud));
        int32_t status = PROT_INIT;
        redis_pack_ctx *pack = redis_unpack(&buf, &ud, &status);
        CuAssertPtrNotNull(tc, pack);
        CuAssertTrue(tc, RESP_VERB == pack->prot);
        CuAssertTrue(tc, 11 == pack->len);
        CuAssertTrue(tc, 0 == memcmp(pack->venc, "txt", 3));
        CuAssertTrue(tc, 0 == memcmp(pack->data, "Some string", 11));
        _redis_pkfree(pack);
        _redis_udfree(&ud);
        buffer_free(&buf);
    }
    // VERB 长度 < 4（不足容纳 3 字节编码 + ':'）: "=2\r\nab\r\n" → PROT_ERROR
    {
        buffer_ctx buf;
        buffer_init(&buf);
        _bput(&buf, "=2\r\nab\r\n");
        ud_cxt ud;
        ZERO(&ud, sizeof(ud));
        int32_t status = PROT_INIT;
        redis_pack_ctx *pack = redis_unpack(&buf, &ud, &status);
        CuAssertTrue(tc, NULL == pack);
        CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
        _redis_udfree(&ud);
        buffer_free(&buf);
    }
    // VERB 第 4 字节非 ':': "=5\r\ntxtXY\r\n" → PROT_ERROR
    {
        buffer_ctx buf;
        buffer_init(&buf);
        _bput(&buf, "=5\r\ntxtXY\r\n");
        ud_cxt ud;
        ZERO(&ud, sizeof(ud));
        int32_t status = PROT_INIT;
        redis_pack_ctx *pack = redis_unpack(&buf, &ud, &status);
        CuAssertTrue(tc, NULL == pack);
        CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
        _redis_udfree(&ud);
        buffer_free(&buf);
    }
}

// RESP3 聚合类型解包：SET/PUSHE/MAP/ATTR
// 对应 lib/protocol/redis.c:494-499 (_reader_agg)，注意 MAP/ATTR 元素数 = nelem*2
static void test_redis_resp3_aggregate(CuTest *tc) {
    // SET: "~3\r\n+a\r\n+b\r\n+c\r\n"
    {
        buffer_ctx buf;
        buffer_init(&buf);
        _bput(&buf, "~3\r\n+a\r\n+b\r\n+c\r\n");
        ud_cxt ud;
        ZERO(&ud, sizeof(ud));
        int32_t status = PROT_INIT;
        redis_pack_ctx *pack = redis_unpack(&buf, &ud, &status);
        CuAssertPtrNotNull(tc, pack);
        CuAssertTrue(tc, RESP_SET == pack->prot);
        CuAssertTrue(tc, 3 == pack->nelem);
        redis_pack_ctx *p = pack->next;
        const char *exp[] = { "a", "b", "c" };
        for (int i = 0; i < 3; i++) {
            CuAssertPtrNotNull(tc, p);
            CuAssertTrue(tc, RESP_STRING == p->prot);
            CuAssertTrue(tc, 1 == p->len);
            CuAssertTrue(tc, 0 == memcmp(p->data, exp[i], 1));
            p = p->next;
        }
        CuAssertTrue(tc, NULL == p);
        _redis_pkfree(pack);
        _redis_udfree(&ud);
        buffer_free(&buf);
    }
    // PUSHE（发布订阅推送）: ">2\r\n+pub\r\n+msg\r\n"
    {
        buffer_ctx buf;
        buffer_init(&buf);
        _bput(&buf, ">2\r\n+pub\r\n+msg\r\n");
        ud_cxt ud;
        ZERO(&ud, sizeof(ud));
        int32_t status = PROT_INIT;
        redis_pack_ctx *pack = redis_unpack(&buf, &ud, &status);
        CuAssertPtrNotNull(tc, pack);
        CuAssertTrue(tc, RESP_PUSHE == pack->prot);
        CuAssertTrue(tc, 2 == pack->nelem);
        CuAssertPtrNotNull(tc, pack->next);
        CuAssertTrue(tc, 3 == pack->next->len);
        CuAssertTrue(tc, 0 == memcmp(pack->next->data, "pub", 3));
        _redis_pkfree(pack);
        _redis_udfree(&ud);
        buffer_free(&buf);
    }
    // MAP: "%2\r\n+k1\r\n:1\r\n+k2\r\n:2\r\n" — 2 个键值对 = 4 个子节点
    {
        buffer_ctx buf;
        buffer_init(&buf);
        _bput(&buf, "%2\r\n+k1\r\n:1\r\n+k2\r\n:2\r\n");
        ud_cxt ud;
        ZERO(&ud, sizeof(ud));
        int32_t status = PROT_INIT;
        redis_pack_ctx *pack = redis_unpack(&buf, &ud, &status);
        CuAssertPtrNotNull(tc, pack);
        CuAssertTrue(tc, RESP_MAP == pack->prot);
        CuAssertTrue(tc, 2 == pack->nelem);
        // 链表跟随 4 个节点: k1, 1, k2, 2
        redis_pack_ctx *p = pack->next;
        CuAssertPtrNotNull(tc, p);
        CuAssertTrue(tc, 0 == memcmp(p->data, "k1", 2));
        p = p->next;
        CuAssertPtrNotNull(tc, p);
        CuAssertTrue(tc, RESP_INTEGER == p->prot);
        CuAssertTrue(tc, 1 == p->ival);
        p = p->next;
        CuAssertPtrNotNull(tc, p);
        CuAssertTrue(tc, 0 == memcmp(p->data, "k2", 2));
        p = p->next;
        CuAssertPtrNotNull(tc, p);
        CuAssertTrue(tc, RESP_INTEGER == p->prot);
        CuAssertTrue(tc, 2 == p->ival);
        CuAssertTrue(tc, NULL == p->next);
        _redis_pkfree(pack);
        _redis_udfree(&ud);
        buffer_free(&buf);
    }
    // ATTR（附加属性，结构同 MAP，nelem*2）: "|1\r\n+key\r\n+val\r\n+OK\r\n"
    // ATTR 后必须紧跟实际响应（这里是 +OK\r\n）才算完整
    {
        buffer_ctx buf;
        buffer_init(&buf);
        _bput(&buf, "|1\r\n+key\r\n+val\r\n+OK\r\n");
        ud_cxt ud;
        ZERO(&ud, sizeof(ud));
        int32_t status = PROT_INIT;
        redis_pack_ctx *pack = redis_unpack(&buf, &ud, &status);
        CuAssertPtrNotNull(tc, pack);
        CuAssertTrue(tc, RESP_ATTR == pack->prot);
        CuAssertTrue(tc, 1 == pack->nelem);
        redis_pack_ctx *p = pack->next;
        CuAssertPtrNotNull(tc, p);
        CuAssertTrue(tc, 0 == memcmp(p->data, "key", 3));
        p = p->next;
        CuAssertPtrNotNull(tc, p);
        CuAssertTrue(tc, 0 == memcmp(p->data, "val", 3));
        p = p->next;
        CuAssertPtrNotNull(tc, p);
        CuAssertTrue(tc, RESP_STRING == p->prot);
        CuAssertTrue(tc, 0 == memcmp(p->data, "OK", 2));
        CuAssertTrue(tc, NULL == p->next);
        _redis_pkfree(pack);
        _redis_udfree(&ud);
        buffer_free(&buf);
    }
}

/* =======================================================================
 * URL —— url_parse 各字段验证
 * ======================================================================= */

static void test_url_parse(CuTest *tc) {
    /* url_parse 在原始 char 数组上就地操作，不能用 const char * */
    char url[] = "http://user:psw@host.com:8080/path/to?k1=v1&k2=v2#anchor";
    url_ctx ctx;
    url_parse(&ctx, url, strlen(url), '/', 1);

    CuAssertTrue(tc, buf_compare(&ctx.scheme, "http", 4));
    CuAssertTrue(tc, buf_compare(&ctx.user,   "user", 4));
    CuAssertTrue(tc, buf_compare(&ctx.psw,    "psw",  3));
    CuAssertTrue(tc, buf_compare(&ctx.host,   "host.com", 8));
    CuAssertTrue(tc, buf_compare(&ctx.port,   "8080", 4));
    CuAssertTrue(tc, 2 == ctx.npath);
    CuAssertTrue(tc, buf_compare(&ctx.segs[0], "path", 4));
    CuAssertTrue(tc, buf_compare(&ctx.segs[1], "to", 2));
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

// URL 缺 path 但含 query / fragment 时，host 段不可越界到 authority 之后
static void test_url_parse_edges(CuTest *tc) {
    // 1. http://host?k=v —— host 仅含 "host"，query 被正确解析
    {
        char u[] = "http://host?k=v";
        url_ctx ctx;
        url_parse(&ctx, u, strlen(u), '/', 1);
        CuAssertTrue(tc, buf_compare(&ctx.scheme, "http", 4));
        CuAssertTrue(tc, buf_compare(&ctx.host, "host", 4));
        CuAssertTrue(tc, 0 == ctx.port.lens);
        CuAssertTrue(tc, 0 == ctx.npath);
        buf_ctx *p = url_get_param(&ctx, "k");
        CuAssertPtrNotNull(tc, p);
        CuAssertTrue(tc, buf_compare(p, "v", 1));
    }
    // 2. http://host#frag —— host 仅含 "host"，anchor 被正确解析
    {
        char u[] = "http://host#frag";
        url_ctx ctx;
        url_parse(&ctx, u, strlen(u), '/', 1);
        CuAssertTrue(tc, buf_compare(&ctx.scheme, "http", 4));
        CuAssertTrue(tc, buf_compare(&ctx.host, "host", 4));
        CuAssertTrue(tc, 0 == ctx.npath);
        CuAssertTrue(tc, buf_compare(&ctx.anchor, "frag", 4));
    }
    // 3. http://user@host?k=v —— userinfo + host + query 均正确切分
    {
        char u[] = "http://user@host?k=v";
        url_ctx ctx;
        url_parse(&ctx, u, strlen(u), '/', 1);
        CuAssertTrue(tc, buf_compare(&ctx.user, "user", 4));
        CuAssertTrue(tc, 0 == ctx.psw.lens);
        CuAssertTrue(tc, buf_compare(&ctx.host, "host", 4));
        CuAssertTrue(tc, 0 == ctx.npath);
        buf_ctx *p = url_get_param(&ctx, "k");
        CuAssertPtrNotNull(tc, p);
        CuAssertTrue(tc, buf_compare(p, "v", 1));
    }
    // 4. http://host —— 仅 scheme + host
    {
        char u[] = "http://host";
        url_ctx ctx;
        url_parse(&ctx, u, strlen(u), '/', 1);
        CuAssertTrue(tc, buf_compare(&ctx.host, "host", 4));
        CuAssertTrue(tc, 0 == ctx.npath);
        CuAssertTrue(tc, 0 == ctx.anchor.lens);
    }
    // 5. http://host:8080?k=v —— port + query（无 path 但含端口）
    {
        char u[] = "http://host:8080?k=v";
        url_ctx ctx;
        url_parse(&ctx, u, strlen(u), '/', 1);
        CuAssertTrue(tc, buf_compare(&ctx.host, "host", 4));
        CuAssertTrue(tc, buf_compare(&ctx.port, "8080", 4));
        buf_ctx *p = url_get_param(&ctx, "k");
        CuAssertPtrNotNull(tc, p);
        CuAssertTrue(tc, buf_compare(p, "v", 1));
    }
    // 6. http://host/ —— 根路径 '/' → 一个空段(RFC path-abempty:前导 sep 后是空 segment)
    {
        char u[] = "http://host/";
        url_ctx ctx;
        url_parse(&ctx, u, strlen(u), '/', 1);
        CuAssertTrue(tc, buf_compare(&ctx.host, "host", 4));
        CuAssertTrue(tc, 1 == ctx.npath);
        CuAssertTrue(tc, 0 == ctx.segs[0].lens);
    }
    // 7. harbor 风格：/call?dst=N&type=M
    {
        char u[] = "/call?dst=123&type=4";
        url_ctx ctx;
        url_parse(&ctx, u, strlen(u), '/', 1);
        CuAssertTrue(tc, 1 == ctx.npath);
        CuAssertTrue(tc, buf_compare(&ctx.segs[0], "call", 4));
        buf_ctx *p = url_get_param(&ctx, "dst");
        CuAssertPtrNotNull(tc, p);
        CuAssertTrue(tc, buf_compare(p, "123", 3));
        p = url_get_param(&ctx, "type");
        CuAssertPtrNotNull(tc, p);
        CuAssertTrue(tc, buf_compare(p, "4", 1));
    }
    // 8. path + 直接 fragment(无 query)：path 段解码不写 '\0',不覆盖紧跟的 '#',anchor 正确
    {
        char u[] = "/a/b#frag";
        url_ctx ctx;
        url_parse(&ctx, u, strlen(u), '/', 1);
        CuAssertTrue(tc, 2 == ctx.npath);
        CuAssertTrue(tc, buf_compare(&ctx.segs[0], "a", 1));
        CuAssertTrue(tc, buf_compare(&ctx.segs[1], "b", 1));
        CuAssertTrue(tc, buf_compare(&ctx.anchor, "frag", 4));
    }
    // 9. fragment 含 '='：整体归 anchor,不被误扫成 query 参数
    {
        char u[] = "/p#x=1";
        url_ctx ctx;
        url_parse(&ctx, u, strlen(u), '/', 1);
        CuAssertTrue(tc, 1 == ctx.npath);
        CuAssertTrue(tc, buf_compare(&ctx.segs[0], "p", 1));
        CuAssertTrue(tc, buf_compare(&ctx.anchor, "x=1", 3));
        CuAssertTrue(tc, NULL == url_get_param(&ctx, "x"));
    }
    // 10. query 键大小写敏感(url_get_param 用 buf_compare)：注册 Key,查 key 不命中
    {
        char u[] = "/p?Key=v";
        url_ctx ctx;
        url_parse(&ctx, u, strlen(u), '/', 1);
        CuAssertPtrNotNull(tc, url_get_param(&ctx, "Key"));
        CuAssertTrue(tc, NULL == url_get_param(&ctx, "key"));
    }
}

/* url_reorg_param：重组 query 参数字符串（decode=0，保留原始编码） */
static void test_url_reorg_param(CuTest *tc) {
    url_ctx ctx;
    char buf[256];
    size_t n;

    // 1. 无参数 → 返回 0，输出空串
    char u1[] = "http://host/path";
    url_parse(&ctx, u1, strlen(u1), '/', 0);
    n = url_reorg_param(&ctx, buf, sizeof(buf));
    CuAssertIntEquals(tc, 0, (int)n);
    CuAssertStrEquals(tc, "", buf);

    // 2. 单参数
    char u2[] = "http://host/path?k=v";
    url_parse(&ctx, u2, strlen(u2), '/', 0);
    n = url_reorg_param(&ctx, buf, sizeof(buf));
    CuAssertIntEquals(tc, 3, (int)n);
    CuAssertStrEquals(tc, "k=v", buf);

    // 3. 多参数
    char u3[] = "http://host?k1=v1&k2=v2";
    url_parse(&ctx, u3, strlen(u3), '/', 0);
    url_reorg_param(&ctx, buf, sizeof(buf));
    CuAssertStrEquals(tc, "k1=v1&k2=v2", buf);

    // 4. 空值参数
    char u4[] = "/p?k=";
    url_parse(&ctx, u4, strlen(u4), '/', 0);
    url_reorg_param(&ctx, buf, sizeof(buf));
    CuAssertStrEquals(tc, "k=", buf);

    // 5. decode=0：%2F 保留原始编码，不解码
    char u5[] = "/p?k=%2F";
    url_parse(&ctx, u5, strlen(u5), '/', 0);
    url_reorg_param(&ctx, buf, sizeof(buf));
    CuAssertStrEquals(tc, "k=%2F", buf);

    // 6. 容量截断：cap=6 只放第一对 k1=v1（5字节+'\0'），截断 &k2=v2
    char u6[] = "/p?k1=v1&k2=v2";
    url_parse(&ctx, u6, strlen(u6), '/', 0);
    char small[6];
    n = url_reorg_param(&ctx, small, sizeof(small));
    CuAssertStrEquals(tc, "k1=v1", small);
    CuAssertIntEquals(tc, 5, (int)n);

    // 7. url_reorg_path + url_reorg_param 组合：WebSocket URI 场景
    char u7[] = "ws://host/chat?token=abc&v=1";
    url_parse(&ctx, u7, strlen(u7), '/', 0);
    char uribuf[256];
    size_t plen = url_reorg_path(&ctx, uribuf, sizeof(uribuf));
    CuAssertStrEquals(tc, "/chat", uribuf);
    uribuf[plen] = '?';
    url_reorg_param(&ctx, uribuf + plen + 1, sizeof(uribuf) - plen - 1);
    CuAssertStrEquals(tc, "/chat?token=abc&v=1", uribuf);
}

/* =======================================================================
 * Custz —— 三种打包格式往返验证
 * ======================================================================= */

static void _custz_roundtrip(CuTest *tc, pack_type pktype,
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

/* =======================================================================
 * SMTP —— 多行响应识别（_smtp_full_response）
 * 测试目标：覆盖单行 / 多行 / TCP 分包 / 边界 / 协议错误共 20 个 case
 * ======================================================================= */

// 辅助：构造 buffer，跑 helper，断言返回值
static void _smtp_resp_check(CuTest *tc, const char *input, size_t inlen,
                              const char *code, int32_t expected) {
    buffer_ctx buf;
    buffer_init(&buf);
    if (inlen > 0) {
        buffer_append(&buf, (void *)input, inlen);
    }
    int32_t rtn = _smtp_full_response(&buf, code);
    CuAssertIntEquals(tc, expected, rtn);
    buffer_free(&buf);
}

static void test_smtp_full_response(CuTest *tc) {
    /* 1. 空 buffer → 等更多 */
    _smtp_resp_check(tc, "", 0, "220", 0);
    /* 2. 不足 4 字节首行起始 */
    _smtp_resp_check(tc, "22", 2, "220", 0);
    /* 3. 仅 3 字节，缺分隔符 */
    _smtp_resp_check(tc, "220", 3, "220", 0);
    /* 4. 4 字节，无 CRLF */
    _smtp_resp_check(tc, "220 ", 4, "220", 0);
    /* 5. 5 字节，缺 LF */
    _smtp_resp_check(tc, "220 \r", 5, "220", 0);
    /* 6. 6 字节最小完整结束行 */
    _smtp_resp_check(tc, "220 \r\n", 6, "220", 6);
    /* 7. 普通单行完整 */
    _smtp_resp_check(tc, "220 OK\r\n", 8, "220", 8);
    /* 8. 仅中间行（必须等结束行） */
    _smtp_resp_check(tc, "220-host\r\n", 10, "220", 0);
    /* 9. 中间行 + 最小结束行 */
    _smtp_resp_check(tc, "220-host\r\n220 \r\n", 16, "220", 16);
    /* 10. 中间行 + 普通结束行 */
    _smtp_resp_check(tc, "220-host\r\n220 OK\r\n", 18, "220", 18);
    /* 11. 多个中间行 + 结束行 */
    _smtp_resp_check(tc, "220-A\r\n220-B\r\n220 C\r\n", 21, "220", 21);
    /* 12. 结束行后多余字节，helper 应只报到结束行尾 */
    _smtp_resp_check(tc, "220-host\r\n220 OK\r\nEXTRA", 23, "220", 18);
    /* 13. code 不匹配（首行非 220） */
    _smtp_resp_check(tc, "500 Err\r\n", 9, "220", ERR_FAILED);
    /* 14. code 在中间行不一致 */
    _smtp_resp_check(tc, "220-A\r\n500 X\r\n", 14, "220", ERR_FAILED);
    /* 15. 第 4 字节非 '-' / 空格 */
    _smtp_resp_check(tc, "220Xhost\r\n", 10, "220", ERR_FAILED);
    /* 16. 第 2 行截断到 2 字节 */
    _smtp_resp_check(tc, "220-A\r\n22", 9, "220", 0);
    /* 17. 第 2 行截断到 3 字节（缺分隔符） */
    _smtp_resp_check(tc, "220-A\r\n220", 10, "220", 0);
    /* 18. 第 2 行结束行起始但缺 CRLF */
    _smtp_resp_check(tc, "220-A\r\n220 ", 11, "220", 0);
    /* 19. 第 2 行中间行起始但缺 CRLF */
    _smtp_resp_check(tc, "220-A\r\n220-", 11, "220", 0);
    /* 鲁棒性：250 多行（验证 helper 通用、不写死 code） */
    _smtp_resp_check(tc, "250-AUTH LOGIN PLAIN\r\n250 OK\r\n", 30, "250", 30);
    /* 鲁棒性：长正文行 */
    char longline[1100];
    memcpy(longline, "220-", 4);
    memset(longline + 4, 'X', 1024);
    memcpy(longline + 4 + 1024, "\r\n220 OK\r\n", 10);
    _smtp_resp_check(tc, longline, 4 + 1024 + 10, "220", 4 + 1024 + 10);
    /* 20. 握手续行洪泛超 MAX_PACK_SIZE → 拒绝（防恶意 server 续行耗内存） */
    size_t units = (size_t)MAX_PACK_SIZE / 6 + 1; /* 每单元 "220-\r\n" 6 字节 */
    size_t floodlen = units * 6;
    char *flood;
    MALLOC(flood, floodlen);
    for (size_t fi = 0; fi < units; fi++) {
        memcpy(flood + fi * 6, "220-\r\n", 6);
    }
    _smtp_resp_check(tc, flood, floodlen, "220", ERR_FAILED);
    FREE(flood);
    /* 21. 裸结束行 "<code>\r\n"（code 后直接 CRLF，无 sep/text） */
    _smtp_resp_check(tc, "220\r\n", 5, "220", 5);
    /* 22. code 为 NULL：以首行 code 为准，单行 */
    _smtp_resp_check(tc, "354 go\r\n", 8, NULL, 8);
    /* 23. code 为 NULL：多行，首行 code 作后续行校验基准 */
    _smtp_resp_check(tc, "250-A\r\n250 B\r\n", 14, NULL, 14);
    /* 24. code 为 NULL：裸结束行 */
    _smtp_resp_check(tc, "421\r\n", 5, NULL, 5);
    /* 25. code 为 NULL：续行 code 与首行不一致 → 拒绝 */
    _smtp_resp_check(tc, "250-A\r\n500 X\r\n", 14, NULL, ERR_FAILED);
}

// mail_pack 必须对 mail->msg 做 dot-stuffing（RFC 5321 §4.5.2）
// 防 SMTP Smuggling 攻击（CVE-2023-51764 / CVE-2023-51765 同模式）
static void test_smtp_dot_stuffing(CuTest *tc) {
    // 1. 攻击载荷：用户输入含 <CRLF>.<CRLF> + 伪造 SMTP 命令
    {
        mail_ctx mail;
        mail_init(&mail);
        mail_from(&mail, NULL, "alice@example.com");
        mail_addrs_add(&mail, "bob@example.com", TO);
        mail_subject(&mail, "test");
        mail_msg(&mail, "hello\r\n.\r\nMAIL FROM:<evil@attacker>\r\nRCPT TO:<victim>\r\nDATA\r\nworld");

        char *out = mail_pack(&mail);
        CuAssertPtrNotNull(tc, out);
        // 攻击 pattern（\r\n.\r\nMAIL）不应出现 — 否则 SMTP smuggling
        CuAssertTrue(tc, NULL == strstr(out, "\r\n.\r\nMAIL FROM:<evil@attacker>"));
        // 转义后（\r\n..\r\nMAIL）应出现
        CuAssertTrue(tc, NULL != strstr(out, "\r\n..\r\nMAIL FROM:<evil@attacker>"));

        FREE(out);
        mail_free(&mail);
    }
    // 2. 正文以 '.' 开头 — 应转为 ".."
    {
        mail_ctx mail;
        mail_init(&mail);
        mail_from(&mail, NULL, "alice@example.com");
        mail_addrs_add(&mail, "bob@example.com", TO);
        mail_subject(&mail, "test");
        mail_msg(&mail, ".dotted line");

        char *out = mail_pack(&mail);
        CuAssertPtrNotNull(tc, out);
        // Subject: test\r\n\r\n 之后即 body 起始
        char *body = strstr(out, "Subject: test\r\n\r\n");
        CuAssertPtrNotNull(tc, body);
        body += strlen("Subject: test\r\n\r\n");
        // body 首字符应是 ".."（转义后）而非孤立 "."
        CuAssertTrue(tc, '.' == body[0] && '.' == body[1] && 'd' == body[2]);

        FREE(out);
        mail_free(&mail);
    }
    // 3. 普通文本（无行首 '.'）— 不应被修改
    {
        mail_ctx mail;
        mail_init(&mail);
        mail_from(&mail, NULL, "alice@example.com");
        mail_addrs_add(&mail, "bob@example.com", TO);
        mail_subject(&mail, "test");
        mail_msg(&mail, "hello\r\nworld");

        char *out = mail_pack(&mail);
        CuAssertPtrNotNull(tc, out);
        // 原文应原样出现
        CuAssertTrue(tc, NULL != strstr(out, "hello\r\nworld"));
        // 不应有额外 '.' 插入
        CuAssertTrue(tc, NULL == strstr(out, "hello\r\n."));

        FREE(out);
        mail_free(&mail);
    }
    // 4. 连续多行均以 '.' 开头 — 每行都应转义
    {
        mail_ctx mail;
        mail_init(&mail);
        mail_from(&mail, NULL, "alice@example.com");
        mail_addrs_add(&mail, "bob@example.com", TO);
        mail_subject(&mail, "test");
        mail_msg(&mail, "line1\r\n.line2\r\n.line3");

        char *out = mail_pack(&mail);
        CuAssertPtrNotNull(tc, out);
        // 两处行首 '.' 都应转义
        CuAssertTrue(tc, NULL != strstr(out, "line1\r\n..line2\r\n..line3"));

        FREE(out);
        mail_free(&mail);
    }
    // 5. bare LF 注入：mail_msg 入口规范化为 CRLF，后续 dot-stuffing 才拦截到攻击
    {
        mail_ctx mail;
        mail_init(&mail);
        mail_from(&mail, NULL, "alice@example.com");
        mail_addrs_add(&mail, "bob@example.com", TO);
        mail_subject(&mail, "test");
        // 容错 server 把 \n 视为行终止 → \n. 被识别为行首 '.' → \r\n 终止 DATA → smuggling
        mail_msg(&mail, "hello\n.\r\nMAIL FROM:<evil@attacker>\r\nDATA\r\nworld");

        char *out = mail_pack(&mail);
        CuAssertPtrNotNull(tc, out);
        // 攻击 pattern 不应残留：bare LF 已规范化为 CRLF，行首 . 已 dot-stuff
        CuAssertTrue(tc, NULL == strstr(out, "\n.\r\nMAIL FROM:<evil@attacker>"));
        CuAssertTrue(tc, NULL != strstr(out, "hello\r\n..\r\nMAIL FROM:<evil@attacker>"));

        FREE(out);
        mail_free(&mail);
    }
    // 6. bare CR 注入：同上
    {
        mail_ctx mail;
        mail_init(&mail);
        mail_from(&mail, NULL, "alice@example.com");
        mail_addrs_add(&mail, "bob@example.com", TO);
        mail_subject(&mail, "test");
        mail_msg(&mail, "hello\r.\r\nMAIL FROM:<evil@attacker>\r\nDATA");

        char *out = mail_pack(&mail);
        CuAssertPtrNotNull(tc, out);
        CuAssertTrue(tc, NULL != strstr(out, "hello\r\n..\r\nMAIL FROM:<evil@attacker>"));

        FREE(out);
        mail_free(&mail);
    }
    // 7. 合法 CRLF 不应被复述（规范化不破坏既有 CRLF）
    {
        mail_ctx mail;
        mail_init(&mail);
        mail_from(&mail, NULL, "alice@example.com");
        mail_addrs_add(&mail, "bob@example.com", TO);
        mail_subject(&mail, "test");
        mail_msg(&mail, "line1\r\nline2\r\nline3");

        char *out = mail_pack(&mail);
        CuAssertPtrNotNull(tc, out);
        // 合法 CRLF 原样保留，不变形为 \r\n\r\n
        CuAssertTrue(tc, NULL != strstr(out, "line1\r\nline2\r\nline3"));
        CuAssertTrue(tc, NULL == strstr(out, "line1\r\n\r\nline2"));

        FREE(out);
        mail_free(&mail);
    }
}

/* =======================================================================
 * DNS —— 请求组包、响应解包、TCP 长度前缀解析
 * ======================================================================= */

static void test_dns_request_pack(CuTest *tc) {
    char buf[256];
    /* example.com A 查询 */
    size_t n = dns_request_pack(buf, "example.com", 0);
    /* 12(head) + 13(label:\x07example\x03com\x00) + 4(question) = 29 */
    CuAssertTrue(tc, 29 == (int)n);

    /* 标签段：从 offset 12 起 */
    CuAssertTrue(tc, 0x07 == (uint8_t)buf[12]);
    CuAssertTrue(tc, 0 == memcmp(buf + 13, "example", 7));
    CuAssertTrue(tc, 0x03 == (uint8_t)buf[20]);
    CuAssertTrue(tc, 0 == memcmp(buf + 21, "com", 3));
    CuAssertTrue(tc, 0x00 == (uint8_t)buf[24]);

    /* qtype = A(1) 大端，qclass = IN(1) 大端 */
    CuAssertTrue(tc, 0 == (uint8_t)buf[25] && 1 == (uint8_t)buf[26]);
    CuAssertTrue(tc, 0 == (uint8_t)buf[27] && 1 == (uint8_t)buf[28]);

    /* flags1 = 0x01（RD），flags2 = 0 */
    CuAssertTrue(tc, 0x01 == (uint8_t)buf[2]);
    CuAssertTrue(tc, 0x00 == (uint8_t)buf[3]);
    /* q_count = 1（大端） */
    CuAssertTrue(tc, 0 == (uint8_t)buf[4] && 1 == (uint8_t)buf[5]);

    /* ipv6=1 → qtype=AAAA(28) */
    n = dns_request_pack(buf, "example.com", 1);
    CuAssertTrue(tc, 29 == (int)n);
    CuAssertTrue(tc, 0 == (uint8_t)buf[25] && 28 == (uint8_t)buf[26]);
}

static void test_dns_request_pack_tcp(CuTest *tc) {
    char buf[256];
    size_t n = dns_request_pack_tcp(buf, "example.com", 0);
    /* TCP 前置 2 字节长度 + UDP 形态相同 */
    CuAssertTrue(tc, 2 + 29 == (int)n);

    /* 前 2 字节大端长度 = 29 */
    uint16_t plen = (uint16_t)(((uint8_t)buf[0] << 8) | (uint8_t)buf[1]);
    CuAssertTrue(tc, 29 == plen);

    /* 后续与 UDP 形态一致：第 14 字节起为 "example" */
    CuAssertTrue(tc, 0 == memcmp(buf + 2 + 13, "example", 7));
}

static void test_dns_unpack(CuTest *tc) {
    /* 模拟 TCP 流：2 字节长度 + N 字节 payload */
    char body[] = "dnsbody!";
    size_t bsize = strlen(body);

    buffer_ctx buf;
    buffer_init(&buf);
    uint8_t hdr[2];
    hdr[0] = (uint8_t)(bsize >> 8);
    hdr[1] = (uint8_t)(bsize & 0xff);
    buffer_append(&buf, hdr, 2);
    buffer_append(&buf, body, bsize);

    size_t out = 0;
    int32_t status = PROT_INIT;
    void *p = dns_unpack(&buf, &out, &status);
    CuAssertPtrNotNull(tc, p);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertTrue(tc, bsize == out);
    CuAssertTrue(tc, 0 == memcmp(p, body, bsize));
    /* buffer 已消费完 */
    CuAssertTrue(tc, 0 == buffer_size(&buf));
    FREE(p);

    /* 数据不足：仅 2 字节头 → PROT_MOREDATA */
    buffer_append(&buf, hdr, 2);
    status = PROT_INIT;
    p = dns_unpack(&buf, &out, &status);
    CuAssertTrue(tc, NULL == p);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_MOREDATA));
    buffer_free(&buf);

    /* length=0 视为协议错误 */
    buffer_init(&buf);
    uint8_t zero_hdr[2] = { 0, 0 };
    buffer_append(&buf, zero_hdr, 2);
    status = PROT_INIT;
    p = dns_unpack(&buf, &out, &status);
    CuAssertTrue(tc, NULL == p);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
    buffer_free(&buf);
}

static void test_dns_parse_pack(CuTest *tc) {
    /* 构造一个完整 DNS 响应：example.com A 1.2.3.4 */
    uint8_t resp[] = {
        /* head: id=0x1234, flags=0x8180(response+RD+RA, rcode=0),
         * qd=1, an=1, ns=0, ar=0 */
        0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00,
        /* query: \x07example\x03com\x00 + qtype=A(1) + qclass=IN(1) */
        0x07, 'e','x','a','m','p','l','e',
        0x03, 'c','o','m', 0x00,
        0x00, 0x01, 0x00, 0x01,
        /* answer: 压缩指针 \xc0\x0c 指向偏移 12
         * + type=A + class=IN + ttl=300 + rdlen=4 + 1.2.3.4 */
        0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
        0x01, 0x2c, 0x00, 0x04,
        0x01, 0x02, 0x03, 0x04
    };
    size_t cnt = 0;
    dns_ip *ips = dns_parse_pack((char *)resp, sizeof(resp), &cnt);
    CuAssertPtrNotNull(tc, ips);
    CuAssertTrue(tc, cnt >= 1);
    CuAssertStrEquals(tc, "1.2.3.4", ips[0].ip);
    FREE(ips);

    /* 恶意放大：仅 12 字节头 + 计数字段全 0xFFFF（total=196605），无 RR 体。
       修复后按报文剩余字节(0)限上界 → total=0 → 返回 NULL，不再 MALLOC ~12.6MB */
    uint8_t evil[] = {
        0x00, 0x00, 0x81, 0x80,   /* id / flags(response, rcode=0) */
        0x00, 0x00,               /* qd_count=0（无 question）*/
        0xFF, 0xFF,               /* an_count=65535 */
        0xFF, 0xFF,               /* ns_count=65535 */
        0xFF, 0xFF                /* ar_count=65535 */
    };
    size_t ecnt = 0;
    dns_ip *eips = dns_parse_pack((char *)evil, sizeof(evil), &ecnt);
    CuAssertTrue(tc, NULL == eips);
    CuAssertTrue(tc, 0 == ecnt);
}

// DNS-31：查询段截断包拒绝验证
// 压缩指针仅 1 字节 / label 长度字段超缓冲区 → dns_parse_pack 应返回 NULL
static void test_dns_parse_pack_truncated_query(CuTest *tc) {
    size_t cnt;
    dns_ip *ips;
    // 压缩指针截断：0xC0 后缺第二字节
    uint8_t trunc_ptr[] = {
        0x00, 0x00, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        0xC0
    };
    cnt = 0;
    ips = dns_parse_pack((char *)trunc_ptr, sizeof(trunc_ptr), &cnt);
    CuAssertTrue(tc, NULL == ips);
    // label 截断：length=5 但缓冲区仅剩 3 字节数据
    uint8_t trunc_label[] = {
        0x00, 0x00, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x05, 'a', 'b', 'c'
    };
    cnt = 0;
    ips = dns_parse_pack((char *)trunc_label, sizeof(trunc_label), &cnt);
    CuAssertTrue(tc, NULL == ips);
}
static void test_dns_set_get_ip(CuTest *tc) {
    const char *prev = dns_get_ip();
    char saved[64];
    SNPRINTF(saved, sizeof(saved), "%s", prev ? prev : "");

    dns_set_ip("1.2.3.4");
    CuAssertStrEquals(tc, "1.2.3.4", dns_get_ip());
    dns_set_ip("8.8.4.4");
    CuAssertStrEquals(tc, "8.8.4.4", dns_get_ip());

    /* 还原原值（main 中已设过 8.8.8.8）*/
    dns_set_ip(saved);
}

/* =======================================================================
 * custz_head —— 三种长度头编解码往返
 * ======================================================================= */

static void _custz_head_roundtrip_fixed(CuTest *tc, size_t dlens) {
    size_t hlens = 0, size = 0;
    char *pack = _custz_encode_fixed(dlens, &hlens, &size);
    CuAssertPtrNotNull(tc, pack);
    /* 固定头始终 4 字节 */
    CuAssertTrue(tc, 4 == (int)hlens);
    CuAssertTrue(tc, hlens + dlens == size);

    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, pack, size);
    FREE(pack);

    size_t out_hlens = 0, out_size = 0;
    int32_t status = PROT_INIT;
    CuAssertIntEquals(tc, ERR_OK,
        _custz_decode_fixed(&buf, &out_hlens, &out_size, &status));
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertTrue(tc, 4 == (int)out_hlens);
    CuAssertTrue(tc, dlens == out_size);

    buffer_free(&buf);
}

static void test_custz_head_fixed(CuTest *tc) {
    _custz_head_roundtrip_fixed(tc, 0);
    _custz_head_roundtrip_fixed(tc, 5);
    _custz_head_roundtrip_fixed(tc, 65535);
    _custz_head_roundtrip_fixed(tc, 100000);

    /* 数据不足：buffer 只有 2 字节 → PROT_MOREDATA */
    buffer_ctx buf;
    buffer_init(&buf);
    uint8_t partial[2] = { 0, 0 };
    buffer_append(&buf, partial, 2);
    size_t h, s;
    int32_t status = PROT_INIT;
    CuAssertIntEquals(tc, ERR_FAILED,
        _custz_decode_fixed(&buf, &h, &s, &status));
    CuAssertTrue(tc, BIT_CHECK(status, PROT_MOREDATA));
    buffer_free(&buf);
}

static void _custz_head_roundtrip_flag(CuTest *tc, size_t dlens, size_t expected_hlens) {
    size_t hlens = 0, size = 0;
    char *pack = _custz_encode_flag(dlens, &hlens, &size);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, expected_hlens == hlens);
    CuAssertTrue(tc, hlens + dlens == size);

    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, pack, size);
    FREE(pack);

    size_t out_hlens = 0, out_size = 0;
    int32_t status = PROT_INIT;
    CuAssertIntEquals(tc, ERR_OK,
        _custz_decode_flag(&buf, &out_hlens, &out_size, &status));
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertTrue(tc, expected_hlens == out_hlens);
    CuAssertTrue(tc, dlens == out_size);

    buffer_free(&buf);
}

static void test_custz_head_flag(CuTest *tc) {
    /* dlens<=0xfc → 1 字节头 */
    _custz_head_roundtrip_flag(tc, 0, 1);
    _custz_head_roundtrip_flag(tc, 100, 1);
    _custz_head_roundtrip_flag(tc, 0xfc, 1);

    /* dlens 在 (0xfc, 0xffff] → 3 字节头（0xfd + 2 字节）*/
    _custz_head_roundtrip_flag(tc, 0xfd, 3);
    _custz_head_roundtrip_flag(tc, 0xffff, 3);

    /* dlens 在 (0xffff, UINT_MAX] → 5 字节头（0xfe + 4 字节）
     * 实际分配只能在测试中受内存限制，构造一个 0x20000 的 case 即可 */
    _custz_head_roundtrip_flag(tc, 0x20000, 5);

    /* PROT_MOREDATA：flag=0xfd 但只发了 2 字节 */
    buffer_ctx buf;
    buffer_init(&buf);
    uint8_t partial[2] = { 0xfd, 0x00 };
    buffer_append(&buf, partial, 2);
    size_t h, s;
    int32_t status = PROT_INIT;
    CuAssertIntEquals(tc, ERR_FAILED,
        _custz_decode_flag(&buf, &h, &s, &status));
    CuAssertTrue(tc, BIT_CHECK(status, PROT_MOREDATA));
    buffer_free(&buf);
}

static void _custz_head_roundtrip_variable(CuTest *tc, size_t dlens, size_t expected_hlens) {
    size_t hlens = 0, size = 0;
    char *pack = _custz_encode_variable(dlens, &hlens, &size);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, expected_hlens == hlens);
    CuAssertTrue(tc, hlens + dlens == size);

    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, pack, size);
    FREE(pack);

    size_t out_hlens = 0, out_size = 0;
    int32_t status = PROT_INIT;
    CuAssertIntEquals(tc, ERR_OK,
        _custz_decode_variable(&buf, &out_hlens, &out_size, &status));
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertTrue(tc, expected_hlens == out_hlens);
    CuAssertTrue(tc, dlens == out_size);

    buffer_free(&buf);
}

static void test_custz_head_variable(CuTest *tc) {
    /* MQTT 风格变长：1 字节 [0,127]，2 字节 [128,16383]，3 字节 [16384,2097151]，4 字节 [2097152,268435455] */
    _custz_head_roundtrip_variable(tc, 0, 1);
    _custz_head_roundtrip_variable(tc, 127, 1);
    _custz_head_roundtrip_variable(tc, 128, 2);
    _custz_head_roundtrip_variable(tc, 16383, 2);
    _custz_head_roundtrip_variable(tc, 16384, 3);
    _custz_head_roundtrip_variable(tc, 2097151, 3);

    /* 超过上限返回 NULL */
    size_t h, s;
    char *pack = _custz_encode_variable(268435456, &h, &s);
    CuAssertTrue(tc, NULL == pack);

    /* PROT_ERROR：4 字节都有延续位 */
    buffer_ctx buf;
    buffer_init(&buf);
    uint8_t bad[4] = { 0x80, 0x80, 0x80, 0x80 };
    buffer_append(&buf, bad, 4);
    int32_t status = PROT_INIT;
    CuAssertIntEquals(tc, ERR_FAILED,
        _custz_decode_variable(&buf, &h, &s, &status));
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
    buffer_free(&buf);
}

/* =======================================================================
 * WebSocket —— 帧组包格式验证
 * ======================================================================= */

static void test_websock_pack_frames(CuTest *tc) {
    size_t size = 0;
    /* PING 无掩码：byte0 = FIN|opcode(0x9) = 0x89，byte1 = 0x00（无 payload）*/
    void *p = websock_pack_ping(0, &size);
    CuAssertPtrNotNull(tc, p);
    CuAssertTrue(tc, 2 == (int)size);
    CuAssertTrue(tc, 0x89 == ((uint8_t *)p)[0]);
    CuAssertTrue(tc, 0x00 == ((uint8_t *)p)[1]);
    FREE(p);

    /* PONG 无掩码：byte0=0x8a */
    p = websock_pack_pong(0, &size);
    CuAssertPtrNotNull(tc, p);
    CuAssertTrue(tc, 0x8a == ((uint8_t *)p)[0]);
    FREE(p);

    /* CLOSE 无掩码：byte0=0x88 */
    p = websock_pack_close(0, &size);
    CuAssertPtrNotNull(tc, p);
    CuAssertTrue(tc, 0x88 == ((uint8_t *)p)[0]);
    FREE(p);

    /* PING 带掩码：byte1 高位置位 0x80，长度 4 字节掩码键 */
    p = websock_pack_ping(1, &size);
    CuAssertPtrNotNull(tc, p);
    CuAssertTrue(tc, 2 + 4 == (int)size);
    CuAssertTrue(tc, 0x89 == ((uint8_t *)p)[0]);
    CuAssertTrue(tc, 0x80 == ((uint8_t *)p)[1]);
    FREE(p);

    /* TEXT 完整帧（fin=1）：byte0=0x81，byte1=5，payload="hello" */
    p = websock_pack_text(0, 1, "hello", 5, &size);
    CuAssertPtrNotNull(tc, p);
    CuAssertTrue(tc, 7 == (int)size);
    CuAssertTrue(tc, 0x81 == ((uint8_t *)p)[0]);
    CuAssertTrue(tc, 0x05 == ((uint8_t *)p)[1]);
    CuAssertTrue(tc, 0 == memcmp((uint8_t *)p + 2, "hello", 5));
    FREE(p);

    /* TEXT 分片起始（fin=0）：byte0=0x01 */
    p = websock_pack_text(0, 0, "hel", 3, &size);
    CuAssertPtrNotNull(tc, p);
    CuAssertTrue(tc, 0x01 == ((uint8_t *)p)[0]);
    FREE(p);

    /* BINARY fin=1：byte0=0x82 */
    p = websock_pack_binary(0, 1, "bin", 3, &size);
    CuAssertPtrNotNull(tc, p);
    CuAssertTrue(tc, 0x82 == ((uint8_t *)p)[0]);
    FREE(p);

    /* CONTINUA fin=0：byte0=0x00 */
    p = websock_pack_continua(0, 0, "mid", 3, &size);
    CuAssertPtrNotNull(tc, p);
    CuAssertTrue(tc, 0x00 == ((uint8_t *)p)[0]);
    FREE(p);

    /* CONTINUA fin=1（结束帧）：byte0=0x80 */
    p = websock_pack_continua(0, 1, "end", 3, &size);
    CuAssertPtrNotNull(tc, p);
    CuAssertTrue(tc, 0x80 == ((uint8_t *)p)[0]);
    FREE(p);

    /* 扩展长度：payload=126 字节 → byte1=126，后跟 2 字节 BE 长度 */
    char data126[126];
    memset(data126, 'x', sizeof(data126));
    p = websock_pack_text(0, 1, data126, sizeof(data126), &size);
    CuAssertPtrNotNull(tc, p);
    CuAssertTrue(tc, 2 + 2 + 126 == (int)size);
    CuAssertTrue(tc, 126 == ((uint8_t *)p)[1]);
    /* 大端长度 0x007e */
    CuAssertTrue(tc, 0x00 == ((uint8_t *)p)[2] && 0x7e == ((uint8_t *)p)[3]);
    FREE(p);
}

static void test_websock_pack_handshake(CuTest *tc) {
    char signkey[WS_SIGN_KEY_LENS];
    ZERO(signkey, sizeof(signkey));
    char *req = websock_pack_handshake("example.com", NULL, "mqtt", signkey);
    CuAssertPtrNotNull(tc, req);

    /* 握手请求必须含 GET / Upgrade / Sec-WebSocket-Version 等关键头 */
    CuAssertTrue(tc, NULL != strstr(req, "GET "));
    CuAssertTrue(tc, NULL != strstr(req, "Upgrade: websocket"));
    CuAssertTrue(tc, NULL != strstr(req, "Connection: "));
    CuAssertTrue(tc, NULL != strstr(req, "Sec-WebSocket-Key:"));
    CuAssertTrue(tc, NULL != strstr(req, "Sec-WebSocket-Version: 13"));
    CuAssertTrue(tc, NULL != strstr(req, "Host: example.com"));
    /* 子协议字段写入 */
    CuAssertTrue(tc, NULL != strstr(req, "Sec-WebSocket-Protocol: mqtt"));

    /* signkey 是 base64(sha1(key+GUID))，应非空 */
    CuAssertTrue(tc, 0 != signkey[0]);
    FREE(req);
}

/* 内部 websock_ctx 仅在 websock.c 中定义，测试中只复制布局够用的字段 */
typedef struct test_ws_ctx {
    int8_t slice;
    pack_type secprot;
    buffer_ctx *buf;
    ud_cxt *ud;
    void *pack;
} test_ws_ctx;

static void test_websock_unpack_text(CuTest *tc) {
    /* 客户端解服务端无 mask 的 TEXT 帧 */
    size_t size = 0;
    void *frame = websock_pack_text(0, 1, "hello", 5, &size);
    CuAssertPtrNotNull(tc, frame);

    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, frame, size);
    FREE(frame);

    /* 构造一个最小可用的 websock_ctx 实例 */
    test_ws_ctx ws;
    ZERO(&ws, sizeof(ws));
    ws.secprot = PACK_NONE;

    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = 1; /* websock 内部 START 状态 */
    ud.context = &ws;

    int32_t status = PROT_INIT;
    struct websock_pack_ctx *pack = websock_unpack(NULL, INVALID_SOCK, 0,
        1 /*client=true*/, &buf, &ud, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));

    CuAssertIntEquals(tc, 1, websock_fin(pack));
    CuAssertIntEquals(tc, WS_TEXT, websock_prot(pack));

    size_t dlens = 0;
    char *data = websock_data(pack, &dlens);
    CuAssertTrue(tc, 5 == (int)dlens);
    CuAssertTrue(tc, 0 == memcmp(data, "hello", 5));

    _websock_pkfree(pack);
    buffer_free(&buf);
}

static void test_websock_unpack_masked(CuTest *tc) {
    /* 服务端解客户端带 mask 的 BINARY 帧，验证 xor 解掩码 */
    size_t size = 0;
    char payload[] = { 0xde, 0xad, 0xbe, 0xef };
    void *frame = websock_pack_binary(1 /*mask*/, 1, payload, sizeof(payload), &size);
    CuAssertPtrNotNull(tc, frame);
    /* 2 + 4(mask) + 4(data) = 10 */
    CuAssertTrue(tc, 10 == (int)size);

    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, frame, size);
    FREE(frame);

    test_ws_ctx ws;
    ZERO(&ws, sizeof(ws));
    ws.secprot = PACK_NONE;

    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = 1; /* START */
    ud.context = &ws;

    int32_t status = PROT_INIT;
    struct websock_pack_ctx *pack = websock_unpack(NULL, INVALID_SOCK, 0,
        0 /*server*/, &buf, &ud, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertIntEquals(tc, WS_BINARY, websock_prot(pack));

    size_t dlens = 0;
    char *data = websock_data(pack, &dlens);
    CuAssertTrue(tc, sizeof(payload) == dlens);
    CuAssertTrue(tc, 0 == memcmp(data, payload, sizeof(payload)));

    _websock_pkfree(pack);
    buffer_free(&buf);
}

static void test_websock_unpack_fragmented(CuTest *tc) {
    /* 三帧分片：TEXT(fin=0) + CONTINUE(fin=0) + CONTINUE(fin=1)，无 mask */
    size_t s1, s2, s3;
    void *f1 = websock_pack_text(0, 0, "AAA", 3, &s1);
    void *f2 = websock_pack_continua(0, 0, "BBB", 3, &s2);
    void *f3 = websock_pack_continua(0, 1, "CCC", 3, &s3);

    test_ws_ctx ws;
    ZERO(&ws, sizeof(ws));
    ws.secprot = PACK_NONE;

    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = 1;
    ud.context = &ws;

    /* 第一帧：起始 → PROT_SLICE_START */
    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, f1, s1);
    FREE(f1);

    int32_t status = PROT_INIT;
    struct websock_pack_ctx *pack = websock_unpack(NULL, INVALID_SOCK, 0, 1, &buf, &ud, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_SLICE_START));
    _websock_pkfree(pack);

    /* 第二帧：中间 → PROT_SLICE */
    buffer_append(&buf, f2, s2);
    FREE(f2);
    status = PROT_INIT;
    pack = websock_unpack(NULL, INVALID_SOCK, 0, 1, &buf, &ud, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_SLICE));
    _websock_pkfree(pack);

    /* 第三帧：结束 → PROT_SLICE_END */
    buffer_append(&buf, f3, s3);
    FREE(f3);
    status = PROT_INIT;
    pack = websock_unpack(NULL, INVALID_SOCK, 0, 1, &buf, &ud, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_SLICE_END));
    _websock_pkfree(pack);

    buffer_free(&buf);
}

static void test_websock_unpack_close(CuTest *tc) {
    size_t size = 0;
    void *frame = websock_pack_close(0, &size);
    CuAssertPtrNotNull(tc, frame);

    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, frame, size);
    FREE(frame);

    test_ws_ctx ws;
    ZERO(&ws, sizeof(ws));
    ws.secprot = PACK_NONE;

    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = 1;
    ud.context = &ws;

    int32_t status = PROT_INIT;
    struct websock_pack_ctx *pack = websock_unpack(NULL, INVALID_SOCK, 0, 1, &buf, &ud, &status);
    CuAssertPtrNotNull(tc, pack);
    /* CLOSE 帧解出后 status 含 PROT_CLOSE */
    CuAssertTrue(tc, BIT_CHECK(status, PROT_CLOSE));
    CuAssertIntEquals(tc, WS_CLOSE, websock_prot(pack));
    _websock_pkfree(pack);
    buffer_free(&buf);
}

// 服务端收到无掩码客户端帧应触发 PROT_ERROR（RFC 6455 §5.1）
static void test_websock_unpack_server_no_mask(CuTest *tc) {
    size_t size = 0;
    // 客户端本应带 mask 但这里用 mask=0 故意构造非法帧
    void *frame = websock_pack_text(0, 1, "hi", 2, &size);
    CuAssertPtrNotNull(tc, frame);
    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, frame, size);
    FREE(frame);

    test_ws_ctx ws;
    ZERO(&ws, sizeof(ws));
    ws.secprot = PACK_NONE;
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = 1;
    ud.context = &ws;

    int32_t status = PROT_INIT;
    struct websock_pack_ctx *pack = websock_unpack(NULL, INVALID_SOCK, 0,
        0 /*server*/, &buf, &ud, &status);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
    buffer_free(&buf);
}

// 非法 opcode（保留范围 0x3-0x7 和 >= 0xB）应触发 PROT_ERROR
static void test_websock_unpack_reserved_opcode(CuTest *tc) {
    test_ws_ctx ws;
    ud_cxt ud;
    buffer_ctx buf;

    // 保留数据帧 opcode 0x3：byte0 = 0x83 (FIN=1 + opcode=3), byte1 = 0
    uint8_t reserved3[2] = { 0x83, 0x00 };
    buffer_init(&buf);
    buffer_append(&buf, reserved3, sizeof(reserved3));
    ZERO(&ws, sizeof(ws));
    ws.secprot = PACK_NONE;
    ZERO(&ud, sizeof(ud));
    ud.status = 1;
    ud.context = &ws;
    int32_t status = PROT_INIT;
    struct websock_pack_ctx *pack = websock_unpack(NULL, INVALID_SOCK, 0, 1, &buf, &ud, &status);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
    buffer_free(&buf);

    // 保留控制帧 opcode 0xB：byte0 = 0x8B
    uint8_t reservedB[2] = { 0x8B, 0x00 };
    buffer_init(&buf);
    buffer_append(&buf, reservedB, sizeof(reservedB));
    ZERO(&ws, sizeof(ws));
    ws.secprot = PACK_NONE;
    ZERO(&ud, sizeof(ud));
    ud.status = 1;
    ud.context = &ws;
    status = PROT_INIT;
    pack = websock_unpack(NULL, INVALID_SOCK, 0, 1, &buf, &ud, &status);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
    buffer_free(&buf);
}

// RSV 标志位（bit 4/5/6）置 1 应触发 PROT_ERROR
static void test_websock_unpack_rsv_set(CuTest *tc) {
    test_ws_ctx ws;
    ud_cxt ud;
    buffer_ctx buf;
    // FIN=1 + RSV1=1 + opcode=TEXT(1)：0xC1
    uint8_t rsv1[2] = { 0xC1, 0x00 };
    buffer_init(&buf);
    buffer_append(&buf, rsv1, sizeof(rsv1));
    ZERO(&ws, sizeof(ws));
    ws.secprot = PACK_NONE;
    ZERO(&ud, sizeof(ud));
    ud.status = 1;
    ud.context = &ws;
    int32_t status = PROT_INIT;
    struct websock_pack_ctx *pack = websock_unpack(NULL, INVALID_SOCK, 0, 1, &buf, &ud, &status);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
    buffer_free(&buf);
}

// 控制帧不可分片（FIN=0 + opcode=PING）应触发 PROT_ERROR
static void test_websock_unpack_control_fragmented(CuTest *tc) {
    test_ws_ctx ws;
    ud_cxt ud;
    buffer_ctx buf;
    // FIN=0 + opcode=PING(9)：0x09
    uint8_t bad[2] = { 0x09, 0x00 };
    buffer_init(&buf);
    buffer_append(&buf, bad, sizeof(bad));
    ZERO(&ws, sizeof(ws));
    ws.secprot = PACK_NONE;
    ZERO(&ud, sizeof(ud));
    ud.status = 1;
    ud.context = &ws;
    int32_t status = PROT_INIT;
    struct websock_pack_ctx *pack = websock_unpack(NULL, INVALID_SOCK, 0, 1, &buf, &ud, &status);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
    buffer_free(&buf);
}

// 控制帧 payload > 125 应触发 PROT_ERROR
static void test_websock_unpack_control_too_big(CuTest *tc) {
    test_ws_ctx ws;
    ud_cxt ud;
    buffer_ctx buf;
    // FIN=1 + opcode=PING(9) + payloadlen=126 (扩展长度标志)：0x89 0x7E
    uint8_t bad[4] = { 0x89, 0x7E, 0x00, 0x80 }; // 128 字节
    buffer_init(&buf);
    buffer_append(&buf, bad, sizeof(bad));
    ZERO(&ws, sizeof(ws));
    ws.secprot = PACK_NONE;
    ZERO(&ud, sizeof(ud));
    ud.status = 1;
    ud.context = &ws;
    int32_t status = PROT_INIT;
    struct websock_pack_ctx *pack = websock_unpack(NULL, INVALID_SOCK, 0, 1, &buf, &ud, &status);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
    buffer_free(&buf);
}

// 扩展长度 16-bit：65535 字节 payload，确保大数据 round-trip 正确
static void test_websock_unpack_extended_16(CuTest *tc) {
    size_t s = 0;
    // 256 字节 payload 触发 16-bit 扩展长度（126 字节起就用 16-bit）
    char payload[1024];
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (char)(i & 0xff);
    }
    void *frame = websock_pack_text(0, 1, payload, sizeof(payload), &s);
    CuAssertPtrNotNull(tc, frame);
    // 应是 2(head) + 2(ext_len) + 1024(data)
    CuAssertIntEquals(tc, 2 + 2 + 1024, (int)s);
    // byte1 应为 126
    CuAssertIntEquals(tc, 126, ((uint8_t *)frame)[1] & 0x7f);

    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, frame, s);
    FREE(frame);

    test_ws_ctx ws;
    ZERO(&ws, sizeof(ws));
    ws.secprot = PACK_NONE;
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = 1;
    ud.context = &ws;
    int32_t status = PROT_INIT;
    struct websock_pack_ctx *pack = websock_unpack(NULL, INVALID_SOCK, 0, 1, &buf, &ud, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    size_t dlen = 0;
    char *d = websock_data(pack, &dlen);
    CuAssertIntEquals(tc, (int)sizeof(payload), (int)dlen);
    CuAssertTrue(tc, 0 == memcmp(d, payload, sizeof(payload)));
    _websock_pkfree(pack);
    buffer_free(&buf);
}

// websock_secprot / websock_secpack 未启用子协议时 NULL；fin/prot getter 正确
static void test_websock_getters(CuTest *tc) {
    size_t s = 0;
    void *frame = websock_pack_text(0, 1, "x", 1, &s);
    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, frame, s);
    FREE(frame);

    test_ws_ctx ws;
    ZERO(&ws, sizeof(ws));
    ws.secprot = PACK_NONE; // 未启用子协议
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = 1;
    ud.context = &ws;
    int32_t status = PROT_INIT;
    struct websock_pack_ctx *pack = websock_unpack(NULL, INVALID_SOCK, 0, 1, &buf, &ud, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertIntEquals(tc, 1, websock_fin(pack));
    CuAssertIntEquals(tc, WS_TEXT, websock_prot(pack));
    CuAssertIntEquals(tc, PACK_NONE, websock_secprot(pack));
    // 子协议未启用时 secpack 为 NULL
    CuAssertTrue(tc, NULL == websock_secpack(pack));
    _websock_pkfree(pack);
    buffer_free(&buf);
}

// 客户端 mask 帧的 payload 经 xor 后应恢复原文（验证 mask key xor 解掩码）
static void test_websock_unpack_mask_xor(CuTest *tc) {
    size_t s = 0;
    // 客户端发送的 BINARY 帧，mask=1，payload 8 字节
    char payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    void *frame = websock_pack_binary(1, 1, payload, sizeof(payload), &s);
    CuAssertPtrNotNull(tc, frame);
    // wire 上的 payload 部分应已被 mask key xor 过（与原文不同）
    uint8_t *raw = (uint8_t *)frame;
    // 跳过 2 字节 head + 4 字节 mask key
    CuAssertTrue(tc, 0 != memcmp(raw + 2 + 4, payload, sizeof(payload)));

    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, frame, s);
    FREE(frame);

    test_ws_ctx ws;
    ZERO(&ws, sizeof(ws));
    ws.secprot = PACK_NONE;
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = 1;
    ud.context = &ws;
    int32_t status = PROT_INIT;
    // 服务端解码：xor 后应恢复原文
    struct websock_pack_ctx *pack = websock_unpack(NULL, INVALID_SOCK, 0, 0, &buf, &ud, &status);
    CuAssertPtrNotNull(tc, pack);
    size_t dlen = 0;
    char *d = websock_data(pack, &dlen);
    CuAssertIntEquals(tc, (int)sizeof(payload), (int)dlen);
    CuAssertTrue(tc, 0 == memcmp(d, payload, sizeof(payload)));
    _websock_pkfree(pack);
    buffer_free(&buf);
}

// prots 分发层：所有 free 函数 NULL safety + default 分支 FREE 路径
static void test_prots_free_null(CuTest *tc) {
    (void)tc;
    // prots_pkfree 全部 pktype 传 NULL 都安全
    prots_pkfree(PACK_NONE, NULL);
    prots_pkfree(PACK_DNS, NULL);
    prots_pkfree(PACK_HTTP, NULL);
    prots_pkfree(PACK_WEBSOCK, NULL);
    prots_pkfree(PACK_MQTT, NULL);
    prots_pkfree(PACK_SMTP, NULL);
    prots_pkfree(PACK_CUSTZ_FIXED, NULL);
    prots_pkfree(PACK_CUSTZ_FLAG, NULL);
    prots_pkfree(PACK_CUSTZ_VAR, NULL);
    prots_pkfree(PACK_REDIS, NULL);
    prots_pkfree(PACK_MYSQL, NULL);
    prots_pkfree(PACK_PGSQL, NULL);
    prots_pkfree(PACK_MONGO, NULL);
    // prots_hsfree 全部 pktype 传 NULL 都安全
    prots_hsfree(PACK_NONE, NULL);
    prots_hsfree(PACK_MONGO, NULL);
    prots_hsfree(PACK_HTTP, NULL);
    // prots_udfree(NULL) 安全
    prots_udfree(NULL);
    // prots_closed(NULL) 安全
    prots_closed(NULL);
    // prots_free 空实现，多次调用无副作用
    prots_free();
    prots_free();
}

// prots_pkfree default 分支：未识别 pktype 直接 FREE，ASan 验证无 leak
static void test_prots_pkfree_default(CuTest *tc) {
    (void)tc;
    // PACK_NONE / PACK_DNS / PACK_SMTP / PACK_CUSTZ_* 走 default → FREE(data)
    pack_type defaults[] = { PACK_NONE, PACK_DNS, PACK_SMTP,
                             PACK_CUSTZ_FIXED, PACK_CUSTZ_FLAG, PACK_CUSTZ_VAR };
    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
        void *p;
        MALLOC(p, 64);
        memset(p, 0xaa, 64);
        prots_pkfree(defaults[i], p);
    }
}

// prots_hsfree default 分支：除 PACK_MONGO 外都走 default → FREE(data)
static void test_prots_hsfree_default(CuTest *tc) {
    (void)tc;
    pack_type defaults[] = { PACK_NONE, PACK_HTTP, PACK_WEBSOCK, PACK_MQTT,
                             PACK_REDIS, PACK_MYSQL, PACK_PGSQL };
    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
        void *p;
        MALLOC(p, 32);
        prots_hsfree(defaults[i], p);
    }
}

// prots_udfree default 分支：PACK_NONE / PACK_DNS / PACK_CUSTZ_* → FREE(ud->context)
static void test_prots_udfree_default(CuTest *tc) {
    (void)tc;
    pack_type defaults[] = { PACK_NONE, PACK_DNS,
                             PACK_CUSTZ_FIXED, PACK_CUSTZ_FLAG, PACK_CUSTZ_VAR };
    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
        ud_cxt ud;
        ZERO(&ud, sizeof(ud));
        ud.pktype = (subtype_t)defaults[i];
        MALLOC(ud.context, 16);
        prots_udfree(&ud);
        // 释放后 ud.context 仍保留指针值（prots_udfree 不清零），
        // 但底层内存已释放，ASan 应不报泄漏
    }
    // context 为 NULL 时 default 分支调 FREE(NULL) 安全
    ud_cxt ud2;
    ZERO(&ud2, sizeof(ud2));
    ud2.pktype = PACK_NONE;
    ud2.context = NULL;
    prots_udfree(&ud2);
}

// prots_closed default 分支：PACK_NONE / PACK_HTTP / PACK_WEBSOCK 等不做事，仅安全返回
static void test_prots_closed_default(CuTest *tc) {
    (void)tc;
    pack_type defaults[] = { PACK_NONE, PACK_DNS, PACK_HTTP, PACK_WEBSOCK,
                             PACK_MQTT, PACK_CUSTZ_FIXED, PACK_REDIS };
    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
        ud_cxt ud;
        ZERO(&ud, sizeof(ud));
        ud.pktype = (subtype_t)defaults[i];
        // default 分支只 break，应无 side effect、不崩
        prots_closed(&ud);
    }
}

// prots_unpack 默认 PACK_NONE 路径：从 buffer 一次性取出所有数据 → MALLOC 返回
static void test_prots_unpack_default(CuTest *tc) {
    buffer_ctx buf;
    buffer_init(&buf);
    const char *payload = "raw passthrough data";
    size_t plen = strlen(payload);
    buffer_append(&buf, (void *)payload, plen);

    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.pktype = PACK_NONE;
    size_t size = 0;
    int32_t status = PROT_INIT;
    void *out = prots_unpack(NULL, INVALID_SOCK, 0, 1, &buf, &ud, &size, &status);
    CuAssertPtrNotNull(tc, out);
    CuAssertIntEquals(tc, (int)plen, (int)size);
    CuAssertTrue(tc, 0 == memcmp(out, payload, plen));
    // PACK_NONE default 路径不设置任何 status bit
    CuAssertIntEquals(tc, PROT_INIT, status);
    FREE(out);

    // 缓冲区为空时返回 NULL
    size = 999;
    status = PROT_ERROR;
    out = prots_unpack(NULL, INVALID_SOCK, 0, 1, &buf, &ud, &size, &status);
    CuAssertTrue(tc, NULL == out);
    CuAssertIntEquals(tc, 0, (int)size);
    CuAssertIntEquals(tc, PROT_INIT, status);
    buffer_free(&buf);
}

// prots_may_resume default 分支：非 PGSQL 协议返回 ERR_OK
static void test_prots_may_resume_default(CuTest *tc) {
    pack_type defaults[] = { PACK_NONE, PACK_HTTP, PACK_WEBSOCK, PACK_MQTT,
                             PACK_REDIS, PACK_MYSQL, PACK_MONGO };
    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
        CuAssertIntEquals(tc, ERR_OK, prots_may_resume(defaults[i], NULL));
    }
}

/* =======================================================================
 * mail 补充：html / attach_clear / addrs_clear / clear / reply
 * ======================================================================= */

static void test_mail_html_and_clear(CuTest *tc) {
    mail_ctx mail;
    mail_init(&mail);

    /* mail_reply 默认 1，set 0 → reply=0 */
    mail_reply(&mail, 0);
    CuAssertIntEquals(tc, 0, mail.reply);
    mail_reply(&mail, 1);
    CuAssertIntEquals(tc, 1, mail.reply);

    /* mail_html：base64 编码存入 mail.html，组包后 Content-Type 为 text/html */
    mail_from(&mail, "Sender", "sender@example.com");
    mail_addrs_add(&mail, "rcpt@example.com", TO);
    mail_addrs_add(&mail, "cc@example.com", CC);
    mail_addrs_add(&mail, "bcc@example.com", BCC);
    mail_subject(&mail, "subject");
    const char *html = "<p>hello</p>";
    mail_html(&mail, html, strlen(html));

    char *pkt = mail_pack(&mail);
    CuAssertPtrNotNull(tc, pkt);
    /* 含 HTML Content-Type 标记 */
    CuAssertTrue(tc, NULL != strstr(pkt, "text/html"));
    /* base64 编码的密送地址应出现在头部 */
    CuAssertTrue(tc, NULL != strstr(pkt, "Subject: subject"));
    FREE(pkt);

    /* mail_addrs_clear 后地址数归零 */
    CuAssertTrue(tc, 3 == array_size(&mail.addrs));
    mail_addrs_clear(&mail);
    CuAssertTrue(tc, 0 == array_size(&mail.addrs));

    /* mail_attach_clear 在空附件下安全 */
    mail_attach_clear(&mail);
    CuAssertTrue(tc, 0 == array_size(&mail.attach));

    /* mail_clear 不释放字段，只清空内容：subject/html/msg 首字节置 '\0'；
     * addrs 和 attach 数组清空，from 显示名/地址首字节归零 */
    mail_addrs_add(&mail, "rcpt2@example.com", TO);
    mail_clear(&mail);
    /* 字段非 NULL 但首字节归零 */
    CuAssertPtrNotNull(tc, mail.subject);
    CuAssertTrue(tc, '\0' == mail.subject[0]);
    CuAssertPtrNotNull(tc, mail.html);
    CuAssertTrue(tc, '\0' == mail.html[0]);
    /* from 显示名/地址清空 */
    CuAssertTrue(tc, '\0' == mail.from.name[0]);
    CuAssertTrue(tc, '\0' == mail.from.addr[0]);
    /* 地址列表清空 */
    CuAssertTrue(tc, 0 == array_size(&mail.addrs));

    mail_free(&mail);
}

/* =======================================================================
 * SMTP 命令组包：RSET / QUIT / NOOP / DATA / MAIL FROM / RCPT TO
 * smtp_pack_from / smtp_pack_rcpt 拒绝含 CR/LF 的输入（防 CRLF 注入）
 * ======================================================================= */
static void test_smtp_pack_cmds(CuTest *tc) {
    char *cmd;

    /* RSET */
    cmd = smtp_pack_reset();
    CuAssertPtrNotNull(tc, cmd);
    CuAssertStrEquals(tc, "RSET\r\n", cmd);
    FREE(cmd);

    /* QUIT */
    cmd = smtp_pack_quit();
    CuAssertPtrNotNull(tc, cmd);
    CuAssertStrEquals(tc, "QUIT\r\n", cmd);
    FREE(cmd);

    /* NOOP (ping) */
    cmd = smtp_pack_ping();
    CuAssertPtrNotNull(tc, cmd);
    CuAssertStrEquals(tc, "NOOP\r\n", cmd);
    FREE(cmd);

    /* DATA */
    cmd = smtp_pack_data();
    CuAssertPtrNotNull(tc, cmd);
    CuAssertStrEquals(tc, "DATA\r\n", cmd);
    FREE(cmd);

    /* MAIL FROM 正常地址 */
    cmd = smtp_pack_from("alice@example.com");
    CuAssertPtrNotNull(tc, cmd);
    CuAssertStrEquals(tc, "MAIL FROM:<alice@example.com>\r\n", cmd);
    FREE(cmd);

    /* RCPT TO 正常地址 */
    cmd = smtp_pack_rcpt("bob@example.com");
    CuAssertPtrNotNull(tc, cmd);
    CuAssertStrEquals(tc, "RCPT TO:<bob@example.com>\r\n", cmd);
    FREE(cmd);
}

/* SMTP CRLF 注入防御：含 \r 或 \n 的地址应返回 NULL */
static void test_smtp_pack_crlf_inject(CuTest *tc) {
    /* MAIL FROM */
    CuAssertTrue(tc, NULL == smtp_pack_from(NULL));
    CuAssertTrue(tc, NULL == smtp_pack_from("evil@host\r\nMAIL FROM:<bypass>"));
    CuAssertTrue(tc, NULL == smtp_pack_from("a@b\n"));
    CuAssertTrue(tc, NULL == smtp_pack_from("a@b\r"));

    /* RCPT TO */
    CuAssertTrue(tc, NULL == smtp_pack_rcpt(NULL));
    CuAssertTrue(tc, NULL == smtp_pack_rcpt("victim@host\r\nRCPT TO:<extra>"));
    CuAssertTrue(tc, NULL == smtp_pack_rcpt("x@y\r"));
}

/* smtp_check_code / smtp_check_ok 状态码匹配 */
static void test_smtp_check_code(CuTest *tc) {
    char pack[64];

    /* 准确匹配 */
    safe_fill_str(pack, sizeof(pack), "250 OK");
    CuAssertIntEquals(tc, ERR_OK,     smtp_check_code(pack, "250"));
    CuAssertIntEquals(tc, ERR_OK,     smtp_check_ok(pack));

    /* 不匹配 */
    safe_fill_str(pack, sizeof(pack), "500 Error");
    CuAssertIntEquals(tc, ERR_FAILED, smtp_check_code(pack, "250"));
    CuAssertIntEquals(tc, ERR_FAILED, smtp_check_ok(pack));

    /* 仅前缀匹配（450 不应匹配 "45" 但应匹配 "450"） */
    safe_fill_str(pack, sizeof(pack), "450 Mailbox unavailable");
    CuAssertIntEquals(tc, ERR_OK,     smtp_check_code(pack, "450"));
    CuAssertIntEquals(tc, ERR_OK,     smtp_check_code(pack, "45"));   /* memcmp 前缀比较，2 字节前缀确实匹配 */
    CuAssertIntEquals(tc, ERR_FAILED, smtp_check_code(pack, "451"));

    /* 任意自定义 code */
    safe_fill_str(pack, sizeof(pack), "354 Start input");
    CuAssertIntEquals(tc, ERR_OK,     smtp_check_code(pack, "354"));
}

/* smtp_unpack COMMAND 状态：从 buffer 中取出一行响应并返回（拷贝 + 释放）
 * 其他状态需 ev_ctx，无法用 NULL ev 安全测试，但 MOREDATA/ERROR 早返路径可测 */
static void test_smtp_unpack_command(CuTest *tc) {
    buffer_ctx buf;
    ud_cxt ud;
    size_t size;
    int32_t status;
    char *pack;

    /* COMMAND + 完整响应 → 返回字符串（不含 CRLF），size = 行长度 */
    buffer_init(&buf);
    ZERO(&ud, sizeof(ud));
    ud.status = 4; /* COMMAND（parse_status 枚举：INIT/EHLO/AUTH/AUTH_CHECK/COMMAND = 0..4） */
    buffer_append(&buf, "250 OK Hello\r\n", 14);
    size = 0; status = PROT_INIT;
    pack = smtp_unpack(NULL, 0, 0, &buf, &ud, &size, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, 12 == size);
    CuAssertStrEquals(tc, "250 OK Hello", pack);
    FREE(pack);
    buffer_free(&buf);

    /* COMMAND + 不足 SMTP_CODE_LENS+CRLF_SIZE = 5 字节 → PROT_MOREDATA */
    buffer_init(&buf);
    ZERO(&ud, sizeof(ud));
    ud.status = 4;
    buffer_append(&buf, "25", 2);
    size = 0; status = PROT_INIT;
    pack = smtp_unpack(NULL, 0, 0, &buf, &ud, &size, &status);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_MOREDATA));
    buffer_free(&buf);

    /* COMMAND + 5+ 字节但缺 CRLF → PROT_MOREDATA */
    buffer_init(&buf);
    ZERO(&ud, sizeof(ud));
    ud.status = 4;
    buffer_append(&buf, "250 OK", 6);
    size = 0; status = PROT_INIT;
    pack = smtp_unpack(NULL, 0, 0, &buf, &ud, &size, &status);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_MOREDATA));
    buffer_free(&buf);

    /* COMMAND + 多行响应：一次性消费整段，返回完整内容（含内嵌 CRLF、不含末尾 CRLF），buffer 清空 */
    buffer_init(&buf);
    ZERO(&ud, sizeof(ud));
    ud.status = 4;
    buffer_append(&buf, "250-First\r\n250 End\r\n", 20);
    size = 0; status = PROT_INIT;
    pack = smtp_unpack(NULL, 0, 0, &buf, &ud, &size, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, 18 == size);
    CuAssertStrEquals(tc, "250-First\r\n250 End", pack);
    FREE(pack);
    CuAssertTrue(tc, 0 == buffer_size(&buf));
    buffer_free(&buf);

    /* COMMAND + 流水线：先一次性取完首段多行，剩余下一条响应保留待下次取出 */
    buffer_init(&buf);
    ZERO(&ud, sizeof(ud));
    ud.status = 4;
    buffer_append(&buf, "250-A\r\n250 B\r\n221 Bye\r\n", 23);
    size = 0; status = PROT_INIT;
    pack = smtp_unpack(NULL, 0, 0, &buf, &ud, &size, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertStrEquals(tc, "250-A\r\n250 B", pack);
    FREE(pack);
    pack = smtp_unpack(NULL, 0, 0, &buf, &ud, &size, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertStrEquals(tc, "221 Bye", pack);
    FREE(pack);
    CuAssertTrue(tc, 0 == buffer_size(&buf));
    buffer_free(&buf);

    /* COMMAND + 裸结束行 "<code>\r\n"（无 text）→ 返回纯 code */
    buffer_init(&buf);
    ZERO(&ud, sizeof(ud));
    ud.status = 4;
    buffer_append(&buf, "250\r\n", 5);
    size = 0; status = PROT_INIT;
    pack = smtp_unpack(NULL, 0, 0, &buf, &ud, &size, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, 3 == size);
    CuAssertStrEquals(tc, "250", pack);
    FREE(pack);
    CuAssertTrue(tc, 0 == buffer_size(&buf));
    buffer_free(&buf);

    /* COMMAND + 异于 250 的多行 code（如 DATA 的 354）→ 以首行 code 合并 */
    buffer_init(&buf);
    ZERO(&ud, sizeof(ud));
    ud.status = 4;
    buffer_append(&buf, "354-go\r\n354 ahead\r\n", 19);
    size = 0; status = PROT_INIT;
    pack = smtp_unpack(NULL, 0, 0, &buf, &ud, &size, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertStrEquals(tc, "354-go\r\n354 ahead", pack);
    FREE(pack);
    CuAssertTrue(tc, 0 == buffer_size(&buf));
    buffer_free(&buf);
}

/* =======================================================================
 * http_header_at —— 按索引访问头部（与 http_header 按 key 查找的对称变体）
 * ======================================================================= */
static void test_http_header_at(CuTest *tc) {
    buffer_ctx buf;
    buffer_init(&buf);
    _bput(&buf, "GET / HTTP/1.1\r\n");
    _bput(&buf, "Host: example.com\r\n");
    _bput(&buf, "Connection: keep-alive\r\n");
    _bput(&buf, "User-Agent: srey-test\r\n");
    _bput(&buf, "\r\n");

    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    int32_t status = PROT_INIT;

    struct http_pack_ctx *pack = http_unpack(&buf, &ud, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, 3 == http_nheader(pack));

    /* pos=0 应为 Host */
    http_header_ctx *h0 = http_header_at(pack, 0);
    CuAssertPtrNotNull(tc, h0);
    CuAssertTrue(tc, 4 == h0->key.lens);
    CuAssertTrue(tc, 0 == memcmp(h0->key.data, "Host", 4));
    CuAssertTrue(tc, h0->value.lens == strlen("example.com"));
    CuAssertTrue(tc, 0 == memcmp(h0->value.data, "example.com", h0->value.lens));

    /* pos=1 应为 Connection */
    http_header_ctx *h1 = http_header_at(pack, 1);
    CuAssertPtrNotNull(tc, h1);
    CuAssertTrue(tc, 10 == h1->key.lens);
    CuAssertTrue(tc, 0 == memcmp(h1->key.data, "Connection", 10));

    /* pos=2 应为 User-Agent */
    http_header_ctx *h2 = http_header_at(pack, 2);
    CuAssertPtrNotNull(tc, h2);
    CuAssertTrue(tc, 10 == h2->key.lens);
    CuAssertTrue(tc, 0 == memcmp(h2->key.data, "User-Agent", 10));

    /* 注：array_at 在 pos 越界时 ASSERTAB abort，并不返回 NULL；
     * API 契约要求调用方先用 http_nheader 检查范围，故无法测试越界路径 */

    _http_pkfree(pack);
    _http_udfree(&ud);
    buffer_free(&buf);
}

/* =======================================================================
 * websock 解包：mask=1 但 mask key 全 0 的合法边界
 * RFC6455 要求服务端拒绝未掩码客户端帧，但允许 mask key 为全 0 — 等价于不变 xor
 * ======================================================================= */
static void test_websock_unpack_mask_all_zero(CuTest *tc) {
    /* 手工构造 BINARY fin=1 mask=1 len=4 key=0000 payload=de ad be ef */
    unsigned char frame[10] = {
        0x82,                   /* FIN=1, opcode=0x2 (binary) */
        0x84,                   /* MASK=1, len=4 */
        0x00, 0x00, 0x00, 0x00, /* mask key 全 0 */
        0xde, 0xad, 0xbe, 0xef  /* payload（xor 全 0 等于原文） */
    };

    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, frame, sizeof(frame));

    test_ws_ctx ws;
    ZERO(&ws, sizeof(ws));
    ws.secprot = PACK_NONE;

    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = 1; /* START */
    ud.context = &ws;

    int32_t status = PROT_INIT;
    struct websock_pack_ctx *pack = websock_unpack(NULL, INVALID_SOCK, 0,
        0 /* server */, &buf, &ud, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertIntEquals(tc, WS_BINARY, websock_prot(pack));

    size_t dlens = 0;
    char *data = websock_data(pack, &dlens);
    CuAssertTrue(tc, 4 == dlens);
    /* mask 全 0 → payload 解码结果与原文一致 */
    CuAssertTrue(tc, 0xde == (uint8_t)data[0]);
    CuAssertTrue(tc, 0xad == (uint8_t)data[1]);
    CuAssertTrue(tc, 0xbe == (uint8_t)data[2]);
    CuAssertTrue(tc, 0xef == (uint8_t)data[3]);

    _websock_pkfree(pack);
    buffer_free(&buf);
}

// 手工构造 payloadlen=127 + 8 字节大端长度的 64-bit 扩展长度帧
// 覆盖 _websock_parse_payloadlen 中 payloadlen==127 分支（lib/protocol/websock.c:518-543）
//   1) 合法 100 字节 payload 通过 ntohll 正确解析
//   2) 长度恰等于 MAX_PACK_SIZE(65536) 触发 PACK_TOO_LONG 防御
static void test_websock_unpack_extended_64(CuTest *tc) {
    // 子用例 1：payloadlen=127 + 长度=100 + 100 字节 payload，期望正确解码
    unsigned char head[10] = {
        0x82, // FIN=1 opcode=BINARY
        0x7f, // MASK=0 payloadlen=127（用 8 字节扩展长度）
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64 // 大端 64-bit = 100
    };
    char payload[100];
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (char)((i * 17 + 3) & 0xff);
    }

    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, head, sizeof(head));
    buffer_append(&buf, payload, sizeof(payload));

    test_ws_ctx ws;
    ZERO(&ws, sizeof(ws));
    ws.secprot = PACK_NONE;
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = 1;
    ud.context = &ws;
    int32_t status = PROT_INIT;
    // client=1：客户端接收服务端帧，服务端帧允许 mask=0
    struct websock_pack_ctx *pack = websock_unpack(NULL, INVALID_SOCK, 0,
        1, &buf, &ud, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertIntEquals(tc, WS_BINARY, websock_prot(pack));
    size_t dlen = 0;
    char *data = websock_data(pack, &dlen);
    CuAssertIntEquals(tc, (int)sizeof(payload), (int)dlen);
    CuAssertTrue(tc, 0 == memcmp(data, payload, sizeof(payload)));
    _websock_pkfree(pack);
    buffer_free(&buf);

    // 子用例 2：payloadlen=127 + 长度=65536(MAX_PACK_SIZE)，触发 PACK_TOO_LONG → PROT_ERROR
    unsigned char head2[10] = {
        0x82,
        0x7f,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00 // 大端 64-bit = 65536
    };
    buffer_ctx buf2;
    buffer_init(&buf2);
    buffer_append(&buf2, head2, sizeof(head2));

    test_ws_ctx ws2;
    ZERO(&ws2, sizeof(ws2));
    ws2.secprot = PACK_NONE;
    ud_cxt ud2;
    ZERO(&ud2, sizeof(ud2));
    ud2.status = 1;
    ud2.context = &ws2;
    int32_t status2 = PROT_INIT;
    struct websock_pack_ctx *pack2 = websock_unpack(NULL, INVALID_SOCK, 0,
        1, &buf2, &ud2, &status2);
    CuAssertTrue(tc, NULL == pack2);
    CuAssertTrue(tc, BIT_CHECK(status2, PROT_ERROR));
    buffer_free(&buf2);
}

// mail.c 附件路径：mail_attach_add 读取临时文件 → base64 编码到 attach->content
// 验证 mail_pack 输出含 MIME multipart/mixed boundary、filename、Content-Disposition、
// base64 编码内容
static void test_mail_attach_pack(CuTest *tc) {
    // 1. 临时文件：写入 binary payload（含 \0 验证 base64 不依赖 strlen）
    const char *dir = procpath();
    char tmpfile[PATH_LENS];
    SNPRINTF(tmpfile, sizeof(tmpfile), "%s%stest_mail_attach.txt", dir, PATH_SEPARATORSTR);
    const char payload[] = "Hello\x00 attach \x01world!";
    size_t plen = sizeof(payload) - 1; // 含中间 \0，需用 sizeof
    FILE *fp = fopen(tmpfile, "wb");
    CuAssertPtrNotNull(tc, fp);
    CuAssertTrue(tc, plen == fwrite(payload, 1, plen, fp));
    fclose(fp);

    // 2. 构造 mail：含正文 + 附件
    mail_ctx mail;
    mail_init(&mail);
    mail_from(&mail, "Alice", "alice@example.com");
    mail_addrs_add(&mail, "bob@example.com", TO);
    mail_addrs_add(&mail, "carol@example.com", CC);
    mail_subject(&mail, "with-attach");
    mail_msg(&mail, "see attach");
    mail_attach_add(&mail, tmpfile);
    CuAssertIntEquals(tc, 1, (int)array_size(&mail.attach));

    // 3. 附件结构字段：extension 取自文件名最后 '.'，file 仅含文件名（不含目录）
    mail_attach *att = array_at(&mail.attach, 0);
    CuAssertTrue(tc, 0 == strcmp(att->extension, ".txt"));
    CuAssertTrue(tc, NULL != strstr(att->file, "test_mail_attach.txt"));
    CuAssertPtrNotNull(tc, att->content);
    CuAssertTrue(tc, strlen(att->content) > 0);

    // 4. mail_pack：含 multipart/mixed boundary + 附件 header + base64 内容
    char *pkt = mail_pack(&mail);
    CuAssertPtrNotNull(tc, pkt);
    CuAssertTrue(tc, NULL != strstr(pkt, "MIME-Version: 1.0"));
    CuAssertTrue(tc, NULL != strstr(pkt, "multipart/mixed"));
    CuAssertTrue(tc, NULL != strstr(pkt, "Content-Type: text/plain"));
    CuAssertTrue(tc, NULL != strstr(pkt, "Content-Transfer-Encoding: base64"));
    CuAssertTrue(tc, NULL != strstr(pkt, "Content-Disposition: attachment; filename=\""));
    // 附件文件名出现在 Content-Disposition 行
    CuAssertTrue(tc, NULL != strstr(pkt, "test_mail_attach.txt"));
    // base64 编码后的附件 content 应在 pkt 中（注意不能用 strlen 验证原文，二进制含 \0）
    CuAssertTrue(tc, NULL != strstr(pkt, att->content));
    // 邮件以 "\r\n.\r\n" 终止（DATA body 终止序列）
    CuAssertTrue(tc, NULL != strstr(pkt, "\r\n.\r\n"));
    FREE(pkt);

    // 5. mail_attach_clear：附件数组清空，内部 content 释放
    mail_attach_clear(&mail);
    CuAssertIntEquals(tc, 0, (int)array_size(&mail.attach));

    // 6. 多附件场景：插入两个，验证 mail_pack 不崩，含两段 base64
    mail_attach_add(&mail, tmpfile);
    mail_attach_add(&mail, tmpfile);
    CuAssertIntEquals(tc, 2, (int)array_size(&mail.attach));
    char *pkt2 = mail_pack(&mail);
    CuAssertPtrNotNull(tc, pkt2);
    // 两个附件的同名 filename 至少出现 2 次（Content-Disposition 各一次）
    const char *p = pkt2;
    int filename_hits = 0;
    while (NULL != (p = strstr(p, "test_mail_attach.txt"))) {
        filename_hits++;
        p++;
    }
    CuAssertTrue(tc, filename_hits >= 2);
    FREE(pkt2);

    mail_free(&mail);
    remove(tmpfile);
}

// SMTP 状态机 ud->status 值（与 lib/protocol/smtp/smtp.c parse_status 对应）：
//   0=INIT, 1=EHLO, 2=AUTH, 3=AUTH_CHECK, 4=COMMAND
// smtp_ctx.authtype（同 smtp_authtype）：1=LOGIN, 2=PLAIN
// ev_send 在 fd==INVALID_SOCK 时会释放 data 并返回 ERR_FAILED 设置 PROT_ERROR；
// 此时 ud->status 已在 ev_send 调用前完成切换，可用于验证状态转移
#define _SMTP_INIT       0
#define _SMTP_EHLO       1
#define _SMTP_AUTH       2
#define _SMTP_AUTH_CHECK 3
#define _SMTP_LOGIN      1
#define _SMTP_PLAIN      2

static void _smtp_ud_setup(smtp_ctx *smtp, ud_cxt *ud, int32_t state) {
    ZERO(smtp, sizeof(smtp_ctx));
    safe_fill_str(smtp->user, sizeof(smtp->user), "alice");
    safe_fill_str(smtp->psw, sizeof(smtp->psw), "secret");
    ZERO(ud, sizeof(ud_cxt));
    ud->status = state;
    ud->context = smtp;
}

// INIT 状态：220 完整响应 → 状态切换到 EHLO（ev_send 会失败但状态已切换）
// 不完整 220 → PROT_MOREDATA，状态保持
// 非 220 → PROT_ERROR
static void test_smtp_unpack_state_init(CuTest *tc) {
    // 完整 220：buffer 被 drain，ud->status → EHLO
    {
        smtp_ctx smtp;
        ud_cxt ud;
        _smtp_ud_setup(&smtp, &ud, _SMTP_INIT);
        buffer_ctx buf;
        buffer_init(&buf);
        _bput(&buf, "220 mx.example.com ESMTP ready\r\n");
        int32_t status = PROT_INIT;
        size_t size = 0;
        void *pack = smtp_unpack(NULL, INVALID_SOCK, 0, &buf, &ud, &size, &status);
        CuAssertTrue(tc, NULL == pack);
        CuAssertIntEquals(tc, _SMTP_EHLO, ud.status);
        CuAssertTrue(tc, 0 == buffer_size(&buf));
        buffer_free(&buf);
    }
    // 不完整 220：无 CRLF → PROT_MOREDATA，状态保持 INIT
    {
        smtp_ctx smtp;
        ud_cxt ud;
        _smtp_ud_setup(&smtp, &ud, _SMTP_INIT);
        buffer_ctx buf;
        buffer_init(&buf);
        _bput(&buf, "220 mx.example.com");
        int32_t status = PROT_INIT;
        size_t size = 0;
        smtp_unpack(NULL, INVALID_SOCK, 0, &buf, &ud, &size, &status);
        CuAssertTrue(tc, BIT_CHECK(status, PROT_MOREDATA));
        CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
        CuAssertIntEquals(tc, _SMTP_INIT, ud.status);
        buffer_free(&buf);
    }
    // 非 220 响应（如 421 service unavailable）→ PROT_ERROR
    {
        smtp_ctx smtp;
        ud_cxt ud;
        _smtp_ud_setup(&smtp, &ud, _SMTP_INIT);
        buffer_ctx buf;
        buffer_init(&buf);
        _bput(&buf, "421 service unavailable\r\n");
        int32_t status = PROT_INIT;
        size_t size = 0;
        smtp_unpack(NULL, INVALID_SOCK, 0, &buf, &ud, &size, &status);
        CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
        CuAssertIntEquals(tc, _SMTP_INIT, ud.status);
        buffer_free(&buf);
    }
}

// EHLO 状态：250-AUTH 多行响应 → 状态切换到 AUTH，authtype 设置
// 无 AUTH 头的 250 响应 → PROT_ERROR
static void test_smtp_unpack_state_ehlo(CuTest *tc) {
    // 250-AUTH LOGIN PLAIN：authtype 优先 PLAIN
    {
        smtp_ctx smtp;
        ud_cxt ud;
        _smtp_ud_setup(&smtp, &ud, _SMTP_EHLO);
        buffer_ctx buf;
        buffer_init(&buf);
        _bput(&buf, "250-mx.example.com Hello\r\n250-AUTH LOGIN PLAIN\r\n250 OK\r\n");
        int32_t status = PROT_INIT;
        size_t size = 0;
        smtp_unpack(NULL, INVALID_SOCK, 0, &buf, &ud, &size, &status);
        CuAssertIntEquals(tc, _SMTP_AUTH, ud.status);
        CuAssertIntEquals(tc, _SMTP_PLAIN, smtp.authtype);
        CuAssertTrue(tc, 0 == buffer_size(&buf));
        buffer_free(&buf);
    }
    // 仅 AUTH LOGIN：authtype = LOGIN
    {
        smtp_ctx smtp;
        ud_cxt ud;
        _smtp_ud_setup(&smtp, &ud, _SMTP_EHLO);
        buffer_ctx buf;
        buffer_init(&buf);
        _bput(&buf, "250-mx.example.com Hello\r\n250 AUTH LOGIN\r\n");
        int32_t status = PROT_INIT;
        size_t size = 0;
        smtp_unpack(NULL, INVALID_SOCK, 0, &buf, &ud, &size, &status);
        CuAssertIntEquals(tc, _SMTP_AUTH, ud.status);
        CuAssertIntEquals(tc, _SMTP_LOGIN, smtp.authtype);
        buffer_free(&buf);
    }
    // 250 响应中无 AUTH 扩展 → PROT_ERROR，状态保持 EHLO
    {
        smtp_ctx smtp;
        ud_cxt ud;
        _smtp_ud_setup(&smtp, &ud, _SMTP_EHLO);
        buffer_ctx buf;
        buffer_init(&buf);
        _bput(&buf, "250-mx.example.com Hello\r\n250 STARTTLS\r\n");
        int32_t status = PROT_INIT;
        size_t size = 0;
        smtp_unpack(NULL, INVALID_SOCK, 0, &buf, &ud, &size, &status);
        CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
        CuAssertIntEquals(tc, _SMTP_EHLO, ud.status);
        buffer_free(&buf);
    }
}

// AUTH 状态 + LOGIN 认证：
//   334 VXNlcm5hbWU6 (b64 "Username:") → 状态保持 AUTH（仍在 LOGIN 中间步骤）
//   334 UGFzc3dvcmQ6 (b64 "Password:") → 状态切换 AUTH_CHECK
// AUTH 状态 + PLAIN：334 任意挑战 → 状态切换 AUTH_CHECK
// AUTH 状态 + 非 334 响应 → PROT_ERROR
static void test_smtp_unpack_state_auth_login(CuTest *tc) {
    // LOGIN username 阶段
    {
        smtp_ctx smtp;
        ud_cxt ud;
        _smtp_ud_setup(&smtp, &ud, _SMTP_AUTH);
        smtp.authtype = _SMTP_LOGIN;
        buffer_ctx buf;
        buffer_init(&buf);
        _bput(&buf, "334 VXNlcm5hbWU6\r\n");
        int32_t status = PROT_INIT;
        size_t size = 0;
        smtp_unpack(NULL, INVALID_SOCK, 0, &buf, &ud, &size, &status);
        // 仍在 AUTH（等待 Password 挑战）
        CuAssertIntEquals(tc, _SMTP_AUTH, ud.status);
        CuAssertTrue(tc, 0 == buffer_size(&buf));
        buffer_free(&buf);
    }
    // LOGIN password 阶段：切换 AUTH_CHECK
    {
        smtp_ctx smtp;
        ud_cxt ud;
        _smtp_ud_setup(&smtp, &ud, _SMTP_AUTH);
        smtp.authtype = _SMTP_LOGIN;
        buffer_ctx buf;
        buffer_init(&buf);
        _bput(&buf, "334 UGFzc3dvcmQ6\r\n");
        int32_t status = PROT_INIT;
        size_t size = 0;
        smtp_unpack(NULL, INVALID_SOCK, 0, &buf, &ud, &size, &status);
        CuAssertIntEquals(tc, _SMTP_AUTH_CHECK, ud.status);
        buffer_free(&buf);
    }
    // 非 334 响应（如 535 auth failed）→ PROT_ERROR
    {
        smtp_ctx smtp;
        ud_cxt ud;
        _smtp_ud_setup(&smtp, &ud, _SMTP_AUTH);
        smtp.authtype = _SMTP_LOGIN;
        buffer_ctx buf;
        buffer_init(&buf);
        _bput(&buf, "535 auth failed\r\n");
        int32_t status = PROT_INIT;
        size_t size = 0;
        smtp_unpack(NULL, INVALID_SOCK, 0, &buf, &ud, &size, &status);
        CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
        // status 不切换
        CuAssertIntEquals(tc, _SMTP_AUTH, ud.status);
        buffer_free(&buf);
    }
}

// AUTH 状态 + PLAIN：单次挑战即切换 AUTH_CHECK
static void test_smtp_unpack_state_auth_plain(CuTest *tc) {
    smtp_ctx smtp;
    ud_cxt ud;
    _smtp_ud_setup(&smtp, &ud, _SMTP_AUTH);
    smtp.authtype = _SMTP_PLAIN;
    buffer_ctx buf;
    buffer_init(&buf);
    _bput(&buf, "334 \r\n");
    int32_t status = PROT_INIT;
    size_t size = 0;
    smtp_unpack(NULL, INVALID_SOCK, 0, &buf, &ud, &size, &status);
    CuAssertIntEquals(tc, _SMTP_AUTH_CHECK, ud.status);
    buffer_free(&buf);
}

// AUTH 状态：buffer 不足 → PROT_MOREDATA，状态保持
static void test_smtp_unpack_state_auth_moredata(CuTest *tc) {
    smtp_ctx smtp;
    ud_cxt ud;
    _smtp_ud_setup(&smtp, &ud, _SMTP_AUTH);
    smtp.authtype = _SMTP_LOGIN;
    buffer_ctx buf;
    buffer_init(&buf);
    _bput(&buf, "33"); // < 5 字节
    int32_t status = PROT_INIT;
    size_t size = 0;
    smtp_unpack(NULL, INVALID_SOCK, 0, &buf, &ud, &size, &status);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_MOREDATA));
    CuAssertIntEquals(tc, _SMTP_AUTH, ud.status);
    buffer_free(&buf);
}

// 续行洪泛回归：AUTH / AUTH_CHECK / COMMAND 三态持续无 CRLF 时必须按 PACK_TOO_LONG
// 上界拒绝（PROT_ERROR），而非无限 PROT_MOREDATA 累积耗内存
// 对齐 test_smtp_full_response case 20 对 INIT/EHLO 的洪泛防护
static void test_smtp_unpack_flood(CuTest *tc) {
    size_t floodlen = (size_t)MAX_PACK_SIZE + 16; // 超单包上界且不含任何 CRLF
    char *flood;
    MALLOC(flood, floodlen);
    memset(flood, 'A', floodlen);
    int32_t states[] = { _SMTP_AUTH, _SMTP_AUTH_CHECK, 4 }; // 4 = COMMAND
    smtp_ctx smtp;
    ud_cxt ud;
    buffer_ctx buf;
    int32_t status;
    size_t size;
    void *pack;
    for (int32_t i = 0; i < (int32_t)ARRAY_SIZE(states); i++) {
        _smtp_ud_setup(&smtp, &ud, states[i]);
        smtp.authtype = _SMTP_LOGIN;
        buffer_init(&buf);
        buffer_append(&buf, flood, floodlen);
        status = PROT_INIT;
        size = 0;
        pack = smtp_unpack(NULL, INVALID_SOCK, 0, &buf, &ud, &size, &status);
        CuAssertTrue(tc, NULL == pack);
        CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
        buffer_free(&buf);
    }
    FREE(flood);
}

/* ======================================================================= */

void test_protocol(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_http_response);
    SUITE_ADD_TEST(suite, test_http_pack_req);
    SUITE_ADD_TEST(suite, test_http_smuggling);
    SUITE_ADD_TEST(suite, test_http_chunked_size_smuggle);
    SUITE_ADD_TEST(suite, test_http_check_keyval_token);
    SUITE_ADD_TEST(suite, test_http_chunked_trailer_limit);
    SUITE_ADD_TEST(suite, test_http_moredata);
    SUITE_ADD_TEST(suite, test_http_code_status);
    SUITE_ADD_TEST(suite, test_http_pack_chunked);
    SUITE_ADD_TEST(suite, test_http_header_at);
    SUITE_ADD_TEST(suite, test_redis_simple);
    SUITE_ADD_TEST(suite, test_redis_bulk);
    SUITE_ADD_TEST(suite, test_redis_null_bulk);
    SUITE_ADD_TEST(suite, test_redis_array);
    SUITE_ADD_TEST(suite, test_redis_pack);
    SUITE_ADD_TEST(suite, test_redis_moredata);
    SUITE_ADD_TEST(suite, test_redis_oversize_no_crlf);
    SUITE_ADD_TEST(suite, test_redis_resp3_scalar);
    SUITE_ADD_TEST(suite, test_redis_resp3_aggregate);
    SUITE_ADD_TEST(suite, test_url_parse);
    SUITE_ADD_TEST(suite, test_url_parse_edges);
    SUITE_ADD_TEST(suite, test_url_reorg_param);
    SUITE_ADD_TEST(suite, test_custz);
    SUITE_ADD_TEST(suite, test_smtp_full_response);
    SUITE_ADD_TEST(suite, test_smtp_dot_stuffing);
    SUITE_ADD_TEST(suite, test_smtp_pack_cmds);
    SUITE_ADD_TEST(suite, test_smtp_pack_crlf_inject);
    SUITE_ADD_TEST(suite, test_smtp_check_code);
    SUITE_ADD_TEST(suite, test_smtp_unpack_command);
    SUITE_ADD_TEST(suite, test_smtp_unpack_state_init);
    SUITE_ADD_TEST(suite, test_smtp_unpack_state_ehlo);
    SUITE_ADD_TEST(suite, test_smtp_unpack_state_auth_login);
    SUITE_ADD_TEST(suite, test_smtp_unpack_state_auth_plain);
    SUITE_ADD_TEST(suite, test_smtp_unpack_state_auth_moredata);
    SUITE_ADD_TEST(suite, test_smtp_unpack_flood);
    SUITE_ADD_TEST(suite, test_dns_request_pack);
    SUITE_ADD_TEST(suite, test_dns_request_pack_tcp);
    SUITE_ADD_TEST(suite, test_dns_unpack);
    SUITE_ADD_TEST(suite, test_dns_parse_pack);
    SUITE_ADD_TEST(suite, test_dns_parse_pack_truncated_query);
    SUITE_ADD_TEST(suite, test_dns_set_get_ip);
    SUITE_ADD_TEST(suite, test_custz_head_fixed);
    SUITE_ADD_TEST(suite, test_custz_head_flag);
    SUITE_ADD_TEST(suite, test_custz_head_variable);
    SUITE_ADD_TEST(suite, test_websock_pack_frames);
    SUITE_ADD_TEST(suite, test_websock_pack_handshake);
    SUITE_ADD_TEST(suite, test_websock_unpack_text);
    SUITE_ADD_TEST(suite, test_websock_unpack_masked);
    SUITE_ADD_TEST(suite, test_websock_unpack_fragmented);
    SUITE_ADD_TEST(suite, test_websock_unpack_close);
    SUITE_ADD_TEST(suite, test_websock_unpack_server_no_mask);
    SUITE_ADD_TEST(suite, test_websock_unpack_reserved_opcode);
    SUITE_ADD_TEST(suite, test_websock_unpack_rsv_set);
    SUITE_ADD_TEST(suite, test_websock_unpack_control_fragmented);
    SUITE_ADD_TEST(suite, test_websock_unpack_control_too_big);
    SUITE_ADD_TEST(suite, test_websock_unpack_extended_16);
    SUITE_ADD_TEST(suite, test_websock_unpack_extended_64);
    SUITE_ADD_TEST(suite, test_websock_getters);
    SUITE_ADD_TEST(suite, test_websock_unpack_mask_xor);
    SUITE_ADD_TEST(suite, test_websock_unpack_mask_all_zero);
    SUITE_ADD_TEST(suite, test_prots_free_null);
    SUITE_ADD_TEST(suite, test_prots_pkfree_default);
    SUITE_ADD_TEST(suite, test_prots_hsfree_default);
    SUITE_ADD_TEST(suite, test_prots_udfree_default);
    SUITE_ADD_TEST(suite, test_prots_closed_default);
    SUITE_ADD_TEST(suite, test_prots_unpack_default);
    SUITE_ADD_TEST(suite, test_prots_may_resume_default);
    SUITE_ADD_TEST(suite, test_mail_html_and_clear);
    SUITE_ADD_TEST(suite, test_mail_attach_pack);
}
