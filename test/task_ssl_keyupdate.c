#include "task_ssl_keyupdate.h"
#include "task_pub.h"

#if WITH_SSL

typedef struct ssl_keyupdate_args {
    uint16_t port;
    void *evssl;
    int32_t *ok;
}ssl_keyupdate_args;

// A 推 8MB;B 每收满 KU_STEP 排一次 KeyUpdate 并回 1 字节 ack,共 ACK_TOTAL 次。上界论证见 .h
#define BLK_BYTES   (1024 * 1024)
#define BLK_CNT     8
#define TOTAL_BYTES ((size_t)BLK_BYTES * BLK_CNT)
#define KU_STEP     (256 * 1024)
#define ACK_TOTAL   (TOTAL_BYTES / KU_STEP)
#define DEADLINE_MS 15000
#define POLL_MS     100

static atomic_t g_recv;   // B 累计收到
static atomic_t g_ack;    // A 累计收到的 ack 字节数
static atomic_t g_bad;    // 收到的内容不对的次数(B 那边该全是 'K'，A 那边该全是 'k')
static atomic_t g_closed; // 任一侧 close 回调次数,非 0 即失败
static atomic_t g_ku;     // ev_keyupdate 的投递次数：只是投递，排没排上不回传
static atomic_t g_ku_seen;// A 真读到的 KeyUpdate 条数

// 只数 A(客户端)读到的 KeyUpdate：到了 A 才说明 B 那边排上了、也真上了线路
static void _msg_cb(int write_p, int version, int content_type, const void *buf, size_t len, SSL *ssl, void *arg) {
    (void)version; (void)arg;
    if (0 == write_p
        && SSL3_RT_HANDSHAKE == content_type
        && len > 0
        && SSL3_MT_KEY_UPDATE == ((const unsigned char *)buf)[0]
        && !SSL_is_server(ssl)) {
        ATOMIC_ADD(&g_ku_seen, 1);
    }
}

