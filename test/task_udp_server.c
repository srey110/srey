#include "task_udp_server.h"

static int32_t _prt = 0;
static uint16_t _port = 0;

// 收到 UDP 数据报后原样回发给发送方
static void _net_recvfrom(task_ctx *task, SOCKET fd, uint64_t skid,
    char ip[IP_LENS], uint16_t port, void *data, size_t size) {
    ev_sendto(&task->loader->netev, fd, skid, ip, port, data, size, 1);
}
static void _startup(task_ctx *task) {
    task_recvedfrom(task, _net_recvfrom);
    SOCKET fd;
    uint64_t id;
    if (ERR_OK != task_udp(task, "0.0.0.0", _port, &fd, &id)) {
        LOG_WARN("start udp server error.");
    }
}
void task_udp_server_start(loader_ctx *loader, const char *name, uint16_t port, int32_t pt) {
    _port = port;
    _prt = pt;
    task_ctx *task = task_new(loader, name, 0, NULL, NULL, NULL);
    if (ERR_OK != task_register(task, _startup, NULL)) {
        task_free(task);
    }
}
