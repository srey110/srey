#include "test_event.h"
#include "lib.h"
// 连接失败后发送的用例要看 watcher 的攒发链，投递多趟的用例要在 watcher 上挂 tick
#if defined(EV_IOCP)
#include "event/iocp.h"
#else
#include "event/uev.h"
#endif
#if defined(EV_EPOLL)
#include <sys/syscall.h>
#endif

// 延迟关闭用例：服务端收到第一段就回这条响应并 ev_close
#define LINGER_RESP "HTTP/1.1 411 Length Required\r\nContent-Length: 0\r\n\r\n"
// 各用例各占一个端口：Windows 监听口带 SO_EXCLUSIVEADDRUSE，立即重绑同端口会失败
#define LINGER_PORT 15090
// accept 选项用例的端口，紧跟延迟关闭那组(15090~15095)之后
#define ACP_OPTS_PORT 15096
#define UDP_CLOSE_PORT 15097
// UDP 端口复用按需打开的用例：独占与共用各占一个端口
#define UDP_EXCL_PORT 15059
#define UDP_SHARE_PORT 15060
// SSL 合并写用例：端口、服务端一共发的字节数、每批入队的小块数(须低于 MAX_SENDQ_CNT，否则入队即断连)
#define SSL_MERGE_PORT 15098
#define CLOSE_IN_CB_PORT 15099
// accept / connect 回调里关连接的用例
#define CLOSE_IN_ACP_PORT 15028
#define CLOSE_IN_CONN_PORT 15029
// 连接失败回调里关连接的用例：连一个没人监听的端口
#define CONN_FAIL_PORT 15030
// 连通后注册读事件失败、回调里还发数据的用例
#define CONN_FAIL_SEND_PORT 15032
// 指定 watcher 的用例：TCP 监听端口与 UDP 端口
#define LAUNCH_IDX_PORT 15022
#define LAUNCH_UDP_PORT 15023
// 只在部分线程上收连接的监听用例：两组各占一个端口
#define LISTEN_THRS_PORT 15033
#define LISTEN_THRS_PORT2 15034
// 在目标线程上 ev_connect 复用池中对象的用例：端口与客户端发、服务端回显的那条消息
#define CONN_POOL_PORT 15039
#define CONN_POOL_MSG "abc"
// send_multi 用例：客户端按下标轮流落在两条 event 线程上，按轮广播再单发，服务端攒齐后逐字比对
#define SMULTI_PORT 15036
#define SMULTI_INLOOP_PORT 15037
#define SMULTI_FREE_PORT 15038
#define SMULTI_NCLI 6
#define SMULTI_ROUNDS 40
#define SMULTI_REPEAT (MAX_SEND_NIOV + 8)// 末轮同一连接在列表里重复的次数：组还没跑完就会攒满一批冲出去
#define SMULTI_MAXLEN (2 + SMULTI_ROUNDS * 6 + SMULTI_REPEAT)
// 线程数多于 64(ev_send_multi 栈上计数表的格数)的用例：一个 UDP 与一个 TCP 客户端都落在最后一条线程上
#define SMULTI_WIDE_PORT 15052
#define SMULTI_WIDE_UDP_PORT 15053
#define SMULTI_WIDE_NTHS 65
#define SMULTI_WIDE_MSG "wide-multi"
#define SMULTI_WIDE_LEN (sizeof(SMULTI_WIDE_MSG) - 1)
// 别的线程投来的 CMD_ADD / CMD_LSN 还在队里时，目标线程上当场 ev_close / ev_unlisten 的用例
#define PEND_UDP_PORT 15054
#define PEND_LSN_PORT 15055
// UDP 关闭后当场 ud_free 的用例
#define UDP_UDFREE_PORT 15056
// 已挂上下文的连接切协议类型的用例
#define UD_PKTYPE_PORT 15088
// tick 回调里同线程发送失败要关 UDP 的用例
#define TICK_CLOSE_PORT 15089
// 收包回调里关、对端随即关写的用例
#define CLOSE_FIN_PORT 15057
// 发送队列超 MAX_SENDQ_CNT 断连的用例：端口，以及先塞满内核缓冲的那一大块
#define SENDQ_PORT 15058
#define SENDQ_BIG (1024 * 1024)
// 等隔离队列把对象真放掉：隔离期(usock.c 的 QTN_MS，500ms)满后，最多再隔 EVENT_CHECK_INTERVAL 轮空等才收一次
#define QTN_WAIT_MS (500 + EVENT_CHECK_INTERVAL * EVENT_WAIT_TIMEOUT + 200)
#define SSL_MERGE_TOTAL (256 * 1024)
#define SSL_MERGE_BATCH 512
// 读保活空闲时长的选项名，取法同 sock_keepalive
#if defined(TCP_KEEPIDLE)
    #define ACP_IDLE_OPT TCP_KEEPIDLE
#elif defined(TCP_KEEPALIVE) && !defined(OS_SUN)
    #define ACP_IDLE_OPT TCP_KEEPALIVE
#endif
// 忙连接用例里服务端一次回的大包：要大到内核发送缓冲装不下，发送队列留有积压、写事件挂着
#define LINGER_BIG (8 * 1024 * 1024)
// _linger_server 的服务端行为
typedef enum linger_mode {
    LINGER_CLOSE_IN_RECV = 0,   // 收到就回响应并关
    LINGER_CLOSE_IN_SENT,       // 只回响应，关放在 s_cb 里
    LINGER_CLOSE_BUSY           // 先回大包制造积压，第 2 字节卡住事件线程，第 3 字节再关
}linger_mode;

