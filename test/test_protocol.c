
#include "test_protocol.h"
#include "lib.h"
#include "protocol/custz_head.h"

// SMTP 状态机 ud->status 值（与 lib/protocol/smtp/smtp.c parse_status 对应）：
//   0=INIT, 1=EHLO, 2=AUTH, 3=AUTH_CHECK, 4=COMMAND
// ev_send 在 fd==INVALID_SOCK 时会释放 data 并返回 ERR_FAILED 设置 PROT_ERROR；
// 此时 ud->status 已在 ev_send 调用前完成切换，可用于验证状态转移
#define _SMTP_INIT       0
#define _SMTP_EHLO       1
#define _SMTP_AUTH       2
#define _SMTP_AUTH_CHECK 3
// authtype 直接取 smtp.h 的枚举，别再抄一份数值
#define _SMTP_LOGIN      LOGIN
#define _SMTP_PLAIN      PLAIN

/* =======================================================================
 * 公共辅助 —— 将字符串字面量追加到 buffer_ctx
 * ======================================================================= */
static void _bput(buffer_ctx *b, const char *s) {
    buffer_append(b, (void *)s, strlen(s));
}

// 解包桩共用的"无连接"标识: 取代旧的 (INVALID_SOCK, 0) 实参对
static sock_ctx _t_nosk = { INVALID_SOCK, INVALID_INDEX, 0 };
// 解包入口的 ev 与连接标识在测试里恒为空：只喂缓冲，不发包也不认连接。
// 三个恒定实参收进薄封装，签名再变时只改这里，不必逐个改调用点
static void *_t_http_unpack(int32_t client, buffer_ctx *buf, ud_cxt *ud,
    size_t *size, int32_t *status) {
    return http_unpack(NULL, &_t_nosk, client, buf, ud, size, status);
}
static void *_t_redis_unpack(int32_t client, buffer_ctx *buf, ud_cxt *ud,
    size_t *size, int32_t *status) {
    return redis_unpack(NULL, &_t_nosk, client, buf, ud, size, status);
}
static void *_t_websock_unpack(int32_t client, buffer_ctx *buf, ud_cxt *ud,
    size_t *size, int32_t *status) {
    return websock_unpack(NULL, &_t_nosk, client, buf, ud, size, status);
}

// websock 解包用的最小上下文：ws 清零 + 无子协议，ud 摆到握手已完成的 START 状态并挂上 ws。
// 全文件 20 多处用例都要这七行，改布局时只改这里
static void _ws_ctx_init(websock_ctx *ws, ud_cxt *ud) {
    ZERO(ws, sizeof(websock_ctx));
    ws->secprot = PACK_NONE;
    ZERO(ud, sizeof(ud_cxt));
    ud->status = 1;// websock 内部 START
    ud->context = ws;
}
// redis 解包用的最小上下文:buf 装上 wire、ud 清零、status 摆到 PROT_INIT,随后解一次。
// 理由同 _ws_ctx_init —— 改 ud_cxt 布局或前置状态时只改这里,不必逐个改用例。
// buf 与 ud 仍归调用方,收尾照旧 buffer_free / _redis_udfree
static redis_pack_ctx *_t_redis_one(buffer_ctx *buf, ud_cxt *ud,
    const char *wire, int32_t *status) {
    buffer_init(buf);
    _bput(buf, wire);
    ZERO(ud, sizeof(ud_cxt));
    *status = PROT_INIT;
    return _t_redis_unpack(0, buf, ud, NULL, status);
}
static void *_t_smtp_unpack(int32_t client, buffer_ctx *buf, ud_cxt *ud,
    size_t *size, int32_t *status) {
    return smtp_unpack(NULL, &_t_nosk, client, buf, ud, size, status);
}
static void *_t_custz_unpack(int32_t client, buffer_ctx *buf, ud_cxt *ud,
    size_t *size, int32_t *status) {
    return custz_unpack(NULL, &_t_nosk, client, buf, ud, size, status);
}
static void *_t_dns_unpack(int32_t client, buffer_ctx *buf, ud_cxt *ud,
    size_t *size, int32_t *status) {
    return dns_unpack(NULL, &_t_nosk, client, buf, ud, size, status);
}
static void *_t_prots_unpack(int32_t client, buffer_ctx *buf, ud_cxt *ud,
    size_t *size, int32_t *status) {
    return prots_unpack(NULL, &_t_nosk, client, buf, ud, size, status);
}
static void *_t_mqtt_unpack(int32_t client, buffer_ctx *buf, ud_cxt *ud,
    size_t *size, int32_t *status) {
    return mqtt_unpack(NULL, &_t_nosk, client, buf, ud, size, status);
}

/* =======================================================================
 * HTTP —— 解包与组包验证
 * ======================================================================= */

/* HEAD 的响应按 RFC 7230 §3.3.3 规则 1 不带 body，却照样带 Content-Length。
   发起方把 method 传给 http_set_method 登记，它把连接状态置成内部的 INIT_NOBODY；
   本用例直接摆那个状态值（同 websock 用例里直接摆 START 的做法），改枚举顺序须同步这里 */
static void test_http_head_nobody(CuTest *tc) {
    buffer_ctx buf;
    buffer_init(&buf);
    ud_cxt ud;
    ZERO(&ud, sizeof(ud_cxt));
    int32_t status = PROT_INIT;
    size_t dlen = 999;
    void *data;
    struct http_pack_ctx *pack;

    /* 未登记时的老行为：Content-Length 被当真，挂着等 1234 字节 body */
    _bput(&buf, "HTTP/1.1 200 OK\r\nContent-Length: 1234\r\n\r\n");
    pack = _t_http_unpack(1, &buf, &ud, NULL, &status);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_MOREDATA));
    _http_udfree(&ud);
    buffer_free(&buf);

    /* 登记之后：同一条响应不吃 body，其后紧跟的那条照常解析 */
    buffer_init(&buf);
    ZERO(&ud, sizeof(ud_cxt));
    ud.status = 4;/* http 内部 INIT_NOBODY，由 http_set_method 置位 */
    status = PROT_INIT;
    _bput(&buf, "HTTP/1.1 200 OK\r\nContent-Length: 1234\r\n\r\n");
    _bput(&buf, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi");

    pack = _t_http_unpack(1, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    data = http_data(pack, &dlen);
    CuAssertTrue(tc, 0 == dlen);
    CuAssertTrue(tc, NULL == data);
    CuAssertTrue(tc, buf_compare(&http_status(pack)[1], "200", 3));
    _http_pkfree(pack);
    _http_udfree(&ud);

    /* 1xx 是中间响应，不能把登记消耗掉：103 之后那条 200 仍须按无 body 解析 */
    buffer_free(&buf);
    buffer_init(&buf);
    ZERO(&ud, sizeof(ud_cxt));
    ud.status = 4;/* http 内部 INIT_NOBODY */
    status = PROT_INIT;
    _bput(&buf, "HTTP/1.1 103 Early Hints\r\nLink: </s.css>\r\n\r\n");
    _bput(&buf, "HTTP/1.1 200 OK\r\nContent-Length: 1234\r\n\r\n");
    pack = _t_http_unpack(1, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, buf_compare(&http_status(pack)[1], "103", 3));
    _http_pkfree(pack);
    status = PROT_INIT;
    pack = _t_http_unpack(1, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, buf_compare(&http_status(pack)[1], "200", 3));
    data = http_data(pack, &dlen);
    CuAssertTrue(tc, 0 == dlen);/* 登记活到了最终响应 */
    CuAssertTrue(tc, NULL == data);
    _http_pkfree(pack);
    _http_udfree(&ud);

    /* 标记只作用于紧随的那一条：第二条响应的 body 照常读出来 */
    buffer_free(&buf);
    buffer_init(&buf);
    ZERO(&ud, sizeof(ud_cxt));
    ud.status = 4;
    status = PROT_INIT;
    _bput(&buf, "HTTP/1.1 200 OK\r\nContent-Length: 1234\r\n\r\n");
    _bput(&buf, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi");
    pack = _t_http_unpack(1, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    _http_pkfree(pack);
    status = PROT_INIT;
    pack = _t_http_unpack(1, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    data = http_data(pack, &dlen);
    CuAssertTrue(tc, 2 == dlen);
    CuAssertTrue(tc, 0 == memcmp(data, "hi", 2));
    _http_pkfree(pack);

    _http_udfree(&ud);
    buffer_free(&buf);
}
/* 响应既无 Content-Length 又无 Transfer-Encoding：body 由连接关闭界定(RFC 7230 §3.3.3 规则 7)，
   按分片投递，末片由 _http_on_close 在关闭时补。顺带钉住原来的响应拆分隐患：
   body 里含空行时不能再被当成第二条响应的状态行 */
static void test_http_tillclose(CuTest *tc) {
    buffer_ctx buf;
    buffer_init(&buf);
    ud_cxt ud;
    ZERO(&ud, sizeof(ud_cxt));
    int32_t status = PROT_INIT;
    size_t dlen = 0;
    void *data;
    struct http_pack_ctx *pack;

    /* 1) 头部到齐即首片：无 CL/TE，状态行仍可读 */
    _bput(&buf, "HTTP/1.1 200 OK\r\n");
    _bput(&buf, "Content-Type: text/plain\r\n");
    _bput(&buf, "\r\n");
    pack = _t_http_unpack(1, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_SLICE_START));
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertTrue(tc, buf_compare(&http_status(pack)[1], "200", 3));
    _http_pkfree(pack);

    /* 2) 随后的字节全是 body，切片投出；body 内含空行照样是 body，不再被当状态行解析 */
    status = PROT_INIT;
    _bput(&buf, "line1\r\n\r\nline2");
    pack = _t_http_unpack(1, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_SLICE));
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    data = http_data(pack, &dlen);
    CuAssertTrue(tc, strlen("line1\r\n\r\nline2") == dlen);
    CuAssertTrue(tc, 0 == memcmp(data, "line1\r\n\r\nline2", dlen));
    _http_pkfree(pack);

    /* 3) 缓冲空了只报 MOREDATA，不产生空片 */
    status = PROT_INIT;
    pack = _t_http_unpack(1, &buf, &ud, NULL, &status);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_MOREDATA));

    /* 4) 关闭即末片：空载荷，且状态复位——再问一次返 NULL */
    pack = _http_on_close(&ud);
    CuAssertPtrNotNull(tc, pack);
    data = http_data(pack, &dlen);
    CuAssertTrue(tc, 0 == dlen);
    CuAssertTrue(tc, NULL == data);
    _http_pkfree(pack);
    CuAssertTrue(tc, NULL == _http_on_close(&ud));

    _http_udfree(&ud);
    buffer_free(&buf);
}
/* 规则 7 只对响应成立：请求无 CL/TE 就是无 body，不能进分片模式 */
static void test_http_tillclose_request_only(CuTest *tc) {
    buffer_ctx buf;
    buffer_init(&buf);
    ud_cxt ud;
    ZERO(&ud, sizeof(ud_cxt));
    int32_t status = PROT_INIT;

    _bput(&buf, "GET /x HTTP/1.1\r\nHost: h\r\n\r\n");
    struct http_pack_ctx *pack = _t_http_unpack(0, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_SLICE_START));
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    _http_pkfree(pack);
    /* 服务端侧关闭不该补末片 */
    CuAssertTrue(tc, NULL == _http_on_close(&ud));

    _http_udfree(&ud);
    buffer_free(&buf);
}
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

    struct http_pack_ctx *pack = _t_http_unpack(1, &buf, &ud, NULL, &status);
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
    binary_init_write(&bw, 0, 256);
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

    struct http_pack_ctx *pack = _t_http_unpack(0, &buf, &ud, NULL, &status);
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

// 同下，但显式给长度，可构造含 NUL 的报文（_bput 走 strlen，喂不进 NUL）
static void _http_smuggle_checkn(CuTest *tc, const char *raw, size_t rlens, int32_t expect_error) {
    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, (void *)raw, rlens);

    ud_cxt ud;
    ZERO(&ud, sizeof(ud_cxt));
    int32_t status = PROT_INIT;

    struct http_pack_ctx *pack = _t_http_unpack(0, &buf, &ud, NULL, &status);
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

// HTTP smuggling 辅助：输入 raw HTTP 字节，断言解析是否触发 PROT_ERROR
// expect_error=1 期望被拒；0 期望成功解析
static void _http_smuggle_check(CuTest *tc, const char *raw, int32_t expect_error) {
    _http_smuggle_checkn(tc, raw, strlen(raw), expect_error);
}

// RFC 7230 §3.3.2 / §3.3.3 — HTTP Request Smuggling 防御。
// 本套件全部按服务端方向(client=0)解析：喂进去的是请求，方向搞反的话
// _http_nobody_resp 那条 1xx/204/304 规则会被误用到请求上。
// 伪装成状态行的请求行(如 "HTTP/1.1 204 z")那一路在 test_http_nobody_status 用例 4b
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
    // 9. chunked 不是最后一个 transfer-coding（RFC 7230 §3.3.3 消息体长度无法可靠确定）→ 拒绝
    _http_smuggle_check(tc,
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Transfer-Encoding: chunked, gzip\r\n"
        "\r\n",
        1);
    // 10. chunked 是最后一个 transfer-coding（合法的层叠编码）→ 接受
    _http_smuggle_check(tc,
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Transfer-Encoding: gzip, chunked\r\n"
        "\r\n"
        "0\r\n\r\n",
        0);
    // 11. 两行独立 Transfer-Encoding（RFC 7230 §3.2.2 同名头视为逗号拼接，等价于
    //     "chunked, identity"，chunked 不在拼接后的最后）→ 拒绝，不可被拆行绕过第 9 条的校验
    _http_smuggle_check(tc,
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Transfer-Encoding: identity\r\n"
        "\r\n",
        1);
    // 12. 两行独立 Transfer-Encoding，拼接后 chunked 在最后 → 接受
    _http_smuggle_check(tc,
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Transfer-Encoding: identity\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "0\r\n\r\n",
        0);
    // 13. Transfer-Encoding 值末尾带多余逗号（RFC 7230 §7：list 末尾空元素须被忽略，
    //     "chunked," 语义等价于 "chunked"）→ 接受，不可因反向扫描把空元素当"最后一个 token"而误拒
    _http_smuggle_check(tc,
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Transfer-Encoding: chunked,\r\n"
        "\r\n"
        "0\r\n\r\n",
        0);
    // 14. 字段名与冒号间带空白 OWS（RFC 7230 §3.2.4 禁止）：`Transfer-Encoding :chunked` 若被当未知头静默入库，
    //     会与去 OWS 后识别为 chunked 的上游代理对报文边界产生分歧 → CL.TE 请求走私 → 拒绝
    _http_smuggle_check(tc,
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Transfer-Encoding : chunked\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello",
        1);
    // 15. 冒号前为制表符 OWS，同 §3.2.4 → 拒绝
    _http_smuggle_check(tc,
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Length\t: 5\r\n"
        "\r\n"
        "hello",
        1);
    // 16. 首个头部行以空格 OWS 开头(obs-fold 折叠)：旧实现把前导 OWS 静默剥离后当合法 chunked 入库,
    //     与将其视为 obs-fold 折进请求行(看不到 TE)的上游代理产生边界分歧 → 请求走私 → 拒绝
    _http_smuggle_check(tc,
        "POST / HTTP/1.1\r\n"
        " Transfer-Encoding: chunked\r\n"
        "\r\n"
        "0\r\n\r\n",
        1);
    // 17. 首个头部行以制表符 OWS 开头，同 §3.2.4 → 拒绝
    _http_smuggle_check(tc,
        "POST / HTTP/1.1\r\n"
        "\tHost: x\r\n"
        "\r\n",
        1);
    // 18. 后续头部行 obs-fold 续行(行首 OWS) → 拒绝(回归:行首 OWS 判定从尾部前瞻移到字段入口后仍生效)
    _http_smuggle_check(tc,
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        " Transfer-Encoding: chunked\r\n"
        "\r\n",
        1);
    _http_smuggle_check(tc,
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Length:\n5\r\n"
        "\r\n"
        "hello",
        1);
    _http_smuggle_check(tc,
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Length:\v5\r\n"
        "\r\n"
        "hello",
        1);
    _http_smuggle_check(tc,
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Length:\f5\r\n"
        "\r\n"
        "hello",
        1);
    _http_smuggle_check(tc,
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Length:\r5\r\n"
        "\r\n"
        "hello",
        1);
    _http_smuggle_check(tc,
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Length: 1 2\r\n"
        "\r\n"
        "hello",
        1);
    _http_smuggle_check(tc,
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Length: 5 \r\n"
        "\r\n"
        "hello",
        0);
    // 19. 字段值里的裸 LF：按 CRLF 收行会把后面那条 TE 折进 X 的值里，本模块的四道 TE/CL
    //     守卫按字段名精确比长匹配，一条都看不见它；上游若按裸 LF 切行就看得见 → 边界分歧 → 拒绝
    _http_smuggle_check(tc,
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "X: a\nTransfer-Encoding: chunked\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello",
        1);
    // 20. 字段名里的裸 LF，同理拒绝
    _http_smuggle_check(tc,
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "X\nY: 1\r\n"
        "\r\n",
        1);
    // 21. 字段行里的裸 CR（后面不是 LF）→ 拒绝
    _http_smuggle_check(tc,
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "X: a\rb\r\n"
        "\r\n",
        1);
    // 22. 字段名含 SP，不是 RFC 7230 §3.2.6 的 token → 拒绝
    _http_smuggle_check(tc,
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "X Y: 1\r\n"
        "\r\n",
        1);
    // 23. 字段名含分隔符 '(' → 拒绝
    _http_smuggle_check(tc,
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "X(Y): 1\r\n"
        "\r\n",
        1);
    // 24. 字段行里的 NUL → 拒绝（长度与字面量同源，避免两处手抄的字节数走样）
    static const char nul_field[] =
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "X: a\0b\r\n"
        "\r\n";
    _http_smuggle_checkn(tc, nul_field, sizeof(nul_field) - 1, 1);
    // 25. 全部 tchar 都得放行，别把合法头名误伤了
    _http_smuggle_check(tc,
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "X-Foo_bar.baz!#$%&'*+^`|~9: 1\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello",
        0);
}

// 首行三段拆分必须限定在首行内，空格不足的畸形首行不得越行吞并头部字段
static void test_http_status_line(CuTest *tc) {
    // 1. 请求行缺 HTTP 版本（只有一个空格），第二个空格不得越行匹配到 Host 行 → 拒绝
    _http_smuggle_check(tc,
        "GET /\r\n"
        "Host: a\r\n"
        "\r\n",
        1);
    // 2. 状态行无 reason-phrase 且无尾随空格（RFC 7230 §3.1.2 status-code 后必须有 SP）→ 拒绝
    _http_smuggle_check(tc,
        "HTTP/1.1 200\r\n"
        "Content-Length: 0\r\n"
        "\r\n",
        1);
    // 3. reason-phrase 为空但保留尾随空格 → 合法，接受
    _http_smuggle_check(tc,
        "HTTP/1.1 200 \r\n"
        "Content-Length: 0\r\n"
        "\r\n",
        0);
    // 4. 报文以空行开头 → 拒绝
    _http_smuggle_check(tc,
        "\r\n"
        "GET / HTTP/1.1\r\n"
        "\r\n",
        1);
    // 5. 请求行前导空格(SP)：不得静默剥离，须与字段行一致拒绝，防上游折进的边界分歧走私
    _http_smuggle_check(tc,
        " GET /admin HTTP/1.1\r\n"
        "Host: a\r\n"
        "\r\n",
        1);
    // 6. 请求行前导制表符(HTAB)：同上拒绝
    _http_smuggle_check(tc,
        "\tGET /admin HTTP/1.1\r\n"
        "Host: a\r\n"
        "\r\n",
        1);
    _http_smuggle_check(tc,
        "GET /admin /public HTTP/1.1\r\n"
        "Host: a\r\n"
        "\r\n",
        1);
    _http_smuggle_check(tc,
        "GET /a JUNKVERSION\r\n"
        "Host: a\r\n"
        "\r\n",
        1);
    _http_smuggle_check(tc,
        "GET / http/1.1\r\n"
        "Host: a\r\n"
        "\r\n",
        1);
    _http_smuggle_check(tc,
        "GET / HTTP/11\r\n"
        "Host: a\r\n"
        "\r\n",
        1);
    // 7. 请求行前导裸 LF：本端读成方法名带前导 LF 的一个请求，按裸 LF 切行的上游读成两个 → 拒绝
    _http_smuggle_check(tc,
        "\nGET / HTTP/1.1\r\n"
        "Host: a\r\n"
        "\r\n",
        1);
    // 8. 请求行中间的裸 CR（后面不是 LF）→ 拒绝
    _http_smuggle_check(tc,
        "GET /a\rb HTTP/1.1\r\n"
        "Host: a\r\n"
        "\r\n",
        1);
    // 9. 请求行里的 NUL → 拒绝
    static const char nul_line[] =
        "GET /a\0b HTTP/1.1\r\n"
        "Host: a\r\n"
        "\r\n";
    _http_smuggle_checkn(tc, nul_line, sizeof(nul_line) - 1, 1);
    _http_smuggle_check(tc,
        "GET /a HTTP/1.0\r\n"
        "Content-Length: 0\r\n"
        "\r\n",
        0);
    // 10. 状态行 reason-phrase 含空格 → 合法。首行只按前两个 SP 切三段，多出来的空格全归 reason；
    //     若切分实现改成"必须恰好两个 SP",这类再常见不过的响应会被整片拒掉
    buffer_ctx rbuf;
    buffer_init(&rbuf);
    _bput(&rbuf, "HTTP/1.1 404 Not Found\r\n");
    _bput(&rbuf, "Content-Length: 0\r\n");
    _bput(&rbuf, "\r\n");
    ud_cxt rud;
    ZERO(&rud, sizeof(ud_cxt));
    int32_t rstatus = PROT_INIT;
    struct http_pack_ctx *rpack = _t_http_unpack(1, &rbuf, &rud, NULL, &rstatus);
    CuAssertPtrNotNull(tc, rpack);
    CuAssertTrue(tc, !BIT_CHECK(rstatus, PROT_ERROR));
    buf_ctx *rst = http_status(rpack);
    CuAssertTrue(tc, buf_compare(&rst[0], "HTTP/1.1", 8));
    CuAssertTrue(tc, buf_compare(&rst[1], "404", 3));
    CuAssertTrue(tc, buf_compare(&rst[2], "Not Found", 9));
    _http_pkfree(rpack);
    _http_udfree(&rud);
    buffer_free(&rbuf);
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
    struct http_pack_ctx *pack = _t_http_unpack(0, &buf, &ud, NULL, &status);// 1) header；chunked 分支不设 ud->context，手动释放
    CuAssertPtrNotNull(tc, pack);
    _http_pkfree(pack);
    status = PROT_INIT;
    size_t left = buffer_size(&buf);
    pack = _t_http_unpack(0, &buf, &ud, NULL, &status);// 2) chunk-size 行
    if (expect_error) {
        CuAssertTrue(tc, NULL == pack);
        CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
    } else {
        CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
        // status 刚被清成 PROT_INIT(=0)，只判"没有 PROT_ERROR 位"的话什么都不做也能过。
        // 认下这行的唯一凭据是它真被吃掉了
        CuAssertTrue(tc, buffer_size(&buf) < left);
    }
    _http_udfree(&ud);
    buffer_free(&buf);
}
// RFC 7230 §4.1：chunk-size 严格按 1*HEXDIG；前导空白 / '+' '-' / "0x" 都是旧实现走 strtoul 时被放过的走私写法，一律拒
static void test_http_chunked_size_smuggle(CuTest *tc) {
    _chunked_size_check(tc, " 0x10\r\n", 1);// 前导空白 + 0x（旧代码绕过 0x 拒绝）
    _chunked_size_check(tc, "0x10\r\n", 1);// 无空白 0x
    _chunked_size_check(tc, "+5\r\n", 1);// 符号位（旧实现 strtoul 会吞 '+'）
    _chunked_size_check(tc, " 5\r\n", 1);// 前导空白
    _chunked_size_check(tc, "a\r\n", 0);// 合法 hex，不误拒（解析成功后等 data）
}

// RFC 7230 §4.1：chunk = chunk-size [ chunk-ext ] CRLF。ext 有多长都不该影响 chunk-size 的解析。
// 原来是把整行拷进 lensbuf[16] 再解析，于是 AWS 的 aws-chunked（"400;chunk-signature=<64 位 hex>"，
// 80 多字节）和零填充写法（"0000000000000005" 正好 16 字节）这两种合法传输都被当协议错断连
static void test_http_chunked_ext(CuTest *tc) {
    // 1. AWS aws-chunked 风格：长 chunk-ext
    char line[256];
    char sig[65];
    memset(sig, 'a', sizeof(sig) - 1);
    sig[sizeof(sig) - 1] = '\0';
    SNPRINTF(line, sizeof(line), "400;chunk-signature=%s\r\n", sig);
    _chunked_size_check(tc, line, 0);
    // 2. 零填充到 16 字节：旧实现正好卡在 lensbuf[16] 的边界上
    _chunked_size_check(tc, "0000000000000005\r\n", 0);
    // 3. 空 ext
    _chunked_size_check(tc, "5;\r\n", 0);
    // 4. 只有 ext 没有 chunk-size → 拒
    _chunked_size_check(tc, ";chunk-signature=x\r\n", 1);
    // 5. hex 段里混进非 hex（ext 已切走，这里不再放行 ';' 之外的任何尾巴）→ 拒
    _chunked_size_check(tc, "5g;ext=1\r\n", 1);
    // 6. hex 段超过 16 位（64 位十六进制的上限）→ 拒
    _chunked_size_check(tc, "00000000000000005\r\n", 1);
    // 7. 整行超过 HTTP_MAX_HEADLENS → 拒（ext 再长也有个头）
    char *big;
    MALLOC(big, HTTP_MAX_HEADLENS + 64);
    memset(big, 'e', HTTP_MAX_HEADLENS + 8);
    memcpy(big, "5;x=", 4);
    memcpy(big + HTTP_MAX_HEADLENS + 8, "\r\n", 3);
    _chunked_size_check(tc, big, 1);
    FREE(big);
}

// chunk-size 声明的长度受 HTTP_MAX_CHUNK_LENS 限制，判据是 > 上限，故恰等于上限合法。
// 不挡的话 _http_chunkedpack 会在解出长度行的当下就按这个值 MALLOC —— "fffffffe\r\n" 一行 4GB
static void test_http_chunked_lens_bound(CuTest *tc) {
    char line[64];
    // 恰等于上限：接着等 data，不判协议错
    SNPRINTF(line, sizeof(line), "%zx\r\n", (size_t)HTTP_MAX_CHUNK_LENS);
    _chunked_size_check(tc, line, 0);
    // 上限 +1：拒
    SNPRINTF(line, sizeof(line), "%zx\r\n", (size_t)HTTP_MAX_CHUNK_LENS + 1);
    _chunked_size_check(tc, line, 1);
    // 16 位十六进制满值：逐位累加到第 5 位就超上限，拒
    _chunked_size_check(tc, "ffffffffffffffff\r\n", 1);
}

