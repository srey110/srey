#include "test_wbsk.h"

struct _send_ck_arg {
    int32_t n;
    char data[128];
};
static void *_send_huncked(size_t *lens, void *arg) {
    struct _send_ck_arg *ckarg = arg;
    ckarg->n++;
    if (ckarg->n <= 3) {
        ZERO(ckarg->data, sizeof(ckarg->data));
        SNPRINTF(ckarg->data, sizeof(ckarg->data) - 1, "1234567890 %d.", ckarg->n);
        *lens = strlen(ckarg->data);
        return ckarg->data;
    } else {
        return NULL;
    }
}
static void _recv(task_ctx *task, message_ctx *msg) {
    int32_t fin = websock_pack_fin(msg->data);
    int32_t proto = websock_pack_proto(msg->data);
    switch (proto) {
    case WBSK_CLOSE: {
        ev_close(&task->srey->netev, msg->fd, msg->skid);
        break;
    }
    case WBSK_PING: {
        websock_pong(&task->srey->netev, msg->fd, msg->skid, 0);
        break;
    }
    default: {
        if (1 == fin) {
            if (WBSK_CONTINUE == proto) {
                PRINT("continua end");
            }
            size_t lens;
            char time[TIME_LENS];
            nowtime("%Y-%m-%d %H:%M:%S ", time);
            char *data = websock_pack_data(msg->data, &lens);
            if (0 == memcmp(data, "ck", strlen("ck"))) {
                struct _send_ck_arg arg;
                arg.n = 0;
                syn_websock_text(task, msg->fd, msg->skid, 0, _send_huncked, NULL, &arg);                
            } else {
                websock_text(&task->srey->netev, msg->fd, msg->skid, 0, 1, time, strlen(time));
            }
        } else {
            size_t lens;
            //char *cdata = websock_pack_data(msg->data, &lens);
            websock_pack_data(msg->data, &lens);
            PRINT("continua size %d", (uint32_t)lens);
        }
        break;
    }
    }
}
void test_wbsk(void) {
    task_ctx *task = srey_task_new(TTYPE_C, TEST_WBSK, 0, NULL, NULL);
    srey_task_regcb(task, MSG_TYPE_RECV, _recv);
    srey_task_register(srey, task);
    uint64_t lsnid;
    srey_listen(task, PACK_WEBSOCK, NULL, "0.0.0.0", 15003, 1, &lsnid);
}
