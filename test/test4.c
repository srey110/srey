#include "test4.h"

void test4_run(task_ctx *task, message_ctx *msg) {
    switch (msg->mtype) {
    case MSG_TYPE_STARTUP: 
#if WITH_CORO
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
