#include "task_close_flush.h"

typedef struct close_flush_args {
    uint16_t port;
    int32_t *ok;
}close_flush_args;

// 小包取 4KB:远小于任何平台的默认发送缓冲,ev_send 那步就全部写进内核
// 大包取 4MB:必然超出发送缓冲,用来验证"丢尾巴但照样关得掉"
#define SMALL_BYTES (4 * 1024)
#define BIG_BYTES   (4 * 1024 * 1024)
#define ROUNDS      4

static atomic_t g_recv_bytes;// server 端累计收到字节(整 task 内共享)
static atomic_t g_close_cnt;// server 端 _net_close 触发次数

static void _net_recv(task_ctx *task, sk_id *sk, subtype_t pktype, uint8_t client,
                       uint8_t slice, void *data, size_t size) {
    (void)task; (void)sk; (void)pktype; (void)slice; (void)data;
    // 仅累计 accept 进来的连接(client=0);client=1 是 coro_connect 出去的 client 端
    if (client) {
        return;
    }
    ATOMIC_ADD(&g_recv_bytes, (atomic_t)size);
}
static void _net_close(task_ctx *task, sk_id *sk, subtype_t pktype, uint8_t client) {
    (void)task; (void)sk; (void)pktype;
    if (client) {
        return;
    }
    ATOMIC_ADD(&g_close_cnt, 1);
}
static void _startup(task_ctx *task) {
    close_flush_args *arg = (close_flush_args *)coro_get_arg(task);
    task_recved(task, _net_recv);
    task_closed(task, _net_close);
    uint64_t lsnid;
    if (ERR_OK != task_listen(task, PACK_NONE, NULL, "127.0.0.1", arg->port, &lsnid, 0)) {
        LOG_ERROR("close_flush: task_listen %u failed.", arg->port);
        return;
    }
    ATOMIC_SET(&g_recv_bytes, 0);
    ATOMIC_SET(&g_close_cnt, 0);

    int32_t i;
    SOCKET fd;
    uint64_t skid;
    char *data;
    for (i = 0; i < ROUNDS; i++) {
        if (task_isclosing(task)) {
            return;
        }
        if (ERR_OK != coro_connect(task, PACK_NONE, NULL, "127.0.0.1", arg->port, 0, NULL, &fd, &skid)) {
            LOG_ERROR("close_flush iter %d: coro_connect failed.", i);
            return;
        }
        // 小包 + 立即 close:ev_send 当场写进内核,server 端应收全
        MALLOC(data, SMALL_BYTES);
        memset(data, 'X', SMALL_BYTES);
        ev_send(&task->loader->netev, fd, skid, data, SMALL_BYTES, 0);
        ev_close(&task->loader->netev, fd, skid);
    }

    // 等所有 server 端 close 回调触发(意味着对端 EOF, 即所有数据已收完并关闭)
    int32_t wait_ms = 0;
    while (ATOMIC_GET(&g_close_cnt) < ROUNDS && wait_ms < 10000) {
        if (task_isclosing(task)) {
            return;
        }
        coro_sleep(task, 50);
        wait_ms += 50;
    }
    int32_t expect = ROUNDS * SMALL_BYTES;
    int32_t received = ATOMIC_GET(&g_recv_bytes);
    int32_t closed = ATOMIC_GET(&g_close_cnt);
    if (received != expect || closed != ROUNDS) {
        LOG_ERROR("close_flush small: expect %d bytes / %d closes, got %d bytes / %d closes.",
                  expect, ROUNDS, received, closed);
        return;
    }
    // 第二段:大包必然剩一截在 buf_s 里被丢掉,但连接一定关得掉——不断言送达量,只断言 close_cb
    if (ERR_OK != coro_connect(task, PACK_NONE, NULL, "127.0.0.1", arg->port, 0, NULL, &fd, &skid)) {
        LOG_ERROR("close_flush big: coro_connect failed.");
        return;
    }
    MALLOC(data, BIG_BYTES);
    memset(data, 'Z', BIG_BYTES);
    ev_send(&task->loader->netev, fd, skid, data, BIG_BYTES, 0);
    ev_close(&task->loader->netev, fd, skid);
    wait_ms = 0;
    while (ATOMIC_GET(&g_close_cnt) < ROUNDS + 1 && wait_ms < 10000) {
        if (task_isclosing(task)) {
            return;
        }
        coro_sleep(task, 50);
        wait_ms += 50;
    }
    if (ATOMIC_GET(&g_close_cnt) != ROUNDS + 1) {
        LOG_ERROR("close_flush big: close_cb not fired, %d closes.", (int32_t)ATOMIC_GET(&g_close_cnt));
        return;
    }
    *(arg->ok) = 1;
    LOG_INFO("close_flush tested (%d x %dKB intact, %dMB truncated but closed).",
             ROUNDS, SMALL_BYTES / 1024, BIG_BYTES / 1024 / 1024);
}
void task_close_flush_start(loader_ctx *loader, const char *name, uint16_t port, int32_t *ok) {
    if (NULL == ok || 0 == port) {
        return;
    }
    close_flush_args *arg;
    CALLOC(arg, 1, sizeof(close_flush_args));
    arg->port = port;
    arg->ok = ok;
    coro_task_register(loader, name, 0, _startup, NULL, _free, arg);
}