// 长度行合法时再核数值：后面正好补 expect 字节数据 + CRLF，第二次 unpack 须吐出 expect 字节的块并把缓冲吃空。
// 数值解错的话要么等不齐(MOREDATA)，要么块尾 CRLF 对不上(ERROR)，都过不了
static void _chunked_size_value(CuTest *tc, const char *sizeline, size_t expect) {
    buffer_ctx buf;
    buffer_init(&buf);
    _bput(&buf, "GET / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n");
    _bput(&buf, sizeline);
    char *body;
    MALLOC(body, expect + CRLF_SIZE);
    memset(body, 'x', expect);
    memcpy(body + expect, "\r\n", CRLF_SIZE);
    buffer_append(&buf, body, expect + CRLF_SIZE);
    FREE(body);
    ud_cxt ud;
    ZERO(&ud, sizeof(ud_cxt));
    int32_t status = PROT_INIT;
    struct http_pack_ctx *pack = _t_http_unpack(0, &buf, &ud, NULL, &status);// 1) header
    CuAssertPtrNotNull(tc, pack);
    _http_pkfree(pack);
    status = PROT_INIT;
    pack = _t_http_unpack(0, &buf, &ud, NULL, &status);// 2) 长度行 + 数据块
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertIntEquals(tc, 2, http_chunked(pack));
    size_t dlen = 0;
    http_data(pack, &dlen);
    CuAssertTrue(tc, expect == dlen);
    CuAssertTrue(tc, 0 == buffer_size(&buf));
    _http_pkfree(pack);
    _http_udfree(&ud);
    buffer_free(&buf);
}
// chunk-size 改成 fromhex 逐位累加、每位卡 HTTP_MAX_CHUNK_LENS 后，接受集合须与原来
// "首字符 HEXDIG + 非 0x + strtoul 整段吃完 + 不超上限"一致，且数值与 strtoul 按 16 进制解出的相同
static void test_http_chunked_size_strict(CuTest *tc) {
    char line[64];
    // 拒：符号 / 0X / 空白夹在 hex 与 ';' 之间或行尾 / 零填充后超上限 / 16 位大值
    _chunked_size_check(tc, "-10\r\n", 1);
    _chunked_size_check(tc, "0X10\r\n", 1);
    _chunked_size_check(tc, "+10\r\n", 1);
    _chunked_size_check(tc, " 10\r\n", 1);
    _chunked_size_check(tc, "\t10\r\n", 1);
    _chunked_size_check(tc, "10 \r\n", 1);
    _chunked_size_check(tc, "10 ;ext=1\r\n", 1);
    SNPRINTF(line, sizeof(line), "%016zx\r\n", (size_t)HTTP_MAX_CHUNK_LENS + 1);
    _chunked_size_check(tc, line, 1);// 16 位零填充的上限 + 1
    _chunked_size_check(tc, "1000000000000000\r\n", 1);// 2^60，逐位卡上限时不能先溢出再判
    _chunked_size_check(tc, "00000000000000010\r\n", 1);// 17 位，哪怕值只有 16
    // 收，并核数值
    _chunked_size_value(tc, "10\r\n", 16);// 按 16 进制，不是 10
    _chunked_size_value(tc, "10;ext=1\r\n", 16);
    _chunked_size_value(tc, "0000000000000010\r\n", 16);// 16 位零填充
    _chunked_size_value(tc, "aB\r\n", 0xab);// 大小写混用
    // 恰等于上限：大写，以及 16 位零填充的大写
    SNPRINTF(line, sizeof(line), "%zX\r\n", (size_t)HTTP_MAX_CHUNK_LENS);
    _chunked_size_value(tc, line, (size_t)HTTP_MAX_CHUNK_LENS);
    SNPRINTF(line, sizeof(line), "%016zX\r\n", (size_t)HTTP_MAX_CHUNK_LENS);
    _chunked_size_value(tc, line, (size_t)HTTP_MAX_CHUNK_LENS);
}

// chunk-size 行迟迟等不到 CRLF 时必须有上限。没有的话对端只要一直发不带 CRLF 的字节，
// 接收缓冲就一直涨，一条连接、不用认证就能把内存吃光；头块与 trailer 块都是按 HTTP_MAX_HEADLENS 挡的
static void test_http_chunked_size_no_crlf_bound(CuTest *tc) {
    buffer_ctx buf;
    buffer_init(&buf);
    _bput(&buf, "GET / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n");
    ud_cxt ud;
    ZERO(&ud, sizeof(ud_cxt));
    int32_t status = PROT_INIT;
    struct http_pack_ctx *pack = _t_http_unpack(0, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    _http_pkfree(pack);

    char filler[1024];
    memset(filler, 'a', sizeof(filler));
    // 未超上限：仍然是"等更多数据"，不能误拒
    buffer_append(&buf, filler, sizeof(filler));
    status = PROT_INIT;
    pack = _t_http_unpack(0, &buf, &ud, NULL, &status);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_MOREDATA));
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));

    // 累计超过 HTTP_MAX_HEADLENS 仍无 CRLF：判协议错，不再继续收
    while (buffer_size(&buf) <= HTTP_MAX_HEADLENS) {
        buffer_append(&buf, filler, sizeof(filler));
    }
    status = PROT_INIT;
    pack = _t_http_unpack(0, &buf, &ud, NULL, &status);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));

    _http_udfree(&ud);
    buffer_free(&buf);
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

// chunked trailer 超 HTTP_MAX_HEADLENS=4KB 应 PROT_ERROR；
// 但上限只量 trailer 自身，buf 里流水线跟随的下一个请求不得计入
static void test_http_chunked_trailer_limit(CuTest *tc) {
    buffer_ctx buf;
    buffer_init(&buf);
    _bput(&buf, "HTTP/1.1 200 OK\r\n");
    _bput(&buf, "Transfer-Encoding: chunked\r\n");
    _bput(&buf, "\r\n");
    _bput(&buf, "5\r\nhello\r\n");
    _bput(&buf, "0\r\n");// chunked 终止行，后跟 trailer
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
    pack = _t_http_unpack(1, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    _http_pkfree(pack);

    // 第 2 轮：解析 chunked "5\r\nhello\r\n"
    status = PROT_INIT;
    pack = _t_http_unpack(1, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    _http_pkfree(pack);

    // 第 3 轮：进入终止块路径，5KB > 4KB 无 \r\n\r\n → PROT_ERROR
    status = PROT_INIT;
    pack = _t_http_unpack(1, &buf, &ud, NULL, &status);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));

    _http_udfree(&ud);
    buffer_free(&buf);

    char rest[4089];
    buffer_init(&buf);
    _bput(&buf, "HTTP/1.1 200 OK\r\n");
    _bput(&buf, "Transfer-Encoding: chunked\r\n");
    _bput(&buf, "\r\n");
    _bput(&buf, "5\r\nhello\r\n");
    _bput(&buf, "0\r\n");
    _bput(&buf, "X: y\r\n\r\n");
    memset(rest, 'A', sizeof(rest));
    buffer_append(&buf, rest, sizeof(rest));

    ZERO(&ud, sizeof(ud_cxt));
    status = PROT_INIT;
    pack = _t_http_unpack(1, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    _http_pkfree(pack);
    status = PROT_INIT;
    pack = _t_http_unpack(1, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    _http_pkfree(pack);

    status = PROT_INIT;
    pack = _t_http_unpack(1, &buf, &ud, NULL, &status);
    CuAssert(tc, "8-byte complete trailer plus 4089 bytes pipelined data (4097>4096 total) must not be rejected",
        NULL != pack && !BIT_CHECK(status, PROT_ERROR));
    CuAssert(tc, "final chunk must set PROT_SLICE_END", BIT_CHECK(status, PROT_SLICE_END));
    CuAssert(tc, "only the trailer is consumed, pipelined data must stay in buf",
        sizeof(rest) == buffer_size(&buf));
    _http_pkfree(pack);

    _http_udfree(&ud);
    buffer_free(&buf);
}

/* 回归：1xx/204/304 响应一律以头部后的空行结束（RFC 7230 §3.3.3 规则 1）。
   修复前只看 CL/TE，304 带 Content-Length 会进 CONTENT 态，
   在 keep-alive 上把下一条响应的头部吃成本条的 body */
static void test_http_nobody_status(CuTest *tc) {
    buffer_ctx buf;
    ud_cxt ud;
    int32_t status;
    struct http_pack_ctx *pack;
    buf_ctx *st;
    void *data;
    size_t dlen;

    // 1. 304 带 Content-Length：本条无 body，紧跟的 200 必须原样解出
    buffer_init(&buf);
    _bput(&buf, "HTTP/1.1 304 Not Modified\r\n");
    _bput(&buf, "ETag: \"v1\"\r\n");
    _bput(&buf, "Content-Length: 11\r\n");
    _bput(&buf, "\r\n");
    _bput(&buf, "HTTP/1.1 200 OK\r\n");
    _bput(&buf, "Content-Length: 3\r\n");
    _bput(&buf, "\r\n");
    _bput(&buf, "abc");
    ZERO(&ud, sizeof(ud_cxt));
    status = PROT_INIT;
    pack = _t_http_unpack(1, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    st = http_status(pack);
    CuAssertTrue(tc, buf_compare(&st[1], "304", 3));
    dlen = 0;
    http_data(pack, &dlen);
    CuAssert(tc, "304 must not carry a body", 0 == dlen);
    _http_pkfree(pack);
    status = PROT_INIT;
    pack = _t_http_unpack(1, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    st = http_status(pack);
    CuAssertTrue(tc, buf_compare(&st[1], "200", 3));
    dlen = 0;
    data = http_data(pack, &dlen);
    CuAssert(tc, "the response pipelined after a 304 must survive intact", 3 == dlen);
    CuAssertTrue(tc, 0 == memcmp(data, "abc", 3));
    _http_pkfree(pack);
    _http_udfree(&ud);
    buffer_free(&buf);

    // 2. 204 带 Content-Length：同上
    buffer_init(&buf);
    _bput(&buf, "HTTP/1.1 204 No Content\r\n");
    _bput(&buf, "Content-Length: 5\r\n");
    _bput(&buf, "\r\n");
    _bput(&buf, "HTTP/1.1 200 OK\r\n");
    _bput(&buf, "Content-Length: 0\r\n");
    _bput(&buf, "\r\n");
    ZERO(&ud, sizeof(ud_cxt));
    status = PROT_INIT;
    pack = _t_http_unpack(1, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    dlen = 0;
    http_data(pack, &dlen);
    CuAssert(tc, "204 must not carry a body", 0 == dlen);
    _http_pkfree(pack);
    status = PROT_INIT;
    pack = _t_http_unpack(1, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    st = http_status(pack);
    CuAssertTrue(tc, buf_compare(&st[1], "200", 3));
    _http_pkfree(pack);
    _http_udfree(&ud);
    buffer_free(&buf);

    // 3. 1xx 中间响应：100 Continue 之后的真正响应不能被吞掉
    buffer_init(&buf);
    _bput(&buf, "HTTP/1.1 100 Continue\r\n");
    _bput(&buf, "Content-Length: 9\r\n");
    _bput(&buf, "\r\n");
    _bput(&buf, "HTTP/1.1 201 Created\r\n");
    _bput(&buf, "Content-Length: 2\r\n");
    _bput(&buf, "\r\n");
    _bput(&buf, "hi");
    ZERO(&ud, sizeof(ud_cxt));
    status = PROT_INIT;
    pack = _t_http_unpack(1, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    dlen = 0;
    http_data(pack, &dlen);
    CuAssert(tc, "100 must not carry a body", 0 == dlen);
    _http_pkfree(pack);
    status = PROT_INIT;
    pack = _t_http_unpack(1, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    st = http_status(pack);
    CuAssertTrue(tc, buf_compare(&st[1], "201", 3));
    dlen = 0;
    data = http_data(pack, &dlen);
    CuAssertTrue(tc, 2 == dlen && 0 == memcmp(data, "hi", 2));
    _http_pkfree(pack);
    _http_udfree(&ud);
    buffer_free(&buf);

    // 4b. 服务端方向(client=0)：伪造的响应式请求行不得套用该规则，否则声明的 body
    //     会留在缓冲里被当成第二个请求解析出来——反代前置时就是一次请求走私
    buffer_init(&buf);
    _bput(&buf, "HTTP/1.1 204 z\r\n");
    // 尾随的这一整条伪装请求恰 32 字节：21 + 9 + 2
    _bput(&buf, "Content-Length: 32\r\n");
    _bput(&buf, "\r\n");
    _bput(&buf, "GET /admin HTTP/1.1\r\nHost: a\r\n\r\n");
    ZERO(&ud, sizeof(ud_cxt));
    status = PROT_INIT;
    pack = _t_http_unpack(0, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    dlen = 0;
    http_data(pack, &dlen);
    CuAssert(tc, "on a request stream the declared body must still be consumed", 32 == dlen);
    CuAssert(tc, "nothing may be left behind for a second parse", 0 == buffer_size(&buf));
    _http_pkfree(pack);
    _http_udfree(&ud);
    buffer_free(&buf);

    // 4. 反向用例：请求行首段不是 HTTP-version，request-target 恰为 "204" 也不得套用该规则
    buffer_init(&buf);
    _bput(&buf, "GET 204 HTTP/1.1\r\n");
    _bput(&buf, "Content-Length: 3\r\n");
    _bput(&buf, "\r\n");
    _bput(&buf, "abc");
    ZERO(&ud, sizeof(ud_cxt));
    status = PROT_INIT;
    pack = _t_http_unpack(1, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    dlen = 0;
    data = http_data(pack, &dlen);
    CuAssert(tc, "requests must keep their body regardless of the target text", 3 == dlen);
    CuAssertTrue(tc, 0 == memcmp(data, "abc", 3));
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

    struct http_pack_ctx *pack = _t_http_unpack(1, &buf, &ud, NULL, &status);
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
    binary_init_write(&bw, 0, 0);
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
    binary_init_write(&bw, 0, 0);
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
    pack = _t_http_unpack(1, &playback, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertIntEquals(tc, 1, http_chunked(pack));
    CuAssertTrue(tc, BIT_CHECK(status, PROT_SLICE_START));
    _http_pkfree(pack);

    // 2) chunk1 "AAA"
    status = PROT_INIT;
    pack = _t_http_unpack(1, &playback, &ud, NULL, &status);
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
    pack = _t_http_unpack(1, &playback, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertIntEquals(tc, 2, http_chunked(pack));
    d = http_data(pack, &dlen);
    CuAssertIntEquals(tc, 4, (int)dlen);
    CuAssertTrue(tc, 0 == memcmp(d, "BBBB", 4));
    CuAssertTrue(tc, BIT_CHECK(status, PROT_SLICE));
    _http_pkfree(pack);

    // 4) 终止块
    status = PROT_INIT;
    pack = _t_http_unpack(1, &playback, &ud, NULL, &status);
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
        ud_cxt ud;
        int32_t status;
        redis_pack_ctx *pack = _t_redis_one(&buf, &ud, "+OK\r\n", &status);
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
        ud_cxt ud;
        int32_t status;
        redis_pack_ctx *pack = _t_redis_one(&buf, &ud, "-ERR some message\r\n", &status);
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
        ud_cxt ud;
        int32_t status;
        redis_pack_ctx *pack = _t_redis_one(&buf, &ud, ":42\r\n", &status);
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
        ud_cxt ud;
        int32_t status;
        redis_pack_ctx *pack = _t_redis_one(&buf, &ud, "_\r\n", &status);
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

    redis_pack_ctx *pack = _t_redis_unpack(0, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, RESP_BSTRING == pack->prot);
    CuAssertTrue(tc, 6 == pack->len);
    CuAssertTrue(tc, 0 == memcmp(pack->data, "foobar", 6));

    _redis_pkfree(pack);
    _redis_udfree(&ud);
    buffer_free(&buf);
}

// Null Bulk String：$-1\r\n → pack->len == -1，修复前按 UINT64_MAX 比对上限，永久拒绝此合法包
static void test_redis_null_bulk(CuTest *tc) {
    buffer_ctx buf;
    buffer_init(&buf);
    _bput(&buf, "$-1\r\n");

    ud_cxt ud;
    ZERO(&ud, sizeof(ud_cxt));
    int32_t status = PROT_INIT;

    redis_pack_ctx *pack = _t_redis_unpack(0, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, RESP_BSTRING == pack->prot);
    CuAssertTrue(tc, -1 == pack->len);
    CuAssertTrue(tc, NULL == pack->next);

    _redis_pkfree(pack);
    _redis_udfree(&ud);
    buffer_free(&buf);
}

// 投一段原始回复，断言被判协议错。下面两个拒收类用例共用
static void _redis_reject_check(CuTest *tc, const char *raw, size_t rlens) {
    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, (void *)raw, rlens);

    ud_cxt ud;
    ZERO(&ud, sizeof(ud_cxt));
    int32_t status = PROT_INIT;

    redis_pack_ctx *pack = _t_redis_unpack(0, &buf, &ud, NULL, &status);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));

    _redis_udfree(&ud);
    buffer_free(&buf);
}
// Bulk 的数据体后面必须紧跟 CRLF。不校验的话对端只要把长度报小，剩下的字节就会被当成
// 下一条回复的开头解析，两边对包边界的认知从此错开
static void test_redis_bulk_bad_crlf(CuTest *tc) {
    // 长度对，尾巴不是 CRLF
    _redis_reject_check(tc, "$6\r\nfoobarXX", 12);
    // 只有 '\r' 对，'\n' 位置是别的字节
    _redis_reject_check(tc, "$6\r\nfoobar\rX", 12);
    // 长度报小：CRLF 落在数据体中间，是典型的包边界错位构造
    _redis_reject_check(tc, "$3\r\nfoobar\r\n", 12);
    // 反向不误拒：长度与尾部 CRLF 都对
    buffer_ctx buf;
    ud_cxt ud;
    int32_t status;
    redis_pack_ctx *pack = _t_redis_one(&buf, &ud, "$6\r\nfoobar\r\n", &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertTrue(tc, 6 == pack->len);
    // 整包已消费干净：尾部 CRLF 也算在 total 里
    CuAssertTrue(tc, 0 == buffer_size(&buf));
    _redis_pkfree(pack);
    _redis_udfree(&ud);
    buffer_free(&buf);
}
// 长度行严格按 ['-']1*DIGIT（'-' 只为 null 的 "-1"）；前导空白与 '+' 是旧实现走 strtoll 时被放过的写法，
// 放行会与严格解析的对端切出不同的包边界
static void test_redis_len_first_char(CuTest *tc) {
    // bulk 侧
    _redis_reject_check(tc, "$+6\r\nfoobar\r\n", 13);
    _redis_reject_check(tc, "$ 6\r\nfoobar\r\n", 13);
    _redis_reject_check(tc, "$x\r\nfoobar\r\n", 12);
    // 聚合侧
    _redis_reject_check(tc, "*+2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n", 23);
    _redis_reject_check(tc, "* 2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n", 23);
    // 首字符过了白名单，数值仍须 >= -1
    _redis_reject_check(tc, "$-2\r\n", 5);
    _redis_reject_check(tc, "*-2\r\n", 5);
}
// 投一段长度行打头的回复，断言整段吃完、首节点类型对、长度(bulk 看 len，聚合看 nelem)等于 expect
static void _redis_len_accept(CuTest *tc, const char *raw, int32_t prot, int64_t expect) {
    buffer_ctx buf;
    ud_cxt ud;
    int32_t status;
    redis_pack_ctx *pack = _t_redis_one(&buf, &ud, raw, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertTrue(tc, prot == pack->prot);
    CuAssertTrue(tc, expect == (RESP_BSTRING == prot ? pack->len : pack->nelem));
    CuAssertTrue(tc, 0 == buffer_size(&buf));
    _redis_pkfree(pack);
    _redis_udfree(&ud);
    buffer_free(&buf);
}
// 长度行改走 parse_int64_strict 后，接受集合须与原来"首字符是数字或 '-'、strtoll 整段吃完、不 ERANGE、>= -1"逐条一致。
// 首字符 '+'/空白、"-2" 等已在 test_redis_len_first_char，这里补数值本身的边界
static void test_redis_len_strict(CuTest *tc) {
    static const char *rejects[] = {
        "$-\r\n",// 只有负号
        "*-\r\n",
        "$--1\r\n",
        "$1-\r\n",
        "$5 \r\nhello\r\n",// 尾随空白：strtoll 停在空格上，没吃完整段
        "$0x5\r\n",// 十进制里没有 0x
        "$9223372036854775808\r\n",// INT64_MAX + 1：原来 ERANGE
        "*9223372036854775808\r\n",
        "$-9223372036854775809\r\n",// 负向越过 INT64_MIN
        "$99999999999999999999\r\n",// 20 位，连 uint64 都装不下
    };
    char line[128];
    size_t i;
    // null 与空：-1 是 null，"-0" 与 "-01" 按 strtoll 分别是 0 和 -1
    _redis_len_accept(tc, "$-1\r\n", RESP_BSTRING, -1);
    _redis_len_accept(tc, "*-1\r\n", RESP_ARRAY, -1);
    _redis_len_accept(tc, "$-0\r\n\r\n", RESP_BSTRING, 0);
    _redis_len_accept(tc, "*-0\r\n", RESP_ARRAY, 0);
    _redis_len_accept(tc, "$-01\r\n", RESP_BSTRING, -1);
    // 前导零照常按十进制：007 就是 7
    _redis_len_accept(tc, "$007\r\nfoobarx\r\n", RESP_BSTRING, 7);
    // 63 位零填充(长度行缓冲 64 字节的上限内)：前导零再多也不能被当成溢出
    line[0] = '$';
    memset(line + 1, '0', 62);
    memcpy(line + 63, "3\r\nfoo\r\n", sizeof("3\r\nfoo\r\n"));
    _redis_len_accept(tc, line, RESP_BSTRING, 3);
    for (i = 0; i < ARRAY_SIZE(rejects); i++) {
        _redis_reject_check(tc, rejects[i], strlen(rejects[i]));
    }
}
// RESP2 的空数组两种写法都是合法回复：*-1 是 null array（BLPOP 超时就发这个），
// *0 是零元素数组。谁在这里补一条 nelem < 0 的"自然防御"，每次 BLPOP 超时就会掉连接
static void test_redis_empty_array(CuTest *tc) {
    const char *raws[2] = { "*-1\r\n", "*0\r\n" };
    const int64_t expect[2] = { -1, 0 };
    buffer_ctx buf;
    ud_cxt ud;
    int32_t status;
    redis_pack_ctx *pack;
    size_t i;
    for (i = 0; i < ARRAY_SIZE(raws); i++) {
        buffer_init(&buf);
        _bput(&buf, raws[i]);
        ZERO(&ud, sizeof(ud_cxt));
        status = PROT_INIT;
        pack = _t_redis_unpack(0, &buf, &ud, NULL, &status);
        CuAssertPtrNotNull(tc, pack);
        CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
        CuAssertTrue(tc, RESP_ARRAY == pack->prot);
        CuAssertTrue(tc, expect[i] == pack->nelem);
        // 一个元素都没有，链表就此结束
        CuAssertTrue(tc, NULL == pack->next);
        CuAssertTrue(tc, 0 == buffer_size(&buf));
        _redis_pkfree(pack);
        _redis_udfree(&ud);
        buffer_free(&buf);
    }
}

/* 数组：*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n */
static void test_redis_array(CuTest *tc) {
    buffer_ctx buf;
    buffer_init(&buf);
    _bput(&buf, "*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");

    ud_cxt ud;
    ZERO(&ud, sizeof(ud_cxt));
    int32_t status = PROT_INIT;

    redis_pack_ctx *pack = _t_redis_unpack(0, &buf, &ud, NULL, &status);
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
// 认不出的转换整条拒掉：之前是"抄成字面量继续走"，既不取走对应的可变参数、也不跳过转换符，
// 后面每个转换都读到错位一格的参数——%s 拿到整数当指针实测就是段错误
static void test_redis_pack_bad_format(CuTest *tc) {
    size_t size = 1;

    // %j / %t / %L 都不在支持列表里
    CuAssertTrue(tc, NULL == redis_pack(&size, "SETEX %s %jd %s", "k", 3600, "v"));
    CuAssertTrue(tc, 0 == size);
    size = 1;
    CuAssertTrue(tc, NULL == redis_pack(&size, "SET %s %td", "k", 1));
    CuAssertTrue(tc, 0 == size);

    // 长度修饰后面跟的不是整数转换
    size = 1;
    CuAssertTrue(tc, NULL == redis_pack(&size, "SET %s %lq", "k", 1));
    CuAssertTrue(tc, 0 == size);
    size = 1;
    CuAssertTrue(tc, NULL == redis_pack(&size, "SET %s %zq", "k", 1));
    CuAssertTrue(tc, 0 == size);

    // 只有标志/宽度没有转换符就到串尾
    size = 1;
    CuAssertTrue(tc, NULL == redis_pack(&size, "GET %"));
    CuAssertTrue(tc, 0 == size);
    size = 1;
    CuAssertTrue(tc, NULL == redis_pack(&size, "GET %-8"));
    CuAssertTrue(tc, 0 == size);

    // 支持列表里的照常成功，别把好的一起拒了
    char *cmd = redis_pack(&size, "SETEX %s %zu %s", "k", (size_t)3600, "v");
    CuAssertPtrNotNull(tc, cmd);
    CuAssertTrue(tc, size > 0);
    FREE(cmd);
    cmd = redis_pack(&size, "SET %s %lld", "k", (long long)-1);
    CuAssertPtrNotNull(tc, cmd);
    FREE(cmd);
    cmd = redis_pack(&size, "SET %s %.2f", "k", 1.5);
    CuAssertPtrNotNull(tc, cmd);
    FREE(cmd);
    cmd = redis_pack(&size, "SET %s 100%%", "k");
    CuAssertPtrNotNull(tc, cmd);
    FREE(cmd);
}
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

    redis_pack_ctx *pack = _t_redis_unpack(0, &buf, &ud, NULL, &status);
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

    redis_pack_ctx *pack = _t_redis_unpack(0, &buf, &ud, NULL, &status);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_MOREDATA));
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));

    _redis_udfree(&ud);
    buffer_free(&buf);
}

// 一条多元素回复分两次到达:已解析的节点留在 ud 里跨调用续挂,补齐后整条交出;
// 同一 ud 紧接着的下一条回复要从空链表重新开始,不能挂到已交出那条的尾上
static void test_redis_resume_across_calls(CuTest *tc) {
    buffer_ctx buf;
    ud_cxt ud;
    int32_t status;
    redis_pack_ctx *pack, *p1, *p2, *p3;
    pack = _t_redis_one(&buf, &ud, "*2\r\n:1\r\n", &status);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_MOREDATA));
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));

    _bput(&buf, ":2\r\n*1\r\n:3\r\n");
    status = PROT_INIT;
    pack = _t_redis_unpack(0, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, RESP_ARRAY == pack->prot && 2 == pack->nelem);
    p1 = pack->next;
    CuAssertTrue(tc, NULL != p1 && RESP_INTEGER == p1->prot && 1 == p1->ival);
    p2 = p1->next;
    CuAssertTrue(tc, NULL != p2 && RESP_INTEGER == p2->prot && 2 == p2->ival);
    CuAssertTrue(tc, NULL == p2->next);
    _redis_pkfree(pack);// 先交还第一条:第二条若还往它尾上挂,下面拿到的是 NULL(ASan 下直接报 UAF)

    status = PROT_INIT;
    pack = _t_redis_unpack(0, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, RESP_ARRAY == pack->prot && 1 == pack->nelem);
    p3 = pack->next;
    CuAssertTrue(tc, NULL != p3 && RESP_INTEGER == p3->prot && 3 == p3->ival);
    CuAssertTrue(tc, NULL == p3->next);
    CuAssertTrue(tc, 0 == buffer_size(&buf));
    _redis_pkfree(pack);
    _redis_udfree(&ud);
    buffer_free(&buf);
}
// 同一 ud 连解两条大回复:节点计数每条回复清零,两条合计超 REDIS_MAX_NODES(1<<18)也不能误判超限
static void test_redis_back_to_back_count(CuTest *tc) {
    const int32_t nelem = 150000;
    buffer_ctx buf;
    ud_cxt ud;
    int32_t status, i, k;
    redis_pack_ctx *pack;
    buffer_init(&buf);
    ZERO(&ud, sizeof(ud_cxt));
    for (k = 0; k < 2; k++) {
        _bput(&buf, "*150000\r\n");
        for (i = 0; i < nelem; i++) {
            _bput(&buf, ":1\r\n");
        }
    }
    for (k = 0; k < 2; k++) {
        status = PROT_INIT;
        pack = _t_redis_unpack(0, &buf, &ud, NULL, &status);
        CuAssertPtrNotNull(tc, pack);
        CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
        CuAssertTrue(tc, RESP_ARRAY == pack->prot && nelem == pack->nelem);
        _redis_pkfree(pack);
    }
    CuAssertTrue(tc, 0 == buffer_size(&buf));
    _redis_udfree(&ud);
    buffer_free(&buf);
}

// _reader_line / _reader_bulk 长度行无 CRLF 持续累积超 REDIS_MAX_LINE_LENS(64KB) 应 PROT_ERROR
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
        redis_pack_ctx *pack = _t_redis_unpack(0, &buf, &ud, NULL, &status);
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
        redis_pack_ctx *pack = _t_redis_unpack(0, &buf, &ud, NULL, &status);
        CuAssertTrue(tc, NULL == pack);
        CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));

        _redis_udfree(&ud);
        buffer_free(&buf);
    }
    // 3. Aggregate 类型 "*9999..." 无 CRLF 超 64KB（_redis_reader_agg 同模式的累积守卫）
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
        redis_pack_ctx *pack = _t_redis_unpack(0, &buf, &ud, NULL, &status);
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
        ud_cxt ud;
        int32_t status;
        redis_pack_ctx *pack = _t_redis_one(&buf, &ud, "#t\r\n", &status);
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
        ud_cxt ud;
        int32_t status;
        redis_pack_ctx *pack = _t_redis_one(&buf, &ud, "#f\r\n", &status);
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
        ud_cxt ud;
        int32_t status;
        redis_pack_ctx *pack = _t_redis_one(&buf, &ud, "#x\r\n", &status);
        CuAssertTrue(tc, NULL == pack);
        CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
        _redis_udfree(&ud);
        buffer_free(&buf);
    }
    // BOOL NUL 字节: "#\x00\r\n" → PROT_ERROR（旧代码 strchr 命中字面量终止符误判为合法 false）
    {
        buffer_ctx buf;
        buffer_init(&buf);
        char nulbool[] = { '#', '\0', '\r', '\n' };
        buffer_append(&buf, nulbool, sizeof(nulbool));
        ud_cxt ud;
        ZERO(&ud, sizeof(ud));
        int32_t status = PROT_INIT;
        redis_pack_ctx *pack = _t_redis_unpack(0, &buf, &ud, NULL, &status);
        CuAssertTrue(tc, NULL == pack);
        CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
        _redis_udfree(&ud);
        buffer_free(&buf);
    }
    // DOUBLE 普通值: ",3.14\r\n"
    {
        buffer_ctx buf;
        ud_cxt ud;
        int32_t status;
        redis_pack_ctx *pack = _t_redis_one(&buf, &ud, ",3.14\r\n", &status);
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
        ud_cxt ud;
        int32_t status;
        redis_pack_ctx *pack = _t_redis_one(&buf, &ud, ",inf\r\n", &status);
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
        ud_cxt ud;
        int32_t status;
        redis_pack_ctx *pack = _t_redis_one(&buf, &ud, ",-inf\r\n", &status);
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
        ud_cxt ud;
        int32_t status;
        redis_pack_ctx *pack = _t_redis_one(&buf, &ud, ",nan\r\n", &status);
        CuAssertPtrNotNull(tc, pack);
        CuAssertTrue(tc, RESP_DOUBLE == pack->prot);
        CuAssertTrue(tc, isnan(pack->dval));
        _redis_pkfree(pack);
        _redis_udfree(&ud);
        buffer_free(&buf);
    }
    // BIGNUM 按字符串原样保留(不解析 ival): "(9223372036854775807\r\n"
    {
        buffer_ctx buf;
        ud_cxt ud;
        int32_t status;
        redis_pack_ctx *pack = _t_redis_one(&buf, &ud, "(9223372036854775807\r\n", &status);
        CuAssertPtrNotNull(tc, pack);
        CuAssertTrue(tc, RESP_BIGNUM == pack->prot);
        CuAssertTrue(tc, 19 == pack->len);
        CuAssertTrue(tc, 0 == memcmp(pack->data, "9223372036854775807", 19));
        _redis_pkfree(pack);
        _redis_udfree(&ud);
        buffer_free(&buf);
    }
    // BERROR: "!21\r\nSYNTAX invalid syntax\r\n"
    {
        buffer_ctx buf;
        ud_cxt ud;
        int32_t status;
        redis_pack_ctx *pack = _t_redis_one(&buf, &ud, "!21\r\nSYNTAX invalid syntax\r\n", &status);
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
        ud_cxt ud;
        int32_t status;
        redis_pack_ctx *pack = _t_redis_one(&buf, &ud, "=15\r\ntxt:Some string\r\n", &status);
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
        ud_cxt ud;
        int32_t status;
        redis_pack_ctx *pack = _t_redis_one(&buf, &ud, "=2\r\nab\r\n", &status);
        CuAssertTrue(tc, NULL == pack);
        CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
        _redis_udfree(&ud);
        buffer_free(&buf);
    }
    // VERB 第 4 字节非 ':': "=5\r\ntxtXY\r\n" → PROT_ERROR
    {
        buffer_ctx buf;
        ud_cxt ud;
        int32_t status;
        redis_pack_ctx *pack = _t_redis_one(&buf, &ud, "=5\r\ntxtXY\r\n", &status);
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
        ud_cxt ud;
        int32_t status;
        redis_pack_ctx *pack = _t_redis_one(&buf, &ud, "~3\r\n+a\r\n+b\r\n+c\r\n", &status);
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
        ud_cxt ud;
        int32_t status;
        redis_pack_ctx *pack = _t_redis_one(&buf, &ud, ">2\r\n+pub\r\n+msg\r\n", &status);
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
        ud_cxt ud;
        int32_t status;
        redis_pack_ctx *pack = _t_redis_one(&buf, &ud, "%2\r\n+k1\r\n:1\r\n+k2\r\n:2\r\n", &status);
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
        ud_cxt ud;
        int32_t status;
        redis_pack_ctx *pack = _t_redis_one(&buf, &ud, "|1\r\n+key\r\n+val\r\n+OK\r\n", &status);
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

/* 聚合嵌套：上限量的是嵌套深度而非累计节点数，扁平大结果集不得被误拒 */
static void test_redis_nesting(CuTest *tc) {
    buffer_ctx buf;
    ud_cxt ud;
    int32_t status;
    redis_pack_ctx *pack;
    int32_t i;
    {
        buffer_init(&buf);
        _bput(&buf, "*70000\r\n");
        for (i = 0; i < 70000; i++) {
            _bput(&buf, ":1\r\n");
        }
        ZERO(&ud, sizeof(ud_cxt));
        status = PROT_INIT;
        pack = _t_redis_unpack(0, &buf, &ud, NULL, &status);
        CuAssert(tc, "flat 70000-element array must not be rejected by the node-count limit", NULL != pack);
        CuAssert(tc, "large flat result set must not set PROT_ERROR", !BIT_CHECK(status, PROT_ERROR));
        CuAssertTrue(tc, RESP_ARRAY == pack->prot);
        CuAssertTrue(tc, 70000 == pack->nelem);
        _redis_pkfree(pack);
        _redis_udfree(&ud);
        buffer_free(&buf);
    }
    {
        buffer_init(&buf);
        for (i = 0; i < 17; i++) {
            _bput(&buf, "*1\r\n");
        }
        _bput(&buf, ":1\r\n");
        ZERO(&ud, sizeof(ud_cxt));
        status = PROT_INIT;
        pack = _t_redis_unpack(0, &buf, &ud, NULL, &status);
        CuAssert(tc, "17 nesting levels exactly fill REDIS_MAX_DEPTH, must be accepted", NULL != pack);
        CuAssert(tc, "depth within limit must not set PROT_ERROR", !BIT_CHECK(status, PROT_ERROR));
        _redis_pkfree(pack);
        _redis_udfree(&ud);
        buffer_free(&buf);
    }
    {
        buffer_init(&buf);
        for (i = 0; i < 18; i++) {
            _bput(&buf, "*1\r\n");
        }
        ZERO(&ud, sizeof(ud_cxt));
        status = PROT_INIT;
        pack = _t_redis_unpack(0, &buf, &ud, NULL, &status);
        CuAssert(tc, "18 nesting levels exceed the depth limit, must be rejected", NULL == pack);
        CuAssert(tc, "exceeding depth must set PROT_ERROR", BIT_CHECK(status, PROT_ERROR));
        _redis_udfree(&ud);
        buffer_free(&buf);
    }
}

/* =======================================================================
 * URL —— url_parse 各字段验证
 * ======================================================================= */

// 断言 query 参数 key 存在且值与 val 相等；val 为空串时断言参数存在但无值
static void _url_check_param(CuTest *tc, url_ctx *ctx, const char *key, const char *val) {
    buf_ctx *prm = url_get_param(ctx, key);
    CuAssertPtrNotNull(tc, prm);
    CuAssertTrue(tc, strlen(val) == prm->lens);
    CuAssertTrue(tc, 0 == prm->lens || buf_compare(prm, val, strlen(val)));
}
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

    _url_check_param(tc, &ctx, "k1", "v1");

    _url_check_param(tc, &ctx, "k2", "v2");

    /* 不存在的参数应返回 NULL */
    CuAssertTrue(tc, NULL == url_get_param(&ctx, "missing"));
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
        _url_check_param(tc, &ctx, "k", "v");
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
        _url_check_param(tc, &ctx, "k", "v");
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
        _url_check_param(tc, &ctx, "k", "v");
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
        _url_check_param(tc, &ctx, "dst", "123");
        _url_check_param(tc, &ctx, "type", "4");
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
    {
        char u[] = "/s?q=x&exact&page=2";
        url_ctx ctx;
        url_parse(&ctx, u, strlen(u), '/', 1);
        _url_check_param(tc, &ctx, "q", "x");
        _url_check_param(tc, &ctx, "exact", "");
        _url_check_param(tc, &ctx, "page", "2");
    }
    {
        char u[] = "/s?a&b&c";
        url_ctx ctx;
        url_parse(&ctx, u, strlen(u), '/', 1);
        CuAssertPtrNotNull(tc, url_get_param(&ctx, "a"));
        CuAssertPtrNotNull(tc, url_get_param(&ctx, "b"));
        CuAssertPtrNotNull(tc, url_get_param(&ctx, "c"));
    }
    {
        // 同名参数取最后一个；router_req_query 直接转调它，两边只此一份实现
        char u[] = "/p?a=1&b=x&a=2&a=3";
        url_ctx ctx;
        url_parse(&ctx, u, strlen(u), '/', 1);
        _url_check_param(tc, &ctx, "a", "3");
        _url_check_param(tc, &ctx, "b", "x");
        // 末值为空串也算命中，不该退回去取前面那个有值的
        char u2[] = "/p?a=1&a=";
        url_parse(&ctx, u2, strlen(u2), '/', 1);
        buf_ctx *last = url_get_param(&ctx, "a");
        CuAssertPtrNotNull(tc, last);
        CuAssertTrue(tc, 0 == last->lens);
    }
    {
        char u[] = "/p?a=1&&b=2";
        url_ctx ctx;
        url_parse(&ctx, u, strlen(u), '/', 1);
        _url_check_param(tc, &ctx, "a", "1");
        _url_check_param(tc, &ctx, "b", "2");
    }
    {
        char u[] = "/p?=x&token=abc";
        url_ctx ctx;
        url_parse(&ctx, u, strlen(u), '/', 1);
        _url_check_param(tc, &ctx, "token", "abc");
    }
}

