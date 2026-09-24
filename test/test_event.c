#include "test_event.h"
#include "lib.h"

// 延迟关闭用例：服务端收到第一段就回这条响应并 ev_close
#define LINGER_RESP "HTTP/1.1 411 Length Required\r\nContent-Length: 0\r\n\r\n"
// 各用例各占一个端口：Windows 监听口带 SO_EXCLUSIVEADDRUSE，立即重绑同端口会失败
#define LINGER_PORT 15090
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
          这一档与 (2) 的分野决定 close_type 判 ABNORMAL 还是 ORDERLY——并进 FIN 那支的话，
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
// 建一对握完手的 SSL。返回 1 成功；0 表示证书没生成（用例跳过）；-1 是真失败
static int32_t _ssl_pair(SOCKET sk[2], SSL **cli, SSL **srv, evssl_ctx **sc, evssl_ctx **cc) {
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
    if (ERR_OK != ev_listen(ev, NULL, "127.0.0.1", port, &cbs, NULL, &id)) {
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
void test_event(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_evpub_close_flush);
    SUITE_ADD_TEST(suite, test_evpub_read_fin);
    SUITE_ADD_TEST(suite, test_evpub_close_type);
#if 0 != CLOSE_LINGER_MS
    SUITE_ADD_TEST(suite, test_ev_linger_late_data);
    SUITE_ADD_TEST(suite, test_ev_linger_timeout);
    SUITE_ADD_TEST(suite, test_ev_linger_close_in_sent);
    SUITE_ADD_TEST(suite, test_ev_linger_bytes);
    SUITE_ADD_TEST(suite, test_ev_linger_free);
    SUITE_ADD_TEST(suite, test_ev_linger_close_busy);
    SUITE_ADD_TEST(suite, test_evpub_linger_want);
#endif
#if WITH_SSL
    SUITE_ADD_TEST(suite, test_evssl_read_close_notify);
    SUITE_ADD_TEST(suite, test_ssl_write_wants_read);
#endif
}
