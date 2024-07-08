#include "task_udp.h"

static int32_t _prt = 0;
static void _net_recvfrom(task_ctx *task, SOCKET fd, uint64_t skid, 
    char ip[IP_LENS], uint16_t port, void *data, size_t size) {
    ev_sendto(&task->loader->netev, fd, skid, ip, port, data, size);
}
static void _startup(task_ctx *task) {
    on_recvedfrom(task, _net_recvfrom);
    uint64_t id;
    trigger_udp(task, "0.0.0.0", 15002, &id);
}
void task_udp_start(loader_ctx *loader, name_t name, int32_t pt) {
    _prt = pt;
    task_ctx *task = task_new(loader, name, NULL, NULL, NULL);
    task_register(task, _startup, NULL);
}