/* 回归：scheme 只认首个 '/' '?' '#' 之前的 "://"。
   修复前 memstr 全串搜，"/public?next=x:///admin" 会切出 scheme="/public?next=x" + path="/admin"，
   与字面 "/admin" 的解析结果逐字节相同 → 请求被派发到另一条路由 */
static void test_url_scheme_relative(CuTest *tc) {
    // 1. query 里带 "://" 的相对 URL：不得被当成绝对 URL
    {
        char u[] = "/public?next=x:///admin";
        url_ctx ctx;
        url_parse(&ctx, u, strlen(u), '/', 1);
        CuAssertTrue(tc, 0 == ctx.scheme.lens);
        CuAssertTrue(tc, 0 == ctx.host.lens);
        CuAssertTrue(tc, 1 == ctx.npath);
        CuAssertTrue(tc, buf_compare(&ctx.segs[0], "public", 6));
        _url_check_param(tc, &ctx, "next", "x:///admin");
    }
    // 2. 回调地址参数：host 不得被 query 里的 URL 顶掉，参数也不能丢
    {
        char u[] = "/api/v1?redirect=https://ex.com/cb";
        url_ctx ctx;
        url_parse(&ctx, u, strlen(u), '/', 1);
        CuAssertTrue(tc, 0 == ctx.scheme.lens);
        CuAssertTrue(tc, 0 == ctx.host.lens);
        CuAssertTrue(tc, 2 == ctx.npath);
        CuAssertTrue(tc, buf_compare(&ctx.segs[0], "api", 3));
        CuAssertTrue(tc, buf_compare(&ctx.segs[1], "v1", 2));
        _url_check_param(tc, &ctx, "redirect", "https://ex.com/cb");
    }
    // 3. fragment 里带 "://"：同样不得触发 scheme 切分
    {
        char u[] = "/x#y://z";
        url_ctx ctx;
        url_parse(&ctx, u, strlen(u), '/', 1);
        CuAssertTrue(tc, 0 == ctx.scheme.lens);
        CuAssertTrue(tc, 1 == ctx.npath);
        CuAssertTrue(tc, buf_compare(&ctx.segs[0], "x", 1));
        CuAssertTrue(tc, buf_compare(&ctx.anchor, "y://z", 5));
    }
    // 4. 真绝对 URL 不受影响：query 里再有 "://" 也只认首个 scheme
    {
        char u[] = "ws://h/a?u=x://q/b";
        url_ctx ctx;
        url_parse(&ctx, u, strlen(u), '/', 1);
        CuAssertTrue(tc, buf_compare(&ctx.scheme, "ws", 2));
        CuAssertTrue(tc, buf_compare(&ctx.host, "h", 1));
        CuAssertTrue(tc, 1 == ctx.npath);
        CuAssertTrue(tc, buf_compare(&ctx.segs[0], "a", 1));
        _url_check_param(tc, &ctx, "u", "x://q/b");
    }
    // 5. authority-form(host:port)：':' 后不是 "//"，不算 scheme
    {
        char u[] = "host:8080/p";
        url_ctx ctx;
        url_parse(&ctx, u, strlen(u), '/', 1);
        CuAssertTrue(tc, 0 == ctx.scheme.lens);
        CuAssertTrue(tc, buf_compare(&ctx.host, "host", 4));
        CuAssertTrue(tc, buf_compare(&ctx.port, "8080", 4));
    }
}

/* 回归：url_parse 入口只清头部（param/segs/buf 三个大数组不再整体清零），
   同一个 url_ctx 复用时必须靠 nparam / npath 划定有效范围，不能残留上次的参数/路径段 */
static void test_url_ctx_reuse(CuTest *tc) {
    url_ctx ctx;
    char u1[] = "/a/b/c?x=1&y=2&z=3";
    url_parse(&ctx, u1, strlen(u1), '/', 1);
    CuAssertIntEquals(tc, 3, ctx.nparam);
    CuAssertIntEquals(tc, 3, ctx.npath);
    _url_check_param(tc, &ctx, "x", "1");

    // 同一个 ctx 再解析一条参数更少、路径更短的 url
    char u2[] = "/q?z=9";
    url_parse(&ctx, u2, strlen(u2), '/', 1);
    CuAssertIntEquals(tc, 1, ctx.nparam);
    CuAssertIntEquals(tc, 1, ctx.npath);
    _url_check_param(tc, &ctx, "z", "9");
    CuAssert(tc, "stale param from the previous parse must not be visible",
        NULL == url_get_param(&ctx, "x"));
    CuAssert(tc, "stale param from the previous parse must not be visible",
        NULL == url_get_param(&ctx, "y"));
    CuAssertTrue(tc, buf_compare(&ctx.segs[0], "q", 1));

    // 完全没有查询串时 nparam 归零，重组也应得空串
    char u3[] = "/onlypath";
    url_parse(&ctx, u3, strlen(u3), '/', 1);
    CuAssertIntEquals(tc, 0, ctx.nparam);
    CuAssertTrue(tc, NULL == url_get_param(&ctx, "z"));
    char qbuf[64];
    CuAssertIntEquals(tc, 0, (int)url_reorg_param(&ctx, qbuf, sizeof(qbuf)));
    CuAssertTrue(tc, '\0' == qbuf[0]);

    // 无值参数的 val 由 _url_param 显式清空，不能读到上次的 val
    char u4[] = "/p?a=vvv";
    url_parse(&ctx, u4, strlen(u4), '/', 1);
    char u5[] = "/p?a";
    url_parse(&ctx, u5, strlen(u5), '/', 1);
    buf_ctx *v = url_get_param(&ctx, "a");
    CuAssertPtrNotNull(tc, v);
    CuAssert(tc, "valueless param must not inherit the previous value", 0 == v->lens);
}

// decode 时 url 要复制进 ctx->buf，判据是 lens >= sizeof(buf)，故最长合法长度是 URL_BUF_LENS-1
// （末字节留给 '\0'）。同时验拒收后头部字段不残留上一次解析的结果——入口的 ZERO 只到
// offsetof(url_ctx, param)，一旦被挪到长度判定之后，忽略返回值的调用方就会读到上一条 url
static void test_url_buf_lens_bound(CuTest *tc) {
    url_ctx ctx;
    char u[URL_BUF_LENS + 8];
    char prev[] = "http://h/a/b?k=v";
    // 先用同一个 ctx 解析一条带 scheme/host/param 的短 url
    CuAssertIntEquals(tc, ERR_OK, url_parse(&ctx, prev, strlen(prev), '/', 1));
    CuAssertTrue(tc, buf_compare(&ctx.host, "h", 1));
    CuAssertIntEquals(tc, 1, ctx.nparam);
    CuAssertIntEquals(tc, 2, ctx.npath);

    // 恰 URL_BUF_LENS 字节 → 拒
    memset(u, 'a', sizeof(u));
    u[0] = '/';
    u[URL_BUF_LENS] = '\0';
    CuAssertIntEquals(tc, ERR_FAILED, url_parse(&ctx, u, URL_BUF_LENS, '/', 1));
    // 拒收后头部全归零，上一条 url 的 host / 参数 / 路径段都不该再取得到
    CuAssertIntEquals(tc, 0, ctx.nparam);
    CuAssertIntEquals(tc, 0, ctx.npath);
    CuAssertTrue(tc, 0 == ctx.scheme.lens);
    CuAssertTrue(tc, 0 == ctx.host.lens);
    CuAssertTrue(tc, 0 == ctx.pathlens);
    CuAssertTrue(tc, 0 == ctx.paramlens);
    CuAssertTrue(tc, NULL == url_get_param(&ctx, "k"));

    // 恰 URL_BUF_LENS-1 字节 → 合法，整条当一个路径段
    u[URL_BUF_LENS - 1] = '\0';
    CuAssertIntEquals(tc, ERR_OK, url_parse(&ctx, u, URL_BUF_LENS - 1, '/', 1));
    CuAssertIntEquals(tc, 1, ctx.npath);
    CuAssertTrue(tc, URL_BUF_LENS - 2 == (int)ctx.segs[0].lens);

    // 不 decode 时不复制、就地解析，故不受 URL_BUF_LENS 约束
    memset(u, 'a', sizeof(u));
    u[0] = '/';
    u[URL_BUF_LENS + 7] = '\0';
    CuAssertIntEquals(tc, ERR_OK, url_parse(&ctx, u, URL_BUF_LENS + 7, '/', 0));
    CuAssertIntEquals(tc, 1, ctx.npath);
    CuAssertTrue(tc, URL_BUF_LENS + 6 == (int)ctx.segs[0].lens);
}

/* 回归：userinfo 以 authority 里最后一个 '@' 为界(RFC 3986 §3.2)。
   取第一个的话 "user@host@evil.com" 的 host 会被认成 "host@evil.com"，
   与浏览器/curl 不一致——先按 host 过白名单再把原 URL 交出去就成了校验绕过 */
static void test_url_userinfo_last_at(CuTest *tc) {
    {
        char u[] = "http://user@host@evil.com/p";
        url_ctx ctx;
        url_parse(&ctx, u, strlen(u), '/', 1);
        CuAssertTrue(tc, buf_compare(&ctx.user, "user@host", 9));
        CuAssertTrue(tc, buf_compare(&ctx.host, "evil.com", 8));
        CuAssertTrue(tc, 1 == ctx.npath);
        CuAssertTrue(tc, buf_compare(&ctx.segs[0], "p", 1));
    }
    // 单个 '@' 的常规形态不受影响，密码段照旧
    {
        char u[] = "http://user:pwd@host:5432/db";
        url_ctx ctx;
        url_parse(&ctx, u, strlen(u), '/', 1);
        CuAssertTrue(tc, buf_compare(&ctx.user, "user", 4));
        CuAssertTrue(tc, buf_compare(&ctx.psw, "pwd", 3));
        CuAssertTrue(tc, buf_compare(&ctx.host, "host", 4));
        CuAssertTrue(tc, buf_compare(&ctx.port, "5432", 4));
    }
    // path/query 里的 '@' 不参与 authority 切分
    {
        char u[] = "http://host/a@b?k=c@d";
        url_ctx ctx;
        url_parse(&ctx, u, strlen(u), '/', 1);
        CuAssertTrue(tc, 0 == ctx.user.lens);
        CuAssertTrue(tc, buf_compare(&ctx.host, "host", 4));
        CuAssertTrue(tc, buf_compare(&ctx.segs[0], "a@b", 3));
        _url_check_param(tc, &ctx, "k", "c@d");
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
    char smallbuf[6];
    n = url_reorg_param(&ctx, smallbuf, sizeof(smallbuf));
    CuAssertStrEquals(tc, "k1=v1", smallbuf);
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

    char u8[] = "/p?a&b=2";
    url_parse(&ctx, u8, strlen(u8), '/', 0);
    url_reorg_param(&ctx, buf, sizeof(buf));
    CuAssertStrEquals(tc, "a&b=2", buf);
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
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.pktype = (subtype_t)pktype;// custz_unpack 的子类型现在从 ud 取
    void *data = _t_custz_unpack(0, &buf, &ud, &out, &status);
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

/* 组包侧不卡接收策略上限：对端可能是自己写的实现，上限未必与本地相同。
   线格式上界与"头长加 lens"的回绕由三个 _custz_encode_* 各自把关 */
static void test_custz_maxpack(CuTest *tc) {
    pack_type types[] = { PACK_CUSTZ_FIXED, PACK_CUSTZ_FLAG, PACK_CUSTZ_VAR };
    size_t pksize;
    size_t i;
    void *pk;
    char *big;
    MALLOC(big, (size_t)CUSTZ_MAX_PACK_LENS + 1);
    memset(big, 'z', (size_t)CUSTZ_MAX_PACK_LENS + 1);
    for (i = 0; i < ARRAY_SIZE(types); i++) {
        // 超接收侧上限照常组包，*size 含头部
        pksize = 0;
        pk = custz_pack(types[i], big, (size_t)CUSTZ_MAX_PACK_LENS + 1, &pksize);
        CuAssertPtrNotNull(tc, pk);
        CuAssertTrue(tc, pksize > (size_t)CUSTZ_MAX_PACK_LENS);
        FREE(pk);
        // 上限内仍能往返（接收侧照旧按 CUSTZ_MAX_PACK_LENS 卡）
        _custz_roundtrip(tc, types[i], big, CUSTZ_MAX_PACK_LENS);
    }
    // 线格式上界仍拒：VAR 的长度是 MQTT 风格变长整数，超 MQTT_VARINT_MAX 编不出来。
    // 走不到 memcpy —— custz_pack 见编码器返 NULL 就退
    pksize = 12345;
    CuAssertPtrEquals(tc, NULL, custz_pack(PACK_CUSTZ_VAR, big, (size_t)MQTT_VARINT_MAX + 1, &pksize));
    CuAssertTrue(tc, 0 == (int)pksize);
    FREE(big);
}

// 手写线格式字节向量投进 custz_unpack，断言解出的长度与数据体都与向量一致。
// 往返用例是编码器与解码器互相作证，两侧一起改（比如大端换小端）照样全绿；这里是独立参照
static void _custz_wire_unpack(CuTest *tc, pack_type pktype,
                               const uint8_t *wire, size_t wlens,
                               const char *expect, size_t elens) {
    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, (void *)wire, wlens);

    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.pktype = (subtype_t)pktype;
    int32_t status = PROT_INIT;
    size_t out = 0;
    void *data = _t_custz_unpack(0, &buf, &ud, &out, &status);
    CuAssertPtrNotNull(tc, data);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertTrue(tc, elens == out);
    CuAssertTrue(tc, 0 == memcmp(data, expect, elens));
    CuAssertTrue(tc, 0 == buffer_size(&buf));

    FREE(data);
    buffer_free(&buf);
}
static void test_custz_wire_unpack(CuTest *tc) {
    // FIXED：4 字节大端长度 0x00000002
    uint8_t fixed[6] = { 0x00, 0x00, 0x00, 0x02, 'h', 'i' };
    _custz_wire_unpack(tc, PACK_CUSTZ_FIXED, fixed, sizeof(fixed), "hi", 2);
    // FLAG：单字节头，长度即标志字节
    uint8_t flag[3] = { 0x02, 'h', 'i' };
    _custz_wire_unpack(tc, PACK_CUSTZ_FLAG, flag, sizeof(flag), "hi", 2);
    // FLAG：0xfd + 2 字节大端长度 0x0002
    uint8_t flag16[5] = { 0xfd, 0x00, 0x02, 'h', 'i' };
    _custz_wire_unpack(tc, PACK_CUSTZ_FLAG, flag16, sizeof(flag16), "hi", 2);
    // VAR：MQTT 变长整数，单字节 0x02
    uint8_t var[3] = { 0x02, 'h', 'i' };
    _custz_wire_unpack(tc, PACK_CUSTZ_VAR, var, sizeof(var), "hi", 2);
    // VAR：0x80 0x01 = 128（低位组在前），数据体 128 字节
    char expect[128];
    uint8_t var128[2 + sizeof(expect)];
    size_t i;
    for (i = 0; i < sizeof(expect); i++) {
        expect[i] = (char)('a' + (i % 26));
    }
    var128[0] = 0x80;
    var128[1] = 0x01;
    memcpy(var128 + 2, expect, sizeof(expect));
    _custz_wire_unpack(tc, PACK_CUSTZ_VAR, var128, sizeof(var128), expect, sizeof(expect));
}
// 接收侧的 CUSTZ_MAX_PACK_LENS 判据是 > 上限，故恰等于上限合法（只是还在等数据体）。
// 头里的长度用宏现算，不写死
static void test_custz_unpack_maxlens(CuTest *tc) {
    const size_t limit = (size_t)CUSTZ_MAX_PACK_LENS;
    uint8_t wire[4];
    buffer_ctx buf;
    ud_cxt ud;
    int32_t status;
    size_t out;
    size_t dlens;
    int32_t pass_i;
    for (pass_i = 0; pass_i < 2; pass_i++) {
        dlens = (0 == pass_i) ? limit : limit + 1;
        wire[0] = (uint8_t)(dlens >> 24);
        wire[1] = (uint8_t)(dlens >> 16);
        wire[2] = (uint8_t)(dlens >> 8);
        wire[3] = (uint8_t)dlens;
        buffer_init(&buf);
        buffer_append(&buf, wire, sizeof(wire));
        ZERO(&ud, sizeof(ud));
        ud.pktype = (subtype_t)PACK_CUSTZ_FIXED;
        status = PROT_INIT;
        out = 0;
        CuAssertTrue(tc, NULL == _t_custz_unpack(0, &buf, &ud, &out, &status));
        if (0 == pass_i) {
            // 恰等于上限：不判协议错，继续等数据体
            CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
            CuAssertTrue(tc, BIT_CHECK(status, PROT_MOREDATA));
        } else {
            CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
        }
        buffer_free(&buf);
    }
}

// 消息汇测试桩，smtp EHLO / websock 握手 / prots_net_close 三处用例共用。
// prots_closed 已改为内部静态分发，只能通过 prots_net_close 间接触发，而 g_emit.begin 必须返回
// 非 NULL 才走得到它；websock 那边则是握手成败都要经 _hs_push，begin 为空函数指针会直接崩
static int32_t g_stub_emit_calls;
static message_ctx g_stub_first_msg;
static message_ctx g_stub_last_msg;
static void *_stub_emit_begin(void *loader, name_t handle) {
    (void)loader;
    (void)handle;
    return (void *)1;
}
static void _stub_emit_emit(void *target, message_ctx *msg) {
    (void)target;
    if (0 == g_stub_emit_calls) {
        g_stub_first_msg = *msg;
    }
    g_stub_last_msg = *msg;
    g_stub_emit_calls++;
}
static void _stub_emit_end(void *target) {
    (void)target;
}
static prot_emit g_stub_emit = { _stub_emit_begin, _stub_emit_emit, _stub_emit_end };
// 装上就不卸：CuTest 阶段跑完 loader_init 会用真 sink 覆盖(loader.c)，本文件是全树唯一
// 碰 prots 的测试文件，排在后面的套件都不经它派发；卸成零值反而留下一个空函数指针窗口

/* =======================================================================
 * SMTP —— 多行响应识别（_smtp_full_response）
 * 测试目标：覆盖单行 / 多行 / TCP 分包 / 边界 / 协议错误共 20 个 case
 * ======================================================================= */

// 辅助：_smtp_full_response 是库内 static，改从公开入口 smtp_unpack 的 COMMAND 阶段间接测。
// 该阶段 code 传 NULL(以首行 code 为准)、不碰 ev/fd/_hs_push；完整时返回内容并 drain 掉整条响应，
// 出参 size 是总长去掉末尾 CRLF 后的长度，故 size + CRLF_SIZE 就是原来那个返回值
static void _smtp_resp_check(CuTest *tc, const char *input, size_t inlen, int32_t expected) {
    buffer_ctx buf;
    buffer_init(&buf);
    if (inlen > 0) {
        buffer_append(&buf, (void *)input, inlen);
    }
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = 4;// COMMAND
    int32_t status = PROT_INIT;
    size_t size = 0;
    char *pack = (char *)_t_smtp_unpack(0, &buf, &ud, &size, &status);
    if (0 == expected) {
        CuAssertTrue(tc, NULL == pack && BIT_CHECK(status, PROT_MOREDATA));
        CuAssertIntEquals(tc, (int)inlen, (int)buffer_size(&buf));// 不完整就一个字节都不消耗
    } else if (ERR_FAILED == expected) {
        CuAssertTrue(tc, NULL == pack && BIT_CHECK(status, PROT_ERROR));
    } else {
        CuAssertTrue(tc, NULL != pack);
        CuAssertIntEquals(tc, expected, (int)(size + CRLF_SIZE));
        CuAssertIntEquals(tc, (int)inlen - expected, (int)buffer_size(&buf));
        FREE(pack);
    }
    buffer_free(&buf);
}

// 辅助：_smtp_get_authtype 是库内 static，改从公开入口 smtp_unpack 的 EHLO 阶段间接测。
// 该阶段先用 "250" 框出本条响应，再把解析结果直接写进 smtp_ctx.authtype，成败都写，
// 故读它即可（authtype 从 1 起，ZERO 后的 0 不与任何合法值撞）。
// 不看 status：成功路径末尾要发 AUTH 命令，fd 无效使 ev_send 早退并置 PROT_ERROR
static void _smtp_auth_check(CuTest *tc, const char *input, int32_t expected) {
    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, (void *)input, strlen(input));
    smtp_ctx smtp;
    ZERO(&smtp, sizeof(smtp));
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = 1;// EHLO
    ud.context = &smtp;
    int32_t status = PROT_INIT;
    size_t size = 0;
    (void)_t_smtp_unpack(0, &buf, &ud, &size, &status);
    CuAssertIntEquals(tc, expected, smtp.authtype);
    // 没有 AUTH 通告时 _smtp_push_errline 会把整份应答推给等待方；stub 不做 _message_clean，
    // 按 g_stub_first_msg 那处的既有约定由用例自己收（成功路径推的是 NULL，FREE 自带判空）
    FREE(g_stub_last_msg.data);
    buffer_free(&buf);
}
// 认证类型解析必须卡在本条 250 响应之内：粘在后面的字节不属于本次通告
static void test_smtp_authtype(CuTest *tc) {
    // 没有 AUTH 通告时 _smtp_ehlo 要把整份 250 应答推给等待方，经 _hs_push
    prots_init(&g_stub_emit);
    /* 1. 中间行通告 LOGIN */
    _smtp_auth_check(tc, "250-fake\r\n250-AUTH LOGIN\r\n250 OK\r\n", LOGIN);
    /* 2. 同一行既有 LOGIN 又有 PLAIN → 优先 PLAIN */
    _smtp_auth_check(tc, "250-fake\r\n250-AUTH LOGIN PLAIN\r\n250 OK\r\n", PLAIN);
    /* 3. 末行通告（空格分隔）同样认 */
    _smtp_auth_check(tc, "250-fake\r\n250 AUTH PLAIN\r\n", PLAIN);
    /* 4. 本条响应内没有 AUTH → 失败 */
    _smtp_auth_check(tc, "250-SIZE 1000\r\n250 HELP\r\n", ERR_FAILED);
    /* 5. 回归：AUTH 只出现在紧跟其后的下一条响应里，不得越界采信 */
    _smtp_auth_check(tc, "250-SIZE 1000\r\n250 HELP\r\n250-AUTH PLAIN\r\n250 OK\r\n", ERR_FAILED);
    /* 6. 回归：本条认 LOGIN，后面粘着的 PLAIN 不得把它顶掉 */
    _smtp_auth_check(tc, "250-AUTH LOGIN\r\n250 OK\r\n250-AUTH PLAIN\r\n250 OK\r\n", LOGIN);
    /* 7. "250-AUTHENTICATION" 不是 AUTH 通告（靠尾随空格区分） */
    _smtp_auth_check(tc, "250-AUTHENTICATION REQUIRED\r\n250 OK\r\n", ERR_FAILED);
    // 原 case 8（直接传 total 为 1 / 0 / -1 撞 buffer_search 的"end 为 0 即到末尾"哨兵）
    // 随 _smtp_get_authtype 改为 static 一并删除：total 由 _smtp_full_response 算出,
    // 公开入口给不出这些值,构造不出来
}

