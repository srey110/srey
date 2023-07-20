#include "test_tcp.h"

static uint64_t lsnid = 0;
static int32_t _prt = 0;
static void _acpt(task_ctx *task, message_ctx *msg) {
    if (0 != _prt) {
        LOG_INFO("accept fd %d, skid %d.", (uint32_t)msg->fd, (uint32_t)msg->skid);
    }
}
static void _recv(task_ctx *task, message_ctx *msg) {
    if (0 != _prt) {
        LOG_INFO("recv fd %d, skid %d, size %d.", (uint32_t)msg->fd, (uint32_t)msg->skid, (uint32_t)msg->size);
    }
#if RAND_CLOSE
    if (randrange(0, 100) <= 1) {
        ev_close(&task->srey->netev, msg->fd, msg->skid);
        return;
    }
#endif
    size_t lens;
    char *sbuf = simple_data(msg->data, &lens);
    void *outbuf = simple_pack(sbuf, lens, &lens);
    ev_send(&task->srey->netev, msg->fd, msg->skid, outbuf, lens, 0);
}
static void _sended(task_ctx *task, message_ctx *msg) {
    if (0 != _prt) {
        LOG_INFO("send fd %d, skid %d, size %d.", (uint32_t)msg->fd, (uint32_t)msg->skid, (uint32_t)msg->size);
    }
}
static void _closed(task_ctx *task, message_ctx *msg) {
    if (0 != _prt) {
        LOG_INFO("close fd %d, skid %d.", (uint32_t)msg->fd, (uint32_t)msg->skid);
    }
}
static void _closing(task_ctx *task, message_ctx *msg) {
    ev_unlisten(&task->srey->netev, lsnid);
}
void test_tcp(void) {
    task_ctx *task = srey_task_new(TTYPE_C, TEST_TCP, 0, 0, NULL, NULL);
    srey_task_regcb(task, MSG_TYPE_ACCEPT, _acpt);
    srey_task_regcb(task, MSG_TYPE_RECV, _recv);
    srey_task_regcb(task, MSG_TYPE_SEND, _sended);
    srey_task_regcb(task, MSG_TYPE_CLOSE, _closed);
    srey_task_regcb(task, MSG_TYPE_CLOSING, _closing);
    srey_task_register(srey, task);
    srey_listen(task, PACK_SIMPLE, NULL, "0.0.0.0", 15000, 1, &lsnid);
}
