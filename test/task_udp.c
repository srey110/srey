#include "task_udp.h"

static int32_t _prt = 0;
static void _net_recvfrom(task_ctx *task, SOCKET fd, uint64_t skid, uint64_t sess,
    char ip[IP_LENS], uint16_t port, void *data, size_t size) {
    ev_sendto(&task->scheduler->netev, fd, skid, ip, port, data, size);
}
void task_udp_start(scheduler_ctx *scheduler, name_t name, int32_t pt) {
    _prt = pt;
    task_ctx *task = task_new(name, NULL, NULL, NULL);
    task_register(scheduler, task, NULL, NULL);
    register_net_recvfrom(task, _net_recvfrom);
    uint64_t id;
    trigger_udp(task, "0.0.0.0", 15002, &id);
}