static void test_smtp_full_response(CuTest *tc) {
    /* 1. 空 buffer → 等更多 */
    _smtp_resp_check(tc, "", 0, 0);
    /* 2. 不足 4 字节首行起始 */
    _smtp_resp_check(tc, "22", 2, 0);
    /* 3. 仅 3 字节，缺分隔符 */
    _smtp_resp_check(tc, "220", 3, 0);
    /* 4. 4 字节，无 CRLF */
    _smtp_resp_check(tc, "220 ", 4, 0);
    /* 5. 5 字节，缺 LF */
    _smtp_resp_check(tc, "220 \r", 5, 0);
    /* 6. 6 字节最小完整结束行 */
    _smtp_resp_check(tc, "220 \r\n", 6, 6);
    /* 7. 普通单行完整 */
    _smtp_resp_check(tc, "220 OK\r\n", 8, 8);
    /* 8. 仅中间行（必须等结束行） */
    _smtp_resp_check(tc, "220-host\r\n", 10, 0);
    /* 9. 中间行 + 最小结束行 */
    _smtp_resp_check(tc, "220-host\r\n220 \r\n", 16, 16);
    /* 10. 中间行 + 普通结束行 */
    _smtp_resp_check(tc, "220-host\r\n220 OK\r\n", 18, 18);
    /* 11. 多个中间行 + 结束行 */
    _smtp_resp_check(tc, "220-A\r\n220-B\r\n220 C\r\n", 21, 21);
    /* 12. 结束行后多余字节，helper 应只报到结束行尾 */
    _smtp_resp_check(tc, "220-host\r\n220 OK\r\nEXTRA", 23, 18);
    /* 13. code 不匹配（首行非 220）：这条依赖"指定 code"语义，COMMAND 阶段以首行 code 为准
           会把它当合法响应放行，故走 INIT 阶段——那里固定拿 "220" 校验 */
    {
        buffer_ctx b13;
        buffer_init(&b13);
        buffer_append(&b13, (void *)"500 Err\r\n", 9);
        ud_cxt ud13;
        ZERO(&ud13, sizeof(ud13));
        ud13.status = 0;// INIT
        int32_t st13 = PROT_INIT;
        size_t sz13 = 0;
        prots_init(&g_stub_emit);// 失败路径要把首行推给等待方，经 _hs_push
        CuAssertTrue(tc, NULL == _t_smtp_unpack(0, &b13, &ud13, &sz13, &st13));
        CuAssertTrue(tc, BIT_CHECK(st13, PROT_ERROR));
        CuAssertIntEquals(tc, 0, (int)ud13.status);// 未推进,响应没被认下
        FREE(g_stub_last_msg.data);// 同 _smtp_auth_check：推给等待方的首行由用例自己收
        buffer_free(&b13);
    }
    /* 14. code 在中间行不一致 */
    _smtp_resp_check(tc, "220-A\r\n500 X\r\n", 14, ERR_FAILED);
    /* 15. 第 4 字节非 '-' / 空格 */
    _smtp_resp_check(tc, "220Xhost\r\n", 10, ERR_FAILED);
    /* 16. 第 2 行截断到 2 字节 */
    _smtp_resp_check(tc, "220-A\r\n22", 9, 0);
    /* 17. 第 2 行截断到 3 字节（缺分隔符） */
    _smtp_resp_check(tc, "220-A\r\n220", 10, 0);
    /* 18. 第 2 行结束行起始但缺 CRLF */
    _smtp_resp_check(tc, "220-A\r\n220 ", 11, 0);
    /* 19. 第 2 行中间行起始但缺 CRLF */
    _smtp_resp_check(tc, "220-A\r\n220-", 11, 0);
    /* 鲁棒性：250 多行（验证 helper 通用、不写死 code） */
    _smtp_resp_check(tc, "250-AUTH LOGIN PLAIN\r\n250 OK\r\n", 30, 30);
    /* 鲁棒性：长正文行 */
    char longline[1100];
    memcpy(longline, "220-", 4);
    memset(longline + 4, 'X', 1024);
    memcpy(longline + 4 + 1024, "\r\n220 OK\r\n", 10);
    _smtp_resp_check(tc, longline, 4 + 1024 + 10, 4 + 1024 + 10);
    /* 20. 握手续行洪泛超 SMTP_MAX_PACK_LENS → 拒绝（防恶意 server 续行耗内存） */
    size_t units = (size_t)SMTP_MAX_PACK_LENS / 6 + 1; /* 每单元 "220-\r\n" 6 字节 */
    size_t floodlen = units * 6;
    char *flood;
    MALLOC(flood, floodlen);
    for (size_t fi = 0; fi < units; fi++) {
        memcpy(flood + fi * 6, "220-\r\n", 6);
    }
    _smtp_resp_check(tc, flood, floodlen, ERR_FAILED);
    FREE(flood);
    /* 21. 裸结束行 "<code>\r\n"（code 后直接 CRLF，无 sep/text） */
    _smtp_resp_check(tc, "220\r\n", 5, 5);
    /* 22. 非 220 的首行 code 同样认（COMMAND 响应 code 不固定） */
    _smtp_resp_check(tc, "354 go\r\n", 8, 8);
    /* 23. 多行：首行 code 作后续行校验基准 */
    _smtp_resp_check(tc, "250-A\r\n250 B\r\n", 14, 14);
    /* 24. 非 220 的裸结束行 */
    _smtp_resp_check(tc, "421\r\n", 5, 5);
    /* 25. 续行 code 与首行不一致 → 拒绝 */
    _smtp_resp_check(tc, "250-A\r\n500 X\r\n", 14, ERR_FAILED);
}

// mail_clear 须把 reply 一并还原到 mail_init 后的状态：漏还原会让复用同一对象的下一封信
// 沿用上一封的 No-Reply，而调用方从 API 上看不出 clear() 没覆盖它
static void test_smtp_clear_reply(CuTest *tc) {
    mail_ctx mail;
    mail_init(&mail);
    mail_from(&mail, NULL, "alice@example.com");
    mail_addrs_add(&mail, "bob@example.com", TO);
    mail_subject(&mail, "first");
    mail_msg(&mail, "body");
    mail_reply(&mail, 0);
    char *out = mail_pack(&mail);
    CuAssertPtrNotNull(tc, out);
    CuAssert(tc, "reply=0 must emit No-Reply", NULL != strstr(out, "No-Reply: alice@example.com"));
    FREE(out);

    mail_clear(&mail);
    mail_from(&mail, NULL, "carol@example.com");
    mail_addrs_add(&mail, "dave@example.com", TO);
    mail_subject(&mail, "second");
    mail_msg(&mail, "body2");
    out = mail_pack(&mail);
    CuAssertPtrNotNull(tc, out);
    CuAssert(tc, "after clear the default Reply-To must be restored", NULL != strstr(out, "Reply-To: carol@example.com"));
    CuAssert(tc, "after clear the previous mail's No-Reply must not persist", NULL == strstr(out, "No-Reply:"));
    FREE(out);
    mail_free(&mail);
}

