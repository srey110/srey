#include "task_sendbuf_warn.h"

typedef struct sendbuf_warn_args {
    uint16_t port;
    int32_t *ok;
}sendbuf_warn_args;

// 每轮 4MB，2 轮共 8MB；单次 ev_send 即让 wb_size 远超 WB_WARN_INIT_SIZE(1MB)，
// 必然触发 LOG_WARN("TCP send buf growing")；之后随 _evpub_sock_send 消费递减至 0
#define BYTES_PER_ROUND (4 * 1024 * 1024)
#define ROUNDS          2

static atomic_t g_recv_bytes;   // server 端累计收到字节
static atomic_t g_close_cnt;    // server 端 close 回调触发次数

static void _net_recv(task_ctx *task, SOCKET fd, uint64_t skid, subtype_t pktype, uint8_t client,
                       uint8_t slice, void *data, size_t size) {
    (void)task; (void)fd; (void)skid; (void)pktype; (void)slice; (void)data;
    if (client) {
        return;
    }
    ATOMIC_ADD(&g_recv_bytes, (atomic_t)size);
}
static void _net_close(task_ctx *task, SOCKET fd, uint64_t skid, subtype_t pktype, uint8_t client) {
    (void)task; (void)fd; (void)skid; (void)pktype;
    if (client) {
        return;
    }
    ATOMIC_ADD(&g_close_cnt, 1);
}
static void _startup(task_ctx *task) {
    sendbuf_warn_args *arg = (sendbuf_warn_args *)coro_get_arg(task);
    task_recved(task, _net_recv);
    task_closed(task, _net_close);
    uint64_t lsnid;
    if (ERR_OK != task_listen(task, PACK_NONE, NULL, "127.0.0.1", arg->port, &lsnid, 0)) {
        LOG_ERROR("sendbuf_warn: task_listen %u failed.", arg->port);
        return;
    }
    ATOMIC_SET(&g_recv_bytes, 0);
    ATOMIC_SET(&g_close_cnt, 0);

    SOCKET fd;
    uint64_t skid;
    if (ERR_OK != coro_connect(task, PACK_NONE, NULL, "127.0.0.1", arg->port, 0, NULL, &fd, &skid)) {
        LOG_ERROR("sendbuf_warn: coro_connect failed.");
        return;
    }

    int32_t i;
    char *data;
    for (i = 0; i < ROUNDS; i++) {
        if (task_isclosing(task)) {
            return;
        }
        MALLOC(data, BYTES_PER_ROUND);
        memset(data, 'Y', BYTES_PER_ROUND);
        // 单次 ev_send 即 _uev_add_write_inloop 把整块入队，wb_size += 4MB 必触告警
        ev_send(&task->loader->netev, fd, skid, data, BYTES_PER_ROUND, 0);
    }
    // graceful 关闭：等 buf_s 全部发完才真正断开，server 端最终累计应等于总发送量
    ev_close(&task->loader->netev, fd, skid, 0);

    int32_t expect = ROUNDS * BYTES_PER_ROUND;
    int32_t wait_ms = 0;
    while (ATOMIC_GET(&g_close_cnt) < 1 && wait_ms < 30000) {
        if (task_isclosing(task)) {
            return;
        }
        coro_sleep(task, 100);
        wait_ms += 100;
    }

    int32_t received = ATOMIC_GET(&g_recv_bytes);
    int32_t closed = ATOMIC_GET(&g_close_cnt);
    if (received != expect || closed != 1) {
        LOG_ERROR("sendbuf_warn: expect %d bytes / 1 close, got %d bytes / %d closes.",
                  expect, received, closed);
        return;
    }
    *(arg->ok) = 1;
    LOG_INFO("sendbuf_warn tested (%d rounds x %d KB = %d KB delivered, wb_size warn triggered).",
             ROUNDS, BYTES_PER_ROUND / 1024, expect / 1024);
}
void task_sendbuf_warn_start(loader_ctx *loader, const char *name, uint16_t port, int32_t *ok) {
    if (NULL == ok || 0 == port) {
        return;
    }
    sendbuf_warn_args *arg;
    CALLOC(arg, 1, sizeof(sendbuf_warn_args));
    arg->port = port;
    arg->ok = ok;
    coro_task_register(loader, name, 0, _startup, NULL, _free, arg);
}