#if WITH_SSL
// mem BIO 之间搬一次数据，limit < 0 表示能搬多少搬多少。返回实际搬运字节数
static size_t _biopump(BIO *from, BIO *to, long limit) {
    char buf[16384];
    int32_t want = (limit < 0 || limit > (long)sizeof(buf)) ? (int32_t)sizeof(buf) : (int32_t)limit;
    int32_t n = BIO_read(from, buf, want);
    if (n <= 0) {
        return 0;
    }
    BIO_write(to, buf, n);
    return (size_t)n;
}
// 两个方向都搬空
static void _biopump_all(BIO *cw, BIO *sr, BIO *sw, BIO *cr) {
    while (_biopump(cw, sr, -1) || _biopump(sw, cr, -1)) {
        ;
    }
}
#endif
// 往发送队列塞一条待发数据，data 由 _evpub_off_buf_release / 冲刷成功后释放
static void _push_sendbuf(obuf_que *bufs, const char *s, size_t lens) {
    off_buf_ctx buf;
    ZERO(&buf, sizeof(buf));
    MALLOC(buf.data, lens);
    memcpy(buf.data, s, lens);
    buf.lens = lens;
    obuf_que_push(bufs, &buf);
}
#if WITH_SSL
// 探一次有没有数据可读：证否用，不等待
static int32_t _recv_none(SOCKET fd) {
    char c;
    return (int32_t)recv(fd, &c, 1, 0) <= 0 ? 1 : 0;
}
#endif
// 从 fd 上收满 want 字节，回带实收字节数；loopback 对上数据未必立刻到，故有界重试
static size_t _recv_all(SOCKET fd, char *out, size_t want) {
    size_t got = 0;
    int32_t n;
    for (int32_t i = 0; i < 200 && got < want; i++) {
        n = (int32_t)recv(fd, out + got, (int32_t)(want - got), 0);
        if (n > 0) {
            got += (size_t)n;
            continue;
        }
        MSLEEP(1);
    }
    return got;
}
// 阻塞连本机 port，连不上返回 INVALID_SOCK。rcvbuf 非 0 时连接前设接收缓冲(连上之后再设管不到通告窗口)
static SOCKET _connect_local(uint16_t port, int32_t rcvbuf) {
    netaddr_ctx addr;
    SOCKET fd;
    if (ERR_OK != netaddr_set(&addr, "127.0.0.1", port)) {
        return INVALID_SOCK;
    }
    fd = sock_create_cloexec(netaddr_family(&addr), SOCK_STREAM, 0, 0);
    if (INVALID_SOCK == fd) {
        return fd;
    }
    if (0 != rcvbuf) {
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (char *)&rcvbuf, (socklen_t)sizeof(rcvbuf));
    }
    if (0 != connect(fd, netaddr_addr(&addr), netaddr_size(&addr))) {
        CLOSE_SOCK(fd);
    }
    return fd;
}
// 关闭前冲刷 _evpub_close_flush_tcp。集成用例（task_close_flush）只走得到常规冲刷那一支，
// 队列空早退、各 SSL 状态下冲不冲这几支造不出确定的现场，在这里直接构造
static void test_evpub_close_flush(CuTest *tc) {
    SOCKET sk[2];
    CuAssertIntEquals(tc, ERR_OK, sock_pair(sk, 1));
    obuf_que bufs;
    obuf_que_init(&bufs, 8);
    char got[32];
    size_t nrecv;
    size_t wb;

    /* 1) 队列空：直接早退，wb_size 不动 */
    wb = 12345;
    _evpub_close_flush_tcp(sk[0], &bufs, STATUS_ESTABLISHED, &wb, NULL);
    CuAssertTrue(tc, 12345 == wb);

    /* 2) 常规冲刷：多条一次写完，队列排空、wb_size 扣干、对端收到拼好的内容 */
    _push_sendbuf(&bufs, "hello", 5);
    _push_sendbuf(&bufs, " world", 6);
    wb = 11;
    _evpub_close_flush_tcp(sk[0], &bufs, STATUS_ESTABLISHED, &wb, NULL);
    CuAssertTrue(tc, 0 == obuf_que_size(&bufs));
    CuAssertTrue(tc, 0 == wb);
    ZERO(got, sizeof(got));
    nrecv = _recv_all(sk[1], got, 11);
    CuAssertTrue(tc, 11 == nrecv);
    CuAssertTrue(tc, 0 == memcmp(got, "hello world", 11));

#if WITH_SSL
    /* 3) KEYUPDATE_WRITE：挂着一个待重试的 SSL_read，此时不能再调 SSL_write，整队不冲 */
    _push_sendbuf(&bufs, "blocked", 7);
    wb = 7;
    _evpub_close_flush_tcp(sk[0], &bufs, STATUS_ESTABLISHED | STATUS_KEYUPDATE_WRITE, &wb, NULL);
    CuAssertTrue(tc, 1 == obuf_que_size(&bufs));
    CuAssertTrue(tc, 7 == wb);
    CuAssertTrue(tc, 0 != _recv_none(sk[1]));
    _evpub_off_buf_clear(&bufs);

    /* 4) SSLEXCHANGE 不再挡：该态下 ssl 恒为 NULL、队列里全是明文，照常冲出去 */
    _push_sendbuf(&bufs, "plain", 5);
    wb = 5;
    _evpub_close_flush_tcp(sk[0], &bufs, STATUS_ESTABLISHED | STATUS_SSLEXCHANGE, &wb, NULL);
    CuAssertTrue(tc, 0 == obuf_que_size(&bufs));
    CuAssertTrue(tc, 0 == wb);
    ZERO(got, sizeof(got));
    nrecv = _recv_all(sk[1], got, 5);
    CuAssertTrue(tc, 5 == nrecv);
    CuAssertTrue(tc, 0 == memcmp(got, "plain", 5));

    /* 5) AUTHSSL 同样不挡：守卫只剩 KEYUPDATE_WRITE 一位，别再按"SSL 相关状态一律不冲"加回来。
          生产里这个组合不可达——两个平台仅有的两处入队都过 _evpub_sendqu_check_tcp，
          它对 AUTHSSL/SSLEXCHANGE 都拒收，握手期队列必空、上面那道 obuf_que_size 早退就返回了 */
    _push_sendbuf(&bufs, "authssl", 7);
    wb = 7;
    _evpub_close_flush_tcp(sk[0], &bufs, STATUS_ESTABLISHED | STATUS_AUTHSSL, &wb, NULL);
    CuAssertTrue(tc, 0 == obuf_que_size(&bufs));
    CuAssertTrue(tc, 0 == wb);
    ZERO(got, sizeof(got));
    nrecv = _recv_all(sk[1], got, 7);
    CuAssertTrue(tc, 7 == nrecv);
    CuAssertTrue(tc, 0 == memcmp(got, "authssl", 7));
#endif

    obuf_que_free(&bufs);
    CLOSE_SOCK(sk[0]);
    CLOSE_SOCK(sk[1]);
}
// FIN 必须与 RST 分开报：两者过去都返 ERR_FAILED，close-delimited 的 body 就没法判完整
static void test_evpub_read_fin(CuTest *tc) {
    SOCKET sk[2];
    CuAssertIntEquals(tc, ERR_OK, sock_pair(sk, 1));
    char rbuf[8];
    IOV_TYPE iov;
    iov.IOV_PTR_FIELD = rbuf;
    iov.IOV_LEN_FIELD = (IOV_LEN_TYPE)sizeof(rbuf);
    size_t nread = 1;

    /* 1) 没数据也没关：EAGAIN 算成功，等下一次可读事件 */
    CuAssertIntEquals(tc, ERR_OK, _evpub_sock_read(sk[0], &iov, 1, NULL, &nread));
    CuAssertTrue(tc, 0 == nread);

    /* 2) 对端关掉：读到 FIN，报 1 而非 ERR_FAILED。
          loopback 上 FIN 未必立刻可见，未到时读的是 EAGAIN(ERR_OK)，故有界重试 */
    CLOSE_SOCK(sk[1]);
    int32_t rtn = ERR_OK;
    int32_t i;
    for (i = 0; i < 200 && ERR_OK == rtn; i++) {
        nread = 1;
        rtn = _evpub_sock_read(sk[0], &iov, 1, NULL, &nread);
        if (ERR_OK == rtn) {
            MSLEEP(1);
        }
    }
    CuAssertIntEquals(tc, 1, rtn);
    CuAssertTrue(tc, 0 == nread);

    /* 3) 读错误既不是 EAGAIN 也不是 FIN：必须报 ERR_FAILED。CLOSE_SOCK 把句柄置成
          INVALID_SOCK，读它得到的 errno 不在 ERR_RW_RETRIABLE 里。
          这一档与 (2) 的分野决定 close_type 判 ABORT 还是 ORDERLY——并进 FIN 那支的话，
          连接被重置时 close-delimited body 会被判成完整 */
    CLOSE_SOCK(sk[0]);
    nread = 1;
    CuAssertIntEquals(tc, ERR_FAILED, _evpub_sock_read(sk[0], &iov, 1, NULL, &nread));
}
// close_type 四档判定 + FIN / TRUNCATED 优先
static void test_evpub_close_type(CuTest *tc) {
    int32_t st;

    /* 1) 三个位都没置：本地主动关 */
    st = STATUS_ESTABLISHED;
    CuAssertIntEquals(tc, CLOSE_TYPE_LOCAL, _evpub_close_type(st));

    /* 2) 读到 FIN：有序结束 */
    st = STATUS_ESTABLISHED;
    _evpub_mark_close(&st, 1);
    CuAssertTrue(tc, BIT_CHECK(st, STATUS_PEER_FIN));
    CuAssertIntEquals(tc, CLOSE_TYPE_ORDERLY, _evpub_close_type(st));

    /* 3) 读写失败：异常中断 */
    st = STATUS_ESTABLISHED;
    _evpub_mark_close(&st, ERR_FAILED);
    CuAssertTrue(tc, BIT_CHECK(st, STATUS_PEER_ABORT));
    CuAssertIntEquals(tc, CLOSE_TYPE_ABORT, _evpub_close_type(st));

#if WITH_SSL
    /* 4) TLS 无 close_notify 断开：截断档，与 ABORT 分开 */
    st = STATUS_ESTABLISHED;
    _evpub_mark_close(&st, 2);
    CuAssertTrue(tc, BIT_CHECK(st, STATUS_PEER_TRUNCATED));
    CuAssertTrue(tc, !BIT_CHECK(st, STATUS_PEER_ABORT));
    CuAssertIntEquals(tc, CLOSE_TYPE_TRUNCATED, _evpub_close_type(st));
#endif

    /* 5) 先 FIN 再叠 ABORT 仍判有序：IOCP 侧收完成与错误处理是两条路径，会叠加 */
    st = STATUS_ESTABLISHED;
    _evpub_mark_close(&st, 1);
    _evpub_mark_close(&st, ERR_FAILED);
    CuAssertIntEquals(tc, CLOSE_TYPE_ORDERLY, _evpub_close_type(st));

#if WITH_SSL
    /* 6) TRUNCATED 叠 ABORT 仍判截断：同上，置位处不互斥 */
    st = STATUS_ESTABLISHED;
    _evpub_mark_close(&st, 2);
    _evpub_mark_close(&st, ERR_FAILED);
    CuAssertIntEquals(tc, CLOSE_TYPE_TRUNCATED, _evpub_close_type(st));
#endif

    /* 7) 不碰其他状态位 */
    CuAssertTrue(tc, BIT_CHECK(st, STATUS_ESTABLISHED));

    /* 8) 本端已在关(STATUS_ERROR)后才读到 FIN / 出错：不再标 PEER_*，仍判 LOCAL。
          回调里 ev_close 后同一次读里带回的 FIN 走的就是这条 */
    st = STATUS_ESTABLISHED | STATUS_ERROR;
    _evpub_mark_close(&st, 1);
    _evpub_mark_close(&st, ERR_FAILED);
#if WITH_SSL
    _evpub_mark_close(&st, 2);
#endif
    CuAssertIntEquals(tc, CLOSE_TYPE_LOCAL, _evpub_close_type(st));
}
#if WITH_SSL
// 非阻塞下两端轮流推进握手：谁 WANT_READ/WANT_WRITE 就换另一边喂，直到两边都成
static int32_t _ssl_shake(SSL *cli, SSL *srv) {
    int32_t i, c = 1, s = 1;
    for (i = 0; i < 500 && (ERR_OK != c || ERR_OK != s); i++) {
        if (ERR_OK != c) {
            c = evssl_tryconn(cli);
            if (ERR_FAILED == c) {
                return ERR_FAILED;
            }
        }
        if (ERR_OK != s) {
            s = evssl_tryacpt(srv);
            if (ERR_FAILED == s) {
                return ERR_FAILED;
            }
        }
        MSLEEP(1);// 握手报文对上未必立刻可见，不睡的话整轮预算不到 1ms（同 _recv_all）
    }
    return (ERR_OK == c && ERR_OK == s) ? ERR_OK : ERR_FAILED;
}
// 收拾一对握完手的 SSL。六件都用自清空的宏/在调用方保证非空，用例单独收过某件后再调是空操作
static void _ssl_drop(SOCKET sk[2], SSL **cli, SSL **srv, evssl_ctx *sc, evssl_ctx *cc) {
    FREE_SSL(*cli);
    FREE_SSL(*srv);
    CLOSE_SOCK(sk[0]);
    CLOSE_SOCK(sk[1]);
    evssl_free(sc);
    evssl_free(cc);
}
// close_notify 与 EOF 都未必立刻可见，未到时读的是 WANT_READ(ERR_OK)，故有界重试（同 test_evpub_read_fin）
static int32_t _ssl_read_until(SSL *cli, char *buf, size_t cap, size_t *readed) {
    int32_t rtn = ERR_OK;
    int32_t i;
    for (i = 0; i < 200 && ERR_OK == rtn; i++) {
        *readed = 1;
        rtn = evssl_read(cli, buf, cap, readed);
        if (ERR_OK == rtn) {
            MSLEEP(1);
        }
    }
    return rtn;
}
// 建服务端与客户端两个 evssl_ctx。返回 1 成功；0 表示证书没生成（用例跳过）；-1 是真失败
static int32_t _ssl_ctxs(evssl_ctx **sc, evssl_ctx **cc) {
    const char *local = procpath();
    char ca[PATH_LENS], crt[PATH_LENS], key[PATH_LENS];
    SNPRINTF(ca, sizeof(ca), "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "ca.crt");
    SNPRINTF(crt, sizeof(crt), "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "server.crt");
    SNPRINTF(key, sizeof(key), "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "server.key");
    // 只有证书文件不在才算"跳过"。三个都在却建不出 ctx（证书与私钥不匹配、CA 载不进等）
    // 是真回归，evssl_new 一律返 NULL 分不出来，混进跳过那条路会被 CuTest 记成通过
    if (ERR_OK != isfile(ca)
        || ERR_OK != isfile(crt)
        || ERR_OK != isfile(key)) {
        return 0;
    }
    *sc = evssl_new(ca, crt, key, SSL_FILETYPE_PEM);
    if (NULL == *sc) {
        return -1;
    }
    *cc = evssl_new(NULL, NULL, NULL, SSL_FILETYPE_PEM);
    if (NULL == *cc) {
        evssl_free(*sc);
        return -1;
    }
    return 1;
}
// 建一对握完手的 SSL。返回值同 _ssl_ctxs
static int32_t _ssl_pair(SOCKET sk[2], SSL **cli, SSL **srv, evssl_ctx **sc, evssl_ctx **cc) {
    int32_t rtn = _ssl_ctxs(sc, cc);
    if (1 != rtn) {
        return rtn;
    }
    if (ERR_OK != sock_pair(sk, 1)) {
        evssl_free(*sc);
        evssl_free(*cc);
        return -1;
    }
    *cli = evssl_setfd(*cc, sk[0]);
    *srv = evssl_setfd(*sc, sk[1]);
    if (NULL == *cli || NULL == *srv
        || ERR_OK != _ssl_shake(*cli, *srv)) {
        // 调用方在这条路径上会断言失败并 longjmp 出去，自己不收就会挂在收尾的 memcheck 上
        _ssl_drop(sk, cli, srv, *sc, *cc);
        return -1;
    }
    return 1;
}
// evssl_read 对"连接怎么结束的"三档分类必须分得开：收到 close_notify 是有序结束（返 1），
// 对端不发 close_notify 就断是截断（返 2），真正的协议错仍是 ERR_FAILED。
// 这三档一路传到 CLOSE 消息的 close_type 上，混在一起就分不出"收全了"与"被人截断了"。
// 判据见 evssl_read 的 <returns>
static void test_evssl_read_close_notify(CuTest *tc) {
    SOCKET sk[2];
    SSL *cli = NULL, *srv = NULL;
    evssl_ctx *sc = NULL, *cc = NULL;
    char buf[64];
    size_t readed;
    int32_t rtn, sent;

    /* 1) 对端 SSL_shutdown 发了 close_notify：读侧必须报 1 */
    rtn = _ssl_pair(sk, &cli, &srv, &sc, &cc);
    if (0 == rtn) {
        PRINT("skip test_evssl_read_close_notify, run bin/keys/create.sh first.");
        return;
    }
    CuAssertIntEquals(tc, 1, rtn);
    evssl_shutdown(srv, sk[1], SHUT_RD);
    rtn = _ssl_read_until(cli, buf, sizeof(buf), &readed);
    // 先收拾再断言：CuAssert 失败走 longjmp，夹在中间会漏掉收尾，一次真失败还要多报一笔假泄漏（同 _ssl_pair）
    _ssl_drop(sk, &cli, &srv, sc, cc);
    CuAssertIntEquals(tc, 1, rtn);
    CuAssertTrue(tc, 0 == readed);

    /* 1b) 延迟关闭走的是先发 close_notify 再关写(SHUT_WR)：读侧照样报 1。顺序颠倒的话
          close_notify 写不出去，读侧只看到 FIN，报 2（截断） */
    CuAssertIntEquals(tc, 1, _ssl_pair(sk, &cli, &srv, &sc, &cc));
    evssl_shutdown(srv, sk[1], SHUT_WR);
    rtn = _ssl_read_until(cli, buf, sizeof(buf), &readed);
    _ssl_drop(sk, &cli, &srv, sc, cc);
    CuAssertIntEquals(tc, 1, rtn);
    CuAssertTrue(tc, 0 == readed);

    /* 2) 对端直接关 TCP、不发 close_notify：报 2（截断）。既不能当有序结束——那样
       close-delimited 的 body 会被判成收全了；也不能混进协议错——那样正常收完的响应
       会被判成失败。FREE_SSL 不发 close_notify（只有 SSL_shutdown 发），故这里就是裸 FIN */
    CuAssertIntEquals(tc, 1, _ssl_pair(sk, &cli, &srv, &sc, &cc));
    FREE_SSL(srv);
    CLOSE_SOCK(sk[1]);
    rtn = _ssl_read_until(cli, buf, sizeof(buf), &readed);
    _ssl_drop(sk, &cli, &srv, sc, cc);
    CuAssertIntEquals(tc, 2, rtn);

    /* 3) 往裸 socket 灌非 TLS 字节：记录头的版本号就对不上，必须仍是 ERR_FAILED，
       不能被截断那一档吞掉（两者都走 SSL_ERROR_SSL，只差 reason 码） */
    CuAssertIntEquals(tc, 1, _ssl_pair(sk, &cli, &srv, &sc, &cc));
    memset(buf, 0xFF, sizeof(buf));
    sent = (int32_t)send(sk[1], buf, (int32_t)sizeof(buf), 0);
    rtn = _ssl_read_until(cli, buf, sizeof(buf), &readed);
    _ssl_drop(sk, &cli, &srv, sc, cc);
    CuAssertIntEquals(tc, (int32_t)sizeof(buf), sent);
    CuAssertIntEquals(tc, ERR_FAILED, rtn);
}
#endif
#if WITH_SSL
// STATUS_KEYUPDATE_READ 赖以存在的前提:SSL_write 真的会返回 WANT_READ。
// 条件不是 KeyUpdate(它走纯写路径)，而是对端把一条 post-handshake 消息拆到多条 TLS 记录、
// 后一条还没到——此时状态机停在读子状态,任何 SSL_write 都得先等那条记录。
// 用 mem BIO 精确控制记录投递:不碰网络、不碰事件层、无 timing。
// OpenSSL 换版本后若这个前提不成立,这里先红,而不是等审计员把接力代码当死码删掉
static void test_ssl_write_wants_read(CuTest *tc) {
    const char *local = procpath();
    char ca[PATH_LENS], crt[PATH_LENS], key[PATH_LENS], ccrt[PATH_LENS], ckey[PATH_LENS];
    SNPRINTF(ca, sizeof(ca), "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "ca.crt");
    SNPRINTF(crt, sizeof(crt), "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "server.crt");
    SNPRINTF(key, sizeof(key), "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "server.key");
    SNPRINTF(ccrt, sizeof(ccrt), "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "client.crt");
    SNPRINTF(ckey, sizeof(ckey), "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "client.key");
    if (ERR_OK != isfile(ca)
        || ERR_OK != isfile(crt)
        || ERR_OK != isfile(key)
        || ERR_OK != isfile(ccrt)
        || ERR_OK != isfile(ckey)) {
        PRINT("skip test_ssl_write_wants_read, run bin/keys/create.sh first.");
        return;
    }
    SSL_CTX *cctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX *sctx = SSL_CTX_new(TLS_server_method());
    CuAssertPtrNotNull(tc, cctx);
    CuAssertPtrNotNull(tc, sctx);
    SSL_CTX_set_min_proto_version(cctx, TLS1_3_VERSION);
    SSL_CTX_set_min_proto_version(sctx, TLS1_3_VERSION);
    CuAssertIntEquals(tc, 1, SSL_CTX_use_certificate_file(sctx, crt, SSL_FILETYPE_PEM));
    CuAssertIntEquals(tc, 1, SSL_CTX_use_PrivateKey_file(sctx, key, SSL_FILETYPE_PEM));
    // mTLS:客户端证书会被服务端塞进 NewSessionTicket,把它撑过下面那个 512 的分片阈值
    CuAssertIntEquals(tc, 1, SSL_CTX_use_certificate_file(cctx, ccrt, SSL_FILETYPE_PEM));
    CuAssertIntEquals(tc, 1, SSL_CTX_use_PrivateKey_file(cctx, ckey, SSL_FILETYPE_PEM));
    SSL_CTX_set_verify(sctx, SSL_VERIFY_PEER, NULL);
    CuAssertIntEquals(tc, 1, SSL_CTX_load_verify_locations(sctx, ca, NULL));
    SSL_CTX_set_verify(cctx, SSL_VERIFY_NONE, NULL);
    SSL_CTX_set_num_tickets(sctx, 0);// 握手尾部不发票,留到握手完成后手工发,才控得住时机
    SSL *cli = SSL_new(cctx);
    SSL *srv = SSL_new(sctx);
    BIO *cli_rb = BIO_new(BIO_s_mem());
    BIO *cli_wb = BIO_new(BIO_s_mem());
    BIO *srv_rb = BIO_new(BIO_s_mem());
    BIO *srv_wb = BIO_new(BIO_s_mem());
    SSL_set_bio(cli, cli_rb, cli_wb);
    SSL_set_bio(srv, srv_rb, srv_wb);
    SSL_set_connect_state(cli);
    SSL_set_accept_state(srv);
    char buf[16384];
    int32_t i;
    for (i = 0; i < 20; i++) {
        SSL_do_handshake(cli);
        _biopump_all(cli_wb, srv_rb, srv_wb, cli_rb);
        SSL_do_handshake(srv);
        _biopump_all(cli_wb, srv_rb, srv_wb, cli_rb);
        if (SSL_is_init_finished(cli) && SSL_is_init_finished(srv)) {
            break;
        }
    }
    CuAssertIntEquals(tc, 1, SSL_is_init_finished(cli));
    CuAssertIntEquals(tc, 1, SSL_is_init_finished(srv));
    CuAssertStrEquals(tc, "TLSv1.3", SSL_get_version(cli));
    // 双向各走一轮,确保没有残留记录干扰后面的"只投第一条"
    SSL_write(srv, "a", 1);
    _biopump_all(cli_wb, srv_rb, srv_wb, cli_rb);
    SSL_read(cli, buf, sizeof(buf));
    _biopump_all(cli_wb, srv_rb, srv_wb, cli_rb);
    SSL_write(cli, "b", 1);
    _biopump_all(cli_wb, srv_rb, srv_wb, cli_rb);
    SSL_read(srv, buf, sizeof(buf));
    _biopump_all(cli_wb, srv_rb, srv_wb, cli_rb);
    CuAssertTrue(tc, 0 == BIO_ctrl_pending(srv_wb));
    // 把 NST 切成多条记录,只投第一条
    SSL_set_max_send_fragment(srv, 512);
    CuAssertIntEquals(tc, 1, SSL_new_session_ticket(srv));
    CuAssertIntEquals(tc, 1, SSL_write(srv, "z", 1));
    CuAssertTrue(tc, BIO_ctrl_pending(srv_wb) > 512);
    unsigned char hdr[5];
    CuAssertIntEquals(tc, 5, BIO_read(srv_wb, hdr, 5));
    long rlen = (long)((hdr[3] << 8) | hdr[4]);
    CuAssertIntEquals(tc, 5, BIO_write(cli_rb, hdr, 5));
    while (rlen > 0) {
        rlen -= (long)_biopump(srv_wb, cli_rb, rlen);
    }
    CuAssertTrue(tc, BIO_ctrl_pending(srv_wb) > 0);// 第二条扣在手里
    // 客户端读到半条 NST,状态机停在读子状态
    CuAssertIntEquals(tc, -1, SSL_read(cli, buf, sizeof(buf)));
    CuAssertIntEquals(tc, 1, SSL_in_init(cli));
    // 这就是本用例要钉的那一行:此刻 SSL_write 必须报 want_read
    CuAssertIntEquals(tc, -1, SSL_write(cli, "w", 1));
    CuAssertIntEquals(tc, 1, SSL_want_read(cli));
    CuAssertIntEquals(tc, SSL_ERROR_WANT_READ, SSL_get_error(cli, -1));
    // 补上第二条记录后必须自行恢复,否则就不是"等数据"而是卡死
    _biopump(srv_wb, cli_rb, -1);
    CuAssertIntEquals(tc, 1, SSL_write(cli, "w", 1));
    CuAssertIntEquals(tc, 0, SSL_want_read(cli));
    SSL_free(cli);
    SSL_free(srv);
    SSL_CTX_free(cctx);
    SSL_CTX_free(sctx);
}
#endif
// accept 出的连接上读到的 nodelay / 保活开关(0 或 1) 与保活空闲时长(秒)，-1 表示还没取到
static atomic_t _g_acp_nodelay;
static atomic_t _g_acp_keep;
static atomic_t _g_acp_idle;
static int32_t _acp_getopt(SOCKET fd, int32_t level, int32_t opt) {
    int32_t v = 0;
    socklen_t lens = (socklen_t)sizeof(v);
    if (0 != getsockopt(fd, level, opt, (char *)&v, &lens)) {
        return -2;
    }
    return v;
}
static int32_t _acp_opts_on_accept(ev_ctx *ev, sock_ctx *sk, ud_cxt *ud) {
    (void)ev;
    (void)ud;
    ATOMIC_SET(&_g_acp_keep, 0 != _acp_getopt(sk->fd, SOL_SOCKET, SO_KEEPALIVE));
#ifdef ACP_IDLE_OPT
    ATOMIC_SET(&_g_acp_idle, _acp_getopt(sk->fd, IPPROTO_TCP, ACP_IDLE_OPT));
#endif
    ATOMIC_SET(&_g_acp_nodelay, 0 != _acp_getopt(sk->fd, IPPROTO_TCP, TCP_NODELAY));// 最后写，主线程拿它当"取完了"
    return ERR_OK;
}
static void _acp_opts_on_recv(ev_ctx *ev, sock_ctx *sk,
                              int32_t client, buffer_ctx *buf, size_t size, ud_cxt *ud) {
    (void)ev; (void)sk; (void)client; (void)size; (void)ud;
    buffer_drain(buf, buffer_size(buf));
}
// accept 出的连接在 acp_cb 里就得带着 nodelay、保活与 KEEPALIVE_TIME 的空闲时长：
// 这几项多数平台靠从监听 socket 继承(ACCEPT_INHERIT_OPTS)，内核哪天不继承了只会悄悄丢
static void test_ev_accept_opts(CuTest *tc) {
    ev_ctx ev;
    cbs_ctx cbs;
    uint64_t id;
    SOCKET fd = INVALID_SOCK;
    int32_t i, lrtn, connected;
    ZERO(&cbs, sizeof(cbs));
    cbs.acp_cb = _acp_opts_on_accept;
    cbs.r_cb = _acp_opts_on_recv;
    ATOMIC_SET(&_g_acp_nodelay, -1);
    ATOMIC_SET(&_g_acp_keep, -1);
    ATOMIC_SET(&_g_acp_idle, -1);
    ev_init(&ev, 1, NULL);
    lrtn = ev_listen(&ev, NULL, "127.0.0.1", ACP_OPTS_PORT, &cbs, NULL, NULL, 0, &id);
    MSLEEP(50);// listen 落地是异步的
    if (ERR_OK == lrtn) {
        fd = _connect_local(ACP_OPTS_PORT, 0);
    }
    connected = INVALID_SOCK != fd;
    for (i = 0; connected && i < 1000 && -1 == (int32_t)ATOMIC_GET(&_g_acp_nodelay); i++) {
        MSLEEP(1);
    }
    CLOSE_SOCK(fd);
    ev_free(&ev);
    // 断言放 ev_free 之后，理由同 test_ev_close_in_acp_conn_deferred
    CuAssertIntEquals(tc, ERR_OK, lrtn);
    CuAssertTrue(tc, connected);
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_acp_nodelay));
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_acp_keep));
#ifdef ACP_IDLE_OPT
    CuAssertIntEquals(tc, KEEPALIVE_TIME, (int32_t)ATOMIC_GET(&_g_acp_idle));
#endif
}
// UDP 用例：已投递的 datagram 数与关闭回调次数
static atomic_t _g_udp_nrecv;
static atomic_t _g_udp_nclose;
static void _udp_close_on_recvfrom(ev_ctx *ev, sock_ctx *sk,
                                   char *buf, size_t size, netaddr_ctx *addr, ud_cxt *ud) {
    (void)buf; (void)size; (void)addr; (void)ud;
    if (1 == ATOMIC_ADD(&_g_udp_nrecv, 1)) {
        ev_close(ev, sk);// 事件线程上同步关
    }
}
static void _udp_close_on_close(ev_ctx *ev, sock_ctx *sk, int32_t client, int32_t erro, ud_cxt *ud) {
    (void)ev; (void)sk; (void)client; (void)erro; (void)ud;
    ATOMIC_ADD(&_g_udp_nclose, 1);
}
// 收包回调里关 socket：一次读事件里已收上来的后续 datagram 不得再投递(批量收时它们已在本地缓冲里)，
// 关闭回调只走一次。不论几包落在同一批，投递数都必须恰好是 2
static void test_ev_udp_close_in_recv(CuTest *tc) {
    ev_ctx ev;
    cbs_ctx cbs;
    netaddr_ctx addr;
    sock_ctx sk;
    SOCKET fd = INVALID_SOCK;
    int32_t i, urtn;
    ZERO(&cbs, sizeof(cbs));
    cbs.rf_cb = _udp_close_on_recvfrom;
    cbs.c_cb = _udp_close_on_close;
    ATOMIC_SET(&_g_udp_nrecv, 0);
    ATOMIC_SET(&_g_udp_nclose, 0);
    ev_init(&ev, 1, NULL);
    urtn = ev_udp(&ev, "127.0.0.1", UDP_CLOSE_PORT, &cbs, NULL, INVALID_INDEX, &sk);
    MSLEEP(50);
    if (ERR_OK == urtn
        && ERR_OK == netaddr_set(&addr, "127.0.0.1", UDP_CLOSE_PORT)) {
        fd = sock_create_cloexec(netaddr_family(&addr), SOCK_DGRAM, 0, 0);
    }
    if (INVALID_SOCK != fd) {
        for (i = 0; i < 16; i++) {
            sendto(fd, "0123456789", 10, 0, netaddr_addr(&addr), netaddr_size(&addr));
        }
        for (i = 0; i < 500 && 0 == (int32_t)ATOMIC_GET(&_g_udp_nclose); i++) {
            MSLEEP(1);
        }
        MSLEEP(50);// 关错了的话后面的包还会陆续投递，多等一会儿再数
    }
    CLOSE_SOCK(fd);
    ev_free(&ev);
    // 断言放 ev_free 之后，理由同 test_ev_close_in_acp_conn_deferred
    CuAssertIntEquals(tc, ERR_OK, urtn);
    CuAssertIntEquals(tc, 2, (int32_t)ATOMIC_GET(&_g_udp_nrecv));
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_udp_nclose));
}
static void _udp_reuse_on_recvfrom(ev_ctx *ev, sock_ctx *sk,
                                   char *buf, size_t size, netaddr_ctx *addr, ud_cxt *ud) {
    (void)ev; (void)sk; (void)buf; (void)size; (void)addr; (void)ud;
}
// UDP 端口复用按需打开：默认独占，同一 addr:port 再绑失败(带不带 UD_REUSEADDR 都一样)；
// 两个都带 UD_REUSEADDR 时能共用
static void test_ev_udp_reuseaddr(CuTest *tc) {
    ev_ctx ev;
    cbs_ctx cbs;
    ud_cxt share;
    sock_ctx sk;
    int32_t rtn[5];
    ZERO(&cbs, sizeof(cbs));
    cbs.rf_cb = _udp_reuse_on_recvfrom;
    ZERO(&share, sizeof(share));
    BIT_SET(share.flags, UD_REUSEADDR);
    ev_init(&ev, 1, NULL);
    rtn[0] = ev_udp(&ev, "127.0.0.1", UDP_EXCL_PORT, &cbs, NULL, INVALID_INDEX, &sk);
    rtn[1] = ev_udp(&ev, "127.0.0.1", UDP_EXCL_PORT, &cbs, NULL, INVALID_INDEX, &sk);
    rtn[2] = ev_udp(&ev, "127.0.0.1", UDP_EXCL_PORT, &cbs, &share, INVALID_INDEX, &sk);
    rtn[3] = ev_udp(&ev, "127.0.0.1", UDP_SHARE_PORT, &cbs, &share, INVALID_INDEX, &sk);
    rtn[4] = ev_udp(&ev, "127.0.0.1", UDP_SHARE_PORT, &cbs, &share, INVALID_INDEX, &sk);
    ev_free(&ev);
    // 断言放 ev_free 之后，理由同 test_ev_close_in_acp_conn_deferred
    CuAssertIntEquals(tc, ERR_OK, rtn[0]);
    CuAssertIntEquals(tc, ERR_FAILED, rtn[1]);
    CuAssertIntEquals(tc, ERR_FAILED, rtn[2]);
    CuAssertIntEquals(tc, ERR_OK, rtn[3]);
    CuAssertIntEquals(tc, ERR_OK, rtn[4]);
}
// 回调里关连接的用例：关闭回调次数，以及 r_cb 发完那串数据后看到的状态(1 连接仍在、关闭回调没来，2 已被当场关掉)
static atomic_t _g_incb_closed;
static atomic_t _g_incb_state;
// 回调里关写再连发一串：攒满那次当场发、失败要关连接，回调里看到的连接仍得有效
static void _incb_burst(ev_ctx *ev, sock_ctx *sk) {
    int32_t i;
    shutdown(sk->fd, SHUT_WR);// 之后每次写都必失败
    // 攒满 MAX_SEND_NIOV 条那次会当场发(Unix writev / IOCP 同步发一批)，失败要关这条连接
    for (i = 0; i <= MAX_SEND_NIOV; i++) {
        ev_send(ev, sk, "x", 1, 1);
    }
    ATOMIC_SET(&_g_incb_state, (0 == ATOMIC_GET(&_g_incb_closed)
                                && !sock_is_invalid(sk) && 0 != sk->skid) ? 1 : 2);
}
static void _incb_on_recv(ev_ctx *ev, sock_ctx *sk,
                          int32_t client, buffer_ctx *buf, size_t size, ud_cxt *ud) {
    (void)client; (void)size; (void)ud;
    buffer_drain(buf, buffer_size(buf));
    _incb_burst(ev, sk);
}
static int32_t _incb_on_acp(ev_ctx *ev, sock_ctx *sk, ud_cxt *ud) {
    (void)ud;
    _incb_burst(ev, sk);
    return ERR_OK;
}
static int32_t _incb_on_conn(ev_ctx *ev, sock_ctx *sk, int32_t erro, ud_cxt *ud) {
    (void)ud;
    if (ERR_OK == erro) {
        _incb_burst(ev, sk);
    }
    return ERR_OK;
}
static void _incb_drain(ev_ctx *ev, sock_ctx *sk,
                        int32_t client, buffer_ctx *buf, size_t size, ud_cxt *ud) {
    (void)ev; (void)sk; (void)client; (void)size; (void)ud;
    buffer_drain(buf, buffer_size(buf));
}
static void _incb_on_close(ev_ctx *ev, sock_ctx *sk, int32_t client, int32_t erro, ud_cxt *ud) {
    (void)ev; (void)sk; (void)client; (void)erro; (void)ud;
    ATOMIC_ADD(&_g_incb_closed, 1);
}
// 收包回调里同线程发送失败：关连接要推迟到回调返回之后，回调里看到的连接始终有效，
// 关闭回调(推出 CLOSE)只来一次且排在回调之后。协议层在解包里发认证包、握手响应就是这个形状
static void test_ev_close_in_recv_deferred(CuTest *tc) {
    ev_ctx ev;
    cbs_ctx cbs;
    uint64_t id;
    SOCKET fd = INVALID_SOCK;
    int32_t i, lrtn, sent = 0;
    ZERO(&cbs, sizeof(cbs));
    cbs.r_cb = _incb_on_recv;
    cbs.c_cb = _incb_on_close;
    ATOMIC_SET(&_g_incb_closed, 0);
    ATOMIC_SET(&_g_incb_state, 0);
    ev_init(&ev, 1, NULL);
    lrtn = ev_listen(&ev, NULL, "127.0.0.1", CLOSE_IN_CB_PORT, &cbs, NULL, NULL, 0, &id);
    MSLEEP(50);// listen 落地是异步的
    if (ERR_OK == lrtn) {
        fd = _connect_local(CLOSE_IN_CB_PORT, 0);
    }
    if (INVALID_SOCK != fd) {
        sent = (int32_t)send(fd, "a", 1, 0);
    }
    for (i = 0; 1 == sent && i < 1000 && 0 == (int32_t)ATOMIC_GET(&_g_incb_closed); i++) {
        MSLEEP(1);
    }
    MSLEEP(50);// 关两次的话第二次会紧跟着来
    CLOSE_SOCK(fd);
    ev_free(&ev);
    // 断言放 ev_free 之后，理由同 test_ev_close_in_acp_conn_deferred
    CuAssertIntEquals(tc, ERR_OK, lrtn);
    CuAssertIntEquals(tc, 1, sent);
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_incb_state));
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_incb_closed));
}
// accept / connect 回调里同线程发送失败：同收包回调，关连接推迟到回调返回之后、关闭回调只来一次
static void test_ev_close_in_acp_conn_deferred(CuTest *tc) {
    ev_ctx ev;
    cbs_ctx cbs, lcbs;
    uint64_t id, id2;
    sock_ctx sk;
    SOCKET fd = INVALID_SOCK;
    int32_t i, acp_state, acp_closed, connected;
    int32_t rtn[3];
    ev_init(&ev, 1, NULL);
    // accept 回调
    ZERO(&cbs, sizeof(cbs));
    cbs.acp_cb = _incb_on_acp;
    cbs.r_cb = _incb_drain;
    cbs.c_cb = _incb_on_close;
    ATOMIC_SET(&_g_incb_closed, 0);
    ATOMIC_SET(&_g_incb_state, 0);
    rtn[0] = ev_listen(&ev, NULL, "127.0.0.1", CLOSE_IN_ACP_PORT, &cbs, NULL, NULL, 0, &id);
    MSLEEP(50);// listen 落地是异步的
    if (ERR_OK == rtn[0]) {
        fd = _connect_local(CLOSE_IN_ACP_PORT, 0);
    }
    connected = INVALID_SOCK != fd;
    for (i = 0; connected && i < 1000 && 0 == (int32_t)ATOMIC_GET(&_g_incb_closed); i++) {
        MSLEEP(1);
    }
    MSLEEP(50);// 关两次的话第二次会紧跟着来
    CLOSE_SOCK(fd);
    // 先记下，ev_free 之后再断言：断言失败会直接跳出用例，event 线程还在跑着用栈上的 ev
    acp_state = (int32_t)ATOMIC_GET(&_g_incb_state);
    acp_closed = (int32_t)ATOMIC_GET(&_g_incb_closed);
    // connect 回调：对端只收不发，关闭回调不计数
    ZERO(&lcbs, sizeof(lcbs));
    lcbs.r_cb = _incb_drain;
    ZERO(&cbs, sizeof(cbs));
    cbs.conn_cb = _incb_on_conn;
    cbs.r_cb = _incb_drain;
    cbs.c_cb = _incb_on_close;
    ATOMIC_SET(&_g_incb_closed, 0);
    ATOMIC_SET(&_g_incb_state, 0);
    rtn[1] = ev_listen(&ev, NULL, "127.0.0.1", CLOSE_IN_CONN_PORT, &lcbs, NULL, NULL, 0, &id2);
    MSLEEP(50);
    rtn[2] = ERR_FAILED;
    if (ERR_OK == rtn[1]) {
        rtn[2] = ev_connect(&ev, NULL, "127.0.0.1", CLOSE_IN_CONN_PORT, &cbs, NULL, 0, INVALID_INDEX, &sk);
    }
    for (i = 0; ERR_OK == rtn[2] && i < 1000 && 0 == (int32_t)ATOMIC_GET(&_g_incb_closed); i++) {
        MSLEEP(1);
    }
    MSLEEP(50);
    ev_free(&ev);
    for (i = 0; i < 3; i++) {
        CuAssertIntEquals(tc, ERR_OK, rtn[i]);
    }
    CuAssertTrue(tc, connected);
    CuAssertIntEquals(tc, 1, acp_state);
    CuAssertIntEquals(tc, 1, acp_closed);
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_incb_state));
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_incb_closed));
}
#if defined(EV_EPOLL)
// 连接失败回调里关连接（只有 epoll 能造）：回调里重新注册事件失败才会真的关掉，kqueue 的注册攒到下一轮才提交、
// 不会当场失败。这里替换整个 test 程序的 epoll_ctl：平时原样转发，置了 fd 后让它的下一次 MOD 失败；
// 另可指定一个 fd 记下对它第一次成功的 DEL
static atomic_t _g_cfail_armfd;// 要让下一次 MOD 失败的 fd + 1，0 表示不注入
static atomic_t _g_cfail_injected;// 注入生效的次数
static atomic_t _g_cfail_delfd;// 要记 DEL 的 fd + 1，记到一次即清 0
static atomic_t _g_cfail_ndel;// 记到的成功 DEL 次数
static atomic_t _g_cfail_conn;// 连接失败回调次数
static atomic_t _g_cfail_closed;// 关闭回调次数
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event) {
    int rc;
    if (EPOLL_CTL_MOD == op
        && ATOMIC_CAS(&_g_cfail_armfd, (atomic_t)(fd + 1), 0)) {
        ATOMIC_ADD(&_g_cfail_injected, 1);
        errno = ENOMEM;
        return -1;
    }
    rc = (int)syscall(SYS_epoll_ctl, epfd, op, fd, event);
    // 只记成功的 DEL：失败(ENOENT)说明登记本来就不在，证明不了撤过
    if (0 == rc
        && EPOLL_CTL_DEL == op
        && ATOMIC_CAS(&_g_cfail_delfd, (atomic_t)(fd + 1), 0)) {
        ATOMIC_ADD(&_g_cfail_ndel, 1);
    }
    return rc;
}
static int32_t _cfail_on_conn(ev_ctx *ev, sock_ctx *sk, int32_t erro, ud_cxt *ud) {
    (void)ud;
    if (ERR_OK != erro) {
        ATOMIC_ADD(&_g_cfail_conn, 1);
        // _uev_disconnect 补注册读事件(epoll 下是 MOD)失败，关闭推迟到回调返回时做。
        // 只在 ev_close 期间注入：没生效的话 fd 号被别的线程复用，会误伤别的用例
        ATOMIC_SET(&_g_cfail_armfd, (atomic_t)(sk->fd + 1));
        ev_close(ev, sk);
        ATOMIC_SET(&_g_cfail_armfd, 0);
    }
    return ERR_OK;
}
static void _cfail_on_close(ev_ctx *ev, sock_ctx *sk, int32_t client, int32_t erro, ud_cxt *ud) {
    (void)ev; (void)sk; (void)client; (void)erro; (void)ud;
    ATOMIC_ADD(&_g_cfail_closed, 1);
}
// 连接失败回调里把连接关掉了：调用方不能再摘除回收。否则同一个对象既在隔离队列里又回了池，
// ev_free 时释放两次（glibc 直接报 double free）；关闭回调也只能来一次
static void test_ev_close_in_conn_fail(CuTest *tc) {
    ev_ctx ev;
    cbs_ctx cbs;
    sock_ctx sk;
    int32_t i, crtn;
    ZERO(&cbs, sizeof(cbs));
    cbs.conn_cb = _cfail_on_conn;
    cbs.r_cb = _incb_drain;// TCP 必须有 r_cb，否则 ev_connect 当场拒绝
    cbs.c_cb = _cfail_on_close;
    ATOMIC_SET(&_g_cfail_armfd, 0);
    ATOMIC_SET(&_g_cfail_injected, 0);
    ATOMIC_SET(&_g_cfail_conn, 0);
    ATOMIC_SET(&_g_cfail_closed, 0);
    ev_init(&ev, 1, NULL);
    // 回环上连没人监听的端口：connect 先回 EINPROGRESS，被拒由事件报回来，走连接失败那条路
    crtn = ev_connect(&ev, NULL, "127.0.0.1", CONN_FAIL_PORT, &cbs, NULL, 0, INVALID_INDEX, &sk);
    for (i = 0; ERR_OK == crtn && i < 1000 && 0 == (int32_t)ATOMIC_GET(&_g_cfail_closed); i++) {
        MSLEEP(1);
    }
    MSLEEP(50);// 关两次的话第二次会紧跟着来
    ev_free(&ev);
    // 断言放 ev_free 之后，理由同 test_ev_close_in_acp_conn_deferred
    CuAssertIntEquals(tc, ERR_OK, crtn);
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_cfail_conn));
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_cfail_injected));
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_cfail_closed));
}
static int32_t _cdel_on_conn(ev_ctx *ev, sock_ctx *sk, int32_t erro, ud_cxt *ud) {
    (void)ev; (void)ud;
    if (ERR_OK != erro) {
        ATOMIC_ADD(&_g_cfail_conn, 1);
        ATOMIC_SET(&_g_cfail_delfd, (atomic_t)(sk->fd + 1));// 回调返回后的收尾该对它发 DEL
    }
    return ERR_OK;
}
// 连接失败收尾要显式撤 epoll 登记：只靠 close 的话，socket 被 fork 出、还没 exec 的子进程共享时
// 登记还在，之后的事件会带着已回收的对象回来
static void test_ev_conn_fail_epoll_del(CuTest *tc) {
    ev_ctx ev;
    cbs_ctx cbs;
    sock_ctx sk;
    int32_t i, crtn;
    ZERO(&cbs, sizeof(cbs));
    cbs.conn_cb = _cdel_on_conn;
    cbs.r_cb = _incb_drain;
    cbs.c_cb = _cfail_on_close;
    ATOMIC_SET(&_g_cfail_delfd, 0);
    ATOMIC_SET(&_g_cfail_ndel, 0);
    ATOMIC_SET(&_g_cfail_conn, 0);
    ATOMIC_SET(&_g_cfail_closed, 0);
    ev_init(&ev, 1, NULL);
    crtn = ev_connect(&ev, NULL, "127.0.0.1", CONN_FAIL_PORT, &cbs, NULL, 0, INVALID_INDEX, &sk);
    for (i = 0; ERR_OK == crtn && i < 1000 && 0 == (int32_t)ATOMIC_GET(&_g_cfail_ndel); i++) {
        MSLEEP(1);
    }
    ev_free(&ev);
    ATOMIC_SET(&_g_cfail_delfd, 0);// 没记到的话别留着误记别的用例
    // 断言放 ev_free 之后，理由同 test_ev_close_in_acp_conn_deferred
    CuAssertIntEquals(tc, ERR_OK, crtn);
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_cfail_conn));
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_cfail_ndel));
    CuAssertIntEquals(tc, 0, (int32_t)ATOMIC_GET(&_g_cfail_closed));// 连接失败不走关闭回调
}
static atomic_t _g_cfsend_gate;// 置 1 放行卡在 event 线程上的 _cfsend_block
static atomic_t _g_cfsend_linked;// 收尾之后、本轮冲刷之前攒发链上还挂着几个，-1 表示还没查
static void _cfsend_block(void *arg) {
    (void)arg;
    while (0 == ATOMIC_GET(&_g_cfsend_gate)) {
        MSLEEP(1);
    }
}
// 投递回调在本轮派发之后、冲刷之前跑，正好看得见回池前没摘掉的攒发链节点
static void _cfsend_check(void *arg) {
    ev_ctx *ev = arg;
    ATOMIC_SET(&_g_cfsend_linked, (atomic_t)ev->watcher[0].flushes.size);
}
static int32_t _cfsend_on_conn(ev_ctx *ev, sock_ctx *sk, int32_t erro, ud_cxt *ud) {
    (void)ud;
    if (ERR_OK != erro) {
        ATOMIC_ADD(&_g_cfail_conn, 1);
        ev_send(ev, sk, (void *)"x", 1, 1);// 同线程当场执行：连接已作废，得拒收
        ev_defer_exec(ev, 0, _cfsend_check, NULL, ev);
    }
    return ERR_OK;
}
// TCP 已连通、注册读事件失败走连接失败收尾，回调里还发了数据：不能挂上攒发链随对象回池，
// 否则本轮冲刷时摘的是池里(池满时已释放)的对象，对象被同轮复用时链表直接坏掉
static void test_ev_conn_fail_send(CuTest *tc) {
    ev_ctx ev;
    cbs_ctx cbs, lcbs;
    sock_ctx sk;
    uint64_t id;
    int32_t i, lrtn, crtn = ERR_FAILED;
    ZERO(&lcbs, sizeof(lcbs));
    lcbs.r_cb = _incb_drain;
    ZERO(&cbs, sizeof(cbs));
    cbs.conn_cb = _cfsend_on_conn;
    cbs.r_cb = _incb_drain;
    cbs.c_cb = _cfail_on_close;
    ATOMIC_SET(&_g_cfail_armfd, 0);
    ATOMIC_SET(&_g_cfail_injected, 0);
    ATOMIC_SET(&_g_cfail_conn, 0);
    ATOMIC_SET(&_g_cfail_closed, 0);
    ATOMIC_SET(&_g_cfsend_gate, 0);
    ATOMIC_SET(&_g_cfsend_linked, -1);
    ev_init(&ev, 1, NULL);
    lrtn = ev_listen(&ev, NULL, "127.0.0.1", CONN_FAIL_SEND_PORT, &lcbs, NULL, NULL, 0, &id);
    MSLEEP(50);// listen 落地是异步的
    if (ERR_OK == lrtn) {
        // 先把 event 线程卡住：注入就位之前它处理不到这条连接的连通事件
        ev_defer_exec(&ev, 0, _cfsend_block, NULL, NULL);
        crtn = ev_connect(&ev, NULL, "127.0.0.1", CONN_FAIL_SEND_PORT, &cbs, NULL, 0, INVALID_INDEX, &sk);
        if (ERR_OK == crtn) {
            ATOMIC_SET(&_g_cfail_armfd, (atomic_t)(sk.fd + 1));// 连通后把写事件改成读事件的那次 MOD 失败
        }
        ATOMIC_SET(&_g_cfsend_gate, 1);// 不论成败都得放行，否则 ev_free 等不到线程退出
    }
    for (i = 0; ERR_OK == crtn && i < 1000 && -1 == (int32_t)ATOMIC_GET(&_g_cfsend_linked); i++) {
        MSLEEP(1);
    }
    ATOMIC_SET(&_g_cfail_armfd, 0);// 没生效的话别留着误伤别的用例
    ev_free(&ev);
    // 断言放 ev_free 之后，理由同 test_ev_close_in_acp_conn_deferred
    CuAssertIntEquals(tc, ERR_OK, lrtn);
    CuAssertIntEquals(tc, ERR_OK, crtn);
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_cfail_injected));
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_cfail_conn));
    CuAssertIntEquals(tc, 0, (int32_t)ATOMIC_GET(&_g_cfsend_linked));
    CuAssertIntEquals(tc, 0, (int32_t)ATOMIC_GET(&_g_cfail_closed));// 连接失败不走关闭回调
}
#endif
// ev_defer_exec 用例：投出去的回调在哪个 event 线程上跑、同线程再投何时跑、停机时没跑成的走 fcb
static ev_ctx *_g_defer_exec_ev;
static ev_ctx *_g_defer_exec_ev2;
static atomic_t _g_defer_exec_idx;// 回调里读到的 ev_cur_index
static atomic_t _g_defer_exec_other;// 回调里对另一个 ev_ctx 调 ev_cur_index 的结果
static atomic_t _g_defer_exec_early;// 同线程再投之后当场看那个回调跑了没有
static atomic_t _g_defer_exec_nested;// 同线程再投的那个回调跑了没有
static atomic_t _g_defer_exec_hold;// 占住线程的回调已开始
static atomic_t _g_defer_exec_go;// 主线程放行
static atomic_t _g_defer_exec_never;// 本该留到停机的回调却跑了的次数
static atomic_t _g_defer_exec_freed;// fcb 被调次数
static void _defer_exec_nested_cb(void *arg) {
    (void)arg;
    ATOMIC_SET(&_g_defer_exec_nested, 1);
}
static void _defer_exec_first_cb(void *arg) {
    int32_t idx = ev_cur_index(_g_defer_exec_ev);
    (void)arg;
    ATOMIC_SET(&_g_defer_exec_idx, (atomic_t)idx);
    ATOMIC_SET(&_g_defer_exec_other, (atomic_t)ev_cur_index(_g_defer_exec_ev2));
    ev_defer_exec(_g_defer_exec_ev, idx, _defer_exec_nested_cb, NULL, NULL);
    ATOMIC_SET(&_g_defer_exec_early, ATOMIC_GET(&_g_defer_exec_nested));
}
// 跨线程投到第 2 个 event 线程：在那条线程上跑，ev_cur_index 只认自己的 ev_ctx；
// 回调里给本线程再投的只排队，不当场跑(什么时候跑见 test_ev_defer_exec_pass)
static void test_ev_defer_exec_run(CuTest *tc) {
    ev_ctx ev, ev2;
    int32_t i, outside;
    ATOMIC_SET(&_g_defer_exec_idx, -2);
    ATOMIC_SET(&_g_defer_exec_other, -2);
    ATOMIC_SET(&_g_defer_exec_early, -1);
    ATOMIC_SET(&_g_defer_exec_nested, 0);
    ev_init(&ev, 2, NULL);
    ev_init(&ev2, 1, NULL);
    _g_defer_exec_ev = &ev;
    _g_defer_exec_ev2 = &ev2;
    outside = ev_cur_index(&ev);// 断言放 ev_free 之后，理由同 test_ev_close_in_acp_conn_deferred
    ev_defer_exec(&ev, 1, _defer_exec_first_cb, NULL, NULL);
    for (i = 0; i < 1000 && 0 == (int32_t)ATOMIC_GET(&_g_defer_exec_nested); i++) {
        MSLEEP(1);
    }
    ev_free(&ev2);
    ev_free(&ev);
    CuAssertIntEquals(tc, INVALID_INDEX, outside);
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_defer_exec_idx));
    CuAssertIntEquals(tc, INVALID_INDEX, (int32_t)ATOMIC_GET(&_g_defer_exec_other));
    CuAssertIntEquals(tc, 0, (int32_t)ATOMIC_GET(&_g_defer_exec_early));
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_defer_exec_nested));
}
static void _defer_exec_never_cb(void *arg) {
    (void)arg;
    ATOMIC_ADD(&_g_defer_exec_never, 1);
}
static void _defer_exec_fcb(void *arg) {
    ATOMIC_ADD((atomic_t *)arg, 1);
}
// 给本线程再投一个，然后占住线程直到主线程已调 ev_free(停止命令已入队)
static void _defer_exec_hold_cb(void *arg) {
    int32_t i;
    (void)arg;
    ev_defer_exec(_g_defer_exec_ev, 0, _defer_exec_never_cb, _defer_exec_fcb, (void *)&_g_defer_exec_freed);// arg 为 NULL 时不调 fcb
    ATOMIC_SET(&_g_defer_exec_hold, 1);
    for (i = 0; i < 2000 && 0 == (int32_t)ATOMIC_GET(&_g_defer_exec_go); i++) {
        MSLEEP(1);
    }
    MSLEEP(200);// IOCP 的 ev_free 先停 AcceptEx 线程才投停止命令，多等一会儿
}
// 停机时还在投递队列里的回调不再跑，arg 交给 fcb 释放，且只放一次。
// 占线程的回调里再投的那个本该本轮第二趟跑，ev_free 已开始就不跑第二趟
static void test_ev_defer_exec_free(CuTest *tc) {
    ev_ctx ev;
    int32_t i;
    ATOMIC_SET(&_g_defer_exec_hold, 0);
    ATOMIC_SET(&_g_defer_exec_go, 0);
    ATOMIC_SET(&_g_defer_exec_never, 0);
    ATOMIC_SET(&_g_defer_exec_freed, 0);
    ev_init(&ev, 1, NULL);
    _g_defer_exec_ev = &ev;
    ev_defer_exec(&ev, 0, _defer_exec_hold_cb, NULL, NULL);
    for (i = 0; i < 1000 && 0 == (int32_t)ATOMIC_GET(&_g_defer_exec_hold); i++) {
        MSLEEP(1);
    }
    ATOMIC_SET(&_g_defer_exec_go, 1);
    ev_free(&ev);
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_defer_exec_hold));
    CuAssertIntEquals(tc, 0, (int32_t)ATOMIC_GET(&_g_defer_exec_never));
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_defer_exec_freed));
}
// 投递多趟用例：tick 在每轮派发与投递之后各跑一次，拿它数轮次
static ev_tick _g_dpass_tick;
static atomic_t _g_dpass_round;// tick 跑过的次数
static atomic_t _g_dpass_at[3];// 三个回调各在第几轮跑(轮次 +1，0 表示还没跑)
static uint32_t _dpass_tick_cb(void *ud, uint64_t now_ms) {
    (void)ud; (void)now_ms;
    ATOMIC_ADD(&_g_dpass_round, 1);
    return 1;
}
static void _dpass_third_cb(void *arg) {
    ATOMIC_SET(&_g_dpass_at[2], ATOMIC_GET(&_g_dpass_round) + 1);
    _evpub_tick_detach((struct watcher_ctx *)arg, &_g_dpass_tick);
}
static void _dpass_second_cb(void *arg) {
    ATOMIC_SET(&_g_dpass_at[1], ATOMIC_GET(&_g_dpass_round) + 1);
    ev_defer_exec(_g_defer_exec_ev, 0, _dpass_third_cb, NULL, arg);
}
static void _dpass_first_cb(void *arg) {
    struct watcher_ctx *watcher = &_g_defer_exec_ev->watcher[0];
    (void)arg;
    _evpub_tick_attach(watcher, &_g_dpass_tick, _dpass_tick_cb, NULL);
    ATOMIC_SET(&_g_dpass_at[0], ATOMIC_GET(&_g_dpass_round) + 1);
    ev_defer_exec(_g_defer_exec_ev, 0, _dpass_second_cb, NULL, watcher);
}
// 回调里给本线程再投的本轮再跑一趟(同线程绑定 task 一来一回的回程不多绕一轮)，那一趟里再投的留到之后的轮
static void test_ev_defer_exec_pass(CuTest *tc) {
    ev_ctx ev;
    int32_t i;
    ZERO(&_g_dpass_tick, sizeof(_g_dpass_tick));
    ATOMIC_SET(&_g_dpass_round, 0);
    for (i = 0; i < 3; i++) {
        ATOMIC_SET(&_g_dpass_at[i], 0);
    }
    ev_init(&ev, 1, NULL);
    _g_defer_exec_ev = &ev;
    ev_defer_exec(&ev, 0, _dpass_first_cb, NULL, NULL);
    for (i = 0; i < 1000 && 0 == (int32_t)ATOMIC_GET(&_g_dpass_at[2]); i++) {
        MSLEEP(1);
    }
    ev_free(&ev);
    CuAssertTrue(tc, 0 != ATOMIC_GET(&_g_dpass_at[0]));
    CuAssertIntEquals(tc, (int32_t)ATOMIC_GET(&_g_dpass_at[0]), (int32_t)ATOMIC_GET(&_g_dpass_at[1]));
    CuAssertTrue(tc, ATOMIC_GET(&_g_dpass_at[2]) > ATOMIC_GET(&_g_dpass_at[1]));
}
// 指定 watcher 用例：各下标那条连接的 conn_cb 跑在哪个 event 线程上(+1，0 表示还没跑)
static atomic_t _g_launch_conn[2];
static int32_t _launch_on_conn(ev_ctx *ev, sock_ctx *sk, int32_t err, ud_cxt *ud) {
    (void)err; (void)ud;
    if (sk->index >= 0 && sk->index < 2) {
        ATOMIC_SET(&_g_launch_conn[sk->index], (atomic_t)(ev_cur_index(ev) + 1));
    }
    return ERR_OK;
}
static void _launch_on_recv(ev_ctx *ev, sock_ctx *sk,
                            int32_t client, buffer_ctx *buf, size_t size, ud_cxt *ud) {
    (void)ev; (void)sk; (void)client; (void)size; (void)ud;
    buffer_drain(buf, buffer_size(buf));
}
static void _launch_on_recvfrom(ev_ctx *ev, sock_ctx *sk,
                                char *buf, size_t size, netaddr_ctx *addr, ud_cxt *ud) {
    (void)ev; (void)sk; (void)buf; (void)size; (void)addr; (void)ud;
}
// ev_connect / ev_udp 指定 watcher：连接落在指定的 event 线程上，回调也在那条线程上跑
static void test_ev_launch_index(CuTest *tc) {
    ev_ctx ev;
    cbs_ctx lcbs, ccbs, ucbs;
    sock_ctx sk[2], usk;
    uint64_t id;
    int32_t i, k;
    int32_t rtn[4];
    ZERO(&lcbs, sizeof(lcbs));
    lcbs.r_cb = _launch_on_recv;
    ZERO(&ccbs, sizeof(ccbs));
    ccbs.conn_cb = _launch_on_conn;
    ccbs.r_cb = _launch_on_recv;
    ZERO(&ucbs, sizeof(ucbs));
    ucbs.rf_cb = _launch_on_recvfrom;
    ATOMIC_SET(&_g_launch_conn[0], 0);
    ATOMIC_SET(&_g_launch_conn[1], 0);
    ev_init(&ev, 2, NULL);
    rtn[0] = ev_listen(&ev, NULL, "127.0.0.1", LAUNCH_IDX_PORT, &lcbs, NULL, NULL, 0, &id);
    MSLEEP(50);// listen 落地是异步的
    for (k = 0; k < 2; k++) {
        rtn[1 + k] = ev_connect(&ev, NULL, "127.0.0.1", LAUNCH_IDX_PORT, &ccbs, NULL, 0, k, &sk[k]);
    }
    rtn[3] = ev_udp(&ev, "127.0.0.1", LAUNCH_UDP_PORT, &ucbs, NULL, 1, &usk);
    for (i = 0; ERR_OK == rtn[1] && ERR_OK == rtn[2] && i < 1000
         && (0 == ATOMIC_GET(&_g_launch_conn[0]) || 0 == ATOMIC_GET(&_g_launch_conn[1])); i++) {
        MSLEEP(1);
    }
    for (k = 0; k < 2; k++) {
        if (ERR_OK == rtn[1 + k]) {
            ev_close(&ev, &sk[k]);
        }
    }
    if (ERR_OK == rtn[3]) {
        ev_close(&ev, &usk);
    }
    MSLEEP(50);
    ev_free(&ev);
    // 下标断言放 ev_free 之后，理由同 test_ev_close_in_acp_conn_deferred
    for (i = 0; i < 4; i++) {
        CuAssertIntEquals(tc, ERR_OK, rtn[i]);
    }
    CuAssertIntEquals(tc, 0, sk[0].index);
    CuAssertIntEquals(tc, 1, sk[1].index);
    CuAssertIntEquals(tc, 1, usk.index);
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_launch_conn[0]));
    CuAssertIntEquals(tc, 2, (int32_t)ATOMIC_GET(&_g_launch_conn[1]));
}
// 线程表监听用例：服务端 accept 回调落在各 event 线程上的次数，以及回调里 sk->index 与所在线程对不上的次数
static atomic_t _g_lthrs_acp[3];
static atomic_t _g_lthrs_bad;
static int32_t _lthrs_on_acp(ev_ctx *ev, sock_ctx *sk, ud_cxt *ud) {
    int32_t idx = ev_cur_index(ev);
    (void)ud;
    if (idx < 0 || idx >= 3 || idx != sk->index) {
        ATOMIC_ADD(&_g_lthrs_bad, 1);
        return ERR_OK;
    }
    ATOMIC_ADD(&_g_lthrs_acp[idx], 1);
    return ERR_OK;
}
// 连 n 条到 port，等服务端 accept 回调凑够 n 次，再全部关掉
static void _lthrs_connect(ev_ctx *ev, uint16_t port, int32_t n) {
    cbs_ctx ccbs;
    sock_ctx sk[16];
    int32_t i;
    ZERO(&ccbs, sizeof(ccbs));
    ccbs.r_cb = _launch_on_recv;
    for (i = 0; i < n; i++) {
        if (ERR_OK != ev_connect(ev, NULL, "127.0.0.1", port, &ccbs, NULL, 0, INVALID_INDEX, &sk[i])) {
            sock_set_invalid(&sk[i]);
        }
    }
    for (i = 0; i < 1000 && (int32_t)(ATOMIC_GET(&_g_lthrs_acp[0]) + ATOMIC_GET(&_g_lthrs_acp[1])
                                      + ATOMIC_GET(&_g_lthrs_acp[2]) + ATOMIC_GET(&_g_lthrs_bad)) < n; i++) {
        MSLEEP(1);
    }
    for (i = 0; i < n; i++) {
        if (!sock_is_invalid(&sk[i])) {
            ev_close(ev, &sk[i]);
        }
    }
}
// ev_listen 带线程表：只在表里的 event 线程上收连接，accept 出的连接也只挂到它们上面；unlisten 按各自所在线程摘
static void test_ev_listen_thrs(CuTest *tc) {
    ev_ctx ev;
    cbs_ctx lcbs;
    uint64_t id[2];
    const int32_t only1[] = { 1 };
    const int32_t skip1[] = { 2, 0 };
    int32_t i, on1, off1, on02, off02;
    int32_t rtn[2];
    ZERO(&lcbs, sizeof(lcbs));
    lcbs.acp_cb = _lthrs_on_acp;
    lcbs.r_cb = _launch_on_recv;
    ATOMIC_SET(&_g_lthrs_bad, 0);
    for (i = 0; i < 3; i++) {
        ATOMIC_SET(&_g_lthrs_acp[i], 0);
    }
    ev_init(&ev, 3, NULL);
    // 1) 只给线程 1：8 条全落在 1 上
    rtn[0] = ev_listen(&ev, NULL, "127.0.0.1", LISTEN_THRS_PORT, &lcbs, NULL, only1, 1, &id[0]);
    MSLEEP(50);// listen 落地是异步的
    _lthrs_connect(&ev, LISTEN_THRS_PORT, 8);
    on1 = (int32_t)ATOMIC_GET(&_g_lthrs_acp[1]);
    off1 = (int32_t)(ATOMIC_GET(&_g_lthrs_acp[0]) + ATOMIC_GET(&_g_lthrs_acp[2]));
    // 2) 给 {2, 0}(顺序不限)：12 条一条都不落在 1 上
    for (i = 0; i < 3; i++) {
        ATOMIC_SET(&_g_lthrs_acp[i], 0);
    }
    rtn[1] = ev_listen(&ev, NULL, "127.0.0.1", LISTEN_THRS_PORT2, &lcbs, NULL, skip1, 2, &id[1]);
    MSLEEP(50);
    _lthrs_connect(&ev, LISTEN_THRS_PORT2, 12);
    on02 = (int32_t)(ATOMIC_GET(&_g_lthrs_acp[0]) + ATOMIC_GET(&_g_lthrs_acp[2]));
    off02 = (int32_t)ATOMIC_GET(&_g_lthrs_acp[1]);
    if (ERR_OK == rtn[0]) {
        ev_unlisten(&ev, id[0]);
    }
    if (ERR_OK == rtn[1]) {
        ev_unlisten(&ev, id[1]);
    }
    MSLEEP(50);
    ev_free(&ev);
    // 断言放 ev_free 之后，理由同 test_ev_close_in_acp_conn_deferred
    CuAssertIntEquals(tc, ERR_OK, rtn[0]);
    CuAssertIntEquals(tc, ERR_OK, rtn[1]);
    CuAssertIntEquals(tc, 8, on1);
    CuAssertIntEquals(tc, 0, off1);
    CuAssertIntEquals(tc, 12, on02);
    CuAssertIntEquals(tc, 0, off02);
    CuAssertIntEquals(tc, 0, (int32_t)ATOMIC_GET(&_g_lthrs_bad));
}
// 在目标线程上 ev_connect 的用例：客户端连接的状态，以及线程 1 上那次 ev_connect 前后的池
static ev_ctx *_g_cpool_ev;
static atomic_t _g_cpool_conn;// conn_cb 所在线程 +1，连接失败记 -1，0 表示还没来
static atomic_t _g_cpool_echo;// 回显收齐：1 内容对，2 内容错
static atomic_t _g_cpool_closed;// 客户端关闭回调次数
static atomic_t _g_cpool_probe;// _cpool_launch_cb 跑过的次数
static atomic_t _g_cpool_launched;// 线程 1 上的 ev_connect 调过了
static atomic_t _g_cpool_before;// 调 ev_connect 前线程 1 池里的空闲对象数
static atomic_t _g_cpool_after;// 调完之后的空闲对象数
static atomic_t _g_cpool_rtn;// 那次 ev_connect 的返回值
static atomic_t _g_cpool_index;// 那次带出的 sk.index
// 服务端：收到多少回多少
static void _cpool_on_echo(ev_ctx *ev, sock_ctx *sk,
                           int32_t client, buffer_ctx *buf, size_t size, ud_cxt *ud) {
    char out[64];
    size_t n;
    (void)client; (void)size; (void)ud;
    n = buffer_copyout(buf, 0, out, sizeof(out));
    buffer_drain(buf, n);
    if (n > 0) {
        ev_send(ev, sk, out, n, 1);
    }
}
static int32_t _cpool_on_conn(ev_ctx *ev, sock_ctx *sk, int32_t err, ud_cxt *ud) {
    (void)ud;
    if (ERR_OK != err) {
        ATOMIC_SET(&_g_cpool_conn, (atomic_t)-1);
        return ERR_OK;
    }
    ATOMIC_SET(&_g_cpool_conn, (atomic_t)(ev_cur_index(ev) + 1));
    ev_send(ev, sk, (void *)CONN_POOL_MSG, strlen(CONN_POOL_MSG), 1);
    return ERR_OK;
}
// 客户端：回显收齐就比对内容并在回调里关掉自己
static void _cpool_on_recv(ev_ctx *ev, sock_ctx *sk,
                           int32_t client, buffer_ctx *buf, size_t size, ud_cxt *ud) {
    char out[sizeof(CONN_POOL_MSG) - 1];
    (void)client; (void)size; (void)ud;
    if (buffer_size(buf) < sizeof(out)) {
        return;
    }
    buffer_copyout(buf, 0, out, sizeof(out));
    buffer_drain(buf, buffer_size(buf));
    ATOMIC_SET(&_g_cpool_echo, 0 == memcmp(out, CONN_POOL_MSG, sizeof(out)) ? 1 : 2);
    ev_close(ev, sk);
}
static void _cpool_on_close(ev_ctx *ev, sock_ctx *sk, int32_t client, int32_t erro, ud_cxt *ud) {
    (void)ev; (void)sk; (void)client; (void)erro; (void)ud;
    ATOMIC_ADD(&_g_cpool_closed, 1);
}
// 跑在线程 1 上：池里有空闲对象才发起(只发起一次)，前后各读一次池的空闲数。池只在本线程上动，这里读没有竞争
static void _cpool_launch_cb(void *arg) {
    pool_ctx *pool = &_g_cpool_ev->watcher[1].pool;
    sock_ctx sk;
    uint32_t n = pool_size(pool);
    if (0 != n && 0 == ATOMIC_GET(&_g_cpool_launched)) {
        sk.index = INVALID_INDEX;
        ATOMIC_SET(&_g_cpool_before, (atomic_t)n);
        ATOMIC_SET(&_g_cpool_rtn, (atomic_t)ev_connect(_g_cpool_ev, NULL, "127.0.0.1", CONN_POOL_PORT,
                                                       (cbs_ctx *)arg, NULL, 0, 1, &sk));
        ATOMIC_SET(&_g_cpool_after, (atomic_t)pool_size(pool));
        ATOMIC_SET(&_g_cpool_index, (atomic_t)sk.index);
        ATOMIC_SET(&_g_cpool_launched, 1);
    }
    ATOMIC_ADD(&_g_cpool_probe, 1);
}
// ev_connect 在目标 event 线程上调用时从该线程的池里取回收的对象，不新建；复用的对象照常连通、收发、关闭。
// 服务端只在线程 0 收连接，线程 1 的池只会进 1) 那条客户端连接回收的一个对象
static void test_ev_connect_inloop_pool(CuTest *tc) {
    ev_ctx ev;
    cbs_ctx lcbs, ccbs;
    sock_ctx sk;
    uint64_t id;
    const int32_t only0[] = { 0 };
    int32_t i, j, conn1, echo1, closed1;
    int32_t rtn[2];
    atomic_t probe;
    ZERO(&lcbs, sizeof(lcbs));
    lcbs.r_cb = _cpool_on_echo;
    ZERO(&ccbs, sizeof(ccbs));
    ccbs.conn_cb = _cpool_on_conn;
    ccbs.r_cb = _cpool_on_recv;
    ccbs.c_cb = _cpool_on_close;
    ATOMIC_SET(&_g_cpool_conn, 0);
    ATOMIC_SET(&_g_cpool_echo, 0);
    ATOMIC_SET(&_g_cpool_closed, 0);
    ATOMIC_SET(&_g_cpool_probe, 0);
    ATOMIC_SET(&_g_cpool_launched, 0);
    ATOMIC_SET(&_g_cpool_before, 0);
    ATOMIC_SET(&_g_cpool_after, 0);
    ATOMIC_SET(&_g_cpool_rtn, (atomic_t)ERR_FAILED);
    ATOMIC_SET(&_g_cpool_index, (atomic_t)INVALID_INDEX);
    ev_init(&ev, 2, NULL);
    _g_cpool_ev = &ev;
    rtn[0] = ev_listen(&ev, NULL, "127.0.0.1", CONN_POOL_PORT, &lcbs, NULL, only0, 1, &id);
    MSLEEP(50);// listen 落地是异步的
    // 1) 主线程发起、落在线程 1：不在目标线程上，对象新建；回显收齐后客户端自己关，对象回线程 1 的池
    rtn[1] = ev_connect(&ev, NULL, "127.0.0.1", CONN_POOL_PORT, &ccbs, NULL, 0, 1, &sk);
    for (i = 0; i < 1000 && 0 == ATOMIC_GET(&_g_cpool_closed); i++) {
        MSLEEP(1);
    }
    conn1 = (int32_t)ATOMIC_GET(&_g_cpool_conn);
    echo1 = (int32_t)ATOMIC_GET(&_g_cpool_echo);
    closed1 = (int32_t)ATOMIC_GET(&_g_cpool_closed);
    ATOMIC_SET(&_g_cpool_conn, 0);
    ATOMIC_SET(&_g_cpool_echo, 0);
    ATOMIC_SET(&_g_cpool_closed, 0);
    // 2) 投到线程 1 上发起。Unix 关闭的对象先过隔离期才进池，池还空就隔一会儿再投
    for (i = 0; i < 500 && 0 == ATOMIC_GET(&_g_cpool_launched); i++) {
        probe = ATOMIC_GET(&_g_cpool_probe);
        ev_defer_exec(&ev, 1, _cpool_launch_cb, NULL, &ccbs);
        for (j = 0; j < 1000 && probe == ATOMIC_GET(&_g_cpool_probe); j++) {
            MSLEEP(1);
        }
        if (0 == ATOMIC_GET(&_g_cpool_launched)) {
            MSLEEP(10);
        }
    }
    for (i = 0; i < 1000 && 0 == ATOMIC_GET(&_g_cpool_closed); i++) {
        MSLEEP(1);
    }
    MSLEEP(50);// 关两次的话第二次会紧跟着来
    ev_free(&ev);
    // 断言放 ev_free 之后，理由同 test_ev_close_in_acp_conn_deferred
    CuAssertIntEquals(tc, ERR_OK, rtn[0]);
    CuAssertIntEquals(tc, ERR_OK, rtn[1]);
    CuAssertIntEquals(tc, 2, conn1);
    CuAssertIntEquals(tc, 1, echo1);
    CuAssertIntEquals(tc, 1, closed1);
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_cpool_launched));
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_cpool_before));
    CuAssertIntEquals(tc, 0, (int32_t)ATOMIC_GET(&_g_cpool_after));// 那一个被取走了
    CuAssertIntEquals(tc, ERR_OK, (int32_t)ATOMIC_GET(&_g_cpool_rtn));
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_cpool_index));
    CuAssertIntEquals(tc, 2, (int32_t)ATOMIC_GET(&_g_cpool_conn));
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_cpool_echo));
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_cpool_closed));
}
// send_multi 用例的共用状态
static ev_ctx *_g_smulti_ev;
static cbs_ctx _g_smulti_ccbs;// 客户端回调：只计连通，收到的丢掉
static sock_ctx _g_smulti_sk[SMULTI_NCLI];// 第 k 个客户端落在线程 k % 2
static atomic_t _g_smulti_conn;// 连通的客户端数
static atomic_t _g_smulti_res[SMULTI_NCLI];// 0 未收齐，1 内容对，2 内容错
static atomic_t _g_smulti_bad;// 返回值不符预期、或收到认不出的数据的次数
static atomic_t _g_smulti_hold;// 停机用例：1 回调已开始，2 广播已发出
static atomic_t _g_smulti_nrecv;// 宽线程用例：服务端累计收到的字节数
// 客户端 k 应收到的字节流：编号 "I"k；每轮 "M"r(sk[0] 在列表里出现两次，收两份)，轮到它时再收 "S"r；
// sk[0]、sk[1] 最后各收 SMULTI_REPEAT 个 'Z'
static size_t _smulti_expect(int32_t k, char *out) {
    size_t n = 0;
    int32_t r, i;
    out[n++] = 'I';
    out[n++] = (char)('0' + k);
    for (r = 0; r < SMULTI_ROUNDS; r++) {
        for (i = 0; i < (0 == k ? 2 : 1); i++) {
            out[n++] = 'M';
            out[n++] = (char)r;
        }
        if (r % SMULTI_NCLI == k) {
            out[n++] = 'S';
            out[n++] = (char)r;
        }
    }
    if (k < 2) {
        memset(out + n, 'Z', SMULTI_REPEAT);
        n += SMULTI_REPEAT;
    }
    return n;
}
// 服务端：首两字节定是哪个客户端，攒够期望长度就逐字比对、记结果；缓冲不清，编号一直可读，
// 记过结果后这条连接再来任何数据(哪怕 1 字节)都算多收
static void _smulti_on_recv(ev_ctx *ev, sock_ctx *sk,
                            int32_t client, buffer_ctx *buf, size_t size, ud_cxt *ud) {
    char want[SMULTI_MAXLEN];
    char got[SMULTI_MAXLEN];
    size_t n;
    int32_t k;
    (void)ev; (void)sk; (void)client; (void)size; (void)ud;
    if (buffer_size(buf) < 2) {
        return;
    }
    buffer_copyout(buf, 0, got, 2);
    k = got[1] - '0';
    if ('I' != got[0] || k < 0 || k >= SMULTI_NCLI) {
        ATOMIC_ADD(&_g_smulti_bad, 1);
        buffer_drain(buf, buffer_size(buf));
        return;
    }
    if (0 != ATOMIC_GET(&_g_smulti_res[k])) {
        ATOMIC_ADD(&_g_smulti_bad, 1);
        return;
    }
    n = _smulti_expect(k, want);
    if (buffer_size(buf) < n) {
        return;
    }
    buffer_copyout(buf, 0, got, n);
    ATOMIC_SET(&_g_smulti_res[k], (buffer_size(buf) == n && 0 == memcmp(got, want, n)) ? 1 : 2);
}
static int32_t _smulti_on_conn(ev_ctx *ev, sock_ctx *sk, int32_t err, ud_cxt *ud) {
    (void)ev; (void)sk; (void)ud;
    if (ERR_OK == err) {
        ATOMIC_ADD(&_g_smulti_conn, 1);
    }
    return ERR_OK;
}
static void _smulti_want(int32_t rtn, int32_t want) {
    if (rtn != want) {
        ATOMIC_ADD(&_g_smulti_bad, 1);
    }
}
// 发送序列：先各发编号；再验两条失败返回(全失效列表 + copy=0 载荷、n=0)；然后每轮广播一次再单发一个客户端；
// 最后 sk[0]、sk[1] 各重复 SMULTI_REPEAT 次广播 1 字节
static void _smulti_rounds(void) {
    ev_ctx *ev = _g_smulti_ev;
    sock_ctx list[SMULTI_NCLI + 3];
    sock_ctx heavy[2 * SMULTI_REPEAT];
    char msg[2];
    char *own;
    int32_t i, r;
    ZERO(list, sizeof(list));
    for (i = 0; i < SMULTI_NCLI; i++) {
        msg[0] = 'I';
        msg[1] = (char)('0' + i);
        _smulti_want(ev_send(ev, &_g_smulti_sk[i], msg, 2, 1), ERR_OK);
    }
    sock_set_invalid(&list[0]);
    sock_set_invalid(&list[1]);
    MALLOC(own, 1);
    own[0] = 'X';
    _smulti_want(ev_send_multi(ev, list, 2, own, 1, 0), ERR_FAILED);// 载荷当场放掉，漏放由内存检查抓
    _smulti_want(ev_send_multi(ev, list, 0, msg, 2, 1), ERR_FAILED);
    // 列表：sk[1] 改了 skid(查表失败)排第一，组 1 第 0 步就交回一份计数；全部客户端；sk[0] 重复一次；一个失效项
    list[0] = _g_smulti_sk[1];
    list[0].skid++;
    for (i = 0; i < SMULTI_NCLI; i++) {
        list[1 + i] = _g_smulti_sk[i];
    }
    list[SMULTI_NCLI + 1] = _g_smulti_sk[0];
    sock_set_invalid(&list[SMULTI_NCLI + 2]);
    for (r = 0; r < SMULTI_ROUNDS; r++) {
        msg[0] = 'M';
        msg[1] = (char)r;
        _smulti_want(ev_send_multi(ev, list, SMULTI_NCLI + 3, msg, 2, 1), ERR_OK);
        msg[0] = 'S';
        _smulti_want(ev_send(ev, &_g_smulti_sk[r % SMULTI_NCLI], msg, 2, 1), ERR_OK);
    }
    // 一组里攒满 MAX_SEND_NIOV 条就当场冲刷，组循环中途就有计数交回，不能提前归 0
    for (i = 0; i < 2 * SMULTI_REPEAT; i++) {
        heavy[i] = _g_smulti_sk[i % 2];
    }
    msg[0] = 'Z';
    _smulti_want(ev_send_multi(ev, heavy, 2 * SMULTI_REPEAT, msg, 1, 1), ERR_OK);
}
static void _smulti_rounds_cb(void *arg) {
    (void)arg;
    _smulti_rounds();
}
// 建 2 线程 ev、监听 port、第 k 个客户端连到线程 k % 2，等全部连通。返回连通数
static int32_t _smulti_setup(ev_ctx *ev, uint16_t port, cbs_ctx *lcbs) {
    uint64_t id;
    int32_t i;
    ATOMIC_SET(&_g_smulti_conn, 0);
    ATOMIC_SET(&_g_smulti_bad, 0);
    ATOMIC_SET(&_g_smulti_hold, 0);
    for (i = 0; i < SMULTI_NCLI; i++) {
        ATOMIC_SET(&_g_smulti_res[i], 0);
        ZERO(&_g_smulti_sk[i], sizeof(sock_ctx));
        sock_set_invalid(&_g_smulti_sk[i]);
    }
    ZERO(&_g_smulti_ccbs, sizeof(_g_smulti_ccbs));
    _g_smulti_ccbs.conn_cb = _smulti_on_conn;
    _g_smulti_ccbs.r_cb = _launch_on_recv;
    ev_init(ev, 2, NULL);
    _g_smulti_ev = ev;
    if (ERR_OK != ev_listen(ev, NULL, "127.0.0.1", port, lcbs, NULL, NULL, 0, &id)) {
        return 0;
    }
    MSLEEP(50);// listen 落地是异步的
    for (i = 0; i < SMULTI_NCLI; i++) {
        ev_connect(ev, NULL, "127.0.0.1", port, &_g_smulti_ccbs, NULL, 0, i % 2, &_g_smulti_sk[i]);
    }
    for (i = 0; i < 1000 && (int32_t)ATOMIC_GET(&_g_smulti_conn) < SMULTI_NCLI; i++) {
        MSLEEP(1);
    }
    return (int32_t)ATOMIC_GET(&_g_smulti_conn);
}
// 用例 1、2 共用：建连、发、有界等 6 个结果到齐、停机后断言。inloop 为 1 时整套发送投到线程 1 上跑：
// 线程 1 那组当场跑、线程 0 那组投递，单发同理
static void _smulti_check(CuTest *tc, uint16_t port, int32_t inloop) {
    ev_ctx ev;
    cbs_ctx lcbs;
    int32_t i, k, conn, done;
    int32_t res[SMULTI_NCLI];
    ZERO(&lcbs, sizeof(lcbs));
    lcbs.r_cb = _smulti_on_recv;
    conn = _smulti_setup(&ev, port, &lcbs);
    if (SMULTI_NCLI == conn) {
        if (inloop) {
            ev_defer_exec(&ev, 1, _smulti_rounds_cb, NULL, NULL);
        } else {
            _smulti_rounds();
        }
    }
    done = 0;
    for (i = 0; i < 2000 && !done; i++) {
        done = 1;
        for (k = 0; k < SMULTI_NCLI; k++) {
            if (0 == ATOMIC_GET(&_g_smulti_res[k])) {
                done = 0;
            }
        }
        if (!done) {
            MSLEEP(1);
        }
    }
    MSLEEP(50);// 多收的话会紧跟着来
    for (k = 0; k < SMULTI_NCLI; k++) {
        res[k] = (int32_t)ATOMIC_GET(&_g_smulti_res[k]);
    }
    ev_free(&ev);
    // 断言放 ev_free 之后，理由同 test_ev_close_in_acp_conn_deferred
    CuAssertIntEquals(tc, SMULTI_NCLI, conn);
    CuAssertIntEquals(tc, 0, (int32_t)ATOMIC_GET(&_g_smulti_bad));
    for (k = 0; k < SMULTI_NCLI; k++) {
        CuAssertIntEquals(tc, 1, res[k]);
    }
}
// ev_send_multi 按线程合批：主线程发，两条线程的组都走投递。同一调用方"广播→单发"在每条连接上按序到达，
// 组内按原序、重复项各发一份，查表失败与失效项不影响别人
static void test_ev_send_multi_order(CuTest *tc) {
    _smulti_check(tc, SMULTI_PORT, 0);
}
// 同上，在线程 1 上发：同线程那组必须当场跑，否则单发的 "S"r 会跑到广播的 "M"r 前面
static void test_ev_send_multi_inloop(CuTest *tc) {
    _smulti_check(tc, SMULTI_INLOOP_PORT, 1);
}
// 占住线程 0 直到 ev_free 已开始，再对全部连接广播一个 copy=0 载荷
static void _smulti_free_cb(void *arg) {
    char *own;
    int32_t i;
    (void)arg;
    ATOMIC_SET(&_g_smulti_hold, 1);
    for (i = 0; i < 2000 && 0 == ATOMIC_GET(&_g_smulti_ev->stopping); i++) {
        MSLEEP(1);
    }
    MSLEEP(200);// IOCP 的 ev_free 先停 AcceptEx 线程才投停止命令，多等一会儿
    MALLOC(own, 1);
    own[0] = 'F';
    _smulti_want(ev_send_multi(_g_smulti_ev, _g_smulti_sk, SMULTI_NCLI, own, 1, 0), ERR_OK);
    ATOMIC_SET(&_g_smulti_hold, 2);
}
// 停机回收：线程 1 已处理停止命令退出，它那组排在停止命令之后，由 ev_free 的排空收走(替它减父计数)；
// 线程 0 那组当场入队，随 ev_free 关连接清掉。块与载荷只放一次靠进程末尾的内存检查与 ASan 抓。
// 线程 1 万一 200ms 内没退出，那组照常执行，用例仍过、只是没走到排空那支
static void test_ev_send_multi_free(CuTest *tc) {
    ev_ctx ev;
    cbs_ctx lcbs;
    int32_t i, conn;
    ZERO(&lcbs, sizeof(lcbs));
    lcbs.r_cb = _launch_on_recv;
    conn = _smulti_setup(&ev, SMULTI_FREE_PORT, &lcbs);
    ev_defer_exec(&ev, 0, _smulti_free_cb, NULL, NULL);
    for (i = 0; i < 1000 && 0 == ATOMIC_GET(&_g_smulti_hold); i++) {
        MSLEEP(1);
    }
    ev_free(&ev);
    CuAssertIntEquals(tc, SMULTI_NCLI, conn);
    CuAssertIntEquals(tc, 2, (int32_t)ATOMIC_GET(&_g_smulti_hold));
    CuAssertIntEquals(tc, 0, (int32_t)ATOMIC_GET(&_g_smulti_bad));
}
// 宽线程用例的服务端：缓冲不清，记下累计收到的字节数，正好攒够载荷长度时逐字比对，结果记在 _g_smulti_res[0]
static void _smulti_wide_on_recv(ev_ctx *ev, sock_ctx *sk,
                                 int32_t client, buffer_ctx *buf, size_t size, ud_cxt *ud) {
    char got[SMULTI_WIDE_LEN];
    (void)ev; (void)sk; (void)client; (void)size; (void)ud;
    ATOMIC_SET(&_g_smulti_nrecv, (atomic_t)buffer_size(buf));
    if (SMULTI_WIDE_LEN == buffer_size(buf)) {
        buffer_copyout(buf, 0, got, SMULTI_WIDE_LEN);
        ATOMIC_SET(&_g_smulti_res[0], 0 == memcmp(got, SMULTI_WIDE_MSG, SMULTI_WIDE_LEN) ? 1 : 2);
    }
}
// 65 条线程：ev_send_multi 的计数表改走堆上分配，下标 64 那格必须落在表内(越界由 ASan 抓)；
// 列表先放同线程的 UDP，按非 TCP 丢掉并交回一份组内计数，同组的 TCP 照常发出 copy=0 载荷，服务端一字不多不少收到。
// 块与载荷各放一次靠进程末尾的内存检查与 ASan 抓
static void test_ev_send_multi_wide(CuTest *tc) {
    ev_ctx ev;
    cbs_ctx lcbs, ccbs, ucbs;
    sock_ctx list[2];
    uint64_t id;
    char *own;
    const int32_t only0[] = { 0 };// 只在线程 0 上收连接，免得 65 条线程各开一个监听
    int32_t i, conn, nrecv, res;
    int32_t rtn[4];
    ZERO(&lcbs, sizeof(lcbs));
    lcbs.r_cb = _smulti_wide_on_recv;
    ZERO(&ccbs, sizeof(ccbs));
    ccbs.conn_cb = _smulti_on_conn;
    ccbs.r_cb = _launch_on_recv;
    ZERO(&ucbs, sizeof(ucbs));
    ucbs.rf_cb = _launch_on_recvfrom;
    ZERO(list, sizeof(list));
    sock_set_invalid(&list[0]);
    sock_set_invalid(&list[1]);
    ATOMIC_SET(&_g_smulti_conn, 0);
    ATOMIC_SET(&_g_smulti_nrecv, 0);
    ATOMIC_SET(&_g_smulti_res[0], 0);
    ev_init(&ev, SMULTI_WIDE_NTHS, NULL);
    rtn[0] = ev_listen(&ev, NULL, "127.0.0.1", SMULTI_WIDE_PORT, &lcbs, NULL, only0, 1, &id);
    MSLEEP(50);// listen 落地是异步的
    // 两个都在最后一条线程上：UDP 的注册命令排在广播之前，广播到时它已在表里
    rtn[1] = ev_udp(&ev, "127.0.0.1", SMULTI_WIDE_UDP_PORT, &ucbs, NULL, SMULTI_WIDE_NTHS - 1, &list[0]);
    rtn[2] = ev_connect(&ev, NULL, "127.0.0.1", SMULTI_WIDE_PORT, &ccbs, NULL, 0, SMULTI_WIDE_NTHS - 1, &list[1]);
    for (i = 0; i < 1000 && 0 == ATOMIC_GET(&_g_smulti_conn); i++) {
        MSLEEP(1);
    }
    conn = (int32_t)ATOMIC_GET(&_g_smulti_conn);
    rtn[3] = ERR_FAILED;
    if (1 == conn) {
        MALLOC(own, SMULTI_WIDE_LEN);
        memcpy(own, SMULTI_WIDE_MSG, SMULTI_WIDE_LEN);
        rtn[3] = ev_send_multi(&ev, list, 2, own, SMULTI_WIDE_LEN, 0);
    }
    for (i = 0; i < 2000 && 0 == ATOMIC_GET(&_g_smulti_res[0]); i++) {
        MSLEEP(1);
    }
    MSLEEP(50);// 多收的话会紧跟着来
    nrecv = (int32_t)ATOMIC_GET(&_g_smulti_nrecv);
    res = (int32_t)ATOMIC_GET(&_g_smulti_res[0]);
    ev_free(&ev);
    // 断言放 ev_free 之后，理由同 test_ev_close_in_acp_conn_deferred
    for (i = 0; i < 4; i++) {
        CuAssertIntEquals(tc, ERR_OK, rtn[i]);
    }
    CuAssertIntEquals(tc, SMULTI_WIDE_NTHS - 1, list[0].index);
    CuAssertIntEquals(tc, SMULTI_WIDE_NTHS - 1, list[1].index);
    CuAssertIntEquals(tc, 1, conn);
    CuAssertIntEquals(tc, (int32_t)SMULTI_WIDE_LEN, nrecv);
    CuAssertIntEquals(tc, 1, res);
}
// 当场快捷路径用例的共用状态：目标线程上的投递回调起个辅助线程调 ev_udp / ev_listen 并等它返回，
// 这时对应的 CMD_ADD / CMD_LSN 已排进本线程的命令队列、还没执行，回调接着当场 ev_close / ev_unlisten
static ev_ctx *_g_pend_ev;
static cbs_ctx _g_pend_cbs;
static sock_ctx _g_pend_sk;
static atomic_t _g_pend_rtn;// 辅助线程里 ev_udp / ev_listen 的返回值
static atomic_t _g_pend_done;// 投递回调跑完了
static void _pend_udp_th(void *arg) {
    (void)arg;
    ATOMIC_SET(&_g_pend_rtn, (atomic_t)ev_udp(_g_pend_ev, "127.0.0.1", PEND_UDP_PORT, &_g_pend_cbs, NULL,
                                              0, &_g_pend_sk));
}
static void _pend_udp_cb(void *arg) {
    (void)arg;
    thread_join(thread_creat(_pend_udp_th, NULL));
    if (ERR_OK == (int32_t)ATOMIC_GET(&_g_pend_rtn)) {
        ev_close(_g_pend_ev, &_g_pend_sk);
    }
    ATOMIC_SET(&_g_pend_done, 1);
}
// 别的线程建的 UDP、CMD_ADD 还在目标线程队里时，在目标线程上 ev_close：fd 还不在本线程的表里，
// 这次关闭不能丢，得排到 CMD_ADD 后面执行，关闭回调照常来
static void test_ev_close_pending_add(CuTest *tc) {
    ev_ctx ev;
    int32_t i, rtn, done, closed;
    ZERO(&_g_pend_cbs, sizeof(_g_pend_cbs));
    _g_pend_cbs.rf_cb = _launch_on_recvfrom;
    _g_pend_cbs.c_cb = _udp_close_on_close;
    ATOMIC_SET(&_g_pend_rtn, (atomic_t)ERR_FAILED);
    ATOMIC_SET(&_g_pend_done, 0);
    ATOMIC_SET(&_g_udp_nclose, 0);
    ev_init(&ev, 1, NULL);
    _g_pend_ev = &ev;
    ev_defer_exec(&ev, 0, _pend_udp_cb, NULL, NULL);
    for (i = 0; i < 1000 && 0 == ATOMIC_GET(&_g_udp_nclose); i++) {
        MSLEEP(1);
    }
    MSLEEP(50);// 关两次的话第二次会紧跟着来
    rtn = (int32_t)ATOMIC_GET(&_g_pend_rtn);
    done = (int32_t)ATOMIC_GET(&_g_pend_done);
    closed = (int32_t)ATOMIC_GET(&_g_udp_nclose);
    ev_free(&ev);
    // 断言放 ev_free 之后，理由同 test_ev_close_in_acp_conn_deferred
    CuAssertIntEquals(tc, ERR_OK, rtn);
    CuAssertIntEquals(tc, 1, done);
    CuAssertIntEquals(tc, 1, closed);
}
#ifndef EV_IOCP
static uint64_t _g_pend_id;
static atomic_t _g_pend_stale;// 元素表里键为 INVALID_SOCK 的残留项：1 有，0 没有，-1 还没查
static void _pend_lsn_th(void *arg) {
    const int32_t only0[] = { 0 };
    (void)arg;
    ATOMIC_SET(&_g_pend_rtn, (atomic_t)ev_listen(_g_pend_ev, NULL, "127.0.0.1", PEND_LSN_PORT, &_g_pend_cbs, NULL,
                                                 only0, 1, &_g_pend_id));
}
static void _pend_lsn_cb(void *arg) {
    (void)arg;
    thread_join(thread_creat(_pend_lsn_th, NULL));
    if (ERR_OK == (int32_t)ATOMIC_GET(&_g_pend_rtn)) {
        ev_unlisten(_g_pend_ev, _g_pend_id);
    }
    ATOMIC_SET(&_g_pend_done, 1);
}
// 主线程投来的走命令队列、排在 CMD_LSN 之后：跑到这里时 CMD_LSN 已执行过
static void _pend_stale_cb(void *arg) {
    (void)arg;
    ATOMIC_SET(&_g_pend_stale, NULL != _evpub_sockel_get(&_g_pend_ev->watcher[0], INVALID_SOCK) ? 1 : 0);
}
// 同上换成监听(只在 Unix：IOCP 的监听不走命令队列，没有这个先后)：CMD_LSN 晚于当场的 ev_unlisten 到达，
// 不能把已关的 fd 再登记进元素表。否则 lsn 过了隔离期释放后表里留着悬空项，ev_free 扫表时读已释放内存
static void test_ev_unlisten_pending_lsn(CuTest *tc) {
    ev_ctx ev;
    SOCKET fd;
    int32_t i, rtn, done, stale, connected;
    ZERO(&_g_pend_cbs, sizeof(_g_pend_cbs));
    _g_pend_cbs.r_cb = _launch_on_recv;
    ATOMIC_SET(&_g_pend_rtn, (atomic_t)ERR_FAILED);
    ATOMIC_SET(&_g_pend_done, 0);
    ATOMIC_SET(&_g_pend_stale, -1);
    ev_init(&ev, 1, NULL);
    _g_pend_ev = &ev;
    ev_defer_exec(&ev, 0, _pend_lsn_cb, NULL, NULL);
    for (i = 0; i < 1000 && 0 == ATOMIC_GET(&_g_pend_done); i++) {
        MSLEEP(1);
    }
    ev_defer_exec(&ev, 0, _pend_stale_cb, NULL, NULL);
    for (i = 0; i < 1000 && -1 == (int32_t)ATOMIC_GET(&_g_pend_stale); i++) {
        MSLEEP(1);
    }
    rtn = (int32_t)ATOMIC_GET(&_g_pend_rtn);
    done = (int32_t)ATOMIC_GET(&_g_pend_done);
    stale = (int32_t)ATOMIC_GET(&_g_pend_stale);
    fd = _connect_local(PEND_LSN_PORT, 0);
    connected = INVALID_SOCK != fd;
    CLOSE_SOCK(fd);
    // 等 lsn 真被放掉再 ev_free，悬空项由 ASan 抓。已查到残留项就不等：lsn 还活着，ev_free 不会崩，交给断言报
    if (0 == stale) {
        MSLEEP(QTN_WAIT_MS);
    }
    ev_free(&ev);
    // 断言放 ev_free 之后，理由同 test_ev_close_in_acp_conn_deferred
    CuAssertIntEquals(tc, ERR_OK, rtn);
    CuAssertIntEquals(tc, 1, done);
    CuAssertIntEquals(tc, 0, stale);
    CuAssertTrue(tc, !connected);
}
#endif
// UDP 关闭用例：ud.context 只当"还没放"的标记，ud_free 放过一次就置空，之后重复调用是空操作
static char _g_udfree_mark;
static atomic_t _g_udfree_n;// ud_free 真正放掉 context 的次数
static void _udfree_cb(void *arg) {
    ud_cxt *ud = arg;
    if (NULL != ud->context) {
        ud->context = NULL;
        ATOMIC_ADD(&_g_udfree_n, 1);
    }
}
// UDP 关闭回调之后当场放 ud(同 TCP)，不拖到隔离期(500ms)满：业务 task 已退出时上下文不该多挂这一段
static void test_ev_udp_close_udfree(CuTest *tc) {
    ev_ctx ev;
    cbs_ctx cbs;
    ud_cxt ud;
    sock_ctx sk;
    int32_t i, rtn, closed, nfree;
    ZERO(&cbs, sizeof(cbs));
    cbs.rf_cb = _launch_on_recvfrom;
    cbs.c_cb = _udp_close_on_close;
    cbs.ud_free = _udfree_cb;
    ZERO(&ud, sizeof(ud));
    ud.context = &_g_udfree_mark;
    ATOMIC_SET(&_g_udfree_n, 0);
    ATOMIC_SET(&_g_udp_nclose, 0);
    ev_init(&ev, 1, NULL);
    rtn = ev_udp(&ev, "127.0.0.1", UDP_UDFREE_PORT, &cbs, &ud, INVALID_INDEX, &sk);
    if (ERR_OK == rtn) {
        ev_close(&ev, &sk);// 同一线程投的命令按序执行，排在 CMD_ADD 之后
    }
    for (i = 0; ERR_OK == rtn && i < 1000 && 0 == ATOMIC_GET(&_g_udp_nclose); i++) {
        MSLEEP(1);
    }
    // 关闭回调与 ud_free 在同一处先后调，给 150ms 足够；拖到隔离期才放的话要 500ms 往上
    for (i = 0; ERR_OK == rtn && i < 150 && 0 == ATOMIC_GET(&_g_udfree_n); i++) {
        MSLEEP(1);
    }
    closed = (int32_t)ATOMIC_GET(&_g_udp_nclose);
    nfree = (int32_t)ATOMIC_GET(&_g_udfree_n);
    ev_free(&ev);
    CuAssertIntEquals(tc, ERR_OK, rtn);
    CuAssertIntEquals(tc, 1, closed);
    CuAssertIntEquals(tc, 1, nfree);
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_udfree_n));// 隔离期满与 ev_free 再调也不重复放
}
// 切协议用例：ud_free 真放掉 context 时连接当时的 pktype 与次数，以及事件线程上当场切之后的观察值
static sock_ctx _g_pkt_sk;
static atomic_t _g_pkt_freed_type;// 放掉时的 pktype，0xffff 表示还没放过
static atomic_t _g_pkt_nfree;
static atomic_t _g_pkt_step[3];// 切成同类型后的放掉次数、换类型后的放掉次数、换类型后 context 已空且 pktype 已换(1)；-1 还没跑
static void _pkt_udfree(void *arg) {
    ud_cxt *ud = arg;
    if (NULL != ud->context) {
        ud->context = NULL;
        ATOMIC_SET(&_g_pkt_freed_type, (atomic_t)ud->pktype);
        ATOMIC_ADD(&_g_pkt_nfree, 1);
    }
}
// 事件线程上当场切：先切成同一类型(不该放)，再换类型(按旧类型放一次)，然后关掉
static void _pkt_switch_cb(void *arg) {
    ev_ctx *ev = arg;
    ev_ud_pktype(ev, &_g_pkt_sk, PACK_CUSTZ_FIXED);
    ATOMIC_SET(&_g_pkt_step[0], ATOMIC_GET(&_g_pkt_nfree));
    ev_ud_pktype(ev, &_g_pkt_sk, PACK_NONE);
    ATOMIC_SET(&_g_pkt_step[1], ATOMIC_GET(&_g_pkt_nfree));
    evsock_ctx *evsk = _evpub_sockel_get(&ev->watcher[_g_pkt_sk.index], _g_pkt_sk.fd);
    ud_cxt *ud = NULL == evsk ? NULL : _evpub_get_ud(evsk);
    ATOMIC_SET(&_g_pkt_step[2], (NULL != ud && NULL == ud->context && PACK_NONE == ud->pktype) ? 1 : 0);
    ev_close(ev, &_g_pkt_sk);
}
// 已挂协议上下文的连接切类型：按旧类型调一次 ud_free、context 置空，切成同一类型不动；
// 之后关闭再调 ud_free 是空操作，不重复放也不漏放
static void test_ev_ud_pktype_udfree(CuTest *tc) {
    ev_ctx ev;
    cbs_ctx cbs;
    ud_cxt ud;
    ZERO(&cbs, sizeof(cbs));
    cbs.rf_cb = _launch_on_recvfrom;
    cbs.c_cb = _udp_close_on_close;
    cbs.ud_free = _pkt_udfree;
    ZERO(&ud, sizeof(ud));
    ud.pktype = PACK_CUSTZ_FIXED;
    ud.context = &_g_udfree_mark;
    ATOMIC_SET(&_g_pkt_freed_type, 0xffff);
    ATOMIC_SET(&_g_pkt_nfree, 0);
    int32_t i;
    for (i = 0; i < 3; i++) {
        ATOMIC_SET(&_g_pkt_step[i], (atomic_t)-1);
    }
    ATOMIC_SET(&_g_udp_nclose, 0);
    ev_init(&ev, 1, NULL);
    int32_t rtn = ev_udp(&ev, "127.0.0.1", UD_PKTYPE_PORT, &cbs, &ud, INVALID_INDEX, &_g_pkt_sk);
    if (ERR_OK == rtn) {
        ev_defer_exec(&ev, _g_pkt_sk.index, _pkt_switch_cb, NULL, &ev);// 走命令队列，排在 CMD_ADD 之后
    }
    for (i = 0; ERR_OK == rtn && i < 1000 && 0 == ATOMIC_GET(&_g_udp_nclose); i++) {
        MSLEEP(1);
    }
    MSLEEP(50);// 关闭后再调 ud_free 若重复放会紧跟着来
    ev_free(&ev);
    CuAssertIntEquals(tc, ERR_OK, rtn);
    CuAssertIntEquals(tc, 0, (int32_t)ATOMIC_GET(&_g_pkt_step[0]));
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_pkt_step[1]));
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_pkt_step[2]));
    CuAssertIntEquals(tc, PACK_CUSTZ_FIXED, (int32_t)ATOMIC_GET(&_g_pkt_freed_type));
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_pkt_nfree));// 关闭、隔离期满与 ev_free 再调都是空操作
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_udp_nclose));
}
#ifndef EV_IOCP
// tick 里关 UDP 用例：tick 回调里把 fd 换成非 socket，同线程当场发送必失败、要关这个 UDP。
// 关闭回调与 ud_free 都得等本轮 tick 跑完才来，tick 里看到的 socket 仍在表里
static ev_ctx *_g_tclose_ev;
static sock_ctx _g_tclose_sk;
static ev_tick _g_tclose_tick;
static atomic_t _g_tclose_intick;// tick 回调正在发送那一段
static atomic_t _g_tclose_bad;// 关闭回调或 ud_free 嵌在那一段里来的次数
static atomic_t _g_tclose_alive;// 发送失败之后当场查：socket 还在表里且 skid 没变记 1；0 还没跑，2 不在了
static atomic_t _g_tclose_closed;// 关闭回调次数
static atomic_t _g_tclose_freed;// ud_free 真放掉 context 的次数
static void _tclose_on_close(ev_ctx *ev, sock_ctx *sk, int32_t client, int32_t erro, ud_cxt *ud) {
    (void)ev; (void)sk; (void)client; (void)erro; (void)ud;
    ATOMIC_ADD(&_g_tclose_bad, ATOMIC_GET(&_g_tclose_intick));
    ATOMIC_ADD(&_g_tclose_closed, 1);
}
static void _tclose_udfree(void *arg) {
    ud_cxt *ud = arg;
    if (NULL != ud->context) {
        ud->context = NULL;
        ATOMIC_ADD(&_g_tclose_bad, ATOMIC_GET(&_g_tclose_intick));
        ATOMIC_ADD(&_g_tclose_freed, 1);
    }
}
static uint32_t _tclose_tick_cb(void *ud, uint64_t now_ms) {
    struct watcher_ctx *watcher = ud;
    int nullfd = open("/dev/null", O_RDWR);
    (void)now_ms;
    _evpub_tick_detach(watcher, &_g_tclose_tick);// 只跑这一次
    if (nullfd < 0) {
        return EVENT_WAIT_TIMEOUT;
    }
    // 换掉之后原 socket 随之关闭，sendmsg 报 ENOTSOCK，按 fd 已废处理
    if (dup2(nullfd, (int)_g_tclose_sk.fd) >= 0) {
        ATOMIC_SET(&_g_tclose_intick, 1);
        ev_sendto(_g_tclose_ev, &_g_tclose_sk, "127.0.0.1", TICK_CLOSE_PORT, "x", 1, 1);
        evsock_ctx *evsk = _evpub_sockel_get(watcher, _g_tclose_sk.fd);
        ATOMIC_SET(&_g_tclose_alive, (NULL != evsk && ERR_OK == _evpub_checkid(evsk, _g_tclose_sk.skid)) ? 1 : 2);
        ATOMIC_SET(&_g_tclose_intick, 0);
    }
    close(nullfd);
    return EVENT_WAIT_TIMEOUT;
}
static void _tclose_attach_cb(void *arg) {
    struct watcher_ctx *watcher = arg;
    _evpub_tick_attach(watcher, &_g_tclose_tick, _tclose_tick_cb, watcher);
}
// tick 回调里同线程发送失败：关闭推迟到本轮 tick 全部跑完，关闭回调与 ud_free 各只来一次
static void test_ev_udp_close_in_tick(CuTest *tc) {
    ev_ctx ev;
    cbs_ctx cbs;
    ud_cxt ud;
    ZERO(&cbs, sizeof(cbs));
    cbs.rf_cb = _launch_on_recvfrom;
    cbs.c_cb = _tclose_on_close;
    cbs.ud_free = _tclose_udfree;
    ZERO(&ud, sizeof(ud));
    ud.context = &_g_udfree_mark;
    ZERO(&_g_tclose_tick, sizeof(_g_tclose_tick));
    ATOMIC_SET(&_g_tclose_intick, 0);
    ATOMIC_SET(&_g_tclose_bad, 0);
    ATOMIC_SET(&_g_tclose_alive, 0);
    ATOMIC_SET(&_g_tclose_closed, 0);
    ATOMIC_SET(&_g_tclose_freed, 0);
    ev_init(&ev, 1, NULL);
    _g_tclose_ev = &ev;
    int32_t rtn = ev_udp(&ev, "127.0.0.1", TICK_CLOSE_PORT, &cbs, &ud, INVALID_INDEX, &_g_tclose_sk);
    if (ERR_OK == rtn) {
        ev_defer_exec(&ev, _g_tclose_sk.index, _tclose_attach_cb, NULL, &ev.watcher[_g_tclose_sk.index]);
    }
    for (int32_t i = 0; ERR_OK == rtn && i < 1000 && 0 == ATOMIC_GET(&_g_tclose_closed); i++) {
        MSLEEP(1);
    }
    MSLEEP(50);// 关两次的话第二次会紧跟着来
    ev_free(&ev);
    // 断言放 ev_free 之后，理由同 test_ev_close_in_acp_conn_deferred
    CuAssertIntEquals(tc, ERR_OK, rtn);
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_tclose_alive));
    CuAssertIntEquals(tc, 0, (int32_t)ATOMIC_GET(&_g_tclose_bad));
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_tclose_closed));
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_tclose_freed));
}
#endif
// net 线程上的末次 ungrab 用例：自起一个 loader(1 个 net 线程、1 个 worker)，task 在 net 线程上丢掉最后一份引用。
// 摘表当场做，释放推迟到本轮投递回调之后、仍在 net 线程上跑，loader_free 要等它做完才停 net 线程
static loader_ctx *_g_nfree_loader;
static task_ctx *_g_nfree_task;
static atomic_t _g_nfree_started;// startup 跑过
static atomic_t _g_nfree_freed;// _argfree 跑过
static atomic_t _g_nfree_idx;// _argfree 跑在第几个 net 线程上(+1)，0 表示不在 net 线程上
static atomic_t _g_nfree_stopping;// _argfree 跑时 ev_free 已开始
static atomic_t _g_nfree_in[3];// ungrab 之后当场：任务还在表里、_argfree 已跑、nfreeing；-1 还没跑
static atomic_t _g_nfree_done;// 投递回调里的 ungrab 与当场查看做完了
static void _nfree_argfree(void *arg) {
    (void)arg;
    ATOMIC_SET(&_g_nfree_idx, (atomic_t)(ev_cur_index(&_g_nfree_loader->netev) + 1));
    ATOMIC_SET(&_g_nfree_stopping, ATOMIC_GET(&_g_nfree_loader->netev.stopping));
    ATOMIC_SET(&_g_nfree_freed, 1);
}
static void _nfree_startup(task_ctx *task) {
    (void)task;
    ATOMIC_SET(&_g_nfree_started, 1);
}
// 丢掉最后一份引用后当场看一眼，再占住线程到 loader_free 已开始关停：推迟的释放这时还没跑，关停得等它
static void _nfree_ungrab_cb(void *arg) {
    name_t handle = _g_nfree_task->handle;
    (void)arg;
    task_ungrab(_g_nfree_task);
    ATOMIC_SET(&_g_nfree_in[0], (atomic_t)task_exists(_g_nfree_loader, handle));
    ATOMIC_SET(&_g_nfree_in[1], ATOMIC_GET(&_g_nfree_freed));
    ATOMIC_SET(&_g_nfree_in[2], ATOMIC_GET(&_g_nfree_loader->nfreeing));
    ATOMIC_SET(&_g_nfree_done, 1);
    for (int32_t i = 0; i < 2000 && 0 == ATOMIC_GET(&_g_nfree_loader->closing); i++) {
        MSLEEP(1);
    }
}
static void test_task_ungrab_on_net(CuTest *tc) {
    ATOMIC_SET(&_g_nfree_started, 0);
    ATOMIC_SET(&_g_nfree_freed, 0);
    ATOMIC_SET(&_g_nfree_idx, 0);
    ATOMIC_SET(&_g_nfree_stopping, 0);
    ATOMIC_SET(&_g_nfree_done, 0);
    int32_t i;
    for (i = 0; i < 3; i++) {
        ATOMIC_SET(&_g_nfree_in[i], (atomic_t)-1);
    }
    loader_ctx *loader = loader_init(1, 1, 0);
    _g_nfree_loader = loader;
    task_ctx *task = task_new(loader, NULL, 0, NULL, _nfree_argfree, &_g_nfree_freed);// arg 非 NULL 才调 _argfree
    _g_nfree_task = task;
    int32_t rtn = task_register(task, _nfree_startup, NULL);
    // 等 startup 跑完、worker 放掉在途引用，只剩注册时那一份：net 线程上的 ungrab 才是最后一次
    for (i = 0; ERR_OK == rtn && i < 1000
         && (0 == ATOMIC_GET(&_g_nfree_started) || 1 != ATOMIC_GET(&task->ref)); i++) {
        MSLEEP(1);
    }
    if (ERR_OK == rtn) {
        ev_defer_exec(&loader->netev, 0, _nfree_ungrab_cb, NULL, NULL);
    }
    for (i = 0; ERR_OK == rtn && i < 1000 && 0 == ATOMIC_GET(&_g_nfree_done); i++) {
        MSLEEP(1);
    }
    loader_free(loader);
    CuAssertIntEquals(tc, ERR_OK, rtn);
    CuAssertIntEquals(tc, 0, (int32_t)ATOMIC_GET(&_g_nfree_in[0]));// 摘表当场做
    CuAssertIntEquals(tc, 0, (int32_t)ATOMIC_GET(&_g_nfree_in[1]));// 释放没当场做
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_nfree_in[2]));
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_nfree_freed));
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&_g_nfree_idx));// 在 0 号 net 线程上放的
    CuAssertIntEquals(tc, 0, (int32_t)ATOMIC_GET(&_g_nfree_stopping));// 放在 ev_free 之前：关停等到了它
}
#if 0 != MAX_SENDQ_CNT
static atomic_t _g_sendq_closed;// 服务端关闭回调次数
static atomic_t _g_sendq_erro;// 服务端关闭回调带的 close_type
// accept 回调里(事件线程上)先发一大块塞满两端内核缓冲，再连发小块：之后的只能进发送队列，条数一到上限就得断开
static int32_t _sendq_on_acp(ev_ctx *ev, sock_ctx *sk, ud_cxt *ud) {
    int32_t sndbuf = 4096;
    int32_t i;
    char *big;
    (void)ud;
    (void)setsockopt(sk->fd, SOL_SOCKET, SO_SNDBUF, (char *)&sndbuf, (socklen_t)sizeof(sndbuf));
    MALLOC(big, SENDQ_BIG);
    memset(big, 'q', SENDQ_BIG);
    ev_send(ev, sk, big, SENDQ_BIG, 0);
    for (i = 0; i < 2 * MAX_SENDQ_CNT; i++) {
        ev_send(ev, sk, "x", 1, 1);
    }
    return ERR_OK;
}
static void _sendq_on_close(ev_ctx *ev, sock_ctx *sk, int32_t client, int32_t erro, ud_cxt *ud) {
    (void)ev; (void)sk; (void)client; (void)ud;
    ATOMIC_SET(&_g_sendq_erro, (atomic_t)erro);
    ATOMIC_ADD(&_g_sendq_closed, 1);
}
// 对端一直不读、本端发送队列条数到 MAX_SENDQ_CNT：框架主动断开(报 LOCAL)，不无限攒内存
static void test_ev_sendq_overflow(CuTest *tc) {
    ev_ctx ev;
    cbs_ctx cbs;
    uint64_t id;
    SOCKET fd = INVALID_SOCK;
    int32_t i, lrtn, connected, closed, erro;
    ZERO(&cbs, sizeof(cbs));
    cbs.acp_cb = _sendq_on_acp;
    cbs.r_cb = _launch_on_recv;
    cbs.c_cb = _sendq_on_close;
    ATOMIC_SET(&_g_sendq_closed, 0);
    ATOMIC_SET(&_g_sendq_erro, -1);
    ev_init(&ev, 1, NULL);
    lrtn = ev_listen(&ev, NULL, "127.0.0.1", SENDQ_PORT, &cbs, NULL, NULL, 0, &id);
    MSLEEP(50);// listen 落地是异步的
    if (ERR_OK == lrtn) {
        fd = _connect_local(SENDQ_PORT, 16 * 1024);// 接收缓冲调小，之后一直不读
    }
    connected = INVALID_SOCK != fd;
    for (i = 0; connected && i < 2000 && 0 == ATOMIC_GET(&_g_sendq_closed); i++) {
        MSLEEP(1);
    }
    MSLEEP(50);// 关两次的话第二次会紧跟着来
    closed = (int32_t)ATOMIC_GET(&_g_sendq_closed);
    erro = (int32_t)ATOMIC_GET(&_g_sendq_erro);
    CLOSE_SOCK(fd);
    ev_free(&ev);
    // 断言放 ev_free 之后，理由同 test_ev_close_in_acp_conn_deferred
    CuAssertIntEquals(tc, ERR_OK, lrtn);
    CuAssertTrue(tc, connected);
    CuAssertIntEquals(tc, 1, closed);
    CuAssertIntEquals(tc, CLOSE_TYPE_LOCAL, erro);
}
#endif
#if 0 != CLOSE_LINGER_MS
// 服务端关闭回调次数：用来确认"本端关闭"确实走完了，没有卡在等对端动
static atomic_t _g_linger_closed;
// 服务端最后一次关闭回调带的 close_type：本端主动关必须是 LOCAL，不能被报成 ABORT
static atomic_t _g_linger_erro;
// 忙连接模式下服务端累计收到的字节数，按它区分第几条消息
static atomic_t _g_linger_nrecv;
// 服务端：收到第一段就回一条响应并关连接（对端的请求体还在路上）
static void _linger_on_recv(ev_ctx *ev, sock_ctx *sk,
                            int32_t client, buffer_ctx *buf, size_t size, ud_cxt *ud) {
    (void)client;
    (void)size;
    (void)ud;
    buffer_drain(buf, buffer_size(buf));
    ev_send(ev, sk, (void *)LINGER_RESP, strlen(LINGER_RESP), 1);
    ev_close(ev, sk);
}
// 同上但只回响应，关连接放到发送完成回调里做
static void _linger_on_recv_sendonly(ev_ctx *ev, sock_ctx *sk,
                                     int32_t client, buffer_ctx *buf, size_t size, ud_cxt *ud) {
    (void)client;
    (void)size;
    (void)ud;
    buffer_drain(buf, buffer_size(buf));
    ev_send(ev, sk, (void *)LINGER_RESP, strlen(LINGER_RESP), 1);
}
// 忙连接：第 1 字节回一个大包(发送队列积压、写事件挂上)；第 2 字节把事件线程卡住 200ms，
// 让客户端趁这段时间读走一截(变可写)再发第 3 字节(可读)，于是下一轮 epoll 读写同时就绪；
// 第 3 字节到了就关。epoll 下这正是"同一次回调里读到后同步 ev_close、接着又走写分支"
static void _linger_on_recv_busy(ev_ctx *ev, sock_ctx *sk,
                                 int32_t client, buffer_ctx *buf, size_t size, ud_cxt *ud) {
    char *big;
    size_t n = buffer_size(buf);
    atomic_t old;
    (void)client;
    (void)size;
    (void)ud;
    buffer_drain(buf, n);
    old = ATOMIC_ADD(&_g_linger_nrecv, (atomic_t)n);
    if (0 == old) {
        MALLOC(big, LINGER_BIG);
        memset(big, 'b', LINGER_BIG);
        ev_send(ev, sk, big, LINGER_BIG, 0);
        return;
    }
    if (1 == old) {
        MSLEEP(200);
        return;
    }
    ev_close(ev, sk);
}
static void _linger_on_sent(ev_ctx *ev, sock_ctx *sk, int32_t client, size_t size, ud_cxt *ud) {
    (void)client;
    (void)size;
    (void)ud;
    ev_close(ev, sk);
}
static void _linger_on_close(ev_ctx *ev, sock_ctx *sk, int32_t client, int32_t erro, ud_cxt *ud) {
    (void)ev;
    (void)sk;
    (void)client;
    (void)ud;
    ATOMIC_SET(&_g_linger_erro, (atomic_t)erro);
    ATOMIC_ADD(&_g_linger_closed, 1);
}
static int32_t _linger_server(ev_ctx *ev, uint16_t port, linger_mode mode) {
    cbs_ctx cbs;
    uint64_t id;
    ZERO(&cbs, sizeof(cbs));
    cbs.c_cb = _linger_on_close;
    switch (mode) {
    case LINGER_CLOSE_IN_SENT:
        cbs.r_cb = _linger_on_recv_sendonly;
        cbs.s_cb = _linger_on_sent;
        break;
    case LINGER_CLOSE_BUSY:
        cbs.r_cb = _linger_on_recv_busy;
        break;
    default:
        cbs.r_cb = _linger_on_recv;
        break;
    }
    ATOMIC_SET(&_g_linger_closed, 0);
    ATOMIC_SET(&_g_linger_erro, -1);
    ATOMIC_SET(&_g_linger_nrecv, 0);
    ev_init(ev, 1, NULL);
    if (ERR_OK != ev_listen(ev, NULL, "127.0.0.1", port, &cbs, NULL, NULL, 0, &id)) {
        ev_free(ev);
        return ERR_FAILED;
    }
    MSLEEP(50);// listen 落地是异步的
    return ERR_OK;
}
// 阻塞 connect 后切非阻塞，便于有界轮询读
static SOCKET _linger_client(uint16_t port) {
    netaddr_ctx addr;
    SOCKET fd;
    if (ERR_OK != netaddr_set(&addr, "127.0.0.1", port)) {
        return INVALID_SOCK;
    }
    fd = sock_create_cloexec(netaddr_family(&addr), SOCK_STREAM, 0, 0);
    if (INVALID_SOCK == fd) {
        return fd;
    }
    if (0 != connect(fd, netaddr_addr(&addr), netaddr_size(&addr))
        || ERR_OK != sock_nonblock(fd)) {
        CLOSE_SOCK(fd);
        return INVALID_SOCK;
    }
    return fd;
}
// 读到 FIN 返回 1、出错(如收到 RST)返回 -1、ms 毫秒内没结果返回 0；读到的字节累加进 *got
static int32_t _linger_read_end(SOCKET fd, char *out, size_t cap, size_t *got, int32_t ms) {
    int32_t n;
    int32_t i = 0;
    while (i < ms) {
        if (*got >= cap) {
            return -1;
        }
        n = (int32_t)recv(fd, out + *got, (int32_t)(cap - *got), 0);
        if (n > 0) {
            *got += (size_t)n;
            continue;
        }
        if (0 == n) {
            return 1;
        }
        if (!IS_EAGAIN(ERRNO)) {
            return -1;
        }
        MSLEEP(1);
        i++;
    }
    return 0;
}
// 对端是否已回 RST：发一段，等它到对端，再发一次。收到 RST 之后的那次 send 必失败；
// 读侧看不出来——读到 FIN 之后 Linux 上 recv 一直返回 0
static int32_t _linger_peer_reset(SOCKET fd) {
    (void)send(fd, "x", 1, 0);
    MSLEEP(100);
    return (int32_t)send(fd, "y", 1, 0) < 0 && !IS_EAGAIN(ERRNO) ? 1 : 0;
}
// 服务端关闭回调在 ms 毫秒内来了没有
static int32_t _linger_wait_closed(int32_t ms) {
    int32_t i;
    for (i = 0; i < ms && 0 == ATOMIC_GET(&_g_linger_closed); i++) {
        MSLEEP(1);
    }
    return (int32_t)ATOMIC_GET(&_g_linger_closed);
}
// 连上、发第一段、等服务端回完响应并关闭、读到 FIN，此后客户端不关也不发
static SOCKET _linger_half_closed(uint16_t port, int32_t *fin) {
    char out[256];
    size_t got = 0;
    SOCKET fd = _linger_client(port);
    *fin = 0;
    if (INVALID_SOCK == fd) {
        return fd;
    }
    (void)send(fd, "HDR+CHUNK", 9, 0);
    MSLEEP(100);
    *fin = _linger_read_end(fd, out, sizeof(out), &got, 1000);
    return fd;
}
// 服务端回完响应就关、客户端的后半段在关闭之后才到：客户端仍须读全响应并读到 FIN，
// 之后再发的数据被服务端静默读掉而不是回 RST。
// 关读或带着没读的数据关 fd 都会让内核回 RST，Windows 收到 RST 连已到的响应一起丢
static void test_ev_linger_late_data(CuTest *tc) {
    ev_ctx ev;
    char out[256];
    size_t got = 0;
    int32_t sent1, sent2, early, rtn = 0;
    CuAssertIntEquals(tc, ERR_OK, _linger_server(&ev, LINGER_PORT, LINGER_CLOSE_IN_RECV));
    SOCKET fd = _linger_client(LINGER_PORT);
    if (INVALID_SOCK == fd) {
        ev_free(&ev);
        CuFail(tc, "connect failed");
    }
    sent1 = (int32_t)send(fd, "HDR+CHUNK", 9, 0);
    MSLEEP(100);// 服务端已回响应并关闭
    sent2 = (int32_t)send(fd, "TERM", 4, 0);
    MSLEEP(100);
    rtn = _linger_read_end(fd, out, sizeof(out), &got, 1000);
    early = _linger_peer_reset(fd);
    CLOSE_SOCK(fd);
    ev_free(&ev);
    CuAssertIntEquals(tc, 9, sent1);
    CuAssertIntEquals(tc, 4, sent2);
    CuAssertTrue(tc, strlen(LINGER_RESP) == got);
    CuAssertTrue(tc, 0 == memcmp(out, LINGER_RESP, got));
    CuAssertIntEquals(tc, 1, rtn);
    CuAssertIntEquals(tc, 0, early);
}
// 对端读到 FIN 后完全静默：关闭回调照常马上来(计时从本端关闭起算，不等对端动)，
// 到期后服务端 fd 已关，对端再发就会收到 RST
static void test_ev_linger_timeout(CuTest *tc) {
    ev_ctx ev;
    int32_t fin, closed, late, erro;
    CuAssertIntEquals(tc, ERR_OK, _linger_server(&ev, LINGER_PORT + 1, LINGER_CLOSE_IN_RECV));
    SOCKET fd = _linger_half_closed(LINGER_PORT + 1, &fin);
    if (INVALID_SOCK == fd) {
        ev_free(&ev);
        CuFail(tc, "connect failed");
    }
    closed = _linger_wait_closed(500);
    erro = (int32_t)ATOMIC_GET(&_g_linger_erro);
    MSLEEP(CLOSE_LINGER_MS + 300);
    late = _linger_peer_reset(fd);
    CLOSE_SOCK(fd);
    ev_free(&ev);
    CuAssertIntEquals(tc, 1, fin);
    CuAssertIntEquals(tc, 1, closed);
    CuAssertIntEquals(tc, CLOSE_TYPE_LOCAL, erro);
    CuAssertIntEquals(tc, 1, late);
}
// 关连接放在 s_cb 里：发送路径里同步 ev_close 之后，关闭照样要马上走完
static void test_ev_linger_close_in_sent(CuTest *tc) {
    ev_ctx ev;
    int32_t fin, closed, early, erro;
    CuAssertIntEquals(tc, ERR_OK, _linger_server(&ev, LINGER_PORT + 4, LINGER_CLOSE_IN_SENT));
    SOCKET fd = _linger_half_closed(LINGER_PORT + 4, &fin);
    if (INVALID_SOCK == fd) {
        ev_free(&ev);
        CuFail(tc, "connect failed");
    }
    closed = _linger_wait_closed(500);
    erro = (int32_t)ATOMIC_GET(&_g_linger_erro);
    early = _linger_peer_reset(fd);
    CLOSE_SOCK(fd);
    ev_free(&ev);
    CuAssertIntEquals(tc, 1, fin);
    CuAssertIntEquals(tc, 1, closed);
    CuAssertIntEquals(tc, CLOSE_TYPE_LOCAL, erro);
    CuAssertIntEquals(tc, 0, early);
}
// 对端读到 FIN 后猛灌数据：丢满 CLOSE_LINGER_BYTES 就提前关，不等到期
static void test_ev_linger_bytes(CuTest *tc) {
    ev_ctx ev;
    char blk[65536];
    int32_t fin, n, reset = 0;
    size_t total = 0;
    uint64_t t0, cost;
    CuAssertIntEquals(tc, ERR_OK, _linger_server(&ev, LINGER_PORT + 2, LINGER_CLOSE_IN_RECV));
    SOCKET fd = _linger_half_closed(LINGER_PORT + 2, &fin);
    if (INVALID_SOCK == fd) {
        ev_free(&ev);
        CuFail(tc, "connect failed");
    }
    ZERO(blk, sizeof(blk));
    t0 = nowms();
    // 灌到出错为止，最多灌上限的 4 倍、最长 CLOSE_LINGER_MS
    while (total < 4 * (size_t)CLOSE_LINGER_BYTES && nowms() - t0 < CLOSE_LINGER_MS) {
        n = (int32_t)send(fd, blk, (int32_t)sizeof(blk), 0);
        if (n > 0) {
            total += (size_t)n;
            continue;
        }
        if (n < 0 && !IS_EAGAIN(ERRNO)) {
            reset = 1;
            break;
        }
        MSLEEP(1);
    }
    if (0 == reset) {
        reset = _linger_peer_reset(fd);
    }
    cost = nowms() - t0;
    CLOSE_SOCK(fd);
    ev_free(&ev);
    CuAssertIntEquals(tc, 1, fin);
    CuAssertIntEquals(tc, 1, reset);
    CuAssertTrue(tc, total > (size_t)CLOSE_LINGER_BYTES);
    CuAssertTrue(tc, cost < CLOSE_LINGER_MS);
}
// 有连接正在延迟关闭时 ev_free：立即拆完，不等它到期(泄漏由结尾的内存检查兜)
static void test_ev_linger_free(CuTest *tc) {
    ev_ctx ev;
    int32_t fin, closed, erro;
    uint64_t t0, cost;
    CuAssertIntEquals(tc, ERR_OK, _linger_server(&ev, LINGER_PORT + 3, LINGER_CLOSE_IN_RECV));
    SOCKET fd = _linger_half_closed(LINGER_PORT + 3, &fin);
    int32_t connected = INVALID_SOCK != fd;
    closed = _linger_wait_closed(500);// 确认已进入延迟关闭再拆
    erro = (int32_t)ATOMIC_GET(&_g_linger_erro);
    t0 = nowms();
    ev_free(&ev);
    cost = nowms() - t0;
    if (connected) {
        CLOSE_SOCK(fd);// 会把 fd 置成 INVALID_SOCK，所以上面先记下 connected
    }
    CuAssertTrue(tc, connected);
    CuAssertIntEquals(tc, 1, fin);
    CuAssertIntEquals(tc, 1, closed);
    CuAssertIntEquals(tc, CLOSE_TYPE_LOCAL, erro);
    CuAssertTrue(tc, cost < CLOSE_LINGER_MS / 2);
}
// 读写同时就绪的那一次回调里同步 ev_close（epoll 才会把两者合成一次回调，kqueue/IOCP 上照跑不报错）：
// 关闭照样要马上走完且报 LOCAL。发送路径若不认"本端已在关"，队列冲空时会把驱动关闭的写事件摘掉
// (关闭卡住)，没冲空时会往已关写的 socket 上发(被报成 ABORT)
static void test_ev_linger_close_busy(CuTest *tc) {
    ev_ctx ev;
    char blk[65536];
    size_t got = 0;
    int32_t n, closed, erro;
    CuAssertIntEquals(tc, ERR_OK, _linger_server(&ev, LINGER_PORT + 5, LINGER_CLOSE_BUSY));
    SOCKET fd = _linger_client(LINGER_PORT + 5);
    if (INVALID_SOCK == fd) {
        ev_free(&ev);
        CuFail(tc, "connect failed");
    }
    (void)send(fd, "g", 1, 0);
    MSLEEP(100);// 服务端回大包，发送队列积压、写事件挂上
    (void)send(fd, "h", 1, 0);
    MSLEEP(50);// 服务端正卡在处理 "h" 的回调里
    while (got < 4 * sizeof(blk)) {
        n = (int32_t)recv(fd, blk, (int32_t)sizeof(blk), 0);
        if (n <= 0) {
            break;
        }
        got += (size_t)n;
    }
    (void)send(fd, "c", 1, 0);// 服务端这时既可写(刚被读走一截)又可读
    closed = _linger_wait_closed(1000);
    erro = (int32_t)ATOMIC_GET(&_g_linger_erro);
    CLOSE_SOCK(fd);
    ev_free(&ev);
    CuAssertTrue(tc, got > 0);
    CuAssertIntEquals(tc, 1, closed);
    CuAssertIntEquals(tc, CLOSE_TYPE_LOCAL, erro);
}
// 收包回调里 ev_close、对端发完随即关写：FIN 与数据同一次读到时本端已在关，close_type 仍报 LOCAL。
// 借延迟关闭这组的服务端(收到就回响应并关)。默认构建下明文读没填满就停、FIN 留到下一轮，这里未必撞得上
// 同一次读，确定的那档见 test_evpub_close_type 第 8 段
static void test_ev_close_in_recv_peer_fin(CuTest *tc) {
    ev_ctx ev;
    int32_t sent, closed, erro;
    CuAssertIntEquals(tc, ERR_OK, _linger_server(&ev, CLOSE_FIN_PORT, LINGER_CLOSE_IN_RECV));
    SOCKET fd = _linger_client(CLOSE_FIN_PORT);
    if (INVALID_SOCK == fd) {
        ev_free(&ev);
        CuFail(tc, "connect failed");
    }
    sent = (int32_t)send(fd, "HDR", 3, 0);
    (void)shutdown(fd, SHUT_WR);
    closed = _linger_wait_closed(1000);
    erro = (int32_t)ATOMIC_GET(&_g_linger_erro);
    CLOSE_SOCK(fd);
    ev_free(&ev);
    CuAssertIntEquals(tc, 3, sent);
    CuAssertIntEquals(tc, 1, closed);
    CuAssertIntEquals(tc, CLOSE_TYPE_LOCAL, erro);
}
// 延迟关闭只给"已连通、本端主动关"的连接：未连通、对端已表态(FIN/ABORT/TRUNCATED)的一律不延迟，保留关读
static void test_evpub_linger_want(CuTest *tc) {
    int32_t st;
    CuAssertTrue(tc, 0 != _evpub_linger_want(STATUS_ESTABLISHED));
    CuAssertTrue(tc, 0 != _evpub_linger_want(STATUS_ESTABLISHED | STATUS_CLIENT | STATUS_ERROR));
    CuAssertTrue(tc, 0 == _evpub_linger_want(STATUS_NONE));
    CuAssertTrue(tc, 0 == _evpub_linger_want(STATUS_CLIENT | STATUS_ERROR));
    st = STATUS_ESTABLISHED;
    _evpub_mark_close(&st, 1);
    CuAssertTrue(tc, 0 == _evpub_linger_want(st));
    st = STATUS_ESTABLISHED;
    _evpub_mark_close(&st, ERR_FAILED);
    CuAssertTrue(tc, 0 == _evpub_linger_want(st));
#if WITH_SSL
    st = STATUS_ESTABLISHED;
    _evpub_mark_close(&st, 2);
    CuAssertTrue(tc, 0 == _evpub_linger_want(st));
#endif
}
#endif
#if WITH_SSL
// SSL 合并写用例的服务端状态，只在事件线程里读写：已入队的块数与字节数、s_cb 报的已发字节数、正在批量入队
static size_t _g_mrg_nchunk;
static size_t _g_mrg_queued;
static size_t _g_mrg_sent;
static int32_t _g_mrg_pushing;
// 流里第 p 个字节与第 i 块的长度(1~300)，收发两端按同一规律生成与校验
static char _mrg_byte(size_t p) {
    return (char)((p * 7) ^ (p >> 8));
}
static size_t _mrg_chunk_len(size_t i) {
    return 1 + (i * 131) % 300;
}
// 在事件线程同一次回调里成批入队小块，让它们攒在发送队列里走合并写。
// 一批在入队途中就全发完的话不会再有 s_cb 来接力，接着补下一批
static void _mrg_push(ev_ctx *ev, sock_ctx *sk) {
    char blk[300];
    size_t n, i, lens;
    _g_mrg_pushing = 1;
    while (_g_mrg_queued < SSL_MERGE_TOTAL) {
        for (n = 0; n < SSL_MERGE_BATCH && _g_mrg_queued < SSL_MERGE_TOTAL; n++) {
            lens = _mrg_chunk_len(_g_mrg_nchunk++);
            if (lens > SSL_MERGE_TOTAL - _g_mrg_queued) {
                lens = SSL_MERGE_TOTAL - _g_mrg_queued;
            }
            for (i = 0; i < lens; i++) {
                blk[i] = _mrg_byte(_g_mrg_queued + i);
            }
            _g_mrg_queued += lens;
            (void)ev_send(ev, sk, blk, lens, 1);
        }
        if (_g_mrg_sent != _g_mrg_queued) {
            break;
        }
    }
    _g_mrg_pushing = 0;
}
// 握手一完成就把发送缓冲调小(合并写才会频繁写不动)，然后开始成批入队；
// 这里能当场 ev_send 也顺带钉住"先清握手位再回调"
static int32_t _mrg_on_exchanged(ev_ctx *ev, sock_ctx *sk, int32_t client, ud_cxt *ud, void *ssl) {
    int32_t sndbuf = 4096;
    (void)client;
    (void)ud;
    (void)ssl;
    (void)setsockopt(sk->fd, SOL_SOCKET, SO_SNDBUF, (char *)&sndbuf, (socklen_t)sizeof(sndbuf));
    _mrg_push(ev, sk);
    return ERR_OK;
}
// 队列发空了就补下一批
static void _mrg_on_sent(ev_ctx *ev, sock_ctx *sk, int32_t client, size_t size, ud_cxt *ud) {
    (void)client;
    (void)ud;
    _g_mrg_sent += size;
    if (0 == _g_mrg_pushing
        && _g_mrg_sent == _g_mrg_queued) {
        _mrg_push(ev, sk);
    }
}
static void _mrg_on_recv(ev_ctx *ev, sock_ctx *sk, int32_t client, buffer_ctx *buf, size_t size, ud_cxt *ud) {
    (void)ev; (void)sk; (void)client; (void)size; (void)ud;
    buffer_drain(buf, buffer_size(buf));
}
// SSL 合并写撞 WANT_WRITE 后的重试：服务端在事件线程里成批连发 1~300 字节的小块、发送缓冲调小；
// 客户端接收缓冲也调小、握手后先停一会儿再读，逼合并写写不动、等写事件来了按同一前缀重试。
// 客户端逐字节校验内容与总量
static void test_ev_ssl_merge_send(CuTest *tc) {
    ev_ctx ev;
    cbs_ctx cbs;
    netaddr_ctx addr;
    evssl_ctx *sc = NULL, *cc = NULL;
    SSL *cli = NULL;
    SOCKET fd = INVALID_SOCK;
    char buf[8192];
    uint64_t id, t0;
    size_t got = 0, n, i, bad = SIZE_MAX;
    int32_t rtn, shake = ERR_FAILED, rcvbuf = 16 * 1024;
    rtn = _ssl_ctxs(&sc, &cc);
    if (0 == rtn) {
        PRINT("skip test_ev_ssl_merge_send, run bin/keys/create.sh first.");
        return;
    }
    CuAssertIntEquals(tc, 1, rtn);
    ZERO(&cbs, sizeof(cbs));
    cbs.exch_cb = _mrg_on_exchanged;
    cbs.s_cb = _mrg_on_sent;
    cbs.r_cb = _mrg_on_recv;
    _g_mrg_nchunk = 0;
    _g_mrg_queued = 0;
    _g_mrg_sent = 0;
    _g_mrg_pushing = 0;
    ev_init(&ev, 1, NULL);
    rtn = ev_listen(&ev, sc, "127.0.0.1", SSL_MERGE_PORT, &cbs, NULL, NULL, 0, &id);
    MSLEEP(50);// listen 落地是异步的
    if (ERR_OK == rtn
        && ERR_OK == netaddr_set(&addr, "127.0.0.1", SSL_MERGE_PORT)) {
        fd = sock_create_cloexec(netaddr_family(&addr), SOCK_STREAM, 0, 0);
    }
    if (INVALID_SOCK != fd) {
        // 连接前设才管得到通告窗口
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (char *)&rcvbuf, (socklen_t)sizeof(rcvbuf));
        if (0 == connect(fd, netaddr_addr(&addr), netaddr_size(&addr))
            && ERR_OK == sock_nonblock(fd)) {
            cli = evssl_setfd(cc, fd);
        }
    }
    for (i = 0; NULL != cli && i < 2000 && ERR_OK != shake; i++) {
        shake = evssl_tryconn(cli);
        if (ERR_FAILED == shake) {
            break;
        }
        if (ERR_OK != shake) {
            MSLEEP(1);
        }
    }
    if (ERR_OK == shake) {
        MSLEEP(50);// 先不读，让服务端把两端缓冲塞满
        t0 = nowms();
        while (got < SSL_MERGE_TOTAL && SIZE_MAX == bad && nowms() - t0 < 10000) {
            if (ERR_OK != evssl_read(cli, buf, sizeof(buf), &n)) {
                break;
            }
            if (0 == n) {
                MSLEEP(1);
                continue;
            }
            for (i = 0; i < n && SIZE_MAX == bad; i++) {
                if (buf[i] != _mrg_byte(got + i)) {
                    bad = got + i;
                }
            }
            got += n;
        }
    }
    // 先收拾再断言(同 _ssl_pair)
    FREE_SSL(cli);
    CLOSE_SOCK(fd);
    ev_free(&ev);
    evssl_free(sc);
    evssl_free(cc);
    CuAssertIntEquals(tc, ERR_OK, shake);
    CuAssertTrue(tc, SIZE_MAX == bad);
    CuAssertTrue(tc, SSL_MERGE_TOTAL == got);
}
#endif
void test_event(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_evpub_close_flush);
    SUITE_ADD_TEST(suite, test_evpub_read_fin);
    SUITE_ADD_TEST(suite, test_evpub_close_type);
    SUITE_ADD_TEST(suite, test_ev_accept_opts);
    SUITE_ADD_TEST(suite, test_ev_udp_close_in_recv);
    SUITE_ADD_TEST(suite, test_ev_udp_reuseaddr);
    SUITE_ADD_TEST(suite, test_ev_close_in_recv_deferred);
    SUITE_ADD_TEST(suite, test_ev_close_in_acp_conn_deferred);
