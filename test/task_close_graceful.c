#include "task_close_graceful.h"

typedef struct close_graceful_args {
    uint16_t port;
    int32_t *ok;
}close_graceful_args;

// 每轮发 256KB,4 轮累计 1MB;PACK_NONE 透传协议下 _net_recv 直接拿原始字节
#define BYTES_PER_ROUND (256 * 1024)
#define ROUNDS          4

static atomic_t g_recv_bytes;   // server 端累计收到字节(整 task 内共享)
static atomic_t g_close_cnt;    // server 端 _net_close 触发次数

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
    close_graceful_args *arg = (close_graceful_args *)coro_get_arg(task);
    task_recved(task, _net_recv);
    task_closed(task, _net_close);
    uint64_t lsnid;
    if (ERR_OK != task_listen(task, PACK_NONE, NULL, "127.0.0.1", arg->port, &lsnid, 0)) {
        LOG_ERROR("close_graceful: task_listen %u failed.", arg->port);
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
            LOG_ERROR("close_graceful iter %d: coro_connect failed.", i);
            return;
        }
        // 业务 ev_send 完一大段数据后立即 ev_close(immed=0);
        // graceful 路径应等 buf_s 全部发完才真正关闭, server 端最终累计应等于发送总量
        MALLOC(data, BYTES_PER_ROUND);
        memset(data, 'X', BYTES_PER_ROUND);
        ev_send(&task->loader->netev, fd, skid, data, BYTES_PER_ROUND, 0);
        ev_close(&task->loader->netev, fd, skid, 0);
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

    int32_t expect = ROUNDS * BYTES_PER_ROUND;
    int32_t received = ATOMIC_GET(&g_recv_bytes);
    int32_t closed = ATOMIC_GET(&g_close_cnt);
    if (received != expect || closed != ROUNDS) {
        LOG_ERROR("close_graceful: expect %d bytes / %d closes, got %d bytes / %d closes.",
                  expect, ROUNDS, received, closed);
        return;
    }
    *(arg->ok) = 1;
    LOG_INFO("close_graceful tested (%d rounds x %dKB = %dKB delivered intact).",
             ROUNDS, BYTES_PER_ROUND / 1024, expect / 1024);
}
void task_close_graceful_start(loader_ctx *loader, const char *name, uint16_t port, int32_t *ok) {
    if (NULL == ok || 0 == port) {
        return;
    }
    close_graceful_args *arg;
    CALLOC(arg, 1, sizeof(close_graceful_args));
    arg->port = port;
    arg->ok = ok;
    coro_task_register(loader, name, 0, _startup, NULL, _free, arg);
}
