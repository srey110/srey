#include "test4.h"

#if WITH_CORO
struct _send_ck_arg {
    int32_t n;
    size_t nbuf;
    char data[128];
};
void _recv_chunck(void *data, size_t size, int32_t end, void *arg) {
    struct _send_ck_arg *ck = arg;
    ck->nbuf += size;
    if (0 != end) {
        //LOG_INFO("chunck end.");
    } else {
        //LOG_INFO("chunck lens %d.", size);
    }
}
void _free_ckdata(void *data) {
}
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
void _test_http(task_ctx *task) {
    uint64_t skid;
    SOCKET fd = syn_connect(task, PACK_HTTP, NULL, "127.0.0.1", 15004, 0, &skid);
    if (INVALID_SOCK == fd) {
        LOG_WARN("syn_connect error.");
        return;
    }
    buffer_ctx buf;
    buffer_init(&buf);
    http_pack_req(&buf, "Post", "/test");
    http_pack_head(&buf, "Server", "Srey");
    struct _send_ck_arg arg;
    arg.n = 0;
    arg.nbuf = 0;
    struct http_pack_ctx *pack = http_post(task, fd, skid, &buf, _send_huncked, _recv_chunck, _free_ckdata, &arg);
    ev_close(&task->srey->netev, fd, skid);
    if (NULL == pack
        || 39 != arg.nbuf) {
        LOG_WARN("http_post error.");
    }
}
void test4_wbsk(task_ctx *task) {
    uint64_t skid;
    SOCKET fd = syn_websock_connect(task, "127.0.0.1", 15003, NULL, &skid);
    if (INVALID_SOCK == fd) {
        LOG_WARN("syn_websock_connect error.");
        return;
    }
    struct _send_ck_arg arg;
    arg.n = 0;
    arg.nbuf = 0;
    syn_websock_text(task, fd, skid, 1, _send_huncked, _free_ckdata, &arg);
    ev_close(&task->srey->netev, fd, skid);
}
#endif
void test4_run(task_ctx *task, message_ctx *msg) {
    switch (msg->mtype) {
    case MSG_TYPE_STARTUP: 
#if WITH_CORO
        _test_http(task);
        test4_wbsk(task);
        syn_timeout(task, createid(), 5000);//5秒后释放
        uint64_t bg = nowsec();
        syn_sleep(task, 1000);
        if (1 != nowsec() - bg) {
            LOG_WARN("syn_sleep error.");
        }
#else
        srey_timeout(task, createid(), 5000);//5秒后释放
#endif
        break;
    case MSG_TYPE_CLOSING:
        LOG_INFO(".....test4 closing.....");
        break;
    case MSG_TYPE_TIMEOUT:
        srey_task_release(task);
        break;
    case MSG_TYPE_REQUEST: {
        if (INVALID_TNAME != msg->src) {
            task_ctx *src = srey_task_grab(task->srey, msg->src);
            if (NULL != src) {
                srey_response(src, msg->sess, ERR_OK, msg->data, msg->size, 1);
                srey_task_release(src);
            }
        } else {
            const char *call = "this is srey_call.";
            if (NULL == msg->data
                || msg->size != strlen(call)
                || 0 != memcmp(msg->data, call, msg->size)) {
                LOG_WARN("srey_call error.");
            }
        }
        break;
    }
    default:
        break;
    }
}