#if defined(EV_EPOLL)
    SUITE_ADD_TEST(suite, test_ev_close_in_conn_fail);
    SUITE_ADD_TEST(suite, test_ev_conn_fail_epoll_del);
    SUITE_ADD_TEST(suite, test_ev_conn_fail_send);
#endif
    SUITE_ADD_TEST(suite, test_ev_defer_exec_run);
    SUITE_ADD_TEST(suite, test_ev_defer_exec_free);
    SUITE_ADD_TEST(suite, test_ev_defer_exec_pass);
    SUITE_ADD_TEST(suite, test_ev_launch_index);
    SUITE_ADD_TEST(suite, test_ev_listen_thrs);
    SUITE_ADD_TEST(suite, test_ev_connect_inloop_pool);
    SUITE_ADD_TEST(suite, test_ev_send_multi_order);
    SUITE_ADD_TEST(suite, test_ev_send_multi_inloop);
    SUITE_ADD_TEST(suite, test_ev_send_multi_free);
    SUITE_ADD_TEST(suite, test_ev_send_multi_wide);
    SUITE_ADD_TEST(suite, test_ev_close_pending_add);
#ifndef EV_IOCP
    SUITE_ADD_TEST(suite, test_ev_unlisten_pending_lsn);
#endif
    SUITE_ADD_TEST(suite, test_ev_udp_close_udfree);
    SUITE_ADD_TEST(suite, test_ev_ud_pktype_udfree);
