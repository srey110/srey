#include "task_tcp.h"

static int32_t _prt = 0;
static void _net_accept(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype) {
    if (_prt) {
        LOG_INFO("accept socket %d.", (uint32_t)fd);
    }
}
static void _net_connect(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, int32_t erro) {
    if (_prt) {
        LOG_INFO("connect socket %d error %d.", (uint32_t)fd, erro);
    }
}
static void _net_recv(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client, uint8_t slice, void *data, size_t size) {
    if (randrange(0, 100) <= 1) {
        ev_close(&task->scheduler->netev,fd, skid);
        return;
    }
    size_t lens;
    char *sbuf = custz_data(data, &lens);
    void *outbuf = custz_pack(sbuf, lens, &lens);
    ev_send(&task->scheduler->netev, fd, skid, outbuf, lens, 0);
}
static void _net_send(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client, size_t size) {
    if (_prt) {
        LOG_INFO("socket %d sended %d byte.", (uint32_t)fd, (uint32_t)size);
    }
}
static void _net_close(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client) {
    if (_prt) {
        LOG_INFO("socket %d closed", (uint32_t)fd);
    }
}
static void _startup(task_ctx *task) {
    on_accepted(task, _net_accept);
    on_connected(task, _net_connect);
    on_recved(task, _net_recv);
    on_sended(task, _net_send);
    on_closed(task, _net_close);
    uint64_t id;
    trigger_listen(task, PACK_CUSTZ, NULL, "0.0.0.0", 15000, &id, APPEND_ACCEPT | APPEND_SEND | APPEND_CLOSE);
    trigger_connect(task, PACK_CUSTZ, NULL, "127.0.0.1", 15000, NULL, &id, 0, APPEND_SEND | APPEND_CLOSE);
    //trigger_connect(task, PACK_CUSTZ, NULL, "127.0.0.1", 15001, NULL, &id, 0, APPEND_SEND | APPEND_CLOSE);
}
void task_tcp_start(scheduler_ctx *scheduler, name_t name, int32_t pt) {
    _prt = pt;
    task_ctx *task = task_new(scheduler, name, NULL, NULL, NULL);
    task_register(task, _startup, NULL);
}
