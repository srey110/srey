#include "test_http.h"

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
    int32_t chunck = http_chunked(msg->data);
    switch (chunck) {
    case 0: {
        buffer_ctx buf;
        buffer_init(&buf);
        http_pack_resp(&buf, 200);
        http_pack_head(&buf, "Server", "Srey");
        char time[TIME_LENS];
        nowtime("%Y-%m-%d %H:%M:%S ", time);
        http_pack_content(&buf, time, strlen(time));
#if WITH_CORO
        http_response(task, msg->fd, msg->skid, &buf, NULL, NULL, NULL);
#else
        size_t rlen = buffer_size(&buf);
        char *resp;
        MALLOC(resp, rlen);
        buffer_copyout(&buf, 0, resp, rlen);
        ev_send(&task->srey->netev, msg->fd, msg->skid, resp, rlen, 0);
#endif
        buffer_free(&buf);
        break;
    }
    case 1: 
        break;
    case 2: {
        size_t lens;
        //char *pack = http_data(msg->data, &lens);
        http_data(msg->data, &lens);
        if (0 == lens) {
            buffer_ctx buf;
            buffer_init(&buf);
            http_pack_resp(&buf, 200);
            http_pack_head(&buf, "Server", "Srey");
#if WITH_CORO
            struct _send_ck_arg arg;
            arg.n = 0;
            http_response(task, msg->fd, msg->skid, &buf, _send_huncked, NULL, &arg);
#else
            char data[128];
            size_t rlen;
            char *resp;
            for (int32_t i = 0; i < 3; i++) {
                ZERO(data, sizeof(data));
                SNPRINTF(data, sizeof(data) - 1, "1234567890 %d.", i);
                http_pack_chunked(&buf, data, strlen(data));
                rlen = buffer_size(&buf);
                MALLOC(resp, rlen);
                buffer_remove(&buf, resp, rlen);
                ev_send(&task->srey->netev, msg->fd, msg->skid, resp, rlen, 0);
            }
            http_pack_chunked(&buf, NULL, 0);
            rlen = buffer_size(&buf);
            MALLOC(resp, rlen);
            buffer_remove(&buf, resp, rlen);
            ev_send(&task->srey->netev, msg->fd, msg->skid, resp, rlen, 0);
#endif
            buffer_free(&buf);
        } else {
            //fwrite(pack, 1, lens, file);
        }
        break;
    }
    }
}
void test_httpsv(void) {
    task_ctx *task = srey_task_new(TTYPE_C, TEST_HTTP, 0, 0, NULL, NULL);
    srey_task_regcb(task, MSG_TYPE_RECV, _recv);
    srey_task_register(srey, task);
    uint64_t lsnid;
    srey_listen(task, PACK_HTTP, NULL, "0.0.0.0", 15004, 1, &lsnid);
}
