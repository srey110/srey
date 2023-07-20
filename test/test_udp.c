#include "test_udp.h"

static void _recvfrom(task_ctx *task, message_ctx *msg) {
    netaddr_ctx *addr = msg->data;
    char ip[IP_LENS];
    netaddr_ip(addr, ip);
    uint16_t port = netaddr_port(addr);
    ev_sendto(&task->srey->netev, msg->fd, msg->skid, ip, port, (char *)msg->data + sizeof(netaddr_ctx), msg->size);
}
void test_udp(void) {
    task_ctx *task = srey_task_new(TTYPE_C, TEST_UDP, 0, 0, NULL, NULL);
    srey_task_regcb(task, MSG_TYPE_RECVFROM, _recvfrom);
    srey_task_register(srey, task);
    uint64_t lsnid;
    srey_udp(task, "0.0.0.0", 15002, &lsnid);
}
