#include "task_timeout.h"
#include "task_auto_close.h"

#define TASK_NAME_AUTOCLOSE "task_auto_close"
//每种协议多轮回显的次数
#define ECHO_ROUNDS 3

typedef struct task_timeout_ctx {
    int32_t _prt;
    int32_t _err;
    int32_t _autoclose;
    name_t _rpcname;
    int32_t *_ok;
    name_val_ctx *_ports;
    void *_evssl;
    char _haborkey[129];
}task_timeout_ctx;

// 测试 coro_sleep 在指定时长下的唤醒精度，diff 超出容忍范围返回 ERR_FAILED
static int32_t _check_sleep(task_ctx *task, uint32_t ms, int32_t lo, int32_t hi) {
    uint64_t bgts = nowms();
    coro_sleep(task, ms);
    int32_t diff = (int32_t)(nowms() - bgts);
    if (diff > hi || diff < lo) {
        LOG_WARN("coro_sleep %ums wake up late or early. diff: %d", ms, diff);
        return ERR_FAILED;
    }
    return ERR_OK;
}
static int32_t _timeout_sleep(task_ctx *task) {
    // 50ms：容忍 [46, 62]
    if (ERR_OK != _check_sleep(task, 50, 46, 62)) {
        return ERR_FAILED;
    }
    if (task_isclosing(task)) {
        return ERR_OK;
    }
    // 100ms：容忍 [95, 125]
    if (ERR_OK != _check_sleep(task, 100, 95, 125)) {
        return ERR_FAILED;
    }
    if (task_isclosing(task)) {
        return ERR_OK;
    }
    // 200ms：容忍 [195, 220]
    if (ERR_OK != _check_sleep(task, 200, 195, 220)) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
static int32_t _timeout_auto_close(task_ctx *task) {
    task_timeout_ctx *ctx = coro_get_arg(task);
    //多线程请求，会一堆告警，任务重复注册。
    if (!ctx->_autoclose) {
        return ERR_OK;
    }
    task_ctx *autoclose = task_grab(task->loader, task_find_name(task->loader, TASK_NAME_AUTOCLOSE));
    if (NULL == autoclose) {
        task_auto_close_start(task->loader, TASK_NAME_AUTOCLOSE, 0);
    } else {
        task_ungrab(autoclose);
        task_close(autoclose);
    }
    return ERR_OK;
}
static int32_t _timeout_rpc(task_ctx *task) {
    task_timeout_ctx *ctx = coro_get_arg(task);
    task_ctx *dest = task_grab(task->loader, ctx->_rpcname);
    if (NULL == dest) {
        return ERR_OK;
    }
    int32_t rtn = ERR_FAILED;
    // type 1: 整数加法，task_call 先做 fire-and-forget 覆盖无回复路径
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 16, 0);
    int32_t a = rand() / 2;
    int32_t b = rand() / 2;
    binary_set_integer(&bwriter, a, 4, 0);
    binary_set_integer(&bwriter, b, 4, 0);
    task_call(dest, 100, bwriter.data, bwriter.offset, 1);
    int32_t erro;
    size_t lens;
    // copy=0：转移 bwriter.data 所有权给框架
    int32_t *sum = coro_request(dest, task, 100, bwriter.data, bwriter.offset, 0, &erro, &lens);
    if (ERR_OK != erro || NULL == sum) {
        LOG_WARN("coro_request type1 error.");
        goto done;
    }
    int32_t rst = (int32_t)ntohl((uint32_t)*sum);
    if (rst != a + b) {
        LOG_WARN("coro_request type1 result error. %d + %d = %d", a, b, rst);
        goto done;
    }
    // type 2: 字节串回显，覆盖变长数据路径
    char data2[257];
    int32_t dlen = randrange(1, 256);
    randstr(data2, (size_t)dlen);
    size_t rlen2;
    int32_t erro2;
    // copy=1：data2 在栈上，不能转移所有权
    void *echo = coro_request(dest, task, 101, data2, (size_t)dlen, 1, &erro2, &rlen2);
    if (ERR_OK != erro2 || NULL == echo) {
        LOG_WARN("coro_request type2 error.");
        goto done;
    }
    if (rlen2 != (size_t)dlen || 0 != memcmp(data2, echo, rlen2)) {
        LOG_WARN("coro_request type2 result error.");
        goto done;
    }
    rtn = ERR_OK;
done:
    task_ungrab(dest);
    return rtn;
}
// 依次发送三次数据并验证回显：1 字节、4095 字节、随机 2~4094 字节
static int32_t _timeout_udp(task_ctx *task) {
    task_timeout_ctx *ctx = coro_get_arg(task);
    uint16_t udpport = (uint16_t)*_get_name_val(ctx->_ports, "udp_echo");
    SOCKET fd;
    uint64_t skid;
    char buf[4096];
    size_t rlens;
    void *resp;
    size_t rlen;
    if (ERR_OK != task_udp(task, "0.0.0.0", 0, &fd, &skid)) {
        LOG_WARN("task_udp error.");
        return ERR_FAILED;
    }
    // 固定边界：1 字节
    randstr(buf, 1);
    resp = coro_sendto(task, fd, skid, "127.0.0.1", udpport, buf, 1, &rlens, 1);
    if (NULL == resp || rlens != 1 || 0 != memcmp(buf, resp, 1)) {
        LOG_WARN("udp 1-byte echo error.");
        goto erro;
    }
    // 固定边界：4095 字节
    randstr(buf, 4095);
    resp = coro_sendto(task, fd, skid, "127.0.0.1", udpport, buf, 4095, &rlens, 1);
    if (NULL == resp || rlens != 4095 || 0 != memcmp(buf, resp, 4095)) {
        LOG_WARN("udp 4095-byte echo error.");
        goto erro;
    }
    // 随机大小（2~4094，避免重复边界值）
    rlen = (size_t)randrange(2, 4094);
    randstr(buf, rlen);
    resp = coro_sendto(task, fd, skid, "127.0.0.1", udpport, buf, rlen, &rlens, 1);
    if (NULL == resp || rlens != rlen || 0 != memcmp(buf, resp, rlen)) {
        LOG_WARN("udp random echo error.");
        goto erro;
    }
    ev_close(&task->loader->netev, fd, skid, 1);
    return ERR_OK;
erro:
    ev_close(&task->loader->netev, fd, skid, 1);
    return ERR_FAILED;
}
// 对当前连接发送 count 轮随机大小的 TEST_ECHO 和 TEST_RPC_ECHO 包，验证回显内容完全一致。
// 每轮复用同一段 randstr 数据：先测直接回显，再测经 task_tcp_server → task_rpc 中转的回显。
static int32_t _tcp_echo(task_ctx *task, SOCKET fd, uint64_t skid, pack_type curtype, int32_t count) {
    char buf[4096];
    int32_t lens;
    size_t size;
    void *pack, *resp;
    for (int32_t i = 0; i < count; i++) {
        lens = randrange(10, 4096 - 2);
        randstr(buf + 1, lens);
        // TEST_ECHO：直接回显
        buf[0] = TEST_ECHO;
        pack = custz_pack(curtype, buf, (size_t)(lens + 1), &size);
        resp = coro_send(task, fd, skid, pack, size, &size, 0);
        if (NULL == resp) {
            LOG_WARN("send echo error.");
            return ERR_FAILED;
        }
        if (size != (size_t)(lens + 1) || 0 != memcmp(buf, resp, size)) {
            LOG_WARN("recv echo data error.");
            return ERR_FAILED;
        }
        // TEST_RPC_ECHO：复用同一段随机字符串，经 task_tcp_server → task_rpc 中转回显
        buf[0] = TEST_RPC_ECHO;
        pack = custz_pack(curtype, buf, (size_t)(lens + 1), &size);
        resp = coro_send(task, fd, skid, pack, size, &size, 0);
        if (NULL == resp) {
            LOG_WARN("send rpc echo error.");
            return ERR_FAILED;
        }
        if (size != (size_t)(lens + 1) || 0 != memcmp(buf, resp, size)) {
            LOG_WARN("recv rpc echo data error.");
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
// 发送 TEST_PKTYPE_CHANGE 切换连接协议类型，等待服务端回显确认后本端同步切换
static int32_t _tcp_switch_pktype(task_ctx *task, SOCKET fd, uint64_t skid,
    pack_type curtype, pack_type newtype) {
    char buf[4];
    buf[0] = TEST_PKTYPE_CHANGE;
    buf[1] = (char)(uint8_t)newtype;
    size_t size;
    void *pack = custz_pack(curtype, buf, 2, &size);
    void *resp = coro_send(task, fd, skid, pack, size, &size, 0);
    if (NULL == resp) {
        LOG_WARN("send pack type change error.");
        return ERR_FAILED;
    }
    if (size != 2 || 0 != memcmp(buf, resp, size)) {
        LOG_WARN("recv pack type change data error.");
        return ERR_FAILED;
    }
    ev_ud_pktype(&task->loader->netev, fd, skid, newtype);
    return ERR_OK;
}
static int32_t _timeout_tcp(task_ctx *task) {
    SOCKET fd;
    uint64_t skid;
    char sslbuf[4];
    size_t sslsize;
    void *sslpack;
    void *sslresp;
    task_timeout_ctx *ctx = coro_get_arg(task);
    uint16_t tcpport = (uint16_t)*_get_name_val(ctx->_ports, "tcp_sv");
    pack_type curtype = PACK_CUSTZ_FIXED;
    //链接
    if (ERR_OK != coro_connect(task, curtype, NULL, "127.0.0.1", tcpport, NETEV_AUTHSSL, NULL, &fd, &skid)) {
        LOG_WARN("connect error");
        return ERR_FAILED;
    }
    //FIXED 协议多轮回显
    if (ERR_OK != _tcp_echo(task, fd, skid, curtype, ECHO_ROUNDS)) {
        goto erro;
    }
    if (NULL != ctx->_evssl) {
        //ssl 切换：用 coro_send 等待服务端回显，确认 CMD_SSL 已入队后再握手，
        // 避免多任务并发时客户端早于服务端进入 SSL 模式导致握手失败
        sslbuf[0] = TEST_SSL_CHANGE;
        sslpack = custz_pack(curtype, sslbuf, 1, &sslsize);
        sslresp = coro_send(task, fd, skid, sslpack, sslsize, &sslsize, 0);
        if (NULL == sslresp) {
            LOG_WARN("send ssl change error.");
            goto erro;
        }
        if (sslsize != 1 || sslbuf[0] != ((char *)sslresp)[0]) {
            LOG_WARN("recv ssl echo data error.");
            goto erro;
        }
        if (ERR_OK != coro_ssl_exchange(task, fd, skid, 1, ctx->_evssl)) {
            LOG_WARN("coro_ssl_exchange error.");
            goto erro;
        }
    }
    //切换到 PACK_CUSTZ_FLAG 并多轮回显
    if (ERR_OK != _tcp_switch_pktype(task, fd, skid, curtype, PACK_CUSTZ_FLAG)) {
        goto erro;
    }
    curtype = PACK_CUSTZ_FLAG;
    if (ERR_OK != _tcp_echo(task, fd, skid, curtype, ECHO_ROUNDS)) {
        goto erro;
    }
    //切换到 PACK_CUSTZ_VAR 并多轮回显
    if (ERR_OK != _tcp_switch_pktype(task, fd, skid, curtype, PACK_CUSTZ_VAR)) {
        goto erro;
    }
    curtype = PACK_CUSTZ_VAR;
    if (ERR_OK != _tcp_echo(task, fd, skid, curtype, ECHO_ROUNDS)) {
        goto erro;
    }
    ev_close(&task->loader->netev, fd, skid, 1);
    return ERR_OK;
erro:
    ev_close(&task->loader->netev, fd, skid, 1);
    return ERR_FAILED;
}
// 测试 HTTP GET 请求（验证 200 响应）+ chunked POST 请求三帧往返验证
static int32_t _timeout_http(task_ctx *task) {
    task_timeout_ctx *ctx = coro_get_arg(task);
    uint16_t httpport = (uint16_t)*_get_name_val(ctx->_ports, "http_sv");
    SOCKET fd;
    uint64_t skid;
    binary_ctx bwriter;
    struct http_pack_ctx *resp;
    buf_ctx *st;
    size_t rsize;
    int32_t slend;
    // 普通 HTTP GET 请求，验证服务端返回 200
    if (ERR_OK != coro_connect(task, PACK_HTTP, NULL, "127.0.0.1", httpport, 0, NULL, &fd, &skid)) {
        LOG_WARN("http connect error.");
        return ERR_FAILED;
    }
    binary_init(&bwriter, NULL, 0, 0);
    http_pack_req(&bwriter, "GET", "/");
    http_pack_head(&bwriter, "Host", "127.0.0.1");
    http_pack_end(&bwriter);
    resp = coro_send(task, fd, skid, bwriter.data, bwriter.offset, &rsize, 0);
    if (NULL == resp) {
        LOG_WARN("http GET error.");
        ev_close(&task->loader->netev, fd, skid, 1);
        return ERR_FAILED;
    }
    st = http_status(resp);
    if (!buf_compare(&st[1], "200", 3)) {
        LOG_WARN("http GET status error.");
        ev_close(&task->loader->netev, fd, skid, 1);
        return ERR_FAILED;
    }
    ev_close(&task->loader->netev, fd, skid, 1);
    // chunked POST 请求（三帧：header+chunk1、chunk2、终止块），验证服务端回复 chunked 响应
    if (ERR_OK != coro_connect(task, PACK_HTTP, NULL, "127.0.0.1", httpport, 0, NULL, &fd, &skid)) {
        LOG_WARN("http chunked connect error.");
        return ERR_FAILED;
    }
    binary_init(&bwriter, NULL, 0, 0);
    http_pack_req(&bwriter, "POST", "/");
    http_pack_head(&bwriter, "Host", "127.0.0.1");
    http_pack_chunked(&bwriter, "a", 1);
    ev_send(&task->loader->netev, fd, skid, bwriter.data, bwriter.offset, 1);
    binary_offset(&bwriter, 0);
    http_pack_chunked(&bwriter, "b", 1);
    ev_send(&task->loader->netev, fd, skid, bwriter.data, bwriter.offset, 1);
    binary_offset(&bwriter, 0);
    http_pack_chunked(&bwriter, NULL, 0);
    ev_send(&task->loader->netev, fd, skid, bwriter.data, bwriter.offset, 0);
    slend = 0;
    do {
        resp = coro_slice(task, fd, skid, &rsize, &slend);
        if (NULL == resp) {
            LOG_WARN("http chunked recv error.");
            ev_close(&task->loader->netev, fd, skid, 1);
            return ERR_FAILED;
        }
    } while (0 == slend);
    ev_close(&task->loader->netev, fd, skid, 1);
    return ERR_OK;
}
// 测试纯 WebSocket 服务端的文本帧和二进制帧回显
static int32_t _timeout_ws(task_ctx *task) {
    task_timeout_ctx *ctx = coro_get_arg(task);
    uint16_t wsport = (uint16_t)*_get_name_val(ctx->_ports, "ws_sv");
    SOCKET fd;
    uint64_t skid;
    struct websock_pack_ctx *resp;
    size_t psize;
    size_t rsize;
    size_t wlen;
    void *pack;
    char *wdata;
    char buf[256];
    int32_t dlen;
    int32_t slend;
    char wsurl[64];
    SNPRINTF(wsurl, sizeof(wsurl), "ws://127.0.0.1:%d", (int)wsport);
    fd = wbsock_connect(task, NULL, wsurl, NULL, &skid, 0);
    if (INVALID_SOCK == fd) {
        LOG_WARN("ws connect error.");
        return ERR_FAILED;
    }
    // 发送文本帧，验证回显
    dlen = randrange(10, 200);
    randstr(buf, (size_t)dlen);
    pack = websock_pack_text(1, 1, buf, (size_t)dlen, &psize);
    resp = coro_send(task, fd, skid, pack, psize, &rsize, 0);
    if (NULL == resp || WS_TEXT != websock_prot(resp)) {
        LOG_WARN("ws text echo error.");
        goto erro;
    }
    wdata = websock_data(resp, &wlen);
    if (wlen != (size_t)dlen || 0 != memcmp(buf, wdata, wlen)) {
        LOG_WARN("ws text echo data error.");
        goto erro;
    }
    // 发送二进制帧，验证回显
    dlen = randrange(10, 200);
    randstr(buf, (size_t)dlen);
    pack = websock_pack_binary(1, 1, buf, (size_t)dlen, &psize);
    resp = coro_send(task, fd, skid, pack, psize, &rsize, 0);
    if (NULL == resp || WS_BINARY != websock_prot(resp)) {
        LOG_WARN("ws binary echo error.");
        goto erro;
    }
    wdata = websock_data(resp, &wlen);
    if (wlen != (size_t)dlen || 0 != memcmp(buf, wdata, wlen)) {
        LOG_WARN("ws binary echo data error.");
        goto erro;
    }
    // 发送 ping 帧，验证服务端回 pong
    pack = websock_pack_ping(1, &psize);
    resp = coro_send(task, fd, skid, pack, psize, &rsize, 0);
    if (NULL == resp || WS_PONG != websock_prot(resp)) {
        LOG_WARN("ws ping/pong error.");
        goto erro;
    }
    // 发送三帧分片消息（start + middle + end），验证服务端收齐后回复分片消息
    pack = websock_pack_text(1, 0, "a", 1, &psize);
    ev_send(&task->loader->netev, fd, skid, pack, psize, 0);
    pack = websock_pack_continua(1, 0, "b", 1, &psize);
    ev_send(&task->loader->netev, fd, skid, pack, psize, 0);
    pack = websock_pack_continua(1, 1, "c", 1, &psize);
    ev_send(&task->loader->netev, fd, skid, pack, psize, 0);
    slend = 0;
    do {
        resp = coro_slice(task, fd, skid, &rsize, &slend);
        if (NULL == resp) {
            LOG_WARN("ws fragmented recv error.");
            goto erro;
        }
    } while (0 == slend);
    ev_close(&task->loader->netev, fd, skid, 1);
    return ERR_OK;
erro:
    ev_close(&task->loader->netev, fd, skid, 1);
    return ERR_FAILED;
}
static int32_t _timeout_habor(task_ctx *task) {
    task_timeout_ctx *ctx = coro_get_arg(task);
    uint16_t port = (uint16_t)*_get_name_val(ctx->_ports, "harbor");
    SOCKET fd;
    uint64_t skid;
    if (ERR_OK != coro_connect(task, PACK_HTTP, ctx->_evssl, "127.0.0.1", port, 0, NULL, &fd, &skid)) {
        LOG_WARN("habor connect error.");
        return ERR_FAILED;
    }
    size_t rsize;
    char data[257];
    int32_t dlen = randrange(1, 256);
    randstr(data, (size_t)dlen);
    void *pack = harbor_pack(ctx->_rpcname, 1, 2, ctx->_haborkey, data, dlen, &rsize);
    //call
    struct http_pack_ctx *rpack = coro_send(task, fd, skid, pack, rsize, NULL, 0);
    if (NULL == rpack) {
        LOG_WARN("coro_send error.");
        goto erro;
    }
    buf_ctx *status = http_status(rpack);
    if (3 != status[1].lens || 0 != memcmp(status[1].data, "200", 3)) {
        LOG_WARN("return code error.");
        goto erro;
    }
    //request
    dlen = randrange(1, 256);
    randstr(data, (size_t)dlen);
    pack = harbor_pack(ctx->_rpcname, 0, 101, ctx->_haborkey, data, dlen, &rsize);
    rpack = coro_send(task, fd, skid, pack, rsize, NULL, 0);
    if (NULL == rpack) {
        LOG_WARN("coro_send error.");
        goto erro;
    }
    status = http_status(rpack);
    if (3 != status[1].lens || 0 != memcmp(status[1].data, "200", 3)) {
        LOG_WARN("return code error.");
        goto erro;
    }
    void *rdata = http_data(rpack, &rsize);
    if (dlen != (int32_t)rsize || 0 != memcmp(rdata, data, dlen)) {
        LOG_WARN("return data error.");
        goto erro;
    }
    ev_close(&task->loader->netev, fd, skid, 1);
    return ERR_OK;
erro:
    ev_close(&task->loader->netev, fd, skid, 1);
    return ERR_FAILED;
}
static void _timeout(task_ctx *task, uint64_t sess) {
    (void)sess;
    task_timeout_ctx *ctx = coro_get_arg(task);
    if (ERR_OK != _timeout_sleep(task)) {
        ctx->_err = 1;
        LOG_WARN("sleep test error.");
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _timeout_auto_close(task)) {
        ctx->_err = 1;
        LOG_WARN("auto close test error.");
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _timeout_rpc(task)) {
        ctx->_err = 1;
        LOG_WARN("rpc call test error.");
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _timeout_udp(task)) {
        ctx->_err = 1;
        LOG_WARN("udp test error.");
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _timeout_tcp(task)) {
        ctx->_err = 1;
        LOG_WARN("tcp test error.");
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _timeout_http(task)) {
        ctx->_err = 1;
        LOG_WARN("http test error.");
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _timeout_ws(task)) {
        ctx->_err = 1;
        LOG_WARN("ws test error.");
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _timeout_habor(task)) {
        ctx->_err = 1;
        LOG_WARN("habor test error.");
    }
    if (ctx->_err) {
        *ctx->_ok = 0;
    } else {
        *ctx->_ok = 1;
    }
    task_timeout(task, 0, 1000, _timeout);
}
static void _startup(task_ctx *task) {
    task_timeout(task, 0, 100, _timeout);
}
static void _closing(task_ctx *task) {
    (void)task;
}
void task_timeout_start(loader_ctx *loader, const char *name,
    const char *rpcname, name_val_ctx *ports, void *evssl,
    int32_t autoclose, const char *haborkey, int32_t pt, int32_t *ok) {
    task_timeout_ctx *ctx;
    CALLOC(ctx, 1, sizeof(task_timeout_ctx));
    ctx->_rpcname = task_find_name(loader, rpcname);
    ctx->_prt = pt;
    ctx->_autoclose = autoclose;
    ctx->_ports = ports;
    ctx->_evssl = evssl;
    ctx->_ok = ok;
    safe_fill_str(ctx->_haborkey, sizeof(ctx->_haborkey), haborkey);
    coro_task_register(loader, name, 0, _startup, _closing, _free, ctx);
}
