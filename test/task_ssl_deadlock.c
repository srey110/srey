#include "task_ssl_deadlock.h"

typedef struct ssl_deadlock_args {
    uint16_t port;
    void *evssl;
    int32_t *ok;
}ssl_deadlock_args;

// 每块 1MB x 8 = 8MB:远超任何平台的发送缓冲,却只占 8 个 buf,离 MAX_SENDQ_CNT 很远
#define BLK_BYTES (1024 * 1024)
#define BLK_CNT   8
#define TOTAL_BYTES ((size_t)BLK_BYTES * BLK_CNT)
// 判"成环且无人收割"要等过看门狗:阈值 + 一轮事件循环 + 余量。
// 绑到宏上,不能写死——SSL_WPEND_MAX_MS 调了这里必须跟着动,否则用例会在看门狗动手前先放弃
#define STALL_MS  (SSL_WPEND_MAX_MS + EVENT_WAIT_TIMEOUT + 5000)
#define POLL_MS   200

static atomic_t g_cli_recv;// 客户端侧累计收到
static atomic_t g_srv_recv;// 服务端侧累计收到
static atomic_t g_closed;  // 任一侧的 close 回调次数
static atomic_t g_srv_sent;// 服务端是否已推过(只推一次)
static atomic_t g_bad_erro;// close_type 不是 ABORT 的次数

// 往一条连接上压 BLK_CNT 块。copy=0 转移所有权,由事件层释放
static void _push(task_ctx *task, sock_ctx *sk) {
    char *data;
    int32_t i;
    for (i = 0; i < BLK_CNT; i++) {
        MALLOC(data, BLK_BYTES);
        memset(data, 'D', BLK_BYTES);
        ev_send(&task->loader->netev, sk, data, BLK_BYTES, 0);
    }
}
// 服务端收到第一批数据时才回推:accept 回调在裸 accept 时就触发,那时 SSL 握手还没完成,
// 在那里 ev_send 会被 _evpub_sendqu_check_tcp 判成握手期发送并断连。收到数据即握手已完成
static void _net_recv(task_ctx *task, sock_ctx *sk, subtype_t pktype, uint8_t client,
                      uint8_t slice, void *data, size_t size) {
    (void)pktype; (void)slice; (void)data;
    if (client) {
        ATOMIC_ADD(&g_cli_recv, (atomic_t)size);
        return;
    }
    ATOMIC_ADD(&g_srv_recv, (atomic_t)size);
    if (0 != ATOMIC_ADD(&g_srv_sent, 1)) {
        return;
    }
    _push(task, sk);
}
static void _net_close(task_ctx *task, sock_ctx *sk, subtype_t pktype, uint8_t client, int32_t erro) {
    (void)task; (void)sk; (void)pktype; (void)client;
    if (CLOSE_TYPE_ABORT != erro) {
        ATOMIC_ADD(&g_bad_erro, 1);
    }
    ATOMIC_ADD(&g_closed, 1);
}
static void _startup(task_ctx *task) {
    ssl_deadlock_args *arg = (ssl_deadlock_args *)coro_get_arg(task);
    ATOMIC_SET(&g_cli_recv, 0);
    ATOMIC_SET(&g_srv_recv, 0);
    ATOMIC_SET(&g_closed, 0);
    ATOMIC_SET(&g_srv_sent, 0);
    ATOMIC_SET(&g_bad_erro, 0);
    task_recved(task, _net_recv);
    task_closed(task, _net_close);
    uint64_t lsnid;
    if (ERR_OK != task_listen(task, PACK_NONE, arg->evssl, "127.0.0.1", arg->port, &lsnid, 0)) {
        LOG_ERROR("ssl_deadlock: task_listen %u failed.", arg->port);
        return;
    }
    sock_ctx sk;
    if (ERR_OK != coro_connect(task, PACK_NONE, arg->evssl, "127.0.0.1", arg->port,
                               NETEV_AUTHSSL, NULL, &sk)) {
        LOG_ERROR("ssl_deadlock: coro_connect failed.");
        return;
    }
    _push(task, &sk);

    atomic_t lastc = -1;
    atomic_t lasts = -1;
    atomic_t curc;
    atomic_t curs;
    int32_t stall = 0;
    int32_t waited = 0;
    while (waited < STALL_MS + 5000) {
        if (task_isclosing(task)) {
            return;
        }
        coro_sleep(task, POLL_MS);
        waited += POLL_MS;
        curc = ATOMIC_GET(&g_cli_recv);
        curs = ATOMIC_GET(&g_srv_recv);
        if ((size_t)curc >= TOTAL_BYTES
            && (size_t)curs >= TOTAL_BYTES) {
            *(arg->ok) = 1;
            LOG_INFO("ssl_deadlock: both sides received all %zu bytes, no cycle.", TOTAL_BYTES);
            return;
        }
        // 两端都不主动读时环必成:谁都在等对方先读,只有看门狗能拆。故收割即预期结局,
        // 判据是 close_type 必须是 ABORT。真正的失败是停住且无人收割
        if (ATOMIC_GET(&g_closed) > 0) {
            if (0 == ATOMIC_GET(&g_bad_erro)) {
                *(arg->ok) = 1;
                LOG_INFO("ssl_deadlock: reaped by wpend watchdog (cli %lld / srv %lld), as expected.",
                         (long long)curc, (long long)curs);
            } else {
                LOG_ERROR("ssl_deadlock: closed but close_type is not ABORT.");
            }
            return;
        }
        if (curc == lastc
            && curs == lasts) {
            stall += POLL_MS;
        } else {
            stall = 0;
            lastc = curc;
            lasts = curs;
        }
        if (stall >= STALL_MS) {
            LOG_ERROR("ssl_deadlock: cycled, no progress for %ds, cli %lld / srv %lld, want %zu each, no close cb.",
                      STALL_MS / 1000, (long long)curc, (long long)curs, TOTAL_BYTES);
            return;
        }
    }
    LOG_ERROR("ssl_deadlock: %ds elapsed, neither complete nor stalled, cli %lld / srv %lld.", (STALL_MS + 5000) / 1000,
              (long long)ATOMIC_GET(&g_cli_recv), (long long)ATOMIC_GET(&g_srv_recv));
}
void task_ssl_deadlock_start(loader_ctx *loader, const char *name, uint16_t port,
    void *evssl, int32_t *ok) {
    if (NULL == ok
        || 0 == port
        || NULL == evssl) {
        return;
    }
    ssl_deadlock_args *arg;
    CALLOC(arg, 1, sizeof(ssl_deadlock_args));
    arg->port = port;
    arg->evssl = evssl;
    arg->ok = ok;
    coro_task_register(loader, name, 0, _startup, NULL, _free, arg);
}