// 往连接上压 n 块。copy=0 转移所有权,由事件层释放
static void _push(task_ctx *task, sock_ctx *sk, int32_t n) {
    char *data;
    int32_t i;
    for (i = 0; i < n; i++) {
        MALLOC(data, BLK_BYTES);
        memset(data, 'K', BLK_BYTES);
        ev_send(&task->loader->netev, sk, data, BLK_BYTES, 0);
    }
}
static void _net_recv(task_ctx *task, sock_ctx *sk, subtype_t pktype, uint8_t client,
                      uint8_t slice, void *data, size_t size) {
    (void)pktype; (void)slice;
    const char *p = data;
    char expect = client ? 'k' : 'K';
    for (size_t i = 0; i < size; i++) {
        if (expect != p[i]) {
            ATOMIC_ADD(&g_bad, 1);
            break;
        }
    }
    if (client) {
        ATOMIC_ADD(&g_ack, (atomic_t)size);// B 回的 ack
        return;
    }
    atomic_t total = ATOMIC_ADD(&g_recv, (atomic_t)size) + (atomic_t)size;
    atomic_t want = (atomic_t)(total / KU_STEP);
    char *ack;
    while (ATOMIC_GET(&g_ku) < want) {
        ATOMIC_ADD(&g_ku, 1);
        ev_keyupdate(&task->loader->netev, sk, SSL_KEY_UPDATE_REQUESTED);
        // 排程不等于发出,得自己发点数据把它带上线路(契约见 event.h 的 ev_keyupdate)
        MALLOC(ack, 1);
        *ack = 'k';
        ev_send(&task->loader->netev, sk, ack, 1, 0);
    }
}
static void _net_close(task_ctx *task, sock_ctx *sk, subtype_t pktype, uint8_t client, int32_t erro, uint64_t sess) {
    (void)task; (void)sk; (void)pktype; (void)client; (void)erro; (void)sess;
    ATOMIC_ADD(&g_closed, 1);
}
static void _startup_run(task_ctx *task) {
    ssl_keyupdate_args *arg = (ssl_keyupdate_args *)coro_get_arg(task);
    ATOMIC_SET(&g_recv, 0);
    ATOMIC_SET(&g_ack, 0);
    ATOMIC_SET(&g_bad, 0);
    ATOMIC_SET(&g_closed, 0);
    ATOMIC_SET(&g_ku, 0);
    ATOMIC_SET(&g_ku_seen, 0);
    // 两端的 SSL 都从这个上下文建，必须赶在建连之前挂上
    SSL_CTX_set_msg_callback(evssl_sslctx(arg->evssl), _msg_cb);
    task_recved(task, _net_recv);
    task_closed(task, _net_close);
    uint64_t lsnid;
    if (ERR_OK != task_listen(task, PACK_NONE, arg->evssl, "127.0.0.1", arg->port, &lsnid, 0)) {
        LOG_ERROR("ssl_keyupdate: task_listen %u failed.", arg->port);
        return;
    }
    sock_ctx sk;
    if (ERR_OK != coro_connect(task, PACK_NONE, arg->evssl, "127.0.0.1", arg->port,
                               NETEV_AUTHSSL, NULL, &sk)) {
        LOG_ERROR("ssl_keyupdate: coro_connect failed.");
        return;
    }
    // 分两批,中间让一会:第二批入队时 B 多半已经排过 KeyUpdate，KeyUpdate 落在流量中途、之后还有新数据进队。
    // KEYUPDATE_READ 本用例够不到(理由见 .h)，别当它的覆盖
    _push(task, &sk, BLK_CNT / 2);
    coro_sleep(task, 200);
    _push(task, &sk, BLK_CNT - BLK_CNT / 2);
    int32_t waited = 0;
    atomic_t cur;
    while (waited < DEADLINE_MS) {
        if (task_isclosing(task)) {
            return;
        }
        coro_sleep(task, POLL_MS);
        waited += POLL_MS;
        cur = ATOMIC_GET(&g_recv);
        if (ATOMIC_GET(&g_closed) > 0) {
            LOG_ERROR("ssl_keyupdate: closed (got %lld/%zu, %lld KeyUpdates), relay broken.",
                      (long long)cur, TOTAL_BYTES, (long long)ATOMIC_GET(&g_ku));
            return;
        }
        if ((size_t)cur >= TOTAL_BYTES
            && (size_t)ATOMIC_GET(&g_ack) >= ACK_TOTAL) {
            // 投递次数说明不了排没排上：A 一次 KeyUpdate 都没读到就只是普通大流量，内容坏了更不算通过
            if (0 == ATOMIC_GET(&g_ku_seen)
                || 0 != ATOMIC_GET(&g_bad)) {
                LOG_ERROR("ssl_keyupdate: received all, but peer saw %lld KeyUpdates (%lld posted), %lld corrupted.",
                          (long long)ATOMIC_GET(&g_ku_seen), (long long)ATOMIC_GET(&g_ku),
                          (long long)ATOMIC_GET(&g_bad));
                ev_close(&task->loader->netev, &sk);
                return;
            }
            ATOMIC_SET((atomic_t *)arg->ok, 1);
            LOG_INFO("ssl_keyupdate: received all %zu bytes and %zu acks, peer saw %lld KeyUpdates.",
                     TOTAL_BYTES, ACK_TOTAL, (long long)ATOMIC_GET(&g_ku_seen));
            ev_close(&task->loader->netev, &sk);
            return;
        }
    }
    LOG_ERROR("ssl_keyupdate: incomplete after %ds (%lld/%zu, acks %lld/%zu, %lld KeyUpdates seen), relay stalled.",
              DEADLINE_MS / 1000, (long long)ATOMIC_GET(&g_recv), TOTAL_BYTES,
              (long long)ATOMIC_GET(&g_ack), ACK_TOTAL, (long long)ATOMIC_GET(&g_ku_seen));
    ev_close(&task->loader->netev, &sk);
}
// 失败出口直接 return、槽停在初值：跑完没置 1 的记成已失败，汇总不必等满超时
static void _startup(task_ctx *task) {
    _startup_run(task);
    test_fail_unfinished(task, ((ssl_keyupdate_args *)coro_get_arg(task))->ok);
}
void task_ssl_keyupdate_start(loader_ctx *loader, const char *name, uint16_t port,
    void *evssl, int32_t *ok) {
    if (NULL == ok
        || 0 == port
        || NULL == evssl) {
        return;
    }
    ssl_keyupdate_args *arg;
    CALLOC(arg, 1, sizeof(ssl_keyupdate_args));
    arg->port = port;
    arg->evssl = evssl;
    arg->ok = ok;
    coro_task_register(loader, name, 0, _startup, NULL, _free, arg);
}
#else
// 用例整体依赖 TLS1.3 KeyUpdate，没有 SSL 就没有可测的东西
void task_ssl_keyupdate_start(loader_ctx *loader, const char *name, uint16_t port,
    void *evssl, int32_t *ok) {
    (void)loader;
    (void)name;
    (void)port;
    (void)evssl;
    (void)ok;
}
#endif