// RFC 7230 §3.2：value 两侧 OWS 都不属于字段值。旧实现只剥前导、尾随不剥，
// 导致直接比较原始 value 的消费者(WebSocket 握手、业务读 headers)在合法输入下必然失配
static void test_http_value_trailing_ows(CuTest *tc) {
    buffer_ctx buf;
    buffer_init(&buf);
    _bput(&buf,
        "GET / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Type: application/json \t \r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ== \r\n"
        "Content-Length: 0\r\n"
        "\r\n");
    ud_cxt ud;
    ZERO(&ud, sizeof(ud_cxt));
    int32_t status = PROT_INIT;
    struct http_pack_ctx *pack = _t_http_unpack(0, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    size_t vlens = 0;
    char *v = http_header(pack, "Content-Type", &vlens);
    CuAssertPtrNotNull(tc, v);
    CuAssert(tc, "trailing OWS in value must be stripped, else comparing application/json always mismatches",
        strlen("application/json") == vlens && 0 == memcmp(v, "application/json", vlens));
    v = http_header(pack, "Sec-WebSocket-Key", &vlens);
    CuAssertPtrNotNull(tc, v);
    CuAssert(tc, "Sec-WebSocket-Key with trailing OWS must stay 24-byte base64 after strip (else handshake is rejected)",
        strlen("dGhlIHNhbXBsZSBub25jZQ==") == vlens);
    _http_pkfree(pack);
    _http_udfree(&ud);
    buffer_free(&buf);
}

// base64 正文须按 RFC 2045 §6.8 折行：不折行时 DATA 单行会远超
// RFC 5321 §4.5.3.1.6 的 1000 octet 上限，严格 MTA 直接以行长错误退信
// 整封信里最长的一行有多少个八位组（不含分隔的 CRLF）。
// RFC 5321 §4.5.3.1.6 限 1000 含 CRLF，故正文任何一行都不得超过 998
static size_t _max_line_lens(const char *out) {
    size_t maxline = 0;
    size_t cur = 0;
    const char *p = out;
    while ('\0' != *p) {
        if ('\r' == p[0] && '\n' == p[1]) {
            if (cur > maxline) {
                maxline = cur;
            }
            cur = 0;
            p += 2;
            continue;
        }
        cur++;
        p++;
    }
    return cur > maxline ? cur : maxline;
}
static void test_smtp_b64_fold(CuTest *tc) {
    char html[4096];
    memset(html, 'h', sizeof(html));
    mail_ctx mail;
    mail_init(&mail);
    mail_from(&mail, NULL, "alice@example.com");
    mail_addrs_add(&mail, "bob@example.com", TO);
    mail_subject(&mail, "test");
    mail_html(&mail, html, sizeof(html));
    char *out = mail_pack(&mail);
    CuAssertPtrNotNull(tc, out);
    size_t maxline = _max_line_lens(out);
    CuAssert(tc, "no DATA line may exceed 998 octets (RFC 5321 counts CRLF, 1000 total)", maxline <= 998);
    FREE(out);
    mail_free(&mail);
}

// 正文改走 base64 之后，SMTP smuggling 的入口从根上没了：base64 行首只可能是 base64 字符，
// <CRLF>.<CRLF> 在正文里根本无法表达，也就不再需要 dot-stuffing。所以这里不再断言具体手法，
// 改断言那条不变式本身——整封信里 "\r\n.\r\n" 只出现一次（末尾的 DATA 终止符），
// 且正文解码回来与 mail_msg 规范化后的原文逐字相同（内容无损）
static void _smtp_body_check(CuTest *tc, const char *msg, const char *expect) {
    mail_ctx mail;
    mail_init(&mail);
    mail_from(&mail, NULL, "alice@example.com");
    mail_addrs_add(&mail, "bob@example.com", TO);
    mail_subject(&mail, "test");
    mail_msg(&mail, msg);
    char *out = mail_pack(&mail);
    CuAssertPtrNotNull(tc, out);

    // 1) DATA 终止符全文只此一处，且正好在末尾
    const char *term = strstr(out, "\r\n.\r\n");
    CuAssertPtrNotNull(tc, term);
    CuAssert(tc, "DATA terminator must appear exactly once", NULL == strstr(term + 1, "\r\n.\r\n"));
    CuAssertTrue(tc, '\0' == term[5]);

    // 2) 头部空行之后到终止符之间是折行的 base64 正文，去掉 CRLF 再解码
    const char *body = strstr(out, "\r\n\r\n");
    CuAssertPtrNotNull(tc, body);
    body += 4;
    char b64[ONEK];
    char plain[ONEK];
    size_t n = 0;
    const char *p;
    for (p = body; p < term; p++) {
        if ('\r' != *p && '\n' != *p) {
            CuAssertTrue(tc, n < sizeof(b64) - 1);
            b64[n++] = *p;
        }
    }
    b64[n] = '\0';
    size_t plens = bs64_decode(b64, n, plain);
    CuAssertTrue(tc, strlen(expect) == plens);
    CuAssertTrue(tc, 0 == memcmp(plain, expect, plens));

    FREE(out);
    mail_free(&mail);
}
static void test_smtp_body_transparency(CuTest *tc) {
    // 1. 攻击载荷：正文里带 <CRLF>.<CRLF> + 伪造 SMTP 命令，编码后原样还原、不构成终止符
    _smtp_body_check(tc, "hello\r\n.\r\nMAIL FROM:<evil@attacker>\r\nRCPT TO:<victim>\r\nDATA\r\nworld",
                         "hello\r\n.\r\nMAIL FROM:<evil@attacker>\r\nRCPT TO:<victim>\r\nDATA\r\nworld");
    // 2. 正文以 '.' 开头
    _smtp_body_check(tc, ".dotted line", ".dotted line");
    // 3. 普通文本
    _smtp_body_check(tc, "hello\r\nworld", "hello\r\nworld");
    // 4. 连续多行以 '.' 开头
    _smtp_body_check(tc, "line1\r\n.line2\r\n.line3", "line1\r\n.line2\r\n.line3");
    // 5. bare LF：mail_msg 入口规范化为 CRLF（容错 server 会把裸 \n 当行终止）
    _smtp_body_check(tc, "hello\n.\r\nMAIL FROM:<evil@attacker>\r\nDATA\r\nworld",
                         "hello\r\n.\r\nMAIL FROM:<evil@attacker>\r\nDATA\r\nworld");
    // 6. bare CR：同上
    _smtp_body_check(tc, "hello\r.\r\nMAIL FROM:<evil@attacker>\r\nDATA",
                         "hello\r\n.\r\nMAIL FROM:<evil@attacker>\r\nDATA");
    // 7. 合法 CRLF 原样保留，不被复述成 \r\n\r\n
    _smtp_body_check(tc, "line1\r\nline2\r\nline3", "line1\r\nline2\r\nline3");
}

// 单段纯文本邮件也必须带 MIME 头。只在多段时才写的话，最常见的那类信整封没有 Content-Type，
// 按 RFC 2045 缺省成 us-ascii，UTF-8 正文到严格客户端上就是乱码
static void test_smtp_plain_mime_headers(CuTest *tc) {
    mail_ctx mail;
    mail_init(&mail);
    mail_from(&mail, NULL, "alice@example.com");
    mail_addrs_add(&mail, "bob@example.com", TO);
    mail_subject(&mail, "test");
    mail_msg(&mail, "\xe4\xb8\xad\xe6\x96\x87");// UTF-8 "中文"
    char *out = mail_pack(&mail);
    CuAssertPtrNotNull(tc, out);
    CuAssertTrue(tc, NULL != strstr(out, "MIME-Version: 1.0"));
    CuAssertTrue(tc, NULL != strstr(out, "Content-Type: text/plain; charset=utf-8"));
    CuAssertTrue(tc, NULL != strstr(out, "Content-Transfer-Encoding: base64"));
    // 没有 html / 附件就不该出现 multipart
    CuAssertTrue(tc, NULL == strstr(out, "multipart"));
    FREE(out);
    mail_free(&mail);
}

// 长正文不带换行时，8bit 原样写出会造出一条几千 octet 的 DATA 行（RFC 5321 §4.5.3.1.6 限 1000）
static void test_smtp_plain_line_fold(CuTest *tc) {
    char msg[4096];
    memset(msg, 'm', sizeof(msg) - 1);
    msg[sizeof(msg) - 1] = '\0';
    mail_ctx mail;
    mail_init(&mail);
    mail_from(&mail, NULL, "alice@example.com");
    mail_addrs_add(&mail, "bob@example.com", TO);
    mail_subject(&mail, "test");
    mail_msg(&mail, msg);
    char *out = mail_pack(&mail);
    CuAssertPtrNotNull(tc, out);
    size_t maxline = _max_line_lens(out);
    CuAssert(tc, "plain-text body must be folded too, not just base64 attachments", maxline <= 998);
    FREE(out);
    mail_free(&mail);
}

// 纯 ASCII 长主题：原样写出会造出一条超 998 octet 的 Subject 行，须改走 encoded-word（自带折行）。
// 顺带确认短主题不受影响、仍旧裸写
static void test_smtp_subject_line_fold(CuTest *tc) {
    char subject[2048];
    size_t i;
    // 全是字母不带空格：折不了行，只能靠 encoded-word 切段
    for (i = 0; i < sizeof(subject) - 1; i++) {
        subject[i] = (char)('a' + (i % 26));
    }
    subject[sizeof(subject) - 1] = '\0';
    mail_ctx mail;
    mail_init(&mail);
    mail_from(&mail, NULL, "alice@example.com");
    mail_addrs_add(&mail, "bob@example.com", TO);
    mail_subject(&mail, subject);
    mail_msg(&mail, "body");
    char *out = mail_pack(&mail);
    CuAssertPtrNotNull(tc, out);
    size_t maxline = _max_line_lens(out);
    CuAssert(tc, "long ASCII subject must be encoded-word folded, not written raw", maxline <= 998);
    CuAssertPtrNotNull(tc, strstr(out, "=?utf-8?B?"));
    // 短主题不受影响，仍旧裸写
    FREE(out);
    mail_subject(&mail, "plain short");
    out = mail_pack(&mail);
    CuAssertPtrNotNull(tc, out);
    CuAssertPtrNotNull(tc, strstr(out, "Subject: plain short\r\n"));
    FREE(out);
    mail_free(&mail);
}

// MIME boundary 必须每封随机。写死的字面量摆在源码里，正文放一行 "--<boundary>" 就能提前
// 终结 text 段、再伪造出一个附件或 text/html 替代段（RFC 2046 §5.1.1 要求 boundary 不得
// 出现在任何 body part 中，常量做不到）
static void test_smtp_boundary_random(CuTest *tc) {
    char b1[128] = { 0 };
    char b2[128] = { 0 };
    const char *html = "<p>hi</p>";
    mail_ctx mail;
    const char *tag;
    const char *end;
    char *out;
    for (int32_t i = 0; i < 2; i++) {
        mail_init(&mail);
        mail_from(&mail, NULL, "alice@example.com");
        mail_addrs_add(&mail, "bob@example.com", TO);
        mail_subject(&mail, "test");
        mail_html(&mail, html, strlen(html));
        out = mail_pack(&mail);
        CuAssertPtrNotNull(tc, out);
        tag = strstr(out, "boundary=\"");
        CuAssertPtrNotNull(tc, tag);
        tag += strlen("boundary=\"");
        end = strchr(tag, '"');
        CuAssertPtrNotNull(tc, end);
        CuAssertTrue(tc, (size_t)(end - tag) < sizeof(b1) - 1);
        memcpy(0 == i ? b1 : b2, tag, (size_t)(end - tag));
        FREE(out);
        mail_free(&mail);
    }
    CuAssertTrue(tc, strlen(b1) > 0);
    CuAssert(tc, "each message must get its own boundary", 0 != strcmp(b1, b2));
}

// display-name 含 RFC 5322 §3.2.3 的 specials 时必须整体加引号：裸写的话
// "Doe, John <a@b>" 会被解析成 "Doe" 与 "John <a@b>" 两个地址
static void _smtp_from_check(CuTest *tc, const char *name, const char *expect) {
    mail_ctx mail;
    mail_init(&mail);
    mail_from(&mail, name, "alice@example.com");
    mail_addrs_add(&mail, "bob@example.com", TO);
    mail_subject(&mail, "test");
    mail_msg(&mail, "body");
    char *out = mail_pack(&mail);
    CuAssertPtrNotNull(tc, out);
    CuAssert(tc, expect, NULL != strstr(out, expect));
    FREE(out);
    mail_free(&mail);
}
static void test_smtp_display_name_quote(CuTest *tc) {
    // 纯 atom：不加引号
    _smtp_from_check(tc, "srey", "From: srey <alice@example.com>\r\n");
    // 含逗号：整体加引号
    _smtp_from_check(tc, "Doe, John", "From: \"Doe, John\" <alice@example.com>\r\n");
    // 含点号（RFC 5322 把 '.' 也列为 specials）
    _smtp_from_check(tc, "J. Doe", "From: \"J. Doe\" <alice@example.com>\r\n");
    // 含引号：quoted-string 内需转义
    _smtp_from_check(tc, "a\"b", "From: \"a\\\"b\" <alice@example.com>\r\n");
    // 无显示名：裸 addr-spec，不带尖括号
    _smtp_from_check(tc, NULL, "From: alice@example.com\r\n");
}

// 非 ASCII 头字段必须编成 RFC 2047 encoded-word：RFC 5322 §2.2 只允许 US-ASCII，
// 而本实现从不协商 SMTPUTF8，裸 UTF-8 主题在严格服务端上会被改写
static void test_smtp_header_encoded_word(CuTest *tc) {
    mail_ctx mail;
    mail_init(&mail);
    mail_from(&mail, "\xe5\x8f\x91\xe4\xbb\xb6\xe4\xba\xba", "alice@example.com");// "发件人"
    mail_addrs_add(&mail, "bob@example.com", TO);
    // "中文主题" 重复 20 次，逼出多个 encoded-word + 折行
    char subject[256] = { 0 };
    for (int32_t i = 0; i < 20; i++) {
        strcat(subject, "\xe4\xb8\xad\xe6\x96\x87\xe4\xb8\xbb\xe9\xa2\x98");
    }
    mail_subject(&mail, subject);
    char *out = mail_pack(&mail);
    CuAssertPtrNotNull(tc, out);
    // 裸 UTF-8 不得出现在头部
    const char *hdrend = strstr(out, "\r\n\r\n");
    CuAssertPtrNotNull(tc, hdrend);
    const char *p;
    for (p = out; p < hdrend; p++) {
        CuAssertTrue(tc, 0 == (0x80 & (unsigned char)*p));
    }
    CuAssertTrue(tc, NULL != strstr(out, "Subject: =?utf-8?B?"));
    // RFC 5322 §3.4 的 name-addr：display-name <addr-spec>，非 ASCII 名字编成 encoded-word
    CuAssertTrue(tc, NULL != strstr(out, "From: =?utf-8?B?"));
    CuAssertTrue(tc, NULL != strstr(out, "?= <alice@example.com>\r\n"));
    // RFC 2047 §2：单个 encoded-word 连同 "=?utf-8?B?" 与 "?=" 不得超过 75 字符
    const char *ew = out;
    const char *ewend;
    int32_t nword = 0;
    while (NULL != (ew = strstr(ew, "=?utf-8?B?"))) {
        ewend = strstr(ew, "?=");
        CuAssertPtrNotNull(tc, ewend);
        CuAssertTrue(tc, (size_t)(ewend + 2 - ew) <= 75);
        nword++;
        ew = ewend + 2;
    }
    // 240 字节主题按每段 45 字节切，加上发件人显示名，至少 6 段
    CuAssertTrue(tc, nword >= 6);
    // 多段之间按 RFC 5322 §2.2.3 折行（CRLF + 一个空格）
    CuAssertTrue(tc, NULL != strstr(out, "?=\r\n =?utf-8?B?"));
    FREE(out);
    mail_free(&mail);
}

/* =======================================================================
 * DNS —— 请求组包、响应解包、TCP 长度前缀解析
 * ======================================================================= */

static void test_dns_request_pack(CuTest *tc) {
    char buf[256];
    uint16_t id;
    /* example.com A 查询 */
    size_t n = dns_request_pack(buf, "example.com", 0, &id);
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
    n = dns_request_pack(buf, "example.com", 1, &id);
    CuAssertTrue(tc, 29 == (int)n);
    CuAssertTrue(tc, 0 == (uint8_t)buf[25] && 28 == (uint8_t)buf[26]);
}

static void test_dns_request_pack_tcp(CuTest *tc) {
    char buf[256];
    uint16_t id;
    size_t n = dns_request_pack_tcp(buf, "example.com", 0, &id);
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
    void *p = _t_dns_unpack(0, &buf, NULL, &out, &status);
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
    p = _t_dns_unpack(0, &buf, NULL, &out, &status);
    CuAssertTrue(tc, NULL == p);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_MOREDATA));
    buffer_free(&buf);

    /* length=0 视为协议错误 */
    buffer_init(&buf);
    uint8_t zero_hdr[2] = { 0, 0 };
    buffer_append(&buf, zero_hdr, 2);
    status = PROT_INIT;
    p = _t_dns_unpack(0, &buf, NULL, &out, &status);
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
    dns_ip *ips = dns_parse_pack((char *)resp, sizeof(resp), &cnt, 0x1234, NULL);
    CuAssertPtrNotNull(tc, ips);
    /* an=1，解出来必须正好 1 条：多一条就是把 RR 之外的字节当记录读了 */
    CuAssertTrue(tc, 1 == cnt);
    CuAssertStrEquals(tc, "1.2.3.4", ips[0].ip);
    FREE(ips);

    /* 恶意放大：仅 12 字节头 + 计数字段全 0xFFFF（total=196605），无 RR 体。
       修复后按报文剩余字节(0)限上界 → total=0 → 返回 NULL，不再 MALLOC ~12.6MB */
    uint8_t evil[] = {
        0x00, 0x00, 0x81, 0x80,/* id / flags(response, rcode=0) */
        0x00, 0x00,/* qd_count=0（无 question）*/
        0xFF, 0xFF,/* an_count=65535 */
        0xFF, 0xFF,/* ns_count=65535 */
        0xFF, 0xFF/* ar_count=65535 */
    };
    size_t ecnt = 0;
    dns_ip *eips = dns_parse_pack((char *)evil, sizeof(evil), &ecnt, 0x0000, NULL);
    CuAssertTrue(tc, NULL == eips);
    CuAssertTrue(tc, 0 == ecnt);
}
// NOERROR/NODATA：报文完全合法、RR 存在，但没有一条 A/AAAA（只有 AAAA/MX 的名字查 A 时最常见，
// 应答段空、授权段带一条 SOA）。此前这种报文返回的是 MALLOC 出来、一个字节都没写过的缓冲 + cnt=0，
// 调用方按"非 NULL 即成功"读 ips[0].ip 就读到未初始化内存
static void test_dns_parse_pack_nodata(CuTest *tc) {
    uint8_t resp[] = {
        /* head: id=0x1234, flags=0x8180(response, rcode=0 NOERROR),
         * qd=1, an=0, ns=1(一条 SOA), ar=0 */
        0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x00,
        /* query: \x07example\x03com\x00 + qtype=A(1) + qclass=IN(1) */
        0x07, 'e','x','a','m','p','l','e',
        0x03, 'c','o','m', 0x00,
        0x00, 0x01, 0x00, 0x01,
        /* authority: 压缩指针 \xc0\x0c + type=SOA(6) + class=IN + ttl=300 + rdlen=4
         * rdata 内容对本用例无意义，_dns_parse_data 只按 rdlen 跳过非 A/AAAA 记录 */
        0xc0, 0x0c, 0x00, 0x06, 0x00, 0x01, 0x00, 0x00,
        0x01, 0x2c, 0x00, 0x04,
        0x00, 0x00, 0x00, 0x00
    };
    size_t cnt = 12345;// 预置非 0，确认失败路径会把它归 0
    int32_t nodata = 0;
    dns_ip *ips = dns_parse_pack((char *)resp, sizeof(resp), &cnt, 0x1234, &nodata);
    CuAssertTrue(tc, NULL == ips);
    CuAssertTrue(tc, 0 == cnt);
    // 完整答复只是没记录 → nodata=1，dns_lookup 据此跳过 TCP 回退
    CuAssertTrue(tc, 1 == nodata);

    // 反向：同一份报文改坏事务 ID，属"没拿到有效响应"，nodata 必须为 0（值得换 TCP 重试）
    nodata = 1;
    ips = dns_parse_pack((char *)resp, sizeof(resp), &cnt, 0x9999, &nodata);
    CuAssertTrue(tc, NULL == ips);
    CuAssertTrue(tc, 0 == nodata);

    // 截断(TC 位)同样是 nodata=0：TCP 重试正是为这种情况准备的
    resp[2] = 0x83;
    nodata = 1;
    ips = dns_parse_pack((char *)resp, sizeof(resp), &cnt, 0x1234, &nodata);
    CuAssertTrue(tc, NULL == ips);
    CuAssertTrue(tc, 0 == nodata);

    // nodata 传 NULL（不关心该信息的调用方）不崩，其余行为不变
    resp[2] = 0x81;
    ips = dns_parse_pack((char *)resp, sizeof(resp), &cnt, 0x1234, NULL);
    CuAssertTrue(tc, NULL == ips);
    CuAssertTrue(tc, 0 == cnt);
}
// DNS-TC：响应头 TC 位置位时应直接返回 NULL，不产出部分记录
static void test_dns_parse_pack_truncated_flag(CuTest *tc) {
    /* 与 test_dns_parse_pack 首个用例相同的合法响应，仅 flags1 从 0x81 改为 0x83（多置 TC 位） */
    uint8_t resp[] = {
        0x12, 0x34, 0x83, 0x80, 0x00, 0x01, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00,
        0x07, 'e','x','a','m','p','l','e',
        0x03, 'c','o','m', 0x00,
        0x00, 0x01, 0x00, 0x01,
        0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
        0x01, 0x2c, 0x00, 0x04,
        0x01, 0x02, 0x03, 0x04
    };
    size_t cnt = 0;
    dns_ip *ips = dns_parse_pack((char *)resp, sizeof(resp), &cnt, 0x1234, NULL);
    CuAssertTrue(tc, NULL == ips);
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
    ips = dns_parse_pack((char *)trunc_ptr, sizeof(trunc_ptr), &cnt, 0x0000, NULL);
    CuAssertTrue(tc, NULL == ips);
    // label 截断：length=5 但缓冲区仅剩 3 字节数据
    uint8_t trunc_label[] = {
        0x00, 0x00, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x05, 'a', 'b', 'c'
    };
    cnt = 0;
    ips = dns_parse_pack((char *)trunc_label, sizeof(trunc_label), &cnt, 0x0000, NULL);
    CuAssertTrue(tc, NULL == ips);
}
// DNS-欺骗：事务 ID 不匹配的响应(伪造/错配)应直接返回 NULL，正确 ID 才解析
static void test_dns_parse_pack_wrong_id(CuTest *tc) {
    // 合法响应，事务 ID = 0x1234（与 test_dns_parse_pack 相同）
    uint8_t resp[] = {
        0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00,
        0x07, 'e','x','a','m','p','l','e',
        0x03, 'c','o','m', 0x00,
        0x00, 0x01, 0x00, 0x01,
        0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
        0x01, 0x2c, 0x00, 0x04,
        0x01, 0x02, 0x03, 0x04
    };
    size_t cnt = 0;
    // 期望 ID 错配(0x9999) → 视为伪造响应，拒绝
    dns_ip *ips = dns_parse_pack((char *)resp, sizeof(resp), &cnt, 0x9999, NULL);
    CuAssertTrue(tc, NULL == ips);
    // 期望 ID 正确(0x1234) → 正常解析出 1.2.3.4
    cnt = 0;
    ips = dns_parse_pack((char *)resp, sizeof(resp), &cnt, 0x1234, NULL);
    CuAssertPtrNotNull(tc, ips);
    CuAssertTrue(tc, 1 == cnt);
    CuAssertStrEquals(tc, "1.2.3.4", ips[0].ip);
    FREE(ips);
}
// 压缩指针指回自己所在偏移：全报文共享的跳转预算(初值 = 报文长度)耗尽后必须拒包。
// 预算判定不在的话解压会在这条自指指针上原地打转
static void test_dns_parse_pack_ptr_self_loop(CuTest *tc) {
    uint8_t resp[] = {
        0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00,
        0x07, 'e','x','a','m','p','l','e',
        0x03, 'c','o','m', 0x00,
        0x00, 0x01, 0x00, 0x01,
        /* answer 名字 = 0xc0 0x1d，偏移 0x1d = 29 正是这两个字节自己所在的位置 */
        0xc0, 0x1d, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
        0x01, 0x2c, 0x00, 0x04,
        0x01, 0x02, 0x03, 0x04
    };
    size_t cnt = 12345;
    int32_t nodata = 1;
    dns_ip *ips = dns_parse_pack((char *)resp, sizeof(resp), &cnt, 0x1234, &nodata);
    CuAssertTrue(tc, NULL == ips);
    CuAssertTrue(tc, 0 == cnt);
    // 解析失败不是"完整答复没记录"，值得换 TCP 重试
    CuAssertTrue(tc, 0 == nodata);
}
// 构造"多条 A 记录共用同一条压缩指针链"的响应：每条记录的名字都是指向链头的指针，
// 链上各跳逐个串到查询段那个真实域名。单条记录跳数固定且很小，但预算是全报文一份，
// 记录一多总跳数就把它耗尽。返回报文实际长度
static size_t _dns_build_ptr_chain(CuTest *tc, uint8_t *out, size_t cap, int32_t nrec) {
    const int32_t hops = 30;// 链上指针个数；单条记录耗 1(记录名指针) + hops 跳
    const size_t qend = 29;// head 12 + 查询名 13 + qtype/qclass 4
    const size_t reclens = 16;// 名字指针 2 + type/class/ttl/rdlength 10 + rdata 4
    uint8_t head[12] = { 0x12, 0x34, 0x81, 0x80, 0x00, 0x01,
                         (uint8_t)(nrec >> 8), (uint8_t)nrec, 0x00, 0x00, 0x00, 0x00 };
    uint8_t query[17] = { 0x07, 'e','x','a','m','p','l','e',
                          0x03, 'c','o','m', 0x00,
                          0x00, 0x01, 0x00, 0x01 };
    size_t chain = qend + (size_t)nrec * reclens;// 记录区之后才是指针链
    size_t used = chain + (size_t)hops * 2;
    size_t off, next;
    int32_t i;
    CuAssertTrue(tc, used <= cap);
    ZERO(out, used);
    memcpy(out, head, sizeof(head));
    memcpy(out + sizeof(head), query, sizeof(query));
    for (i = 0; i < nrec; i++) {
        off = qend + (size_t)i * reclens;
        out[off] = (uint8_t)(0xc0 | (chain >> 8));
        out[off + 1] = (uint8_t)chain;
        out[off + 3] = 0x01;// type = A
        out[off + 5] = 0x01;// class = IN
        out[off + 11] = 0x04;// rdlength = 4
        out[off + 12] = 0x0a;// rdata = 10.0.0.<i+1>
        out[off + 15] = (uint8_t)(i + 1);
    }
    for (i = 0; i < hops; i++) {
        off = chain + (size_t)i * 2;
        // 最后一跳落到查询段那个真实域名(偏移 12)，其余各跳串到紧邻的下一个指针
        next = (i + 1 < hops) ? (off + 2) : sizeof(head);
        out[off] = (uint8_t)(0xc0 | (next >> 8));
        out[off + 1] = (uint8_t)next;
    }
    return used;
}
// 跳转预算是全报文一份，不是每个域名一份：同一条指针链被多条记录反复走时总跳数会超预算
static void test_dns_parse_pack_jump_budget(CuTest *tc) {
    uint8_t resp[320];
    size_t used, cnt;
    dns_ip *ips;
    char expect[IP_LENS];
    int32_t i;
    // 5 条记录：总跳数 5*31=155，预算 169(报文长度) 够用 → 全部解出
    used = _dns_build_ptr_chain(tc, resp, sizeof(resp), 5);
    cnt = 0;
    ips = dns_parse_pack((char *)resp, used, &cnt, 0x1234, NULL);
    CuAssertPtrNotNull(tc, ips);
    CuAssertTrue(tc, 5 == cnt);
    for (i = 0; i < 5; i++) {
        SNPRINTF(expect, sizeof(expect), "10.0.0.%d", i + 1);
        CuAssertStrEquals(tc, expect, ips[i].ip);
    }
    FREE(ips);
    // 10 条记录：总跳数 10*31=310 超过预算 249 → 整包拒收，不产出部分记录
    used = _dns_build_ptr_chain(tc, resp, sizeof(resp), 10);
    cnt = 12345;
    ips = dns_parse_pack((char *)resp, used, &cnt, 0x1234, NULL);
    CuAssertTrue(tc, NULL == ips);
    CuAssertTrue(tc, 0 == cnt);
}
// 多条 A 记录：条数与每条地址都要精确对上，非 A/AAAA 记录只跳过、不占名额
static void test_dns_parse_pack_multi_a(CuTest *tc) {
    uint8_t resp[] = {
        /* head: id=0x1234, flags=0x8180, qd=1, an=3, ns=0, ar=1 */
        0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x03,
        0x00, 0x00, 0x00, 0x01,
        /* query: \x07example\x03com\x00 + qtype=A + qclass=IN */
        0x07, 'e','x','a','m','p','l','e',
        0x03, 'c','o','m', 0x00,
        0x00, 0x01, 0x00, 0x01,
        /* answer 1: A 1.2.3.4 */
        0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
        0x01, 0x2c, 0x00, 0x04,
        0x01, 0x02, 0x03, 0x04,
        /* answer 2: A 5.6.7.8 */
        0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
        0x01, 0x2c, 0x00, 0x04,
        0x05, 0x06, 0x07, 0x08,
        /* answer 3: A 9.10.11.12 */
        0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
        0x01, 0x2c, 0x00, 0x04,
        0x09, 0x0a, 0x0b, 0x0c,
        /* additional: type=TXT(16)，rdlen=4，不该被计入 */
        0xc0, 0x0c, 0x00, 0x10, 0x00, 0x01, 0x00, 0x00,
        0x01, 0x2c, 0x00, 0x04,
        0xff, 0xff, 0xff, 0xff
    };
    size_t cnt = 0;
    dns_ip *ips = dns_parse_pack((char *)resp, sizeof(resp), &cnt, 0x1234, NULL);
    CuAssertPtrNotNull(tc, ips);
    CuAssertTrue(tc, 3 == cnt);
    CuAssertStrEquals(tc, "1.2.3.4", ips[0].ip);
    CuAssertStrEquals(tc, "5.6.7.8", ips[1].ip);
    CuAssertStrEquals(tc, "9.10.11.12", ips[2].ip);
    FREE(ips);
}
// _dns_encode_domain 的两条长度拒绝路径：整串 >=255 字节、单个 label >63 字节。
// buf 按对外契约留满 head(12) + 域名最长 256 + question(4)
static void test_dns_request_pack_domain_lens(CuTest *tc) {
    char buf[512];
    char domain[512];
    uint16_t id;
    // 单个 label 恰 63 字节 → 合法：12 + (1+63) + (1+3) + 1 + 4 = 85
    memset(domain, 'a', 63);
    memcpy(domain + 63, ".com", 5);
    CuAssertTrue(tc, 85 == (int)dns_request_pack(buf, domain, 0, &id));
    CuAssertTrue(tc, 63 == (uint8_t)buf[12]);
    // 单个 label 64 字节 → 拒
    memset(domain, 'a', 64);
    memcpy(domain + 64, ".com", 5);
    CuAssertTrue(tc, 0 == (int)dns_request_pack(buf, domain, 0, &id));
    // 整串 254 字节 → 合法(内部工作缓冲 256，判据是 >= 255)：12 + 256 + 4 = 272
    memset(domain, 'b', 254);
    domain[63] = '.';
    domain[127] = '.';
    domain[191] = '.';
    domain[254] = '\0';
    CuAssertTrue(tc, 272 == (int)dns_request_pack(buf, domain, 0, &id));
    // 整串 255 字节 → 拒
    memset(domain, 'b', 255);
    domain[63] = '.';
    domain[127] = '.';
    domain[191] = '.';
    domain[255] = '\0';
    CuAssertTrue(tc, 0 == (int)dns_request_pack(buf, domain, 0, &id));
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

// custz 三种头格式的组包-解包往返：编出来的头长与总长要对得上，
// 解回来的头长与载荷长也要与写进去的一致。encode/decode 成对传入，三种格式共用本体
typedef char *(*_custz_enc_cb)(size_t dlens, size_t *hlens, size_t *size);
typedef int32_t (*_custz_dec_cb)(buffer_ctx *buf, size_t *hlens, size_t *size, int32_t *status);
static void _custz_head_roundtrip(CuTest *tc, _custz_enc_cb enc, _custz_dec_cb dec,
    size_t dlens, size_t expected_hlens) {
    size_t hlens = 0, size = 0;
    char *pack = enc(dlens, &hlens, &size);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, expected_hlens == hlens);
    CuAssertTrue(tc, hlens + dlens == size);

    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, pack, size);
    FREE(pack);

    size_t out_hlens = 0, out_size = 0;
    int32_t status = PROT_INIT;
    CuAssertIntEquals(tc, ERR_OK, dec(&buf, &out_hlens, &out_size, &status));
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertTrue(tc, expected_hlens == out_hlens);
    CuAssertTrue(tc, dlens == out_size);

    buffer_free(&buf);
}
static void _custz_head_roundtrip_fixed(CuTest *tc, size_t dlens) {
    _custz_head_roundtrip(tc, _custz_encode_fixed, _custz_decode_fixed, dlens, 4);// 固定头始终 4 字节
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

    /* 回归：dlens 超 UINT32_MAX 须返 NULL。不挡的话头里只落低 32 位，
       对端按截断值收完就把余下载荷当下一个包的头继续解析 —— 静默流错位。
       32 位平台上 size_t 本就装不下这种值，故只在 64 位构造 */
    if (sizeof(size_t) > 4) {
        CuAssertTrue(tc, NULL == _custz_encode_fixed((size_t)UINT32_MAX + 1, &h, &s));
    }
}

static void _custz_head_roundtrip_flag(CuTest *tc, size_t dlens, size_t expected_hlens) {
    _custz_head_roundtrip(tc, _custz_encode_flag, _custz_decode_flag, dlens, expected_hlens);
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
    _custz_head_roundtrip(tc, _custz_encode_variable, _custz_decode_variable, dlens, expected_hlens);
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

// 手写头部字节向量直接投给解码器，断言头长与长度字段。上面三个往返用例是编码器与解码器
// 互相作证，两侧一起改（大端换小端、标志字节换值）照样全绿；这里给的是独立参照
static void _custz_wire_head(CuTest *tc,
                             int32_t (*decode)(buffer_ctx *, size_t *, size_t *, int32_t *),
                             const uint8_t *wire, size_t wlens,
                             size_t expect_hlens, size_t expect_dlens) {
    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, (void *)wire, wlens);

    size_t hlens = 0, dlens = 0;
    int32_t status = PROT_INIT;
    CuAssertIntEquals(tc, ERR_OK, decode(&buf, &hlens, &dlens, &status));
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertTrue(tc, expect_hlens == hlens);
    CuAssertTrue(tc, expect_dlens == dlens);

    buffer_free(&buf);
}
static void test_custz_head_wire(CuTest *tc) {
    // 固定头：4 字节大端。0x01020304 这种四字节互不相同的值，字节序反了立刻对不上
    uint8_t f0[4] = { 0x00, 0x00, 0x00, 0x00 };
    uint8_t f2[4] = { 0x00, 0x00, 0x00, 0x02 };
    uint8_t fbig[4] = { 0x01, 0x02, 0x03, 0x04 };
    _custz_wire_head(tc, _custz_decode_fixed, f0, sizeof(f0), 4, 0);
    _custz_wire_head(tc, _custz_decode_fixed, f2, sizeof(f2), 4, 2);
    _custz_wire_head(tc, _custz_decode_fixed, fbig, sizeof(fbig), 4, 0x01020304);

    // 标志位头：<=0xfc 单字节，0xfd/0xfe/0xff 后跟 2/4/8 字节大端
    uint8_t g0[1] = { 0x00 };
    uint8_t gfc[1] = { 0xfc };
    uint8_t g16[3] = { 0xfd, 0x01, 0x02 };
    uint8_t g32[5] = { 0xfe, 0x01, 0x02, 0x03, 0x04 };
    uint8_t g64[9] = { 0xff, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
    _custz_wire_head(tc, _custz_decode_flag, g0, sizeof(g0), 1, 0);
    _custz_wire_head(tc, _custz_decode_flag, gfc, sizeof(gfc), 1, 0xfc);
    _custz_wire_head(tc, _custz_decode_flag, g16, sizeof(g16), 3, 0x0102);
    _custz_wire_head(tc, _custz_decode_flag, g32, sizeof(g32), 5, 0x01020304);
    // 8 字节长度只有 64 位 size_t 装得下，32 位平台上解码器会判 PROT_ERROR
    if (sizeof(size_t) > 4) {
        _custz_wire_head(tc, _custz_decode_flag, g64, sizeof(g64), 9,
            (size_t)0x0102030405060708ull);
    }

    // MQTT 变长头：每字节低 7 位存值、最高位为延续标志，低位组在前
    uint8_t v0[1] = { 0x00 };
    uint8_t v127[1] = { 0x7f };
    uint8_t v128[2] = { 0x80, 0x01 };
    uint8_t v16384[3] = { 0x80, 0x80, 0x01 };
    uint8_t v2097152[4] = { 0x80, 0x80, 0x80, 0x01 };
    uint8_t vmax[4] = { 0xff, 0xff, 0xff, 0x7f };
    _custz_wire_head(tc, _custz_decode_variable, v0, sizeof(v0), 1, 0);
    _custz_wire_head(tc, _custz_decode_variable, v127, sizeof(v127), 1, 127);
    _custz_wire_head(tc, _custz_decode_variable, v128, sizeof(v128), 2, 128);
    _custz_wire_head(tc, _custz_decode_variable, v16384, sizeof(v16384), 3, 16384);
    _custz_wire_head(tc, _custz_decode_variable, v2097152, sizeof(v2097152), 4, 2097152);
    _custz_wire_head(tc, _custz_decode_variable, vmax, sizeof(vmax), 4, MQTT_VARINT_MAX);
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

/* 组包侧只挡长度回绕；WS_MAX_PAYLOAD_LENS 是接收策略，不限制发送 */
static void test_websock_maxpack(CuTest *tc) {
    size_t size;
    size_t dlens = (size_t)WS_MAX_PAYLOAD_LENS + 1;// 超接收侧上限，组包侧照常放行
    // 长度字段按 RFC 6455 §5.2 分档：<=125 不带，<=0xffff 带 2 字节，再大带 8 字节。
    // dlens 落在第三档，顺带覆盖 8 字节扩展长度这条以前组包侧到不了的路
    size_t pllens = (dlens <= 125) ? 0 : ((dlens <= 0xffff) ? sizeof(uint16_t) : sizeof(uint64_t));
    void *p;
    char *big;
    MALLOC(big, dlens);
    memset(big, 'z', dlens);
    /* 回绕即拒，且 size 清 0（调用方按"返回非 NULL 才读 size"约定）。
       守卫在任何拷贝之前，所以传真指针配一个到不了的长度是安全的 */
    size = 12345;
    p = websock_pack_text(0, 1, big, SIZE_MAX - 4, &size);
    CuAssertPtrEquals(tc, NULL, p);
    CuAssertTrue(tc, 0 == (int)size);
    size = 12345;
    p = websock_pack_binary(1, 1, big, SIZE_MAX - 4, &size);
    CuAssertPtrEquals(tc, NULL, p);
    CuAssertTrue(tc, 0 == (int)size);
    size = 12345;
    p = websock_pack_continua(0, 1, big, SIZE_MAX - 4, &size);
    CuAssertPtrEquals(tc, NULL, p);
    CuAssertTrue(tc, 0 == (int)size);
    /* 超接收侧上限照常组包：对端多不是 srey，不拿本地策略卡发送 */
    size = 0;
    p = websock_pack_text(0, 1, big, dlens, &size);
    CuAssertPtrNotNull(tc, p);
    CuAssertTrue(tc, 2 + pllens + dlens == size);// mask=0，无掩码键
    FREE(p);
    FREE(big);
}

static void test_websock_pack_handshake(CuTest *tc) {
    ws_hs_ctx *hsctx = NULL;
    char *req = websock_pack_handshake("example.com", NULL, "mqtt", &hsctx);
    CuAssertPtrNotNull(tc, req);
    CuAssertPtrNotNull(tc, hsctx);

    /* 握手请求必须含 GET / Upgrade / Sec-WebSocket-Version 等关键头 */
    CuAssertTrue(tc, NULL != strstr(req, "GET "));
    CuAssertTrue(tc, NULL != strstr(req, "Upgrade: websocket"));
    CuAssertTrue(tc, NULL != strstr(req, "Connection: "));
    CuAssertTrue(tc, NULL != strstr(req, "Sec-WebSocket-Key:"));
    CuAssertTrue(tc, NULL != strstr(req, "Sec-WebSocket-Version: 13"));
    CuAssertTrue(tc, NULL != strstr(req, "Host: example.com"));
    /* 子协议字段写入 */
    CuAssertTrue(tc, NULL != strstr(req, "Sec-WebSocket-Protocol: mqtt"));

    /* hsctx->signkey 是 base64(sha1(key+GUID))，应非空 */
    CuAssertTrue(tc, 0 != hsctx->signkey[0]);
    FREE(req);
    FREE(hsctx);

    ws_hs_ctx *chatctx = NULL;
    char *chatreq = websock_pack_handshake("example.com", NULL, "chat", &chatctx);
    CuAssertPtrNotNull(tc, chatreq);
    CuAssertPtrNotNull(tc, chatctx);
    CuAssertTrue(tc, NULL != strstr(chatreq, "Sec-WebSocket-Protocol: chat"));
    FREE(chatreq);
    FREE(chatctx);

    ws_hs_ctx *noctx = NULL;
    char *noreq = websock_pack_handshake("example.com", NULL, NULL, &noctx);
    CuAssertPtrNotNull(tc, noreq);
    CuAssertPtrNotNull(tc, noctx);
    CuAssertTrue(tc, NULL == strstr(noreq, "Sec-WebSocket-Protocol"));
    FREE(noreq);
    FREE(noctx);

    /* 多值 + 前后 OWS 去除:req 头原样带列表,hsctx 解析出去空格后的 token */
    ws_hs_ctx *mctx = NULL;
    char *mreq = websock_pack_handshake("example.com", NULL, "mqtt , chat", &mctx);
    CuAssertPtrNotNull(tc, mreq);
    CuAssertPtrNotNull(tc, mctx);
    CuAssertTrue(tc, NULL != strstr(mreq, "Sec-WebSocket-Protocol: mqtt , chat"));
    CuAssertTrue(tc, 2 == mctx->cnt);
    CuAssertTrue(tc, 4 == mctx->prots[0].lens && 0 == memcmp(mctx->prots[0].data, "mqtt", 4));
    CuAssertTrue(tc, 4 == mctx->prots[1].lens && 0 == memcmp(mctx->prots[1].data, "chat", 4));
    FREE(mreq);
    FREE(mctx);

    /* 空元素(RFC 7230 §7)忽略:"a,,b" 解析为 2 个有效 token */
    ws_hs_ctx *ectx = NULL;
    char *ereq = websock_pack_handshake("example.com", NULL, "a,,b", &ectx);
    CuAssertPtrNotNull(tc, ereq);
    CuAssertTrue(tc, 2 == ectx->cnt);
    FREE(ereq);
    FREE(ectx);

    /* 空元素不占 WS_MAXCNT_SECPROT 名额：这一串按逗号是 9 段，按 RFC 7230 §7 只有 1 个 token */
    ws_hs_ctx *pctx = NULL;
    char *preq = websock_pack_handshake("example.com", NULL, "mqtt,,,,,,,,", &pctx);
    CuAssertPtrNotNull(tc, preq);
    CuAssertTrue(tc, 1 == pctx->cnt);
    CuAssertTrue(tc, 4 == pctx->prots[0].lens && 0 == memcmp(pctx->prots[0].data, "mqtt", 4));
    FREE(preq);
    FREE(pctx);

    /* 正好 8 个有效 token → 通过 */
    ws_hs_ctx *fctx = NULL;
    char *freq = websock_pack_handshake("example.com", NULL, "a, b ,c,d,e,f,g,h", &fctx);
    CuAssertPtrNotNull(tc, freq);
    CuAssertTrue(tc, 8 == fctx->cnt);
    FREE(freq);
    FREE(fctx);

    /* 客户端侧不截断：自己配了 9 个是配置错误，组包直接失败而不是悄悄砍成 8 个
       （服务端侧相反，超上限取前 8 个继续协商，见 _websock_secprot_split 的 trunc） */
    ws_hs_ctx *octx = NULL;
    CuAssertTrue(tc, NULL == websock_pack_handshake("example.com", NULL, "a,b,c,d,e,f,g,h,i", &octx));

    /* 非法 token(含裸 LF,即 #1 崩溃向量)→ 返回 NULL */
    ws_hs_ctx *bctx = NULL;
    CuAssertTrue(tc, NULL == websock_pack_handshake("example.com", NULL, "a\nb", &bctx));

    /* host / uri 含 CRLF：原先直落 http_pack_head / http_pack_req 的 ASSERTAB 打死进程，
       现在与 secprot 同口径走 NULL 失败通道 */
    ws_hs_ctx *hctx = NULL;
    CuAssertTrue(tc, NULL == websock_pack_handshake("evil.com\r\nX-Injected: 1", NULL, NULL, &hctx));
    ws_hs_ctx *uctx = NULL;
    CuAssertTrue(tc, NULL == websock_pack_handshake("example.com", "/a\r\nX-Injected: 1", NULL, &uctx));
    ws_hs_ctx *lfctx = NULL;
    CuAssertTrue(tc, NULL == websock_pack_handshake("example.com", "/a\nb", NULL, &lfctx));
}

// 把一段握手报文喂给 websock_unpack，回带 status。client=0 走服务端校验，1 走客户端校验
static int32_t _ws_hs_feed(const char *msg, int32_t client, void *hsctx) {
    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, (void *)msg, strlen(msg));
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.pktype = PACK_WEBSOCK;
    ud.context = hsctx;// 服务端侧为 NULL；客户端侧须是 websock_pack_handshake 产出的 ws_hs_ctx
    int32_t status = PROT_INIT;
    (void)_t_websock_unpack(client, &buf, &ud, NULL, &status);
    // 成功后 ud.context 换成新 CALLOC 的 websock_ctx，失败则仍是传进来的 hsctx（客户端侧）
    // 或一直为 NULL（服务端侧）；三种归宿 _websock_udfree 都认，调用方不要另行释放
    _websock_udfree(&ud);
    buffer_free(&buf);
    return status;
}
// 服务端握手校验的负向用例。表驱动改造后掩码或表项写错会把"拒绝"变成"接受"，
// 而正向路径全绿照不出来，故每条只破坏一处、其余保持合法。
// 这里不设正向对照：合法请求会走到 _websock_handshake_respond，它的 ev_send 在 INVALID_SOCK 上
// 必然失败，同样落 PROT_ERROR，与拒绝无从区分——正向由集成用例(task_ws_server / unit_websock.lua)覆盖
static void test_websock_handshake_server_reject(CuTest *tc) {
    prots_init(&g_stub_emit);// 失败路径也要经 _hs_push，begin 是空函数指针会当场崩
    static const char *cases[] = {
        // 方法不是 GET
        "POST / HTTP/1.1\r\nHost: a\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n",
        // 缺 Connection
        "GET / HTTP/1.1\r\nHost: a\r\nUpgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n",
        // Connection 值不是 upgrade
        "GET / HTTP/1.1\r\nHost: a\r\nConnection: keep-alive\r\nUpgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n",
        // 缺 Upgrade
        "GET / HTTP/1.1\r\nHost: a\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n",
        // Upgrade 值不是 websocket
        "GET / HTTP/1.1\r\nHost: a\r\nConnection: Upgrade\r\nUpgrade: h2c\r\n"
        "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n",
        // 缺 Sec-WebSocket-Version
        "GET / HTTP/1.1\r\nHost: a\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n",
        // Version 不是 13（RFC 6455 之前的草案版本）
        "GET / HTTP/1.1\r\nHost: a\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
        "Sec-WebSocket-Version: 8\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n",
        // 缺 Sec-WebSocket-Key
        "GET / HTTP/1.1\r\nHost: a\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n",
        // Key 解出来不是 16 字节（表项只查键存在，长度由后面的 bs64_decode 卡）
        "GET / HTTP/1.1\r\nHost: a\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: c2hvcnQ=\r\n\r\n"
    };
    size_t i;
    for (i = 0; i < ARRAY_SIZE(cases); i++) {
        CuAssertTrue(tc, BIT_CHECK(_ws_hs_feed(cases[i], 0, NULL), PROT_ERROR));
    }
}
// 客户端握手校验：正向 + 负向。正向这边有对照——成功时 _hs_push 推 ERR_OK，
// 拿桩记下的 erro 就能把"接受"和"拒绝"分开，不像服务端只能看 PROT_ERROR
static void test_websock_handshake_client_reject(CuTest *tc) {
    prots_init(&g_stub_emit);
    // 负向：状态码 / 三个表项各破坏一处，其余保持合法
    static const char *fmts[] = {
        // 状态码不是 101
        "HTTP/1.1 200 OK\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n",
        // 缺 Connection
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n",
        // 缺 Upgrade
        "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n",
        // 缺 Sec-WebSocket-Accept（%s 用不上，照样按格式串走）
        "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
        "X-Ignored: %s\r\n\r\n",
        // Accept 值与本地签名不符
        "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
        "Sec-WebSocket-Accept: %.0sYWJjZGVmZ2hpamtsbW5vcHFyc3R1dg==\r\n\r\n"
    };
    char msg[512];
    ws_hs_ctx *hsctx;
    char *req;
    size_t i;
    for (i = 0; i < ARRAY_SIZE(fmts); i++) {
        req = websock_pack_handshake("example.com", NULL, NULL, &hsctx);
        CuAssertPtrNotNull(tc, req);
        SNPRINTF(msg, sizeof(msg), fmts[i], hsctx->signkey);
        // hsctx 不在此释放：握手失败时 ud->status 还停在 INIT，_ws_hs_feed 末尾的
        // _websock_udfree 会按"客户端未完成握手"把它收掉，再 FREE 就是二次释放
        CuAssertTrue(tc, BIT_CHECK(_ws_hs_feed(msg, 1, hsctx), PROT_ERROR));
        FREE(req);
    }
    // 正向：成功时库自己 FREE(ud->context) 再挂上 websock_ctx，收尾同样归 _websock_udfree
    req = websock_pack_handshake("example.com", NULL, NULL, &hsctx);
    CuAssertPtrNotNull(tc, req);
    SNPRINTF(msg, sizeof(msg),
        "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", hsctx->signkey);
    g_stub_emit_calls = 0;
    CuAssertTrue(tc, !BIT_CHECK(_ws_hs_feed(msg, 1, hsctx), PROT_ERROR));
    CuAssertIntEquals(tc, 1, g_stub_emit_calls);
    CuAssertIntEquals(tc, (int)MSG_TYPE_HANDSHAKED, (int)g_stub_last_msg.mtype);
    CuAssertIntEquals(tc, ERR_OK, g_stub_last_msg.erro);
    FREE(req);
}

static void test_websock_secprot_match(CuTest *tc) {
    pack_type sectype = PACK_NONE;
    CuAssertTrue(tc, ERR_OK == websock_secprot_match("mqtt", 4, &sectype));
    CuAssertTrue(tc, PACK_MQTT == sectype);
    sectype = PACK_NONE;
    CuAssertTrue(tc, ERR_FAILED == websock_secprot_match("MQTT", 4, &sectype));
    CuAssertTrue(tc, PACK_NONE == sectype);
    CuAssertTrue(tc, ERR_FAILED == websock_secprot_match("chat", 4, &sectype));
    CuAssertTrue(tc, ERR_FAILED == websock_secprot_match("mqt", 3, &sectype));
    sectype = PACK_NONE;
    CuAssertTrue(tc, ERR_OK == websock_secprot_match("mqttXX", 4, &sectype));
    CuAssertTrue(tc, PACK_MQTT == sectype);
}

static void test_websock_unpack_text(CuTest *tc) {
    /* 客户端解服务端无 mask 的 TEXT 帧 */
    size_t size = 0;
    void *frame = websock_pack_text(0, 1, "hello", 5, &size);
    CuAssertPtrNotNull(tc, frame);

    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, frame, size);
    FREE(frame);

    websock_ctx ws;
    ud_cxt ud;
    _ws_ctx_init(&ws, &ud);

    int32_t status = PROT_INIT;
    struct websock_pack_ctx *pack = _t_websock_unpack(1 /*client=true*/, &buf, &ud, NULL, &status);
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

    websock_ctx ws;
    ud_cxt ud;
    _ws_ctx_init(&ws, &ud);

    int32_t status = PROT_INIT;
    struct websock_pack_ctx *pack = _t_websock_unpack(0 /*server*/, &buf, &ud, NULL, &status);
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

    websock_ctx ws;
    ud_cxt ud;
    _ws_ctx_init(&ws, &ud);

    /* 第一帧：起始 → PROT_SLICE_START */
    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, f1, s1);
    FREE(f1);

    int32_t status = PROT_INIT;
    struct websock_pack_ctx *pack = _t_websock_unpack(1, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_SLICE_START));
    _websock_pkfree(pack);

    /* 第二帧：中间 → PROT_SLICE */
    buffer_append(&buf, f2, s2);
    FREE(f2);
    status = PROT_INIT;
    pack = _t_websock_unpack(1, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_SLICE));
    _websock_pkfree(pack);

    /* 第三帧：结束 → PROT_SLICE_END */
    buffer_append(&buf, f3, s3);
    FREE(f3);
    status = PROT_INIT;
    pack = _t_websock_unpack(1, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_SLICE_END));
    _websock_pkfree(pack);

    buffer_free(&buf);
}