#ifndef EV_IOCP
    SUITE_ADD_TEST(suite, test_ev_udp_close_in_tick);
#endif
    SUITE_ADD_TEST(suite, test_task_ungrab_on_net);
#if 0 != MAX_SENDQ_CNT
    SUITE_ADD_TEST(suite, test_ev_sendq_overflow);
#endif
#if 0 != CLOSE_LINGER_MS
    SUITE_ADD_TEST(suite, test_ev_linger_late_data);
    SUITE_ADD_TEST(suite, test_ev_linger_timeout);
    SUITE_ADD_TEST(suite, test_ev_linger_close_in_sent);
    SUITE_ADD_TEST(suite, test_ev_linger_bytes);
    SUITE_ADD_TEST(suite, test_ev_linger_free);
    SUITE_ADD_TEST(suite, test_ev_linger_close_busy);
    SUITE_ADD_TEST(suite, test_ev_close_in_recv_peer_fin);
    SUITE_ADD_TEST(suite, test_evpub_linger_want);
#endif
#if WITH_SSL
    SUITE_ADD_TEST(suite, test_evssl_read_close_notify);
    SUITE_ADD_TEST(suite, test_ssl_write_wants_read);
    SUITE_ADD_TEST(suite, test_ev_ssl_merge_send);
#endif
}
