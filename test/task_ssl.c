#include "task_ssl.h"

#if WITH_SSL

static int32_t _prt = 0;
static void _net_accept(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype) {
    if (_prt) {
        LOG_INFO("accept socket %d.", (uint32_t)fd);
    }
}
static void _net_recv(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype,
    uint64_t sess, uint8_t client, uint8_t slice, void *data, size_t size) {
    if (randrange(0, 100) <= 1) {
        ev_close(&task->scheduler->netev, fd, skid);
        return;
    }
    size_t lens;
    char *sbuf = simple_data(data, &lens);
    void *outbuf = simple_pack(sbuf, lens, &lens);
    ev_send(&task->scheduler->netev, fd, skid, outbuf, lens, 0);
}
static void _net_send(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype,
    uint64_t sess, uint8_t client, size_t size) {
    if (_prt) {
        LOG_INFO("socket %d sended %d byte.", (uint32_t)fd, (uint32_t)size);
    }
}
static void _net_close(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype,
    uint64_t sess) {
    if (_prt) {
        LOG_INFO("socket %d closed", (uint32_t)fd);
    }
}
void task_ssl_start(scheduler_ctx *scheduler, name_t name, evssl_ctx *ssl, int32_t pt) {
    _prt = pt;
    task_ctx *task = task_new(name, NULL, NULL, NULL);
    task_register(scheduler, task, NULL, NULL);
    register_net_accpet(task, _net_accept);
    register_net_recv(task, _net_recv);
    register_net_send(task, _net_send);
    register_net_close(task, _net_close);
    uint64_t id;
    trigger_listen(task, PACK_SIMPLE, ssl, "0.0.0.0", 15001, &id, APPEND_ACCEPT | APPEND_SEND | APPEND_CLOSE);
}

#endif