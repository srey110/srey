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

    CLOSE_SOCK(sk[0]);
}
// close_type 三档判定 + FIN 优先
static void test_evpub_close_type(CuTest *tc) {
    int32_t st;

    /* 1) 两个位都没置：本地主动关 */
    st = STATUS_ESTABLISHED;
    CuAssertIntEquals(tc, CLOSE_TYPE_LOCAL, _evpub_close_type(st));

    /* 2) 读到 FIN：有序结束 */
    st = STATUS_ESTABLISHED;
    _evpub_mark_close(&st, 1, NULL);
    CuAssertTrue(tc, BIT_CHECK(st, STATUS_PEER_FIN));
    CuAssertIntEquals(tc, CLOSE_TYPE_ORDERLY, _evpub_close_type(st));

    /* 3) 读写失败：异常中断 */
    st = STATUS_ESTABLISHED;
    _evpub_mark_close(&st, ERR_FAILED, NULL);
    CuAssertTrue(tc, BIT_CHECK(st, STATUS_PEER_ABORT));
    CuAssertIntEquals(tc, CLOSE_TYPE_ABORT, _evpub_close_type(st));

    /* 4) 先 FIN 再叠 ABORT 仍判有序：IOCP 侧收完成与错误处理是两条路径，会叠加 */
    st = STATUS_ESTABLISHED;
    _evpub_mark_close(&st, 1, NULL);
    _evpub_mark_close(&st, ERR_FAILED, NULL);
    CuAssertIntEquals(tc, CLOSE_TYPE_ORDERLY, _evpub_close_type(st));

    /* 5) 不碰其他状态位 */
    CuAssertTrue(tc, BIT_CHECK(st, STATUS_ESTABLISHED));
}
void test_event(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_evpub_close_flush);
    SUITE_ADD_TEST(suite, test_evpub_read_fin);
    SUITE_ADD_TEST(suite, test_evpub_close_type);
}
