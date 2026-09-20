#include "task_udp_server.h"

static pack_type _pktype = PACK_NONE;
static uint16_t _port = 0;

// 收到 UDP 数据报后原样回发给发送方
static void _net_recvfrom(task_ctx *task, sock_ctx *sk, subtype_t pktype,
    char ip[IP_LENS], uint16_t port, void *data, size_t size) {
    (void)pktype;
    ev_sendto(&task->loader->netev, sk, ip, port, data, size, 1);
}
static void _startup(task_ctx *task) {
    task_recvedfrom(task, _net_recvfrom);
    sock_ctx sk;
    if (ERR_OK != task_udp(task, _pktype, "0.0.0.0", _port, &sk)) {
        LOG_WARN("start udp server error.");
    }
}
void task_udp_server_start(loader_ctx *loader, const char *name, uint16_t port, pack_type pktype) {
    _port = port;
    _pktype = pktype;
    task_ctx *task = task_new(loader, name, 0, NULL, NULL, NULL);
    if (ERR_OK != task_register(task, _startup, NULL)) {
        task_free(task);
    }
}
