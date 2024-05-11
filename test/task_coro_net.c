#include "task_coro_net.h"

#if WITH_CORO

static int32_t _prt = 1;

static void _test_syn_send(task_ctx *task) {
    uint64_t skid;
    SOCKET fd = coro_connect(task, 1000, PACK_SIMPLE, NULL, "127.0.0.1", 15000, &skid, APPEND_CLOSE);
    if (INVALID_SOCK == fd) {
        LOG_ERROR("%s", "syn_connect error");
        return;
    }
    const char *msg = "this is tcp task_coro_net.";
    size_t size;
    struct simple_pack_ctx *pack = simple_pack((void*)msg, strlen(msg), &size);
    void *data = coro_send(task, 1000, fd, skid, createid(), pack, size, &size, 0);
    if (NULL == data) {
        LOG_ERROR("%s", "syn_send error");
        ev_close(&task->scheduler->netev, fd, skid);
        return;
    }
    data = simple_data(data, &size);
    ASSERTAB(0 == _memicmp(data, msg, strlen(msg)), "syn_send error");
    ev_close(&task->scheduler->netev, fd, skid);
}
static void _test_syn_sendto(task_ctx *task) {
    uint64_t skid;
    SOCKET fd = trigger_udp(task, "0.0.0.0", 0, &skid);
    if (INVALID_SOCK == fd) {
        LOG_ERROR("%s", "trigger_udp error");
        return;
    }
    const char *msg = "this is udp task_coro_net.";
    size_t size;
    void *data = coro_sendto(task, 1000, fd, skid, "127.0.0.1", 15002, (void*)msg, strlen(msg), &size);
    if (NULL == data) {
        LOG_ERROR("%s", "syn_sendto error");
        ev_close(&task->scheduler->netev, fd, skid);
        return;
    }
    ASSERTAB(0 == _memicmp(data, msg, strlen(msg)), "syn_sendto error");
    ev_close(&task->scheduler->netev, fd, skid);
}
static void _net_close(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype,
    uint64_t sess) { }
static void _timeout(task_ctx *task, uint64_t sess) {
    _test_syn_send(task);
    _test_syn_sendto(task);
    trigger_timeout(task, createid(), 3000, _timeout);
}
void task_coro_net_start(scheduler_ctx *scheduler, name_t name, int32_t pt) {
    _prt = pt;
    task_ctx *task = task_new(name, NULL, NULL, NULL);
    register_net_close(task, _net_close);
    task_register(scheduler, task, NULL, NULL);
    trigger_timeout(task, createid(), 1000, _timeout);
}

#endif
