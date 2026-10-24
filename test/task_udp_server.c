#include "task_udp_server.h"

static pack_type _pktype = PACK_NONE;
static uint16_t _port = 0;

// 收到 UDP 数据报后原样回发给发送方：源地址直接交给 ev_sendto_addr，不经字符串来回；
// 客户端那边 coro_sendto 收得到回显就说明地址原样无误
static void _net_recvfrom(task_ctx *task, sock_ctx *sk, subtype_t pktype,
    const netaddr_ctx *addr, void *data, size_t size) {
    (void)pktype;
    ev_sendto_addr(&task->loader->netev, sk, addr, data, size, 1);
}
static void _startup(task_ctx *task) {
    task_recvedfrom(task, _net_recvfrom);
    sock_ctx sk;
    if (ERR_OK != task_udp(task, _pktype, "0.0.0.0", _port, 0, &sk)) {
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
