#include "task_timeout.h"
#include "task_auto_close.h"

#define TASK_NAME_AUTOCLOSE "task_auto_close"
//每种协议多轮回显的次数
#define ECHO_ROUNDS 3
// coro_sleep 唤醒的上界余量。到期时刻由时间轮按 1ms jiffy 保证，这里放宽的是
// "时间轮唤醒 → 入 task 队列 → worker 取出 → 协程 resume"这段调度尾巴：它与 sleep
// 时长无关，故取固定值。三档 sleep 合起来仍能卡住成比例的偏差
#define SLEEP_SLACK_MS 80

typedef struct task_timeout_ctx {
    int32_t _failed;// 失败粘滞位：本回调每秒自重挂一轮，不粘住则某轮的失败被下一轮的成功覆盖；
                    // 也不能等到轮末再并——中途 task_isclosing 提前 return 会把本轮的失败丢掉
    int32_t _autoclose;
    int32_t _rebind_done;// WS 重复注入只验一次；每 task 一份，timeout_test1/2/3 是三个并发 task
    int32_t _reject_done;// harbor 无证书拒绝只验一次，理由同上
    name_t _rpcname;
    int32_t *_ok;
    name_val_ctx *_ports;
    void *_evssl;
    void *_hbssl;
}task_timeout_ctx;