// RFC 6455 §5.4：没有起始帧就来 CONTINUE（孤立 continuation）必须拒
static void test_websock_unpack_orphan_continue(CuTest *tc) {
    websock_ctx ws;
    ud_cxt ud;
    buffer_ctx buf;
    // FIN=1 + opcode=CONTINUE(0)：0x80；FIN=0 的那个是 0x00。两者都没有起始帧在前
    uint8_t frames[2][2] = { { 0x80, 0x00 }, { 0x00, 0x00 } };
    size_t i;
    int32_t status;
    struct websock_pack_ctx *pack;
    for (i = 0; i < ARRAY_SIZE(frames); i++) {
        buffer_init(&buf);
        buffer_append(&buf, frames[i], sizeof(frames[i]));
        _ws_ctx_init(&ws, &ud);
        status = PROT_INIT;
        pack = _t_websock_unpack(1, &buf, &ud, NULL, &status);
        CuAssertTrue(tc, NULL == pack);
        CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
        // 拒帧不该顺手把连接标成分片中
        CuAssertTrue(tc, 0 == ws.slice);
        buffer_free(&buf);
    }
}
// RFC 6455 §5.4：分片进行中插一个非 CONTINUE 的数据帧必须拒
static void test_websock_unpack_interleaved_data(CuTest *tc) {
    websock_ctx ws;
    ud_cxt ud;
    buffer_ctx buf;
    int32_t status;
    struct websock_pack_ctx *pack;
    // 1. 先发一个真的起始帧 TEXT(fin=0) 把连接置成分片中，再发 TEXT(fin=1)
    size_t s1 = 0, s2 = 0;
    void *f1 = websock_pack_text(0, 0, "AAA", 3, &s1);
    void *f2 = websock_pack_text(0, 1, "BBB", 3, &s2);
    buffer_init(&buf);
    buffer_append(&buf, f1, s1);
    FREE(f1);
    _ws_ctx_init(&ws, &ud);
    status = PROT_INIT;
    pack = _t_websock_unpack(1, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_SLICE_START));
    CuAssertTrue(tc, 1 == ws.slice);
    _websock_pkfree(pack);
    buffer_append(&buf, f2, s2);
    FREE(f2);
    status = PROT_INIT;
    pack = _t_websock_unpack(1, &buf, &ud, NULL, &status);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
    buffer_free(&buf);

    // 2. BINARY 同样不许插入：直接摆分片中状态（同本文件直接摆 ud.status 的做法）
    // FIN=1 + opcode=BINARY(2)：0x82，无 payload
    uint8_t bin[2] = { 0x82, 0x00 };
    buffer_init(&buf);
    buffer_append(&buf, bin, sizeof(bin));
    _ws_ctx_init(&ws, &ud);
    ws.slice = 1;// 摆进分片接收态
    status = PROT_INIT;
    pack = _t_websock_unpack(1, &buf, &ud, NULL, &status);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
    buffer_free(&buf);

    // 3. 反向不误拒：分片进行中的控制帧(PING)是合法的，RFC 6455 §5.4 允许插控制帧
    // FIN=1 + opcode=PING(9)：0x89，无 payload
    uint8_t ping[2] = { 0x89, 0x00 };
    buffer_init(&buf);
    buffer_append(&buf, ping, sizeof(ping));
    _ws_ctx_init(&ws, &ud);
    ws.slice = 1;// 摆进分片接收态
    status = PROT_INIT;
    pack = _t_websock_unpack(1, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertIntEquals(tc, WS_PING, websock_prot(pack));
    // 控制帧不改分片状态
    CuAssertTrue(tc, 1 == ws.slice);
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

    websock_ctx ws;
    ud_cxt ud;
    _ws_ctx_init(&ws, &ud);

    int32_t status = PROT_INIT;
    struct websock_pack_ctx *pack = _t_websock_unpack(1, &buf, &ud, NULL, &status);
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

    websock_ctx ws;
    ud_cxt ud;
    _ws_ctx_init(&ws, &ud);

    int32_t status = PROT_INIT;
    struct websock_pack_ctx *pack = _t_websock_unpack(0 /*server*/, &buf, &ud, NULL, &status);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
    buffer_free(&buf);
}

// 客户端收到带掩码的服务端帧应触发 PROT_ERROR（RFC 6455 §5.1：掩码只许客户端→服务端）
static void test_websock_unpack_client_masked(CuTest *tc) {
    size_t size = 0;
    // 服务端本不该带 mask，这里用 mask=1 故意构造非法帧
    void *frame = websock_pack_text(1, 1, "hi", 2, &size);
    CuAssertPtrNotNull(tc, frame);
    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, frame, size);
    FREE(frame);

    websock_ctx ws;
    ud_cxt ud;
    _ws_ctx_init(&ws, &ud);

    int32_t status = PROT_INIT;
    struct websock_pack_ctx *pack = _t_websock_unpack(1 /*client*/, &buf, &ud, NULL, &status);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
    buffer_free(&buf);
}

// 非法 opcode（保留范围 0x3-0x7 和 >= 0xB）应触发 PROT_ERROR
static void test_websock_unpack_reserved_opcode(CuTest *tc) {
    websock_ctx ws;
    ud_cxt ud;
    buffer_ctx buf;

    // 保留数据帧 opcode 0x3：byte0 = 0x83 (FIN=1 + opcode=3), byte1 = 0
    uint8_t reserved3[2] = { 0x83, 0x00 };
    buffer_init(&buf);
    buffer_append(&buf, reserved3, sizeof(reserved3));
    _ws_ctx_init(&ws, &ud);
    int32_t status = PROT_INIT;
    struct websock_pack_ctx *pack = _t_websock_unpack(1, &buf, &ud, NULL, &status);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
    buffer_free(&buf);

    // 保留控制帧 opcode 0xB：byte0 = 0x8B
    uint8_t reservedB[2] = { 0x8B, 0x00 };
    buffer_init(&buf);
    buffer_append(&buf, reservedB, sizeof(reservedB));
    _ws_ctx_init(&ws, &ud);
    status = PROT_INIT;
    pack = _t_websock_unpack(1, &buf, &ud, NULL, &status);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
    buffer_free(&buf);
}

// RSV 标志位（bit 4/5/6）置 1 应触发 PROT_ERROR
static void test_websock_unpack_rsv_set(CuTest *tc) {
    websock_ctx ws;
    ud_cxt ud;
    buffer_ctx buf;
    // FIN=1 + RSV1=1 + opcode=TEXT(1)：0xC1
    uint8_t rsv1[2] = { 0xC1, 0x00 };
    buffer_init(&buf);
    buffer_append(&buf, rsv1, sizeof(rsv1));
    _ws_ctx_init(&ws, &ud);
    int32_t status = PROT_INIT;
    struct websock_pack_ctx *pack = _t_websock_unpack(1, &buf, &ud, NULL, &status);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
    buffer_free(&buf);
}

// 控制帧不可分片（FIN=0 + opcode=PING）应触发 PROT_ERROR
static void test_websock_unpack_control_fragmented(CuTest *tc) {
    websock_ctx ws;
    ud_cxt ud;
    buffer_ctx buf;
    // FIN=0 + opcode=PING(9)：0x09
    uint8_t bad[2] = { 0x09, 0x00 };
    buffer_init(&buf);
    buffer_append(&buf, bad, sizeof(bad));
    _ws_ctx_init(&ws, &ud);
    int32_t status = PROT_INIT;
    struct websock_pack_ctx *pack = _t_websock_unpack(1, &buf, &ud, NULL, &status);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
    buffer_free(&buf);
}

// 控制帧 payload > 125 应触发 PROT_ERROR
static void test_websock_unpack_control_too_big(CuTest *tc) {
    websock_ctx ws;
    ud_cxt ud;
    buffer_ctx buf;
    // FIN=1 + opcode=PING(9) + payloadlen=126 (扩展长度标志)：0x89 0x7E
    uint8_t bad[4] = { 0x89, 0x7E, 0x00, 0x80 }; // 128 字节
    buffer_init(&buf);
    buffer_append(&buf, bad, sizeof(bad));
    _ws_ctx_init(&ws, &ud);
    int32_t status = PROT_INIT;
    struct websock_pack_ctx *pack = _t_websock_unpack(1, &buf, &ud, NULL, &status);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
    buffer_free(&buf);
}

// 扩展长度 16-bit：1024 字节 payload（126 起就用 16-bit 扩展长度），验证组包 round-trip
static void test_websock_unpack_extended_16(CuTest *tc) {
    size_t s = 0;
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

    websock_ctx ws;
    ud_cxt ud;
    _ws_ctx_init(&ws, &ud);
    int32_t status = PROT_INIT;
    struct websock_pack_ctx *pack = _t_websock_unpack(1, &buf, &ud, NULL, &status);
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

    websock_ctx ws;
    ZERO(&ws, sizeof(ws));
    ws.secprot = PACK_NONE; // 未启用子协议
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = 1;
    ud.context = &ws;
    int32_t status = PROT_INIT;
    struct websock_pack_ctx *pack = _t_websock_unpack(1, &buf, &ud, NULL, &status);
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

    websock_ctx ws;
    ud_cxt ud;
    _ws_ctx_init(&ws, &ud);
    int32_t status = PROT_INIT;
    // 服务端解码：xor 后应恢复原文
    struct websock_pack_ctx *pack = _t_websock_unpack(0, &buf, &ud, NULL, &status);
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
    // prots_free 清消息汇，幂等：连调两次不出事。清干净之后再走网络事件回调是要当场崩的
    // （fail-fast 就是目的），故不在此验证，只验幂等
    prots_free();
    prots_free();
}

// prots_pkfree default 分支：未识别 pktype 直接 FREE，ASan 验证无 leak
static void test_prots_pkfree_default(CuTest *tc) {
    // PACK_NONE / PACK_DNS / PACK_SMTP / PACK_CUSTZ_* 走 default → FREE(data)。
    // 用分配计数当观测点：本用例是单线程的，进出相抵才说明 default 真的释放了
    pack_type defaults[] = { PACK_NONE, PACK_DNS, PACK_SMTP,
                             PACK_CUSTZ_FIXED, PACK_CUSTZ_FLAG, PACK_CUSTZ_VAR };
    uint64_t a0, f0, a1, f1;
    mem_stat(&a0, &f0);
    void *p;
    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
        MALLOC(p, 64);
        memset(p, 0xaa, 64);
        prots_pkfree(defaults[i], p);
    }
    mem_stat(&a1, &f1);
    CuAssertTrue(tc, a1 - a0 == f1 - f0);
}

// prots_hsfree default 分支：除 PACK_MONGO 外都走 default → FREE(data)
static void test_prots_hsfree_default(CuTest *tc) {
    pack_type defaults[] = { PACK_NONE, PACK_HTTP, PACK_WEBSOCK, PACK_MQTT,
                             PACK_REDIS, PACK_MYSQL, PACK_PGSQL };
    uint64_t a0, f0, a1, f1;
    mem_stat(&a0, &f0);
    void *p;
    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
        MALLOC(p, 32);
        prots_hsfree(defaults[i], p);
    }
    mem_stat(&a1, &f1);
    CuAssertTrue(tc, a1 - a0 == f1 - f0);
}

// prots_udfree default 分支：PACK_NONE / PACK_DNS / PACK_CUSTZ_* → FREE(ud->context)
static void test_prots_udfree_default(CuTest *tc) {
    pack_type defaults[] = { PACK_NONE, PACK_DNS,
                             PACK_CUSTZ_FIXED, PACK_CUSTZ_FLAG, PACK_CUSTZ_VAR };
    uint64_t a0, f0, a1, f1;
    mem_stat(&a0, &f0);
    ud_cxt ud;
    sock_ctx sk;
    sock_set_invalid(&sk);
    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
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
    mem_stat(&a1, &f1);
    CuAssertTrue(tc, a1 - a0 == f1 - f0);
}

// prots_net_close default 分支：PACK_NONE / PACK_HTTP / PACK_WEBSOCK 等无协议专属清理，
// 仅推一条 MSG_TYPE_CLOSE（SMTP/MYSQL/PGSQL/MONGO 需要真实协议 context，不在此测试范围）
static void test_prots_net_close_default(CuTest *tc) {
    prots_init(&g_stub_emit);
    pack_type defaults[] = { PACK_NONE, PACK_DNS, PACK_HTTP, PACK_WEBSOCK,
                             PACK_MQTT, PACK_CUSTZ_FIXED, PACK_REDIS };
    ud_cxt ud;
    sock_ctx sk;
    sock_set_invalid(&sk);
    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
        ZERO(&ud, sizeof(ud));
        ud.pktype = (subtype_t)defaults[i];
        g_stub_emit_calls = 0;
        sk.skid = 100 + (uint64_t)i;
        prots_net_close(NULL, &sk, 0, CLOSE_TYPE_LOCAL, &ud);
        CuAssertIntEquals(tc, 1, g_stub_emit_calls);
        CuAssertIntEquals(tc, (int)MSG_TYPE_CLOSE, (int)g_stub_last_msg.mtype);
        CuAssertIntEquals(tc, (int)defaults[i], (int)g_stub_last_msg.subtype);
        CuAssertIntEquals(tc, CLOSE_TYPE_LOCAL, g_stub_last_msg.erro);
    }
}

// 把 ud 送进 TILLCLOSE：响应无 CL/TE，头部到齐即首片
static void _tillclose_enter(CuTest *tc, buffer_ctx *buf, ud_cxt *ud) {
    ZERO(ud, sizeof(ud_cxt));
    ud->pktype = PACK_HTTP;
    int32_t status = PROT_INIT;
    _bput(buf, "HTTP/1.1 200 OK\r\n\r\n");
    struct http_pack_ctx *pack = _t_http_unpack(1, buf, ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_SLICE_START));
    _http_pkfree(pack);
}
// close_type 门禁：规则 7 的 body 没有长度可校验。有序结束与 TLS 无 close_notify 断开(TRUNCATED)
// 都补末片，后者是否可信由业务看 CLOSE 的 erro 自己判；真出错的那两档补末片就等于把半条 body 报成完整的
static void test_prots_net_close_tail_gate(CuTest *tc) {
    prots_init(&g_stub_emit);
    buffer_ctx buf;
    buffer_init(&buf);
    ud_cxt ud;
    sock_ctx sk;
    sock_set_invalid(&sk);

    // 1) 有序结束：末片 + CLOSE 两条，末片带 SLICE_END
    _tillclose_enter(tc, &buf, &ud);
    g_stub_emit_calls = 0;
    sk.skid = 1;
    prots_net_close(NULL, &sk, 1, CLOSE_TYPE_ORDERLY, &ud);
    CuAssertIntEquals(tc, 2, g_stub_emit_calls);
    CuAssertIntEquals(tc, (int)MSG_TYPE_RECV, (int)g_stub_first_msg.mtype);
    CuAssertIntEquals(tc, PROT_SLICE_END, (int)g_stub_first_msg.slice);
    _http_pkfree(g_stub_first_msg.data);// 分发层才会 _message_clean，这里自己收
    CuAssertIntEquals(tc, (int)MSG_TYPE_CLOSE, (int)g_stub_last_msg.mtype);
    CuAssertIntEquals(tc, CLOSE_TYPE_ORDERLY, g_stub_last_msg.erro);

    // 2) TLS 没发 close_notify 就断：同样补末片，但 CLOSE 上的 erro 是 TRUNCATED
    _tillclose_enter(tc, &buf, &ud);
    g_stub_emit_calls = 0;
    sk.skid = 2;
    prots_net_close(NULL, &sk, 1, CLOSE_TYPE_TRUNCATED, &ud);
    CuAssertIntEquals(tc, 2, g_stub_emit_calls);
    CuAssertIntEquals(tc, PROT_SLICE_END, (int)g_stub_first_msg.slice);
    _http_pkfree(g_stub_first_msg.data);
    CuAssertIntEquals(tc, CLOSE_TYPE_TRUNCATED, g_stub_last_msg.erro);

    // 3) 异常中断 / 本地主动：只有 CLOSE，业务在分片循环里拿到失败而不是"收完了"
    int32_t types[] = { CLOSE_TYPE_ABORT, CLOSE_TYPE_LOCAL };
    size_t i;
    for (i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        _tillclose_enter(tc, &buf, &ud);
        g_stub_emit_calls = 0;
        sk.skid = 3 + (uint64_t)i;
        prots_net_close(NULL, &sk, 1, types[i], &ud);
        CuAssertIntEquals(tc, 1, g_stub_emit_calls);
        CuAssertIntEquals(tc, (int)MSG_TYPE_CLOSE, (int)g_stub_last_msg.mtype);
        CuAssertIntEquals(tc, types[i], g_stub_last_msg.erro);
    }

    buffer_free(&buf);
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
    void *out = _t_prots_unpack(1, &buf, &ud, &size, &status);
    CuAssertPtrNotNull(tc, out);
    CuAssertIntEquals(tc, (int)plen, (int)size);
    CuAssertTrue(tc, 0 == memcmp(out, payload, plen));
    // PACK_NONE default 路径不设置任何 status bit
    CuAssertIntEquals(tc, PROT_INIT, status);
    FREE(out);

    // 缓冲区为空时返回 NULL
    size = 999;
    status = PROT_ERROR;
    out = _t_prots_unpack(1, &buf, &ud, &size, &status);
    CuAssertTrue(tc, NULL == out);
    CuAssertIntEquals(tc, 0, (int)size);
    CuAssertIntEquals(tc, PROT_INIT, status);
    buffer_free(&buf);
}

