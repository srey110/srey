#include "task_ws_server.h"

static uint16_t _port = 0;
static int32_t _prt = 0;

// 收到 WebSocket 帧：
//   分片帧 - 收齐完整消息（PROT_SLICE_END）后回复三帧分片消息（text_fin0 + continua_fin0 + continua_fin1）
//   非分片帧 - 回显 text/binary，ping 回 pong，close 关闭连接
static void _net_recv(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client, uint8_t slice, void *data, size_t size) {
    (void)pktype;
    (void)client;
    (void)size;
    struct websock_pack_ctx *pack = (struct websock_pack_ctx *)data;
    int32_t prot;
    size_t dlens;
    size_t fsize;
    char *wdata;
    void *frame;
    // 分片帧优先：仅在完整消息到达（PROT_SLICE_END）时才回复三帧分片消息
    if (0 != slice) {
        if (PROT_SLICE_END == slice) {
            frame = websock_pack_text(0, 0, "a", 1, &fsize);
            ev_send(&task->loader->netev, fd, skid, frame, fsize, 0);
            frame = websock_pack_continua(0, 0, "b", 1, &fsize);
            ev_send(&task->loader->netev, fd, skid, frame, fsize, 0);
            frame = websock_pack_continua(0, 1, "c", 1, &fsize);
            ev_send(&task->loader->netev, fd, skid, frame, fsize, 0);
        }
        return;
    }
    prot = websock_prot(pack);
    if (WS_TEXT == prot || WS_BINARY == prot) {
        wdata = websock_data(pack, &dlens);
        if (WS_TEXT == prot) {
            frame = websock_pack_text(0, 1, wdata, dlens, &fsize);
        } else {
            frame = websock_pack_binary(0, 1, wdata, dlens, &fsize);
        }
        ev_send(&task->loader->netev, fd, skid, frame, fsize, 0);
    } else if (WS_PING == prot) {
        frame = websock_pack_pong(0, &fsize);
        ev_send(&task->loader->netev, fd, skid, frame, fsize, 0);
    } else if (WS_CLOSE == prot) {
        ev_close(&task->loader->netev, fd, skid, 1);
    }
}
static void _startup(task_ctx *task) {
    task_recved(task, _net_recv);
    uint64_t id;
    if (ERR_OK != task_listen(task, PACK_WEBSOCK, NULL, "0.0.0.0", _port, &id, 0)) {
        LOG_WARN("task_listen %d error.", _port);
    }
}
void task_ws_server_start(loader_ctx *loader, const char *name, uint16_t port, int32_t pt) {
    _port = port;
    _prt = pt;
    task_ctx *task = task_new(loader, name, 0, NULL, NULL, NULL);
    if (ERR_OK != task_register(task, _startup, NULL)) {
        task_free(task);
    }
}
