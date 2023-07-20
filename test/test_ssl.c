#include "test_ssl.h"

static void _recv(task_ctx *task, message_ctx *msg) {
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
void test_ssl(void) {
#if WITH_SSL
    task_ctx *task = srey_task_new(TTYPE_C, TEST_SSL, 0, 0, NULL, NULL);
    srey_task_regcb(task, MSG_TYPE_RECV, _recv);
    srey_task_register(srey, task);
    uint64_t lsnid;
    srey_listen(task, PACK_SIMPLE, srey_ssl_qury(srey, SSL_SERVER), "0.0.0.0", 15001, 0, &lsnid);
#endif
}