// parse_int64_strict：mysql / pgsql 文本协议共用的整数解析。重点是 strtoll 骗得过
// "消费长度相符"校验的那两条（空串、溢出钳到 LLONG_MAX）以及 INT64_MIN 的取负边界
static void test_parse_int64_strict(CuTest *tc) {
    // 哨兵初值:取一个没有任何断言期望的值,这样 0 == v 仍能证明函数真写了出参
    int64_t v = -424242;

    CuAssertIntEquals(tc, ERR_OK, parse_int64_strict("0", 1, &v));
    CuAssertTrue(tc, 0 == v);
    CuAssertIntEquals(tc, ERR_OK, parse_int64_strict("9223372036854775807", 19, &v));
    CuAssertTrue(tc, INT64_MAX == v);
    // INT64_MIN：绝对值超出 int64_t，取负前须单独挑出
    CuAssertIntEquals(tc, ERR_OK, parse_int64_strict("-9223372036854775808", 20, &v));
    CuAssertTrue(tc, INT64_MIN == v);
    CuAssertIntEquals(tc, ERR_OK, parse_int64_strict("-1", 2, &v));
    CuAssertTrue(tc, -1 == v);
    // 只取 lens 之内的字节，不要求 NUL 结尾
    CuAssertIntEquals(tc, ERR_OK, parse_int64_strict("123abc", 3, &v));
    CuAssertTrue(tc, 123 == v);

    // 溢出各一格
    CuAssertIntEquals(tc, ERR_FAILED, parse_int64_strict("9223372036854775808", 19, &v));
    CuAssertIntEquals(tc, ERR_FAILED, parse_int64_strict("-9223372036854775809", 20, &v));
    // 空串 / 只有负号 / 含非数字 / 前导正号与空白
    CuAssertIntEquals(tc, ERR_FAILED, parse_int64_strict("", 0, &v));
    CuAssertIntEquals(tc, ERR_FAILED, parse_int64_strict("-", 1, &v));
    CuAssertIntEquals(tc, ERR_FAILED, parse_int64_strict("12a", 3, &v));
    CuAssertIntEquals(tc, ERR_FAILED, parse_int64_strict("+1", 2, &v));
    CuAssertIntEquals(tc, ERR_FAILED, parse_int64_strict(" 1", 2, &v));
}
// parse_colon_triple：取代 sscanf("%d:%d:%d") 的那个带上界解析器。
// 重点是位数超 int 的输入必须被拒——那正是 sscanf 版本的未定义行为入口
static void test_parse_colon_triple(CuTest *tc) {
    static const uint32_t max[3] = { 838, 59, 59 };
    uint32_t v[3];

    // 三段齐
    CuAssertIntEquals(tc, 3, parse_colon_triple("838:59:59", max, v));
    CuAssertIntEquals(tc, 838, (int)v[0]);
    CuAssertIntEquals(tc, 59, (int)v[1]);
    CuAssertIntEquals(tc, 59, (int)v[2]);
    // 尾部小数秒不影响前三段
    CuAssertIntEquals(tc, 3, parse_colon_triple("01:02:03.456789", max, v));
    CuAssertIntEquals(tc, 1, (int)v[0]);
    CuAssertIntEquals(tc, 3, (int)v[2]);
    // 缺段：返回实际段数，未出现的段填 0
    CuAssertIntEquals(tc, 2, parse_colon_triple("05:30", max, v));
    CuAssertIntEquals(tc, 5, (int)v[0]);
    CuAssertIntEquals(tc, 30, (int)v[1]);
    CuAssertIntEquals(tc, 0, (int)v[2]);
    CuAssertIntEquals(tc, 1, parse_colon_triple("07", max, v));
    CuAssertIntEquals(tc, 7, (int)v[0]);
    CuAssertIntEquals(tc, 0, (int)v[1]);

    // 位数装不下 int：sscanf 的 %d 在这里是未定义行为且照样返 3，本函数必须拒
    CuAssertIntEquals(tc, 0, parse_colon_triple("99999999999:00:00", max, v));
    // 逐段上界
    CuAssertIntEquals(tc, 0, parse_colon_triple("839:00:00", max, v));
    CuAssertIntEquals(tc, 0, parse_colon_triple("01:60:00", max, v));
    CuAssertIntEquals(tc, 0, parse_colon_triple("01:00:60", max, v));
    // 首段非数字
    CuAssertIntEquals(tc, 0, parse_colon_triple("", max, v));
    CuAssertIntEquals(tc, 0, parse_colon_triple(":01:02", max, v));
    CuAssertIntEquals(tc, 0, parse_colon_triple("x:01", max, v));
    // 冒号后无数字：前面已成功的段照数，不算失败
    CuAssertIntEquals(tc, 1, parse_colon_triple("09:", max, v));
    CuAssertIntEquals(tc, 9, (int)v[0]);
}
// prots_may_resume default 分支：无专属判定的协议返回 ERR_OK。
// PACK_PGSQL / PACK_MQTT / PACK_WEBSOCK 各有自己的判定，不在此列
static void test_prots_may_resume_default(CuTest *tc) {
    pack_type defaults[] = { PACK_NONE, PACK_DNS, PACK_HTTP, PACK_SMTP,
                             PACK_REDIS, PACK_MYSQL, PACK_MONGO, PACK_UDP_KCP };
    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
        CuAssertIntEquals(tc, ERR_OK, prots_may_resume(defaults[i], NULL));
    }
    // data 为 NULL 时无包可拦，带判定的三个协议一律放行
    CuAssertIntEquals(tc, ERR_OK, prots_may_resume(PACK_PGSQL, NULL));
    CuAssertIntEquals(tc, ERR_OK, prots_may_resume(PACK_MQTT, NULL));
    CuAssertIntEquals(tc, ERR_OK, prots_may_resume(PACK_WEBSOCK, NULL));
}

// WS 承载 MQTT 时 prots_may_resume 必须按内层包判定：broker 主动推的 PUBLISH 不得唤醒
// 等待者，SUBACK 可以。直连 PACK_MQTT 与 WS 承载两条路径的结论必须一致
static void test_prots_may_resume_websock_mqtt(CuTest *tc) {
    mqtt_ctx mctx = { MQTT_311 };
    ud_cxt subud;
    ZERO(&subud, sizeof(subud));
    subud.pktype = PACK_MQTT;
    subud.status = 1; // mqtt 内部 COMMAND 状态
    subud.context = &mctx;
    buffer_ctx subbuf;
    buffer_init(&subbuf);

    websock_ctx ws;
    ZERO(&ws, sizeof(ws));
    ws.secprot = PACK_MQTT;
    ws.buf = &subbuf;
    ws.ud = &subud;

    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = 1; // websock 内部 START 状态
    ud.context = &ws;

    size_t mlen = 0, flen = 0;
    void *frame;
    char *mpack;
    buffer_ctx buf;
    int32_t status;
    struct websock_pack_ctx *pack;
    uint8_t reasons[1] = { 0 };

    // 用例1：PUBLISH 包在 WS 帧里 → 不可唤醒等待者
    mpack = mqtt_pack_publish(MQTT_311, 0, 0, 0, "t", 0, "p", 1, NULL, &mlen);
    CuAssertPtrNotNull(tc, mpack);
    frame = websock_pack_binary(0, 1, mpack, mlen, &flen);
    FREE(mpack);
    CuAssertPtrNotNull(tc, frame);
    buffer_init(&buf);
    buffer_append(&buf, frame, flen);
    FREE(frame);
    status = PROT_INIT;
    pack = _t_websock_unpack(1, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertIntEquals(tc, PACK_MQTT, websock_secprot(pack));
    CuAssertIntEquals(tc, ERR_FAILED, prots_may_resume(PACK_WEBSOCK, pack));
    _websock_pkfree(pack);
    buffer_free(&buf);

    // 用例2：同一条连接上的 SUBACK → 可唤醒
    mpack = mqtt_pack_suback(MQTT_311, 1, reasons, sizeof(reasons), NULL, &mlen);
    CuAssertPtrNotNull(tc, mpack);
    frame = websock_pack_binary(0, 1, mpack, mlen, &flen);
    FREE(mpack);
    CuAssertPtrNotNull(tc, frame);
    buffer_init(&buf);
    buffer_append(&buf, frame, flen);
    FREE(frame);
    status = PROT_INIT;
    pack = _t_websock_unpack(1, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertIntEquals(tc, ERR_OK, prots_may_resume(PACK_WEBSOCK, pack));
    _websock_pkfree(pack);
    buffer_free(&buf);

    buffer_free(&subbuf);
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
    CuAssertTrue(tc, NULL != strstr(pkt, "Subject: subject"));
    /* BCC 只走信封 RCPT TO，绝不进头部（mail.c:_mail_pack_addr）——
       密送名单泄给全体收件人就是这条断言在拦 */
    CuAssertTrue(tc, NULL == strstr(pkt, "bcc@example.com"));
    CuAssertTrue(tc, NULL == strstr(pkt, "Bcc:"));
    /* To / Cc 照常入头部，用来证明上面两条不是因为整个地址段都没写出来才过的 */
    CuAssertTrue(tc, NULL != strstr(pkt, "rcpt@example.com"));
    CuAssertTrue(tc, NULL != strstr(pkt, "cc@example.com"));
    FREE(pkt);

    /* mail_addrs_clear 后地址数归零 */
    CuAssertTrue(tc, 3 == maddr_arr_size(&mail.addrs));
    mail_addrs_clear(&mail);
    CuAssertTrue(tc, 0 == maddr_arr_size(&mail.addrs));

    /* mail_attach_clear 在空附件下安全 */
    mail_attach_clear(&mail);
    CuAssertTrue(tc, 0 == mattach_arr_size(&mail.attach));

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
    CuAssertTrue(tc, 0 == maddr_arr_size(&mail.addrs));

    mail_free(&mail);
}

// smtp_init 超长必须整体拒绝而不是让 safe_fill_str 悄悄截断：psw 只有 64 字节，
// OAuth token 之类轻松超过，截断后拿去认证只换回服务端一句 535，调用方看不出是自己传长了。
// 同 mysql_init / pgsql_init / mongo_init 的口径
static void test_smtp_init_bounds(CuTest *tc) {
    smtp_ctx smtp;
    char toolong[IP_LENS + 32];
    memset(toolong, 'x', sizeof(toolong) - 1);
    toolong[sizeof(toolong) - 1] = '\0';

    // 合法参数
    CuAssertIntEquals(tc, ERR_OK, smtp_init(&smtp, "127.0.0.1", 25, NULL, "user", "psw"));
    CuAssertTrue(tc, 0 == strcmp(smtp.ip, "127.0.0.1"));
    CuAssertTrue(tc, 0 == strcmp(smtp.user, "user"));
    CuAssertTrue(tc, 0 == strcmp(smtp.psw, "psw"));

    // 三个字段各自超长都要被拒。失败时 smtp 已被 ZERO 且可能填了前几个字段，
    // 按 init 失败处理即可，不保证"原样不动"——所以这里只验返回值
    CuAssertIntEquals(tc, ERR_FAILED, smtp_init(&smtp, "127.0.0.1", 25, NULL, "user", toolong));
    CuAssertIntEquals(tc, ERR_FAILED, smtp_init(&smtp, "127.0.0.1", 25, NULL, toolong, "psw"));
    CuAssertIntEquals(tc, ERR_FAILED, smtp_init(&smtp, toolong, 25, NULL, "user", "psw"));

    // 边界：正好填满（容量 - 1）仍然合法。长度从 smtp_ctx 的字段现算，
    // 写死 64/65 的话字段扩容后这两条就悄悄测错了位置
    char just[sizeof(smtp.psw)];
    memset(just, 'y', sizeof(just) - 1);
    just[sizeof(just) - 1] = '\0';
    CuAssertIntEquals(tc, ERR_OK, smtp_init(&smtp, "127.0.0.1", 25, NULL, "user", just));
    CuAssertTrue(tc, 0 == strcmp(smtp.psw, just));
    // 再多一个字节就越界
    char over[sizeof(smtp.psw) + 1];
    memset(over, 'y', sizeof(over) - 1);
    over[sizeof(over) - 1] = '\0';
    CuAssertIntEquals(tc, ERR_FAILED, smtp_init(&smtp, "127.0.0.1", 25, NULL, "user", over));
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

    /* 前缀比较：给几位就比几位，"45" 会命中 450（注释原来写反了，说 45 不该匹配） */
    safe_fill_str(pack, sizeof(pack), "450 Mailbox unavailable");
    CuAssertIntEquals(tc, ERR_OK,     smtp_check_code(pack, "450"));
    CuAssertIntEquals(tc, ERR_OK,     smtp_check_code(pack, "45"));
    CuAssertIntEquals(tc, ERR_FAILED, smtp_check_code(pack, "451"));

    /* 任意自定义 code */
    safe_fill_str(pack, sizeof(pack), "354 Start input");
    CuAssertIntEquals(tc, ERR_OK,     smtp_check_code(pack, "354"));
}

/* smtp_check_codes 多码匹配：RCPT TO 的 250/251 都算成功（RFC 5321 §4.3.2） */
static void test_smtp_check_codes(CuTest *tc) {
    char pack[64];
    static const char *const rcpt[] = { "250", "251" };
    static const char *const one[] = { "250" };

    /* 命中数组首个 */
    safe_fill_str(pack, sizeof(pack), "250 OK");
    CuAssertIntEquals(tc, ERR_OK, smtp_check_codes(pack, rcpt, ARRAY_SIZE(rcpt)));

    /* 命中数组末个：251 表示已接受但将转发，单判 250 会误报失败 */
    safe_fill_str(pack, sizeof(pack), "251 User not local; will forward");
    CuAssertIntEquals(tc, ERR_OK,     smtp_check_codes(pack, rcpt, ARRAY_SIZE(rcpt)));
    CuAssertIntEquals(tc, ERR_FAILED, smtp_check_codes(pack, one, ARRAY_SIZE(one)));
    CuAssertIntEquals(tc, ERR_FAILED, smtp_check_ok(pack));

    /* 全不中 */
    safe_fill_str(pack, sizeof(pack), "550 No such user");
    CuAssertIntEquals(tc, ERR_FAILED, smtp_check_codes(pack, rcpt, ARRAY_SIZE(rcpt)));

    /* ncode 为 0：无码可比，恒失败 */
    safe_fill_str(pack, sizeof(pack), "250 OK");
    CuAssertIntEquals(tc, ERR_FAILED, smtp_check_codes(pack, rcpt, 0));

    /* smtp_check_code 已收敛为 ncode==1 的 smtp_check_codes，两者结果须一致 */
    safe_fill_str(pack, sizeof(pack), "221 Bye");
    CuAssertIntEquals(tc, ERR_OK, smtp_check_code(pack, "221"));
    CuAssertIntEquals(tc, ERR_OK, smtp_check_codes(pack, (const char *const[]){ "221" }, 1));
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
    pack = _t_smtp_unpack(0, &buf, &ud, &size, &status);
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
    pack = _t_smtp_unpack(0, &buf, &ud, &size, &status);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_MOREDATA));
    buffer_free(&buf);

    /* COMMAND + 5+ 字节但缺 CRLF → PROT_MOREDATA */
    buffer_init(&buf);
    ZERO(&ud, sizeof(ud));
    ud.status = 4;
    buffer_append(&buf, "250 OK", 6);
    size = 0; status = PROT_INIT;
    pack = _t_smtp_unpack(0, &buf, &ud, &size, &status);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_MOREDATA));
    buffer_free(&buf);

    /* COMMAND + 多行响应：一次性消费整段，返回完整内容（含内嵌 CRLF、不含末尾 CRLF），buffer 清空 */
    buffer_init(&buf);
    ZERO(&ud, sizeof(ud));
    ud.status = 4;
    buffer_append(&buf, "250-First\r\n250 End\r\n", 20);
    size = 0; status = PROT_INIT;
    pack = _t_smtp_unpack(0, &buf, &ud, &size, &status);
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
    pack = _t_smtp_unpack(0, &buf, &ud, &size, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertStrEquals(tc, "250-A\r\n250 B", pack);
    FREE(pack);
    pack = _t_smtp_unpack(0, &buf, &ud, &size, &status);
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
    pack = _t_smtp_unpack(0, &buf, &ud, &size, &status);
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
    pack = _t_smtp_unpack(0, &buf, &ud, &size, &status);
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

    struct http_pack_ctx *pack = _t_http_unpack(0, &buf, &ud, NULL, &status);
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

    /* 注：hdr_arr_at 在 pos 越界时 ASSERTAB abort，并不返回 NULL；
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
        0x82,/* FIN=1, opcode=0x2 (binary) */
        0x84,/* MASK=1, len=4 */
        0x00, 0x00, 0x00, 0x00, /* mask key 全 0 */
        0xde, 0xad, 0xbe, 0xef/* payload（xor 全 0 等于原文） */
    };

    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, frame, sizeof(frame));

    websock_ctx ws;
    ud_cxt ud;
    _ws_ctx_init(&ws, &ud);

    int32_t status = PROT_INIT;
    struct websock_pack_ctx *pack = _t_websock_unpack(0 /* server */, &buf, &ud, NULL, &status);
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

// 一个 WS BINARY 帧内含 2 个完整 MQTT PINGREQ 包,验证链表机制把两个包都投递出来
// 而不是只返回第一个、第二个丢在 ws->buf 里(回归 lib/protocol/websock.c _websock_sec_mqtt 修复)
static void test_websock_mqtt_multipack(CuTest *tc) {
    size_t p1size, p2size;
    char *p1 = mqtt_pack_ping(&p1size);
    char *p2 = mqtt_pack_ping(&p2size);
    CuAssertPtrNotNull(tc, p1);
    CuAssertPtrNotNull(tc, p2);

    char payload[32];
    memcpy(payload, p1, p1size);
    memcpy(payload + p1size, p2, p2size);
    size_t plens = p1size + p2size;
    FREE(p1);
    FREE(p2);

    size_t fsize;
    void *frame = websock_pack_binary(1 /* mask,server 视角要求 */, 1 /* fin */, payload, plens, &fsize);
    CuAssertPtrNotNull(tc, frame);

    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, frame, fsize);
    FREE(frame);

    mqtt_ctx mctx = { MQTT_311 };
    ud_cxt mqtt_ud;
    ZERO(&mqtt_ud, sizeof(mqtt_ud));
    mqtt_ud.status = 1; /* mqtt 内部 COMMAND 状态,跳过 CONNECT 前置直接测命令阶段 */
    mqtt_ud.context = &mctx;

    buffer_ctx subbuf;
    buffer_init(&subbuf);

    websock_ctx ws;
    ZERO(&ws, sizeof(ws));
    ws.secprot = PACK_MQTT;
    ws.ud = &mqtt_ud;
    ws.buf = &subbuf;

    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = 1; /* websock 内部 START 状态 */
    ud.context = &ws;

    int32_t status = PROT_INIT;
    struct websock_pack_ctx *pack1 = _t_websock_unpack(0 /* server */, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack1);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertIntEquals(tc, PACK_MQTT, websock_secprot(pack1));

    mqtt_pack_ctx *mp1 = (mqtt_pack_ctx *)websock_secpack(pack1);
    CuAssertPtrNotNull(tc, mp1);
    CuAssertIntEquals(tc, MQTT_PINGREQ, mp1->fixhead.prot);

    /* 关键验证:第二个完整包必须能通过链表继续取到,而不是永久积压在 ws->buf 里 */
    struct websock_pack_ctx *pack2 = _websock_pack_next(pack1);
    CuAssertPtrNotNull(tc, pack2);
    mqtt_pack_ctx *mp2 = (mqtt_pack_ctx *)websock_secpack(pack2);
    CuAssertPtrNotNull(tc, mp2);
    CuAssertIntEquals(tc, MQTT_PINGREQ, mp2->fixhead.prot);

    /* 没有第三个包 */
    CuAssertPtrEquals(tc, NULL, _websock_pack_next(pack2));

    _websock_pkfree(pack1);
    _websock_pkfree(pack2);
    buffer_free(&subbuf);
    buffer_free(&buf);
}

// WS 层真分片(fin=0 起始帧 + fin=1 CONTINUE 结束帧)承载子协议完整消息时,
// 两帧各自的 WS 分片状态位都不应残留到 status 上(回归 lib/protocol/websock.c
// _websock_parse_data 修复:子协议吐出的是完整消息,与承载它的 WS 帧自身是否分片无关)
static void test_websock_mqtt_ws_fragment_slice_clear(CuTest *tc) {
    size_t p1size, p2size;
    char *p1 = mqtt_pack_ping(&p1size);
    char *p2 = mqtt_pack_ping(&p2size);
    CuAssertPtrNotNull(tc, p1);
    CuAssertPtrNotNull(tc, p2);

    size_t f1size, f2size;
    // 起始帧:fin=0 opcode=BINARY,承载一个完整 MQTT PINGREQ → 修复前 status 残留 PROT_SLICE_START
    void *f1 = websock_pack_binary(1, 0, p1, p1size, &f1size);
    // 结束帧:fin=1 opcode=CONTINUE,承载另一个完整 MQTT PINGREQ → 修复前 status 残留 PROT_SLICE_END
    void *f2 = websock_pack_continua(1, 1, p2, p2size, &f2size);
    CuAssertPtrNotNull(tc, f1);
    CuAssertPtrNotNull(tc, f2);
    FREE(p1);
    FREE(p2);

    mqtt_ctx mctx = { MQTT_311 };
    ud_cxt mqtt_ud;
    ZERO(&mqtt_ud, sizeof(mqtt_ud));
    mqtt_ud.status = 1;
    mqtt_ud.context = &mctx;

    buffer_ctx subbuf;
    buffer_init(&subbuf);

    websock_ctx ws;
    ZERO(&ws, sizeof(ws));
    ws.secprot = PACK_MQTT;
    ws.ud = &mqtt_ud;
    ws.buf = &subbuf;

    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = 1; /* websock 内部 START 状态 */
    ud.context = &ws;

    buffer_ctx buf;
    buffer_init(&buf);

    buffer_append(&buf, f1, f1size);
    FREE(f1);
    int32_t status = PROT_INIT;
    struct websock_pack_ctx *pack1 = _t_websock_unpack(0 /* server */, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack1);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_SLICE_START));
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_SLICE));
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_SLICE_END));
    mqtt_pack_ctx *mp1 = (mqtt_pack_ctx *)websock_secpack(pack1);
    CuAssertPtrNotNull(tc, mp1);
    CuAssertIntEquals(tc, MQTT_PINGREQ, mp1->fixhead.prot);
    _websock_pkfree(pack1);

    buffer_append(&buf, f2, f2size);
    FREE(f2);
    status = PROT_INIT;
    struct websock_pack_ctx *pack2 = _t_websock_unpack(0 /* server */, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack2);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_SLICE_START));
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_SLICE));
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_SLICE_END));
    mqtt_pack_ctx *mp2 = (mqtt_pack_ctx *)websock_secpack(pack2);
    CuAssertPtrNotNull(tc, mp2);
    CuAssertIntEquals(tc, MQTT_PINGREQ, mp2->fixhead.prot);
    _websock_pkfree(pack2);

    buffer_free(&subbuf);
    buffer_free(&buf);
}

// 手工构造 payloadlen=127 + 8 字节大端长度的 64-bit 扩展长度帧
// 覆盖 _websock_parse_payloadlen 中 payloadlen==127 分支
//   1) 合法 100 字节 payload 通过 ntohll 正确解析
//   2) 长度 65536（WS_MAX_PAYLOAD_LENS + 1）被拒
//   3) 长度 SIZE_MAX-15 被拒（上限那道在回绕之前就挡住了）
//   4) 长度恰等于 WS_MAX_PAYLOAD_LENS 必须放行 —— 上限是 > 判定，等于不算超
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

    websock_ctx ws;
    ud_cxt ud;
    _ws_ctx_init(&ws, &ud);
    int32_t status = PROT_INIT;
    // client=1：客户端接收服务端帧，服务端帧允许 mask=0
    struct websock_pack_ctx *pack = _t_websock_unpack(1, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertIntEquals(tc, WS_BINARY, websock_prot(pack));
    size_t dlen = 0;
    char *data = websock_data(pack, &dlen);
    CuAssertIntEquals(tc, (int)sizeof(payload), (int)dlen);
    CuAssertTrue(tc, 0 == memcmp(data, payload, sizeof(payload)));
    _websock_pkfree(pack);
    buffer_free(&buf);

    // 子用例 2：payloadlen=127 + 长度=65536，超过 WS_MAX_PAYLOAD_LENS → PROT_ERROR
    unsigned char head2[10] = {
        0x82,
        0x7f,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00 // 大端 64-bit = 65536
    };
    buffer_ctx buf2;
    buffer_init(&buf2);
    buffer_append(&buf2, head2, sizeof(head2));

    websock_ctx ws2;
    ud_cxt ud2;
    _ws_ctx_init(&ws2, &ud2);
    int32_t status2 = PROT_INIT;
    struct websock_pack_ctx *pack2 = _t_websock_unpack(1, &buf2, &ud2, NULL, &status2);
    CuAssertTrue(tc, NULL == pack2);
    CuAssertTrue(tc, BIT_CHECK(status2, PROT_ERROR));
    buffer_free(&buf2);

    // 子用例 3：长度取 SIZE_MAX-15，回绕后 sizeof(pack)+dlens 会变成一个小值。
    // WS_MAX_PAYLOAD_LENS 那道在回绕发生之前就挡住了它，本用例钉住"必须拒绝"这个结果
    unsigned char head3[10] = {
        0x82,
        0x7f,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xf0
    };
    buffer_ctx buf3;
    buffer_init(&buf3);
    buffer_append(&buf3, head3, sizeof(head3));

    websock_ctx ws3;
    ud_cxt ud3;
    _ws_ctx_init(&ws3, &ud3);
    int32_t status3 = PROT_INIT;
    struct websock_pack_ctx *pack3 = _t_websock_unpack(1, &buf3, &ud3, NULL, &status3);
    CuAssertTrue(tc, NULL == pack3);
    CuAssertTrue(tc, BIT_CHECK(status3, PROT_ERROR));
    buffer_free(&buf3);

    // 子用例 4：长度恰等于 WS_MAX_PAYLOAD_LENS 必须放行。守卫是 > 不是 >=，
    // 少了这条，把它改成 >= 之后上面三条照样全过
    unsigned char head4[10] = {
        0x82,
        0x7f,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff // 大端 64-bit = 65535
    };
    buffer_ctx buf4;
    buffer_init(&buf4);
    buffer_append(&buf4, head4, sizeof(head4));
    char *big;
    MALLOC(big, WS_MAX_PAYLOAD_LENS);
    memset(big, 0x5a, WS_MAX_PAYLOAD_LENS);
    buffer_append(&buf4, big, WS_MAX_PAYLOAD_LENS);

    websock_ctx ws4;
    ud_cxt ud4;
    _ws_ctx_init(&ws4, &ud4);
    int32_t status4 = PROT_INIT;
    struct websock_pack_ctx *pack4 = _t_websock_unpack(1, &buf4, &ud4, NULL, &status4);
    CuAssertPtrNotNull(tc, pack4);
    CuAssertTrue(tc, !BIT_CHECK(status4, PROT_ERROR));
    size_t dlen4 = 0;
    char *data4 = websock_data(pack4, &dlen4);
    CuAssertIntEquals(tc, WS_MAX_PAYLOAD_LENS, (int)dlen4);
    CuAssertTrue(tc, 0 == memcmp(data4, big, WS_MAX_PAYLOAD_LENS));
    _websock_pkfree(pack4);
    FREE(big);
    buffer_free(&buf4);
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
    size_t nwrite = fwrite(payload, 1, plen, fp);
    fclose(fp);
    CuAssertTrue(tc, plen == nwrite);

    // 2. 构造 mail：含正文 + 附件
    mail_ctx mail;
    mail_init(&mail);
    mail_from(&mail, "Alice", "alice@example.com");
    mail_addrs_add(&mail, "bob@example.com", TO);
    mail_addrs_add(&mail, "carol@example.com", CC);
    mail_subject(&mail, "with-attach");
    mail_msg(&mail, "see attach");
    mail_attach_add(&mail, tmpfile);
    // 先收拾再断言：CuAssert 失败走 longjmp，夹在中间会漏掉 mail / pkt / pkt2 与临时文件，
    // 一次真失败还要在收尾内存检查里多报几笔假泄漏。全部结论先落成局部量，函数尾统一断言
    int32_t natt1 = (int32_t)mattach_arr_size(&mail.attach);

    // 3. 附件结构字段：extension 取自文件名最后 '.'，file 仅含文件名（不含目录）
    // 空数组时 mattach_arr_at 走 ASSERTAB 直接 abort，整轮跑连收尾汇总都没了，故先按 natt1 取
    mail_attach *att = (1 == natt1) ? mattach_arr_at(&mail.attach, 0) : NULL;
    int32_t ext_ok = (NULL != att && 0 == strcmp(att->extension, ".txt"));
    int32_t file_ok = (NULL != att && NULL != strstr(att->file, "test_mail_attach.txt"));
    int32_t content_ok = (NULL != att && NULL != att->content && strlen(att->content) > 0);

    // 4. mail_pack：含 multipart/mixed boundary + 附件 header + base64 内容
    char *pkt = mail_pack(&mail);
    int32_t pkt_ok = (NULL != pkt);
    int32_t mime_ok = 0, mixed_ok = 0, ctype_ok = 0, cte_ok = 0;
    int32_t cdisp_ok = 0, fname_ok = 0, b64_ok = 0, term_ok = 0;
    if (pkt_ok) {
        mime_ok = (NULL != strstr(pkt, "MIME-Version: 1.0"));
        mixed_ok = (NULL != strstr(pkt, "multipart/mixed"));
        ctype_ok = (NULL != strstr(pkt, "Content-Type: text/plain"));
        cte_ok = (NULL != strstr(pkt, "Content-Transfer-Encoding: base64"));
        cdisp_ok = (NULL != strstr(pkt, "Content-Disposition: attachment; filename=\""));
        // 附件文件名出现在 Content-Disposition 行
        fname_ok = (NULL != strstr(pkt, "test_mail_attach.txt"));
        // base64 编码后的附件 content 应在 pkt 中（注意不能用 strlen 验证原文，二进制含 \0）
        b64_ok = (content_ok && NULL != strstr(pkt, att->content));
        // 邮件以 "\r\n.\r\n" 终止（DATA body 终止序列）
        term_ok = (NULL != strstr(pkt, "\r\n.\r\n"));
    }
    FREE(pkt);

    // 5. mail_attach_clear：附件数组清空，内部 content 释放
    mail_attach_clear(&mail);
    int32_t natt2 = (int32_t)mattach_arr_size(&mail.attach);

    // 6. 多附件场景：插入两个，验证 mail_pack 不崩，含两段 base64
    mail_attach_add(&mail, tmpfile);
    mail_attach_add(&mail, tmpfile);
    int32_t natt3 = (int32_t)mattach_arr_size(&mail.attach);
    char *pkt2 = mail_pack(&mail);
    int32_t pkt2_ok = (NULL != pkt2);
    // 两个附件的同名 filename 至少出现 2 次（Content-Disposition 各一次）
    int32_t filename_hits = 0;
    const char *p;
    if (pkt2_ok) {
        for (p = pkt2; NULL != (p = strstr(p, "test_mail_attach.txt")); p++) {
            filename_hits++;
        }
    }
    FREE(pkt2);
    mail_free(&mail);
    remove(tmpfile);

    CuAssertIntEquals(tc, 1, natt1);
    CuAssertTrue(tc, 0 != ext_ok);
    CuAssertTrue(tc, 0 != file_ok);
    CuAssertTrue(tc, 0 != content_ok);
    CuAssertTrue(tc, 0 != pkt_ok);
    CuAssertTrue(tc, 0 != mime_ok);
    CuAssertTrue(tc, 0 != mixed_ok);
    CuAssertTrue(tc, 0 != ctype_ok);
    CuAssertTrue(tc, 0 != cte_ok);
    CuAssertTrue(tc, 0 != cdisp_ok);
    CuAssertTrue(tc, 0 != fname_ok);
    CuAssertTrue(tc, 0 != b64_ok);
    CuAssertTrue(tc, 0 != term_ok);
    CuAssertIntEquals(tc, 0, natt2);
    CuAssertIntEquals(tc, 2, natt3);
    CuAssertTrue(tc, 0 != pkt2_ok);
    CuAssertTrue(tc, filename_hits >= 2);
}

