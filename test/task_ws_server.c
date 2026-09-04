#include "task_ws_server.h"

static uint16_t _port = 0;

// 收到 WebSocket 帧：
//   分片帧 - 收齐完整消息（PROT_SLICE_END）后回复三帧分片消息（text_fin0 + continua_fin0 + continua_fin1）
//   MQTT 子协议帧 - 收到 CONNECT 回 CONNACK
//   非分片帧 - 回显 text/binary，ping 回 pong，close 关闭连接
static void _net_recv(task_ctx *task, sk_id *sk, subtype_t pktype, uint8_t client, uint8_t slice, void *data, size_t size) {
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
            ev_send(&task->loader->netev, sk->fd, sk->skid, frame, fsize, 0);
            frame = websock_pack_continua(0, 0, "b", 1, &fsize);
            ev_send(&task->loader->netev, sk->fd, sk->skid, frame, fsize, 0);
            frame = websock_pack_continua(0, 1, "c", 1, &fsize);
            ev_send(&task->loader->netev, sk->fd, sk->skid, frame, fsize, 0);
        }
        return;
    }
    // MQTT over WS：子协议帧的载荷经 websock_secpack 取 MQTT 包，收到 CONNECT 回 CONNACK。
    // secprot 每帧都带，控制帧与零长帧的 secpack 为 NULL（见 websock.h），按 secpack 分流
    // 才不会把 ping / close 一起吞掉
    mqtt_pack_ctx *mpack = (mqtt_pack_ctx *)websock_secpack(pack);
    if (PACK_MQTT == websock_secprot(pack)
        && NULL != mpack) {
        if (MQTT_CONNECT == mpack->fixhead.prot) {
            size_t alens;
            char *ack = mqtt_pack_connack((mqtt_protversion)mpack->version, 0, 0, NULL, &alens);
            if (NULL != ack) {
                frame = websock_pack_binary(0, 1, ack, alens, &fsize);
                ev_send(&task->loader->netev, sk->fd, sk->skid, frame, fsize, 0);
                FREE(ack);
            }
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
        ev_send(&task->loader->netev, sk->fd, sk->skid, frame, fsize, 0);
    } else if (WS_PING == prot) {
        frame = websock_pack_pong(0, &fsize);
        ev_send(&task->loader->netev, sk->fd, sk->skid, frame, fsize, 0);
    } else if (WS_CLOSE == prot) {
        ev_close(&task->loader->netev, sk->fd, sk->skid);
    }
}
static void _startup(task_ctx *task) {
    task_recved(task, _net_recv);
    uint64_t id;
    if (ERR_OK != task_listen(task, PACK_WEBSOCK, NULL, "0.0.0.0", _port, &id, 0)) {
        LOG_WARN("task_listen %d error.", _port);
    }
}
void task_ws_server_start(loader_ctx *loader, const char *name, uint16_t port) {
    _port = port;
    task_ctx *task = task_new(loader, name, 0, NULL, NULL, NULL);
    if (ERR_OK != task_register(task, _startup, NULL)) {
        task_free(task);
    }
}
