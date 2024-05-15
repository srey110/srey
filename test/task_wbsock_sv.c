#include "task_wbsock_sv.h"

static int32_t _prt = 1;

static void _net_recv(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client, uint8_t slice, void *data, size_t size) {
    struct websock_pack_ctx *pack = data;
    int32_t proto = websock_pack_proto(pack);
    switch (proto) {
    case WBSK_PING:
        websock_pong(&task->scheduler->netev, fd, skid, client);
        break;
    case WBSK_CLOSE:
        websock_close(&task->scheduler->netev, fd, skid, client);
        break;
    case WBSK_TEXT: {
        size_t lens;
        void *msg = websock_pack_data(pack, &lens);
        websock_text(&task->scheduler->netev, fd, skid, client, websock_pack_fin(pack), msg, lens);
        break;
    }
    case WBSK_BINARY: {
        size_t lens;
        void *msg = websock_pack_data(pack, &lens);
        websock_binary(&task->scheduler->netev, fd, skid, client, websock_pack_fin(pack), msg, lens);
        break;
    }
    case WBSK_CONTINUE: {
        size_t lens;
        void *msg = websock_pack_data(pack, &lens);
        websock_continuation(&task->scheduler->netev, fd, skid, client, websock_pack_fin(pack), msg, lens);
        break;
    }
    default:
        break;
    }
}
static void _startup(task_ctx *task) {
    on_recved(task, _net_recv);
    uint64_t id;
    trigger_listen(task, PACK_WEBSOCK, NULL, "0.0.0.0", 15004, &id, 0);
}
void task_wbsock_sv_start(scheduler_ctx *scheduler, name_t name, int32_t pt) {
    _prt = pt;
    task_ctx *task = task_new(scheduler, name, NULL, NULL, NULL);
    task_register(task, _startup, NULL);
}
