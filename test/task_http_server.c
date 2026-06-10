#include "task_http_server.h"

static uint16_t _port = 0;
static int32_t _prt = 0;

// 收到数据包：
//   分片（chunked）请求 - 收齐完整请求（PROT_SLICE_END）后回复三帧 chunked 响应
//   普通请求 - POST 回显请求体，其他返回 "ok"
static void _net_recv(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client, uint8_t slice, void *data, size_t size) {
    (void)pktype;
    (void)client;
    (void)size;
    struct http_pack_ctx *req;
    binary_ctx bwriter;
    size_t dlens;
    void *body;
    // 分片（chunked）请求优先：仅在完整请求到达（PROT_SLICE_END）时才回复
    if (0 != slice) {
        if (PROT_SLICE_END == slice) {
            binary_init(&bwriter, NULL, 0, 0);
            http_pack_resp(&bwriter, 200);
            http_pack_chunked(&bwriter, "a", 1);
            ev_send(&task->loader->netev, fd, skid, bwriter.data, bwriter.offset, 1);
            binary_offset(&bwriter, 0);
            http_pack_chunked(&bwriter, "b", 1);
            ev_send(&task->loader->netev, fd, skid, bwriter.data, bwriter.offset, 1);
            binary_offset(&bwriter, 0);
            http_pack_chunked(&bwriter, NULL, 0);
            ev_send(&task->loader->netev, fd, skid, bwriter.data, bwriter.offset, 0);
        }
        return;
    }
    req = (struct http_pack_ctx *)data;
    binary_init(&bwriter, NULL, 0, 0);
    http_pack_resp(&bwriter, 200);
    dlens = 0;
    body = http_data(req, &dlens);
    if (NULL != body && 0 != dlens) {
        http_pack_content(&bwriter, body, dlens);
    } else {
        http_pack_content(&bwriter, "ok", 2);
    }
    ev_send(&task->loader->netev, fd, skid, bwriter.data, bwriter.offset, 0);
}
static void _startup(task_ctx *task) {
    task_recved(task, _net_recv);
    uint64_t id;
    if (ERR_OK != task_listen(task, PACK_HTTP, NULL, "0.0.0.0", _port, &id, 0)) {
        LOG_WARN("task_listen %d error.", _port);
    }
}
void task_http_server_start(loader_ctx *loader, const char *name, uint16_t port, int32_t pt) {
    _port = port;
    _prt = pt;
    task_ctx *task = task_new(loader, name, 0, NULL, NULL, NULL);
    if (ERR_OK != task_register(task, _startup, NULL)) {
        task_free(task);
    }
}