// 附件名含非 ASCII：RFC 5322 §2.2 只许头字段体是 US-ASCII，而 smtp.c 从不协商 SMTPUTF8，
// 裸 UTF-8 名到严格 MTA 上会被拒或改写。主题走的 RFC 2047 encoded-word 在这里用不了——
// §5 明确禁止它出现在 quoted-string 内，参数值只能按 RFC 2231 编成 filename*=UTF-8''
static void test_mail_attach_name_rfc2231(CuTest *tc) {
    const char *dir = procpath();
    char tmpfile[PATH_LENS];
    SNPRINTF(tmpfile, sizeof(tmpfile), "%s%stest_mail_2231.txt", dir, PATH_SEPARATORSTR);
    FILE *fp = fopen(tmpfile, "wb");
    CuAssertPtrNotNull(tc, fp);
    size_t nwrite = fwrite("x", 1, 1, fp);
    fclose(fp);

    mail_ctx mail;
    mail_init(&mail);
    mail_from(&mail, NULL, "alice@example.com");
    mail_addrs_add(&mail, "bob@example.com", TO);
    mail_subject(&mail, "attach");
    mail_msg(&mail, "body");
    mail_attach_add(&mail, tmpfile);
    int32_t natt = (int32_t)mattach_arr_size(&mail.attach);

    // 直接改 att->file，不去造一个 UTF-8 文件名的真文件：磁盘文件名编码各平台不同，
    // 这里要测的只是组包侧怎么写这个字段。"报表.pdf"
    mail_attach *att = (1 == natt) ? mattach_arr_at(&mail.attach, 0) : NULL;
    if (NULL != att) {
        SNPRINTF(att->file, sizeof(att->file), "%s", "\xe6\x8a\xa5\xe8\xa1\xa8.pdf");
    }
    char *pkt = mail_pack(&mail);
    int32_t pkt_ok = (NULL != pkt);
    int32_t ext_ok = 0, raw_ok = 0, ascii_ok = 1;
    const char *q;
    if (pkt_ok) {
        // name*= 要带上前导 tab 才验得到 Content-Type 那行——不带的话
        // 这个子串被下面 filename*= 那条整个包含，等于白判
        ext_ok = (NULL != strstr(pkt, "Content-Disposition: attachment; filename*=utf-8''%E6%8A%A5%E8%A1%A8.pdf")
            && NULL != strstr(pkt, "\tname*=utf-8''%E6%8A%A5%E8%A1%A8.pdf"));
        raw_ok = (NULL == strstr(pkt, "\xe6\x8a\xa5\xe8\xa1\xa8.pdf"));
        // 主题走 encoded-word、正文与附件走 base64，所以整封信应当逐字节都是 7bit
        for (q = pkt; '\0' != *q; q++) {
            if (0 != (0x80 & (unsigned char)*q)) {
                ascii_ok = 0;
                break;
            }
        }
    }
    FREE(pkt);

    // 纯 ASCII 名不该被一起编码，仍走引号形式
    if (NULL != att) {
        SNPRINTF(att->file, sizeof(att->file), "%s", "plain.pdf");
    }
    pkt = mail_pack(&mail);
    int32_t quoted_ok = (NULL != pkt
        && NULL != strstr(pkt, "Content-Disposition: attachment; filename=\"plain.pdf\"")
        && NULL == strstr(pkt, "filename*="));
    FREE(pkt);
    mail_free(&mail);
    remove(tmpfile);

    CuAssertTrue(tc, 1 == nwrite);
    CuAssertIntEquals(tc, 1, natt);
    CuAssertTrue(tc, 0 != pkt_ok);
    CuAssertTrue(tc, 0 != ext_ok);
    CuAssertTrue(tc, 0 != raw_ok);
    CuAssertTrue(tc, 0 != ascii_ok);
    CuAssertTrue(tc, 0 != quoted_ok);
}

// 长 UTF-8 附件名不能被截断：APFS/NTFS 按字符限 255，中文名可达 765 字节，
// 截在 255 字节会丢掉扩展名、末尾剩半个字符。ext4 按字节限 255 建不出这种文件，建不出就跳过真文件那段
static void test_mail_attach_long_name(CuTest *tc) {
    // 装得下最长的合法 basename 再加 NUL
    CuAssertTrue(tc, sizeof(((mail_attach *)0)->file) > 765);

    char name[512];
    size_t off = 0;
    int32_t i;
    for (i = 0; i < 100; i++) {// 100 个"报"= 300 字节
        memcpy(name + off, "\xe6\x8a\xa5", 3);
        off += 3;
    }
    memcpy(name + off, ".pdf", sizeof(".pdf"));
    char tmpfile[PATH_LENS];
    SNPRINTF(tmpfile, sizeof(tmpfile), "%s%s%s", procpath(), PATH_SEPARATORSTR, name);
    FILE *fp = fopen(tmpfile, "wb");
    if (NULL == fp) {
        return;
    }
    fwrite("x", 1, 1, fp);
    fclose(fp);

    mail_ctx mail;
    mail_init(&mail);
    mail_attach_add(&mail, tmpfile);
    int32_t natt = (int32_t)mattach_arr_size(&mail.attach);
    mail_attach *att = (1 == natt) ? mattach_arr_at(&mail.attach, 0) : NULL;
    int32_t file_ok = (NULL != att && 0 == strcmp(att->file, name));
    int32_t ext_ok = (NULL != att && 0 == strcmp(att->extension, ".pdf"));
    mail_free(&mail);
    remove(tmpfile);

    CuAssertIntEquals(tc, 1, natt);
    CuAssertTrue(tc, 0 != file_ok);
    CuAssertTrue(tc, 0 != ext_ok);
}

// 握手回传桩，_smtp_ud_setup 每个用例都装一次。_smtp_connected 与 _smtp_auth_check 在被
// 服务端拒绝时会把原文推给上层，而纯解析测试没起 loader，不装桩这条路没人收载荷。
// 生产路径由 task 接管并释放，这里桩自己收，免得 MEMORY_CHECK 报泄漏
static int32_t g_smtp_hs_calls;
static int32_t g_smtp_hs_erro;
static char g_smtp_hs_msg[128];
static int32_t _stub_smtp_hspush(sock_ctx *sk, int32_t client,
                                 ud_cxt *ud, int32_t erro, void *data, size_t lens) {
    (void)sk;
    (void)client;
    (void)ud;
    g_smtp_hs_calls++;
    g_smtp_hs_erro = erro;
    g_smtp_hs_msg[0] = '\0';
    if (NULL != data && lens < sizeof(g_smtp_hs_msg)) {
        memcpy(g_smtp_hs_msg, data, lens);
        g_smtp_hs_msg[lens] = '\0';
    }
    FREE(data);
    return ERR_OK;
}
static void _smtp_ud_setup(smtp_ctx *smtp, ud_cxt *ud, int32_t state) {
    _smtp_init(_stub_smtp_hspush);
    g_smtp_hs_calls = 0;
    g_smtp_hs_erro = ERR_OK;
    g_smtp_hs_msg[0] = '\0';
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
        void *pack = _t_smtp_unpack(0, &buf, &ud, &size, &status);
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
        _t_smtp_unpack(0, &buf, &ud, &size, &status);
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
        _t_smtp_unpack(0, &buf, &ud, &size, &status);
        CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
        CuAssertIntEquals(tc, _SMTP_INIT, ud.status);
        // 拒绝原文必须带回给等待者，而不是随连接一起丢掉
        CuAssertIntEquals(tc, 1, g_smtp_hs_calls);
        CuAssertIntEquals(tc, ERR_FAILED, g_smtp_hs_erro);
        CuAssertStrEquals(tc, "421 service unavailable", g_smtp_hs_msg);
        buffer_free(&buf);
    }
    // 拒绝行还没收全(无 CRLF)时不该硬凑：判 MOREDATA，也不推半截原文
    {
        smtp_ctx smtp;
        ud_cxt ud;
        _smtp_ud_setup(&smtp, &ud, _SMTP_INIT);
        buffer_ctx buf;
        buffer_init(&buf);
        _bput(&buf, "421 service");
        int32_t status = PROT_INIT;
        size_t size = 0;
        _t_smtp_unpack(0, &buf, &ud, &size, &status);
        CuAssertIntEquals(tc, 0, g_smtp_hs_calls);
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
        _t_smtp_unpack(0, &buf, &ud, &size, &status);
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
        _t_smtp_unpack(0, &buf, &ud, &size, &status);
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
        _t_smtp_unpack(0, &buf, &ud, &size, &status);
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
        _t_smtp_unpack(0, &buf, &ud, &size, &status);
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
        _t_smtp_unpack(0, &buf, &ud, &size, &status);
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
        _t_smtp_unpack(0, &buf, &ud, &size, &status);
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
    _t_smtp_unpack(0, &buf, &ud, &size, &status);
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
    _t_smtp_unpack(0, &buf, &ud, &size, &status);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_MOREDATA));
    CuAssertIntEquals(tc, _SMTP_AUTH, ud.status);
    buffer_free(&buf);
}

// 续行洪泛回归：AUTH / AUTH_CHECK / COMMAND 三态持续无 CRLF 时必须按 SMTP_MAX_PACK_LENS
// 上界拒绝（PROT_ERROR），而非无限 PROT_MOREDATA 累积耗内存
// 对齐 test_smtp_full_response case 20 对 INIT/EHLO 的洪泛防护
static void test_smtp_unpack_flood(CuTest *tc) {
    size_t floodlen = (size_t)SMTP_MAX_PACK_LENS + 16; // 超单条响应上界且不含任何 CRLF
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
        pack = _t_smtp_unpack(0, &buf, &ud, &size, &status);
        CuAssertTrue(tc, NULL == pack);
        CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
        buffer_free(&buf);
    }
    FREE(flood);
}

// MQTT 3.1.1 连接收到 AUTH(报文类型15，仅 MQTT 5.0 定义)→ 协议错误断连
static void test_mqtt_auth_version_gate(CuTest *tc) {
    mqtt_ctx mctx = { MQTT_311 };
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = 1; // COMMAND
    ud.context = &mctx;
    buffer_ctx buf;
    buffer_init(&buf);
    char auth[] = { (char)0xF0, 0x00 }; // AUTH type=15 flags=0 remaining=0
    buffer_append(&buf, auth, sizeof(auth));
    int32_t status = PROT_INIT;
    mqtt_pack_ctx *pack = _t_mqtt_unpack(0, &buf, &ud, NULL, &status);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
    buffer_free(&buf);
}
// MQTT-1.5.4-2：UTF-8 字符串含 U+0000 即非法报文。解析后 clientid 只剩 char*、长度不再保留，
// 放过去 "victim\0evil" 会在 Lua 侧塌缩成 "victim"，与合法客户端的 clientid 撞成同一个
static void test_mqtt_utf8_embedded_nul(CuTest *tc) {
    /* 变长头: "MQTT" + 级别4 + 标志0x02(clean) + keepalive 60 */
    char nulid[] = {
        (char)0x10, 0x17, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, 0x02, 0x00, 0x3C,
        0x00, 0x0B, 'v', 'i', 'c', 't', 'i', 'm', 0x00, 'e', 'v', 'i', 'l'
    };
    mqtt_ctx mctx = { MQTT_311 };
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = 0;// INIT：CONNECT 只在这个状态被受理，COMMAND 态一律落 default 返 NULL
    // context 留 NULL：_mqtt_connect 不读它，只在成功末尾覆写；预置个栈对象会被它当堆块释放
    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, nulid, sizeof(nulid));
    int32_t status = PROT_INIT;
    CuAssertTrue(tc, NULL == _t_mqtt_unpack(0, &buf, &ud, NULL, &status));
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
    buffer_free(&buf);

    /* 阳性对照：同长度但不含 NUL 的 clientid 照常解出，证明拒收不是因为报文别处畸形 */
    char okid[] = {
        (char)0x10, 0x16, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, 0x02, 0x00, 0x3C,
        0x00, 0x0A, 'v', 'i', 'c', 't', 'i', 'm', 'e', 'v', 'i', 'l'
    };
    ZERO(&ud, sizeof(ud));
    ud.status = 0;
    buffer_init(&buf);
    buffer_append(&buf, okid, sizeof(okid));
    status = PROT_INIT;
    mqtt_pack_ctx *pack = _t_mqtt_unpack(0, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    _mqtt_pkfree(pack);
    // CONNECT 解出来了，_mqtt_connect 末尾就会 CALLOC 一个 mqtt_ctx 挂到 ud->context 上，
    // 真实流程由连接 teardown 的 _mqtt_udfree 回收，这里得自己收
    _mqtt_udfree(&ud);
    buffer_free(&buf);

    /* PUBLISH 主题名不走 _mqtt_data_utf8（就地读进包内块），单独验它也拒内嵌 NUL。
       QoS 0 无报文标识符，剩余长度 = 2(主题长) + 8(主题) = 0x0A，载荷为空 */
    char nultopic[] = {
        (char)0x30, 0x0A, 0x00, 0x08, 'a', '/', 'b', 0x00, 'e', 'v', 'i', 'l'
    };
    ZERO(&ud, sizeof(ud));
    ud.status = 1;// COMMAND：PUBLISH 只在握手完成后受理，INIT 态会落 default
    ud.context = &mctx;
    buffer_init(&buf);
    buffer_append(&buf, nultopic, sizeof(nultopic));
    status = PROT_INIT;
    CuAssertTrue(tc, NULL == _t_mqtt_unpack(0, &buf, &ud, NULL, &status));
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
    buffer_free(&buf);

    /* 阳性对照：同结构、主题名不含 NUL，解出来的 topic 得是完整的 7 字节而非截断值 */
    char oktopic[] = {
        (char)0x30, 0x09, 0x00, 0x07, 'a', '/', 'b', 'e', 'v', 'i', 'l'
    };
    ZERO(&ud, sizeof(ud));
    ud.status = 1;
    ud.context = &mctx;
    buffer_init(&buf);
    buffer_append(&buf, oktopic, sizeof(oktopic));
    status = PROT_INIT;
    pack = _t_mqtt_unpack(0, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertStrEquals(tc, "a/bevil", ((mqtt_publish_varhead *)pack->varhead)->topic);
    _mqtt_pkfree(pack);
    // 这里 ud.context 自始至终是栈上的 mctx（PUBLISH 路径不换它），不能走 _mqtt_udfree
    buffer_free(&buf);
}
// MQTT 3.1.1：CONNECT 有密码无用户名(MQTT-3.1.2-22)打包必拒；同组合在 v5 合法
static void test_mqtt_connect_pwd_no_user(CuTest *tc) {
    size_t size = 0;
    char pwd[] = "pwd";
    char *pack = mqtt_pack_connect(MQTT_311, 1, 60, "cid", NULL, pwd, sizeof(pwd) - 1,
        NULL, NULL, 0, 0, 0, NULL, NULL, &size);
    CuAssertTrue(tc, NULL == pack);
    pack = mqtt_pack_connect(MQTT_50, 1, 60, "cid", NULL, pwd, sizeof(pwd) - 1,
        NULL, NULL, 0, 0, 0, NULL, NULL, &size);
    CuAssertPtrNotNull(tc, pack);
    FREE(pack);
}
// 失败一律 *lens 置 0(与 websock_pack_* 同口径): 绑定层把 lens 按值传给 lpub_rtn_lud,
// 那里的 NULL 判在函数体内, 实参先求值——被调方不写就是读未定值
static void test_mqtt_pack_lens_on_fail(CuTest *tc) {
    size_t size = 12345;
    // 失败点在函数体深处: 3.1.1 零长 clientid 配 cleanstart=0
    char *pack = mqtt_pack_connect(MQTT_311, 0, 60, "", NULL, NULL, 0,
        NULL, NULL, 0, 0, 0, NULL, NULL, &size);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, 0 == size);
    // 失败点是入口第一判: AUTH 仅 MQTT 5.0 定义
    size = 12345;
    pack = mqtt_pack_auth(MQTT_311, 0, NULL, &size);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, 0 == size);
    // 成功路径照常写真实长度, 守卫不能矫枉过正
    size = 0;
    pack = mqtt_pack_ping(&size);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, size > 0);
    FREE(pack);
}

/* ======================================================================= */

void test_protocol(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_http_head_nobody);
    SUITE_ADD_TEST(suite, test_http_tillclose);
    SUITE_ADD_TEST(suite, test_http_tillclose_request_only);
    SUITE_ADD_TEST(suite, test_http_response);
    SUITE_ADD_TEST(suite, test_http_pack_req);
    SUITE_ADD_TEST(suite, test_http_smuggling);
    SUITE_ADD_TEST(suite, test_http_status_line);
    SUITE_ADD_TEST(suite, test_http_nobody_status);
    SUITE_ADD_TEST(suite, test_http_chunked_size_smuggle);
    SUITE_ADD_TEST(suite, test_http_chunked_size_no_crlf_bound);
    SUITE_ADD_TEST(suite, test_http_chunked_ext);
    SUITE_ADD_TEST(suite, test_http_chunked_lens_bound);
    SUITE_ADD_TEST(suite, test_http_chunked_size_strict);
    SUITE_ADD_TEST(suite, test_http_check_keyval_token);
    SUITE_ADD_TEST(suite, test_http_chunked_trailer_limit);
    SUITE_ADD_TEST(suite, test_http_moredata);
    SUITE_ADD_TEST(suite, test_http_code_status);
    SUITE_ADD_TEST(suite, test_http_pack_chunked);
    SUITE_ADD_TEST(suite, test_http_header_at);
    SUITE_ADD_TEST(suite, test_redis_simple);
    SUITE_ADD_TEST(suite, test_redis_bulk);
    SUITE_ADD_TEST(suite, test_redis_null_bulk);
    SUITE_ADD_TEST(suite, test_redis_bulk_bad_crlf);
    SUITE_ADD_TEST(suite, test_redis_len_first_char);
    SUITE_ADD_TEST(suite, test_redis_len_strict);
    SUITE_ADD_TEST(suite, test_redis_empty_array);
    SUITE_ADD_TEST(suite, test_redis_array);
    SUITE_ADD_TEST(suite, test_redis_pack);
    SUITE_ADD_TEST(suite, test_redis_pack_bad_format);
    SUITE_ADD_TEST(suite, test_redis_moredata);
    SUITE_ADD_TEST(suite, test_redis_resume_across_calls);
    SUITE_ADD_TEST(suite, test_redis_back_to_back_count);
    SUITE_ADD_TEST(suite, test_redis_oversize_no_crlf);
    SUITE_ADD_TEST(suite, test_redis_resp3_scalar);
    SUITE_ADD_TEST(suite, test_redis_resp3_aggregate);
    SUITE_ADD_TEST(suite, test_redis_nesting);
    SUITE_ADD_TEST(suite, test_url_parse);
    SUITE_ADD_TEST(suite, test_url_parse_edges);
    SUITE_ADD_TEST(suite, test_url_scheme_relative);
    SUITE_ADD_TEST(suite, test_url_userinfo_last_at);
    SUITE_ADD_TEST(suite, test_url_ctx_reuse);
    SUITE_ADD_TEST(suite, test_url_buf_lens_bound);
    SUITE_ADD_TEST(suite, test_url_reorg_param);
    SUITE_ADD_TEST(suite, test_custz);
    SUITE_ADD_TEST(suite, test_custz_maxpack);
    SUITE_ADD_TEST(suite, test_custz_wire_unpack);
    SUITE_ADD_TEST(suite, test_custz_unpack_maxlens);
    SUITE_ADD_TEST(suite, test_smtp_full_response);
    SUITE_ADD_TEST(suite, test_smtp_authtype);
    SUITE_ADD_TEST(suite, test_smtp_body_transparency);
    SUITE_ADD_TEST(suite, test_smtp_plain_mime_headers);
    SUITE_ADD_TEST(suite, test_smtp_plain_line_fold);
    SUITE_ADD_TEST(suite, test_smtp_subject_line_fold);
    SUITE_ADD_TEST(suite, test_smtp_boundary_random);
    SUITE_ADD_TEST(suite, test_smtp_display_name_quote);
    SUITE_ADD_TEST(suite, test_smtp_header_encoded_word);
    SUITE_ADD_TEST(suite, test_smtp_b64_fold);
    SUITE_ADD_TEST(suite, test_smtp_clear_reply);
    SUITE_ADD_TEST(suite, test_http_value_trailing_ows);
    SUITE_ADD_TEST(suite, test_smtp_init_bounds);
    SUITE_ADD_TEST(suite, test_smtp_pack_cmds);
    SUITE_ADD_TEST(suite, test_smtp_pack_crlf_inject);
    SUITE_ADD_TEST(suite, test_smtp_check_code);
    SUITE_ADD_TEST(suite, test_smtp_check_codes);
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
    SUITE_ADD_TEST(suite, test_dns_parse_pack_nodata);
    SUITE_ADD_TEST(suite, test_dns_parse_pack_truncated_query);
    SUITE_ADD_TEST(suite, test_dns_parse_pack_truncated_flag);
    SUITE_ADD_TEST(suite, test_dns_parse_pack_wrong_id);
    SUITE_ADD_TEST(suite, test_dns_parse_pack_ptr_self_loop);
    SUITE_ADD_TEST(suite, test_dns_parse_pack_jump_budget);
    SUITE_ADD_TEST(suite, test_dns_parse_pack_multi_a);
    SUITE_ADD_TEST(suite, test_dns_request_pack_domain_lens);
    SUITE_ADD_TEST(suite, test_dns_set_get_ip);
    SUITE_ADD_TEST(suite, test_custz_head_fixed);
    SUITE_ADD_TEST(suite, test_custz_head_flag);
    SUITE_ADD_TEST(suite, test_custz_head_variable);
    SUITE_ADD_TEST(suite, test_custz_head_wire);
    SUITE_ADD_TEST(suite, test_websock_pack_frames);
    SUITE_ADD_TEST(suite, test_websock_maxpack);
    SUITE_ADD_TEST(suite, test_websock_pack_handshake);
    SUITE_ADD_TEST(suite, test_websock_handshake_server_reject);
    SUITE_ADD_TEST(suite, test_websock_handshake_client_reject);
    SUITE_ADD_TEST(suite, test_websock_secprot_match);
    SUITE_ADD_TEST(suite, test_websock_unpack_text);
    SUITE_ADD_TEST(suite, test_websock_unpack_masked);
    SUITE_ADD_TEST(suite, test_websock_unpack_fragmented);
    SUITE_ADD_TEST(suite, test_websock_unpack_orphan_continue);
    SUITE_ADD_TEST(suite, test_websock_unpack_interleaved_data);
    SUITE_ADD_TEST(suite, test_websock_unpack_close);
    SUITE_ADD_TEST(suite, test_websock_unpack_server_no_mask);
    SUITE_ADD_TEST(suite, test_websock_unpack_client_masked);
    SUITE_ADD_TEST(suite, test_websock_unpack_reserved_opcode);
    SUITE_ADD_TEST(suite, test_websock_unpack_rsv_set);
    SUITE_ADD_TEST(suite, test_websock_unpack_control_fragmented);
    SUITE_ADD_TEST(suite, test_websock_unpack_control_too_big);
    SUITE_ADD_TEST(suite, test_websock_unpack_extended_16);
    SUITE_ADD_TEST(suite, test_websock_unpack_extended_64);
    SUITE_ADD_TEST(suite, test_websock_getters);
    SUITE_ADD_TEST(suite, test_websock_unpack_mask_xor);
    SUITE_ADD_TEST(suite, test_websock_unpack_mask_all_zero);
    SUITE_ADD_TEST(suite, test_websock_mqtt_multipack);
    SUITE_ADD_TEST(suite, test_websock_mqtt_ws_fragment_slice_clear);
    SUITE_ADD_TEST(suite, test_mqtt_auth_version_gate);
    SUITE_ADD_TEST(suite, test_mqtt_utf8_embedded_nul);
    SUITE_ADD_TEST(suite, test_mqtt_connect_pwd_no_user);
    SUITE_ADD_TEST(suite, test_mqtt_pack_lens_on_fail);
    SUITE_ADD_TEST(suite, test_prots_free_null);
    SUITE_ADD_TEST(suite, test_prots_pkfree_default);
    SUITE_ADD_TEST(suite, test_prots_hsfree_default);
    SUITE_ADD_TEST(suite, test_prots_udfree_default);
    SUITE_ADD_TEST(suite, test_prots_net_close_default);
    SUITE_ADD_TEST(suite, test_prots_net_close_tail_gate);
    SUITE_ADD_TEST(suite, test_prots_unpack_default);
    SUITE_ADD_TEST(suite, test_parse_int64_strict);
    SUITE_ADD_TEST(suite, test_parse_colon_triple);
    SUITE_ADD_TEST(suite, test_prots_may_resume_default);
    SUITE_ADD_TEST(suite, test_prots_may_resume_websock_mqtt);
    SUITE_ADD_TEST(suite, test_mail_html_and_clear);
    SUITE_ADD_TEST(suite, test_mail_attach_pack);
    SUITE_ADD_TEST(suite, test_mail_attach_name_rfc2231);
    SUITE_ADD_TEST(suite, test_mail_attach_long_name);
}
