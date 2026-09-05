#include "test_event.h"
#include "lib.h"

// 往发送队列塞一条待发数据，data 由 _evpub_off_buf_release / 冲刷成功后释放
static void _push_sendbuf(queue_ctx *bufs, const char *s, size_t lens) {
    off_buf_ctx buf;
    ZERO(&buf, sizeof(buf));
    MALLOC(buf.data, lens);
    memcpy(buf.data, s, lens);
    buf.lens = lens;
    queue_push(bufs, &buf);
}
// 探一次有没有数据可读：证否用，不等待
static int32_t _recv_none(SOCKET fd) {
    char c;
    return (int32_t)recv(fd, &c, 1, 0) <= 0 ? 1 : 0;
}
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
// 关闭前冲刷 _evpub_close_flush_tcp。为什么走单测而不是集成用例（task_close_flush）：
// 队列非空是冲刷干活的前提，而 unix 侧只要 socket 可写，正常写路径就已经把队列抽干了，
// 也就是说集成场景下"队列非空"必然意味着"此刻写不进去"，观察不到冲刷把字节送出去
static void test_evpub_close_flush(CuTest *tc) {
    SOCKET sk[2];
    CuAssertIntEquals(tc, ERR_OK, sock_pair(sk, 1));
    queue_ctx bufs;
    queue_init(&bufs, sizeof(off_buf_ctx), 8);
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
    CuAssertTrue(tc, 0 == queue_size(&bufs));
    CuAssertTrue(tc, 0 == wb);
    ZERO(got, sizeof(got));
    nrecv = _recv_all(sk[1], got, 11);
    CuAssertTrue(tc, 11 == nrecv);
    CuAssertTrue(tc, 0 == memcmp(got, "hello world", 11));

    /* 3) KEYUPDATE_WRITE：挂着一个待重试的 SSL_read，此时不能再调 SSL_write，整队不冲 */
    _push_sendbuf(&bufs, "blocked", 7);
    wb = 7;
    _evpub_close_flush_tcp(sk[0], &bufs, STATUS_ESTABLISHED | STATUS_KEYUPDATE_WRITE, &wb, NULL);
    CuAssertTrue(tc, 1 == queue_size(&bufs));
    CuAssertTrue(tc, 7 == wb);
    CuAssertTrue(tc, 0 != _recv_none(sk[1]));
    _evpub_off_buf_clear(&bufs);

    /* 4) SSLEXCHANGE 不再挡：该态下 ssl 恒为 NULL、队列里全是明文，照常冲出去 */
    _push_sendbuf(&bufs, "plain", 5);
    wb = 5;
    _evpub_close_flush_tcp(sk[0], &bufs, STATUS_ESTABLISHED | STATUS_SSLEXCHANGE, &wb, NULL);
    CuAssertTrue(tc, 0 == queue_size(&bufs));
    CuAssertTrue(tc, 0 == wb);
    ZERO(got, sizeof(got));
    nrecv = _recv_all(sk[1], got, 5);
    CuAssertTrue(tc, 5 == nrecv);
    CuAssertTrue(tc, 0 == memcmp(got, "plain", 5));

    /* 5) AUTHSSL 同样不挡：守卫只剩 KEYUPDATE_WRITE 一位，别再按"SSL 相关状态一律不冲"加回来。
          生产里这个组合不可达——两个平台仅有的两处入队都过 _evpub_sendqu_check_tcp，
          它对 AUTHSSL/SSLEXCHANGE 都拒收，握手期队列必空、上面那道 queue_size 早退就返回了 */
    _push_sendbuf(&bufs, "authssl", 7);
    wb = 7;
    _evpub_close_flush_tcp(sk[0], &bufs, STATUS_ESTABLISHED | STATUS_AUTHSSL, &wb, NULL);
    CuAssertTrue(tc, 0 == queue_size(&bufs));
    CuAssertTrue(tc, 0 == wb);
    ZERO(got, sizeof(got));
    nrecv = _recv_all(sk[1], got, 7);
    CuAssertTrue(tc, 7 == nrecv);
    CuAssertTrue(tc, 0 == memcmp(got, "authssl", 7));

    queue_free(&bufs);
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
          这一档与 (2) 的分野决定 close_type 判 ABNORMAL 还是 ORDERLY——并进 FIN 那支的话，
          连接被重置时 close-delimited body 会被判成完整 */
    CLOSE_SOCK(sk[0]);
    nread = 1;
    CuAssertIntEquals(tc, ERR_FAILED, _evpub_sock_read(sk[0], &iov, 1, NULL, &nread));
}
// close_type 三档判定 + FIN 优先
static void test_evpub_close_type(CuTest *tc) {
    int32_t st;

    /* 1) 两个位都没置：本地主动关 */
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

    /* 4) 先 FIN 再叠 ABORT 仍判有序：IOCP 侧收完成与错误处理是两条路径，会叠加 */
    st = STATUS_ESTABLISHED;
    _evpub_mark_close(&st, 1);
    _evpub_mark_close(&st, ERR_FAILED);
    CuAssertIntEquals(tc, CLOSE_TYPE_ORDERLY, _evpub_close_type(st));

    /* 5) 不碰其他状态位 */
    CuAssertTrue(tc, BIT_CHECK(st, STATUS_ESTABLISHED));
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
    }
    return (ERR_OK == c && ERR_OK == s) ? ERR_OK : ERR_FAILED;
}
// 建一对握完手的 SSL。返回 1 成功；0 表示证书没生成（用例跳过）；-1 是真失败
static int32_t _ssl_pair(SOCKET sk[2], SSL **cli, SSL **srv, evssl_ctx **sc, evssl_ctx **cc) {
    const char *local = procpath();
    char ca[PATH_LENS], crt[PATH_LENS], key[PATH_LENS];
    SNPRINTF(ca, sizeof(ca), "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "ca.crt");
    SNPRINTF(crt, sizeof(crt), "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "server.crt");
    SNPRINTF(key, sizeof(key), "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "server.key");
    *sc = evssl_new(ca, crt, key, SSL_FILETYPE_PEM);
    if (NULL == *sc) {
        return 0;
    }
    *cc = evssl_new(NULL, NULL, NULL, SSL_FILETYPE_PEM);
    if (NULL == *cc) {
        evssl_free(*sc);
        return -1;
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
        FREE_SSL(*cli);
        FREE_SSL(*srv);
        CLOSE_SOCK(sk[0]);
        CLOSE_SOCK(sk[1]);
        evssl_free(*sc);
        evssl_free(*cc);
        return -1;
    }
    return 1;
}
// close_notify 与"连接被截断"必须分得开：前者有序结束（返 1），后者异常中断（返 ERR_FAILED）。
// 判据见 evssl_read 的 <returns>
static void test_evssl_read_close_notify(CuTest *tc) {
    SOCKET sk[2];
    SSL *cli = NULL, *srv = NULL;
    evssl_ctx *sc = NULL, *cc = NULL;
    char buf[64];
    size_t readed;
    int32_t rtn;

    /* 1) 对端 SSL_shutdown 发了 close_notify：读侧必须报 1 */
    rtn = _ssl_pair(sk, &cli, &srv, &sc, &cc);
    if (0 == rtn) {
        PRINT("skip test_evssl_read_close_notify, run bin/keys/create.sh first.");
        return;
    }
    CuAssertIntEquals(tc, 1, rtn);
    evssl_shutdown(srv, sk[1]);
    int32_t i;
    // close_notify 未必立刻可见，未到时读的是 WANT_READ(ERR_OK)，故有界重试（同 test_evpub_read_fin）
    rtn = ERR_OK;
    for (i = 0; i < 200 && ERR_OK == rtn; i++) {
        readed = 1;
        rtn = evssl_read(cli, buf, sizeof(buf), &readed);
        if (ERR_OK == rtn) {
            MSLEEP(1);
        }
    }
    CuAssertIntEquals(tc, 1, rtn);
    CuAssertTrue(tc, 0 == readed);
    FREE_SSL(cli);
    FREE_SSL(srv);
    CLOSE_SOCK(sk[0]);
    CLOSE_SOCK(sk[1]);
    evssl_free(sc);
    evssl_free(cc);

    /* 2) 对端直接关 TCP、不发 close_notify：必须报 ERR_FAILED，不能当成有序结束 */
    CuAssertIntEquals(tc, 1, _ssl_pair(sk, &cli, &srv, &sc, &cc));
    FREE_SSL(srv);
    CLOSE_SOCK(sk[1]);
    rtn = ERR_OK;
    // 同上：EOF 未必立刻可见
    for (i = 0; i < 200 && ERR_OK == rtn; i++) {
        readed = 1;
        rtn = evssl_read(cli, buf, sizeof(buf), &readed);
        if (ERR_OK == rtn) {
            MSLEEP(1);
        }
    }
    CuAssertIntEquals(tc, ERR_FAILED, rtn);
    FREE_SSL(cli);
    CLOSE_SOCK(sk[0]);
    evssl_free(sc);
    evssl_free(cc);
}
#endif
void test_event(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_evpub_close_flush);
    SUITE_ADD_TEST(suite, test_evpub_read_fin);
    SUITE_ADD_TEST(suite, test_evpub_close_type);
#if WITH_SSL
    SUITE_ADD_TEST(suite, test_evssl_read_close_notify);
#endif
}
