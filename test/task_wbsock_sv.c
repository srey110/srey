#include "task_wbsock_sv.h"

static int32_t _prt = 1;

static void _net_recv(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client, uint8_t slice, void *data, size_t size) {
    struct websock_pack_ctx *pack = data;
    int32_t prot = websock_prot(pack);
    void *rpack;
    size_t rlens;
    switch (prot) {
    case WS_PING:
        rpack = websock_pack_pong(client, &rlens);
        ev_send(&task->loader->netev, fd, skid, rpack, rlens, 0);
        break;
    case WS_CLOSE:
        rpack = websock_pack_close(client, &rlens);
        ev_send(&task->loader->netev, fd, skid, rpack, rlens, 0);
        break;
    case WS_TEXT: {
        size_t lens;
        void *msg = websock_data(pack, &lens);
        rpack = websock_pack_text(client, websock_fin(pack), msg, lens, &rlens);
        ev_send(&task->loader->netev, fd, skid, rpack, rlens, 0);
        break;
    }
    case WS_BINARY: {
        size_t lens;
        void *msg = websock_data(pack, &lens);
        rpack = websock_pack_binary(client, websock_fin(pack), msg, lens, &rlens);
        ev_send(&task->loader->netev, fd, skid, rpack, rlens, 0);
        break;
    }
    case WS_CONTINUE: {
        size_t lens;
        void *msg = websock_data(pack, &lens);
        rpack = websock_pack_continua(client, websock_fin(pack), msg, lens, &rlens);
        ev_send(&task->loader->netev, fd, skid, rpack, rlens, 0);
        break;
    }
    default:
        break;
    }
}
static void _on_handshaked(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client, int32_t erro, void *data, size_t lens) {
    if (NULL != data) {
        char secprot[32];
        ZERO(secprot, sizeof(secprot));
        memcpy(secprot, data, lens);
        //LOG_INFO("Sec-WebSocket-Protocol:%s", secprot);
    }
}
static void _startup(task_ctx *task) {
    on_handshaked(task, _on_handshaked);
    on_recved(task, _net_recv);
    uint64_t id;
    task_listen(task, PACK_WEBSOCK, NULL, "0.0.0.0", 15004, &id, 0);
}
void task_wbsock_sv_start(loader_ctx *loader, name_t name, int32_t pt) {
    _prt = pt;
    task_ctx *task = task_new(loader, name, NULL, NULL, NULL);
    task_register(task, _startup, NULL);
}