// 测试 coro_sleep 在指定时长下的唤醒精度，diff 超出容忍范围返回 ERR_FAILED
static int32_t _check_sleep(task_ctx *task, uint32_t ms, int32_t lo) {
    uint64_t bgts = nowms();
    coro_sleep(task, ms);
    int32_t diff = (int32_t)(nowms() - bgts);
    if (diff > (int32_t)ms + SLEEP_SLACK_MS || diff < lo) {
        LOG_WARN("coro_sleep %ums wake up late or early. diff: %d", ms, diff);
        return ERR_FAILED;
    }
    return ERR_OK;
}
static int32_t _timeout_sleep(task_ctx *task) {
    if (ERR_OK != _check_sleep(task, 50, 46)) {
        return ERR_FAILED;
    }
    if (task_isclosing(task)) {
        return ERR_OK;
    }
    if (ERR_OK != _check_sleep(task, 100, 95)) {
        return ERR_FAILED;
    }
    if (task_isclosing(task)) {
        return ERR_OK;
    }
    if (ERR_OK != _check_sleep(task, 200, 195)) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
// 无返回值：本函数没有可判失败的分支（找不到就新建，找到就关掉），
// 真正的验收是 main.c 收尾时读 get_close_count()
static void _timeout_auto_close(task_ctx *task) {
    task_timeout_ctx *ctx = coro_get_arg(task);
    //多线程请求，会一堆告警，任务重复注册。
    if (!ctx->_autoclose) {
        return;
    }
    task_ctx *autoclose = task_grab(task->loader, task_find_name(task->loader, TASK_NAME_AUTOCLOSE));
    if (NULL == autoclose) {
        task_auto_close_start(task->loader, TASK_NAME_AUTOCLOSE, 0);
    } else {
        // 先 close 再 ungrab：反过来时若 auto_close 正在自己 teardown，
        // ungrab 可能把引用降到 0 当场 task_free，close 就打在已释放的 task 上
        task_close(autoclose);
        task_ungrab(autoclose);
    }
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
    binary_init_write(&bwriter, 16, 0);
    int32_t a = rand() / 2;
    int32_t b = rand() / 2;
    binary_set_integer(&bwriter, a, 4, 0);
    binary_set_integer(&bwriter, b, 4, 0);
    task_call(dest, 100, bwriter.data, bwriter.offset, 1);
    int32_t erro;
    size_t lens;
    // copy=0：转移 bwriter.data 所有权给框架
    int32_t *sum = coro_request(dest, task, 100, bwriter.data, bwriter.offset, 0, &erro, &lens);
    if (ERR_OK != erro || NULL == sum || sizeof(int32_t) != lens) {
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
    sock_ctx sk;
    char buf[4096];
    size_t rlens;
    void *resp;
    size_t rlen;
    if (ERR_OK != task_udp(task, PACK_NONE, "0.0.0.0", 0, &sk)) {
        LOG_WARN("task_udp error.");
        return ERR_FAILED;
    }
    coro_sync(task, &sk);
    // 固定边界：1 字节
    randstr(buf, 1);
    resp = coro_sendto(task, &sk, "127.0.0.1", udpport, buf, 1, &rlens, 1);
    if (NULL == resp || rlens != 1 || 0 != memcmp(buf, resp, 1)) {
        LOG_WARN("udp 1-byte echo error.");
        goto erro;
    }
    // 固定边界：4095 字节
    randstr(buf, 4095);
    resp = coro_sendto(task, &sk, "127.0.0.1", udpport, buf, 4095, &rlens, 1);
    if (NULL == resp || rlens != 4095 || 0 != memcmp(buf, resp, 4095)) {
        LOG_WARN("udp 4095-byte echo error.");
        goto erro;
    }
    // 随机大小（2~4094，避免重复边界值）
    rlen = (size_t)randrange(2, 4094);
    randstr(buf, rlen);
    resp = coro_sendto(task, &sk, "127.0.0.1", udpport, buf, rlen, &rlens, 1);
    if (NULL == resp || rlens != rlen || 0 != memcmp(buf, resp, rlen)) {
        LOG_WARN("udp random echo error.");
        goto erro;
    }
    ev_close(&task->loader->netev, &sk);
    return ERR_OK;
erro:
    ev_close(&task->loader->netev, &sk);
    return ERR_FAILED;
}
// 对当前连接发送 count 轮随机大小的 TEST_ECHO 和 TEST_RPC_ECHO 包，验证回显内容完全一致。
// 每轮复用同一段 randstr 数据：先测直接回显，再测经 task_tcp_server → task_rpc 中转的回显。
static int32_t _tcp_echo(task_ctx *task, sock_ctx *sk, pack_type curtype, int32_t count) {
    char buf[4096];
    int32_t lens;
    size_t size = 0;
    void *pack, *resp;
    for (int32_t i = 0; i < count; i++) {
        lens = randrange(10, 4096 - 2);
        randstr(buf + 1, lens);
        // TEST_ECHO：直接回显
        buf[0] = TEST_ECHO;
        pack = custz_pack(curtype, buf, (size_t)(lens + 1), &size);
        resp = coro_send(task, sk, pack, size, &size, 0);
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
        resp = coro_send(task, sk, pack, size, &size, 0);
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
static int32_t _tcp_switch_pktype(task_ctx *task, sock_ctx *sk,
    pack_type curtype, pack_type newtype) {
    char buf[4];
    buf[0] = TEST_PKTYPE_CHANGE;
    buf[1] = (char)(uint8_t)newtype;
    size_t size = 0;
    void *pack = custz_pack(curtype, buf, 2, &size);
    void *resp = coro_send(task, sk, pack, size, &size, 0);
    if (NULL == resp) {
        LOG_WARN("send pack type change error.");
        return ERR_FAILED;
    }
    if (size != 2 || 0 != memcmp(buf, resp, size)) {
        LOG_WARN("recv pack type change data error.");
        return ERR_FAILED;
    }
    ev_ud_pktype(&task->loader->netev, sk, newtype);
    return ERR_OK;
}
static int32_t _timeout_tcp(task_ctx *task) {
    sock_ctx sk;
    char sslbuf[4];
    size_t sslsize = 0;
    void *sslpack;
    void *sslresp;
    task_timeout_ctx *ctx = coro_get_arg(task);
    uint16_t tcpport = (uint16_t)*_get_name_val(ctx->_ports, "tcp_sv");
    pack_type curtype = PACK_CUSTZ_FIXED;
    //链接
    if (ERR_OK != coro_connect(task, curtype, NULL, "127.0.0.1", tcpport, NETEV_AUTHSSL, NULL, &sk)) {
        LOG_WARN("connect error");
        return ERR_FAILED;
    }
    //FIXED 协议多轮回显
    if (ERR_OK != _tcp_echo(task, &sk, curtype, ECHO_ROUNDS)) {
        goto erro;
    }
    if (NULL != ctx->_evssl) {
        //ssl 切换：用 coro_send 等待服务端回显，确认 ev_ssl 已入队后再握手，
        // 避免多任务并发时客户端早于服务端进入 SSL 模式导致握手失败
        sslbuf[0] = TEST_SSL_CHANGE;
        sslpack = custz_pack(curtype, sslbuf, 1, &sslsize);
        sslresp = coro_send(task, &sk, sslpack, sslsize, &sslsize, 0);
        if (NULL == sslresp) {
            LOG_WARN("send ssl change error.");
            goto erro;
        }
        if (sslsize != 1 || sslbuf[0] != ((char *)sslresp)[0]) {
            LOG_WARN("recv ssl echo data error.");
            goto erro;
        }
        if (ERR_OK != coro_ssl_exchange(task, &sk, 1, ctx->_evssl)) {
            LOG_WARN("coro_ssl_exchange error.");
            goto erro;
        }
    }
    //切换到 PACK_CUSTZ_FLAG 并多轮回显
    if (ERR_OK != _tcp_switch_pktype(task, &sk, curtype, PACK_CUSTZ_FLAG)) {
        goto erro;
    }
    curtype = PACK_CUSTZ_FLAG;
    if (ERR_OK != _tcp_echo(task, &sk, curtype, ECHO_ROUNDS)) {
        goto erro;
    }
    //切换到 PACK_CUSTZ_VAR 并多轮回显
    if (ERR_OK != _tcp_switch_pktype(task, &sk, curtype, PACK_CUSTZ_VAR)) {
        goto erro;
    }
    curtype = PACK_CUSTZ_VAR;
    if (ERR_OK != _tcp_echo(task, &sk, curtype, ECHO_ROUNDS)) {
        goto erro;
    }
    ev_close(&task->loader->netev, &sk);
    return ERR_OK;
erro:
    ev_close(&task->loader->netev, &sk);
    return ERR_FAILED;
}
// 测试 HTTP GET 请求（验证 200 响应）+ chunked POST 请求三帧往返验证
// 状态码断言: status[1] 是状态码的字符串形态, 按长度精确比对, 免得 "40" 被 "404" 误当命中
static int32_t _status_is(struct http_pack_ctx *rpack, const char *code) {
    // chunked 中间/结束块没有首行，http_status 返 NULL；当作"不是这个状态码"
    buf_ctx *st = http_status(rpack);
    if (NULL == st) {
        return 0;
    }
    return buf_compare(&st[1], code, strlen(code));
}
static int32_t _timeout_http(task_ctx *task) {
    task_timeout_ctx *ctx = coro_get_arg(task);
    uint16_t httpport = (uint16_t)*_get_name_val(ctx->_ports, "http_sv");
    sock_ctx sk;
    binary_ctx bwriter;
    struct http_pack_ctx *resp;
    size_t rsize;
    int32_t slend;
    int32_t nslice;
    // 普通 HTTP GET 请求，验证服务端返回 200
    if (ERR_OK != coro_connect(task, PACK_HTTP, NULL, "127.0.0.1", httpport, 0, NULL, &sk)) {
        LOG_WARN("http connect error.");
        return ERR_FAILED;
    }
    binary_init_write(&bwriter, 0, 0);
    http_pack_req(&bwriter, "GET", "/");
    http_pack_head(&bwriter, "Host", "127.0.0.1");
    http_pack_end(&bwriter);
    resp = coro_send(task, &sk, bwriter.data, bwriter.offset, &rsize, 0);
    if (NULL == resp) {
        LOG_WARN("http GET error.");
        ev_close(&task->loader->netev, &sk);
        return ERR_FAILED;
    }
    if (!_status_is(resp, "200")) {
        LOG_WARN("http GET status error.");
        ev_close(&task->loader->netev, &sk);
        return ERR_FAILED;
    }
    ev_close(&task->loader->netev, &sk);
    // chunked POST 请求（三帧：header+chunk1、chunk2、终止块），验证服务端回复 chunked 响应
    if (ERR_OK != coro_connect(task, PACK_HTTP, NULL, "127.0.0.1", httpport, 0, NULL, &sk)) {
        LOG_WARN("http chunked connect error.");
        return ERR_FAILED;
    }
    binary_init_write(&bwriter, 0, 0);
    http_pack_req(&bwriter, "POST", "/");
    http_pack_head(&bwriter, "Host", "127.0.0.1");
    http_pack_chunked(&bwriter, "a", 1);
    ev_send(&task->loader->netev, &sk, bwriter.data, bwriter.offset, 1);
    binary_offset(&bwriter, 0);
    http_pack_chunked(&bwriter, "b", 1);
    ev_send(&task->loader->netev, &sk, bwriter.data, bwriter.offset, 1);
    binary_offset(&bwriter, 0);
    http_pack_chunked(&bwriter, NULL, 0);
    ev_send(&task->loader->netev, &sk, bwriter.data, bwriter.offset, 0);
    slend = 0;
    nslice = 0;
    do {
        resp = coro_slice(task, &sk, &rsize, &slend);
        if (NULL == resp) {
            LOG_WARN("http chunked recv error.");
            ev_close(&task->loader->netev, &sk);
            return ERR_FAILED;
        }
        // 首片带响应头，状态码必须查：原来这条循环只判非 NULL，服务端把 200 改 500 也照过
        if (0 == nslice && !_status_is(resp, "200")) {
            LOG_WARN("http chunked status error.");
            ev_close(&task->loader->netev, &sk);
            return ERR_FAILED;
        }
        nslice++;
    } while (0 == slend);
    // 必须真的分了片。coro_slice 对非分片消息(slice==0)也置 end=1，只判 end 的话
    // 服务端改回普通 Content-Length 响应，这条循环一轮就退出、照样"通过"
    if (nslice < 2) {
        LOG_WARN("http chunked slice count %d, expect >= 2.", nslice);
        ev_close(&task->loader->netev, &sk);
        return ERR_FAILED;
    }
    ev_close(&task->loader->netev, &sk);
    return ERR_OK;
}
// 测试纯 WebSocket 服务端的文本帧和二进制帧回显
static int32_t _timeout_ws(task_ctx *task) {
    task_timeout_ctx *ctx = coro_get_arg(task);
    uint16_t wsport = (uint16_t)*_get_name_val(ctx->_ports, "ws_sv");
    sock_ctx sk;
    struct websock_pack_ctx *resp;
    size_t psize;
    size_t rsize;
    size_t wlen;
    void *pack;
    char *wdata;
    char buf[256];
    int32_t dlen;
    int32_t slend;
    int32_t nslice;
    ws_secprots_ctx *spctx;
    char wsurl[64];
    SNPRINTF(wsurl, sizeof(wsurl), "ws://127.0.0.1:%d", (int)wsport);
    // 多值 offer：服务端从 "mqtt,foo" 选中内建 mqtt，出参返回协商到的子协议
    if (ERR_OK != wbsock_connect(task, NULL, wsurl, "mqtt,foo", 0, &sk, &spctx)) {
        LOG_WARN("ws mqtt subprotocol handshake error.");
        return ERR_FAILED;
    }
    if (NULL == spctx || 0 != spctx->index || 1 != spctx->cnt
        || 4 != spctx->prots[0].lens || 0 != memcmp("mqtt", spctx->prots[0].data, 4)) {
        LOG_WARN("ws mqtt subprotocol negotiate error.");
        ev_close(&task->loader->netev, &sk);
        return ERR_FAILED;
    }
    // 客户端方向的 mqtt_ctx 只能由这里注入，须在发 CONNECT 之前，规则见 prots_wrap.h
    if (ERR_OK != mqtt_ws_bind(task, &sk, MQTT_311)) {
        LOG_WARN("ws mqtt bind error.");
        ev_close(&task->loader->netev, &sk);
        return ERR_FAILED;
    }
    // MQTT over WS 数据面：发 CONNECT 等服务端回 CONNACK
    size_t clens;
    char *conn = mqtt_pack_connect(MQTT_311, 1, 60, "wsmqtt", NULL, NULL, 0,
        NULL, NULL, 0, 0, 0, NULL, NULL, &clens);
    if (NULL == conn) {
        LOG_WARN("ws mqtt pack connect error.");
        ev_close(&task->loader->netev, &sk);
        return ERR_FAILED;
    }
    pack = websock_pack_binary(1, 1, conn, clens, &psize);
    FREE(conn);
    if (NULL == pack) {
        LOG_WARN("ws mqtt pack binary error.");
        ev_close(&task->loader->netev, &sk);
        return ERR_FAILED;
    }
    resp = coro_send(task, &sk, pack, psize, &rsize, 0);
    if (NULL == resp
        || PACK_MQTT != websock_secprot(resp)) {
        LOG_WARN("ws mqtt connack recv error.");
        ev_close(&task->loader->netev, &sk);
        return ERR_FAILED;
    }
    mqtt_pack_ctx *mpack = (mqtt_pack_ctx *)websock_secpack(resp);
    if (NULL == mpack
        || MQTT_CONNACK != mpack->fixhead.prot) {
        LOG_WARN("ws mqtt connack prot error.");
        ev_close(&task->loader->netev, &sk);
        return ERR_FAILED;
    }
    // 重复注入：_websock_secextra 判误用并就地断连，随后的收发必然失败。放在正常流程验完之后，
    // 且只验一次——_timeout 每秒自我重挂，每轮都绑两次会把那条告警刷几十条
    if (0 == ctx->_rebind_done) {
        ctx->_rebind_done = 1;
        if (ERR_OK != mqtt_ws_bind(task, &sk, MQTT_311)) {
            LOG_WARN("ws mqtt rebind error.");
            ev_close(&task->loader->netev, &sk);
            return ERR_FAILED;
        }
        pack = websock_pack_ping(1, &psize);
        if (NULL == pack) {
            LOG_WARN("ws mqtt rebind pack ping error.");
            ev_close(&task->loader->netev, &sk);
            return ERR_FAILED;
        }
        if (NULL != coro_send(task, &sk, pack, psize, &rsize, 0)) {
            LOG_WARN("ws mqtt rebind did not close the connection.");
            ev_close(&task->loader->netev, &sk);
            return ERR_FAILED;
        }
    }
    ev_close(&task->loader->netev, &sk);
    // 非内建单值 chat：B-lite 透传，服务端回显 chat(PACK_NONE)，出参返回协商到的 chat
    if (ERR_OK != wbsock_connect(task, NULL, wsurl, "chat", 0, &sk, &spctx)) {
        LOG_WARN("ws chat handshake error.");
        return ERR_FAILED;
    }
    if (NULL == spctx || 0 != spctx->index || 1 != spctx->cnt
        || 4 != spctx->prots[0].lens || 0 != memcmp("chat", spctx->prots[0].data, 4)) {
        LOG_WARN("ws chat subprotocol negotiate error.");
        ev_close(&task->loader->netev, &sk);
        return ERR_FAILED;
    }
    pack = websock_pack_text(1, 1, "chat", 4, &psize);
    resp = coro_send(task, &sk, pack, psize, &rsize, 0);
    if (NULL == resp || WS_TEXT != websock_prot(resp)) {
        LOG_WARN("ws chat echo error.");
        ev_close(&task->loader->netev, &sk);
        return ERR_FAILED;
    }
    wdata = websock_data(resp, &wlen);
    if (4 != wlen || 0 != memcmp("chat", wdata, wlen)) {
        LOG_WARN("ws chat echo data error.");
        ev_close(&task->loader->netev, &sk);
        return ERR_FAILED;
    }
    ev_close(&task->loader->netev, &sk);
    if (ERR_OK != wbsock_connect(task, NULL, wsurl, NULL, 0, &sk, NULL)) {
        LOG_WARN("ws connect error.");
        return ERR_FAILED;
    }
    // 发送文本帧，验证回显
    dlen = randrange(10, 200);
    randstr(buf, (size_t)dlen);
    pack = websock_pack_text(1, 1, buf, (size_t)dlen, &psize);
    resp = coro_send(task, &sk, pack, psize, &rsize, 0);
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
    resp = coro_send(task, &sk, pack, psize, &rsize, 0);
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
    resp = coro_send(task, &sk, pack, psize, &rsize, 0);
    if (NULL == resp || WS_PONG != websock_prot(resp)) {
        LOG_WARN("ws ping/pong error.");
        goto erro;
    }
    // 发送三帧分片消息（start + middle + end），验证服务端收齐后回复分片消息
    pack = websock_pack_text(1, 0, "a", 1, &psize);
    ev_send(&task->loader->netev, &sk, pack, psize, 0);
    pack = websock_pack_continua(1, 0, "b", 1, &psize);
    ev_send(&task->loader->netev, &sk, pack, psize, 0);
    pack = websock_pack_continua(1, 1, "c", 1, &psize);
    ev_send(&task->loader->netev, &sk, pack, psize, 0);
    slend = 0;
    nslice = 0;
    do {
        resp = coro_slice(task, &sk, &rsize, &slend);
        if (NULL == resp) {
            LOG_WARN("ws fragmented recv error.");
            goto erro;
        }
        // 服务端回的是 "a"/"b"/"c" 三帧，逐帧比对载荷。原来只判非 NULL，于是删掉中间帧、
        // 让首帧就标末片、或把载荷换成别的字符，全都照过
        wdata = websock_data(resp, &wlen);
        if (nslice > 2 || NULL == wdata || 1 != wlen || (char)('a' + nslice) != wdata[0]) {
            LOG_WARN("ws fragment %d payload error.", nslice);
            goto erro;
        }
        // 前两帧必须"还没完"，第三帧才是末片
        if ((2 == nslice) != (0 != slend)) {
            LOG_WARN("ws fragment %d slend %d unexpected.", nslice, slend);
            goto erro;
        }
        nslice++;
    } while (0 == slend);
    if (3 != nslice) {
        LOG_WARN("ws fragment count %d, expect 3.", nslice);
        goto erro;
    }
    ev_close(&task->loader->netev, &sk);
    return ERR_OK;
erro:
    ev_close(&task->loader->netev, &sk);
    return ERR_FAILED;
}
// 用调用方给定的原始 URL 组 harbor POST 请求：harbor_pack 只会生成规范查询串，
// 百分号编码 / 非法数值等畸形 URL 须在这里自行组包
static void *_harbor_pack_url(const char *url, void *data, size_t size, size_t *lens) {
    binary_ctx bwriter;
    binary_init_write(&bwriter, 0, 0);
    http_pack_req(&bwriter, "POST", url);
    http_pack_head(&bwriter, "Connection", "Keep-Alive");
    http_pack_head(&bwriter, "Content-Type", "application/octet-stream");
    http_pack_content(&bwriter, data, size);
    *lens = bwriter.offset;
    return bwriter.data;
}
// 发一个 harbor 请求并断言响应状态码（code 为状态码字符串，如 "404"），不校验 body
static int32_t _harbor_expect_code(task_ctx *task, sock_ctx *sk, const char *url,
                                   void *data, size_t size, const char *code) {
    size_t rsize = 0;
    void *pack = _harbor_pack_url(url, data, size, &rsize);
    struct http_pack_ctx *rpack = coro_send(task, sk, pack, rsize, NULL, 0);
    if (NULL == rpack) {
        LOG_WARN("coro_send error.");
        return ERR_FAILED;
    }
    if (!_status_is(rpack, code)) {
        LOG_WARN("harbor \"%s\" expect %s.", url, code);
        return ERR_FAILED;
    }
    return ERR_OK;
}
static int32_t _timeout_habor(task_ctx *task) {
    task_timeout_ctx *ctx = coro_get_arg(task);
    uint16_t port = (uint16_t)*_get_name_val(ctx->_ports, "harbor");
    sock_ctx sk;
    if (ERR_OK != coro_connect(task, PACK_HTTP, ctx->_hbssl, "127.0.0.1", port, 0, NULL, &sk)) {
        LOG_WARN("habor connect error.");
        return ERR_FAILED;
    }
    size_t rsize;
    char data[257];
    int32_t dlen = randrange(1, 256);
    randstr(data, (size_t)dlen);
    void *pack = harbor_pack(ctx->_rpcname, 1, 2, data, dlen, &rsize);
    //call
    struct http_pack_ctx *rpack = coro_send(task, &sk, pack, rsize, NULL, 0);
    if (NULL == rpack) {
        LOG_WARN("coro_send error.");
        goto erro;
    }
    if (!_status_is(rpack, "200")) {
        LOG_WARN("return code error.");
        goto erro;
    }
    size_t cblen = 0;
    void *cbody = http_data(rpack, &cblen);
    if (NULL != cbody || 0 != cblen) {
        LOG_WARN("harbor call resp body must be empty, got %zu bytes.", cblen);
        goto erro;
    }
    size_t chlen = 0;
    if (NULL != http_header(rpack, "X-Srey-Erro", &chlen)) {
        LOG_WARN("%s", "harbor call resp must not carry X-Srey-Erro.");
        goto erro;
    }
    //request
    dlen = randrange(1, 256);
    randstr(data, (size_t)dlen);
    pack = harbor_pack(ctx->_rpcname, 0, 101, data, dlen, &rsize);
    rpack = coro_send(task, &sk, pack, rsize, NULL, 0);
    if (NULL == rpack) {
        LOG_WARN("coro_send error.");
        goto erro;
    }
    if (!_status_is(rpack, "200")) {
        LOG_WARN("return code error.");
        goto erro;
    }
    void *rdata = http_data(rpack, &rsize);
    if (dlen != (int32_t)rsize || 0 != memcmp(rdata, data, dlen)) {
        LOG_WARN("return data error.");
        goto erro;
    }
    size_t ehlen = 0;
    char *ehv = http_header(rpack, "X-Srey-Erro", &ehlen);
    if (NULL == ehv || 1 != ehlen || '0' != ehv[0]) {
        LOG_WARN("harbor resp X-Srey-Erro missing or wrong.");
        goto erro;
    }
    subtype_t reserved[] = { REQ_DEBUG };
    for (size_t i = 0; i < ARRAY_SIZE(reserved); i++) {
        pack = harbor_pack(ctx->_rpcname, 0, reserved[i], data, dlen, &rsize);
        rpack = coro_send(task, &sk, pack, rsize, NULL, 0);
        if (NULL == rpack) {
            LOG_WARN("coro_send error.");
            goto erro;
        }
        if (!_status_is(rpack, "404")) {
            LOG_WARN("reserved subtype %d not rejected.", (int32_t)reserved[i]);
            goto erro;
        }
    }
    // 百分号编码的 dst/type：url_decode 就地压缩只缩短 lens、不搬移尾部字节，
    // "%31"+"01" 解码为 "101" 后缓冲仍读作 "10101"，须按 lens 截断解析才拿得到真值。
    // 首位数字编成 %3X（'0'-'9' 即 0x30-0x39），解码后与原 handle 完全一致，应正常路由并回显
    char dstr[24];
    char qurl[128];
    SNPRINTF(dstr, sizeof(dstr), "%"PRIu64, (uint64_t)ctx->_rpcname);
    SNPRINTF(qurl, sizeof(qurl), "/request?dst=%%3%c%s&type=%%3101", dstr[0], dstr + 1);
    dlen = randrange(1, 256);
    randstr(data, (size_t)dlen);
    pack = _harbor_pack_url(qurl, data, (size_t)dlen, &rsize);
    rpack = coro_send(task, &sk, pack, rsize, NULL, 0);
    if (NULL == rpack) {
        LOG_WARN("coro_send error.");
        goto erro;
    }
    if (!_status_is(rpack, "200")) {
        LOG_WARN("percent encoded dst/type not routed, decode residue read into value.");
        goto erro;
    }
    rdata = http_data(rpack, &rsize);
    if (dlen != (int32_t)rsize || 0 != memcmp(rdata, data, dlen)) {
        LOG_WARN("percent encoded dst/type return data error.");
        goto erro;
    }
    // 畸形查询值一律 404：非数字 dst
    if (ERR_OK != _harbor_expect_code(task, &sk, "/request?dst=abc&type=101",
                                      data, (size_t)dlen, "404")) {
        goto erro;
    }
    // '+' 按 plus2space 解码成前导空格，strtoull 会跳过空白把它当合法数值
    SNPRINTF(qurl, sizeof(qurl), "/request?dst=+%s&type=101", dstr);
    if (ERR_OK != _harbor_expect_code(task, &sk, qurl, data, (size_t)dlen, "404")) {
        goto erro;
    }
    // type 超 uint16：截断后会变成另一个合法 subtype（65537 -> 1）投给目标
    SNPRINTF(qurl, sizeof(qurl), "/request?dst=%s&type=65537", dstr);
    if (ERR_OK != _harbor_expect_code(task, &sk, qurl, data, (size_t)dlen, "404")) {
        goto erro;
    }
    // 尾随垃圾字符
    SNPRINTF(qurl, sizeof(qurl), "/request?dst=%s&type=1x", dstr);
    if (ERR_OK != _harbor_expect_code(task, &sk, qurl, data, (size_t)dlen, "404")) {
        goto erro;
    }
    ev_close(&task->loader->netev, &sk);
    return ERR_OK;
erro:
    ev_close(&task->loader->netev, &sk);
    return ERR_FAILED;
}
// harbor mTLS 负面:无 client 证书(_evssl 无证书)连 hbserver 应被 FAIL_IF_NO_PEER_CERT 拒。
// TLS1.3 下无证书 client 握手会先完成返回,server 验证失败后才异步发 certificate_required alert,
// 故 connect 成功不代表通过,须再发请求断言拿不到正常响应
static int32_t _timeout_habor_reject(task_ctx *task) {
#if WITH_SSL
    task_timeout_ctx *ctx = coro_get_arg(task);
    uint16_t port = (uint16_t)*_get_name_val(ctx->_ports, "harbor");
    sock_ctx sk;
    if (ERR_OK != coro_connect(task, PACK_HTTP, ctx->_evssl, "127.0.0.1", port, 0, NULL, &sk)) {
        return ERR_OK;// connect 阶段即被拒,符合预期
    }
    size_t rsize;
    void *pack = harbor_pack(ctx->_rpcname, 1, 2, "x", 1, &rsize);
    struct http_pack_ctx *rpack = coro_send(task, &sk, pack, rsize, NULL, 0);
    ev_close(&task->loader->netev, &sk);
    return (NULL == rpack) ? ERR_OK : ERR_FAILED;// 无证书被拒,不该拿到响应
#else
    (void)task;
    return ERR_OK;
#endif
}
// 失败即时落盘,不等轮末:中途 task_isclosing 提前 return 会把本轮的失败丢掉(见 _failed 注释)。
// 收尾已开始时不记:本任务每秒重跑一轮,main 判定全部通过后关连接,正在跑的那一轮必然失败,
// 那是关闭造成的;此前各轮的真失败已经粘在 _failed 上,不会因此漏掉
static inline void _timeout_failed(task_ctx *task, task_timeout_ctx *ctx) {
    if (task_isclosing(task)) {
        return;
    }
    ctx->_failed = 1;
    *ctx->_ok = 0;
}
static void _timeout(task_ctx *task, uint64_t sess) {
    (void)sess;
    task_timeout_ctx *ctx = coro_get_arg(task);
    if (ERR_OK != _timeout_sleep(task)) {
        _timeout_failed(task, ctx);
        LOG_WARN("sleep test error.");
    }
    if (task_isclosing(task)) {
        return;
    }
    _timeout_auto_close(task);
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _timeout_rpc(task)) {
        _timeout_failed(task, ctx);
        LOG_WARN("rpc call test error.");
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _timeout_udp(task)) {
        _timeout_failed(task, ctx);
        LOG_WARN("udp test error.");
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _timeout_tcp(task)) {
        _timeout_failed(task, ctx);
        LOG_WARN("tcp test error.");
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _timeout_http(task)) {
        _timeout_failed(task, ctx);
        LOG_WARN("http test error.");
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _timeout_ws(task)) {
        _timeout_failed(task, ctx);
        LOG_WARN("ws test error.");
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _timeout_habor(task)) {
        _timeout_failed(task, ctx);
        LOG_WARN("habor test error.");
    }
    if (task_isclosing(task)) {
        return;
    }
    // 只验一次——_timeout 每秒自我重挂，每轮都连一次，被拒握手的 SSL 告警会一直刷到收尾
    if (0 == ctx->_reject_done) {
        ctx->_reject_done = 1;
        if (ERR_OK != _timeout_habor_reject(task)) {
            _timeout_failed(task, ctx);
            LOG_WARN("habor reject test error.");
        }
    }
    *ctx->_ok = ctx->_failed ? 0 : 1;
    task_timeout(task, 0, 1000, _timeout);
}
static void _startup(task_ctx *task) {
    task_timeout(task, 0, 100, _timeout);
}
void task_timeout_start(loader_ctx *loader, const char *name,
    const char *rpcname, name_val_ctx *ports, void *evssl, void *hbssl,
    int32_t autoclose, int32_t *ok) {
    task_timeout_ctx *ctx;
    CALLOC(ctx, 1, sizeof(task_timeout_ctx));
    ctx->_rpcname = task_find_name(loader, rpcname);
    ctx->_autoclose = autoclose;
    ctx->_ports = ports;
    ctx->_evssl = evssl;
    ctx->_hbssl = hbssl;
    ctx->_ok = ok;
    coro_task_register(loader, name, 0, _startup, NULL, _free, ctx);
}
