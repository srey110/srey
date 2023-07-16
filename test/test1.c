#include "test1.h"


void test1_run(task_ctx *task, message_ctx *msg) {
    switch (msg->mtype) {
    case MSG_TYPE_STARTUP: {
#if WITH_CORO
        syn_timeout(task, createid(), 50);
#else
        srey_timeout(task, createid(), 50);
#endif 
        uint64_t lsnid;
        if (ERR_OK != srey_listen(task, PACK_SIMPLE, NULL, "0.0.0.0", 15000, 0, &lsnid)) {
            LOG_WARN("srey_listen error.");
        }
        break;
    }
    case MSG_TYPE_TIMEOUT: {
        task_ctx *test4 = srey_task_grab(task->srey, TEST4);
        if (NULL != test4) {
            const char *call = "this is srey_call.";
            srey_call(test4, (void *)call, strlen(call), 1);
            const char *req = "this is test1 syn_request.";
#if WITH_CORO
            int32_t erro;
            size_t lens;
            char *rtn = syn_request(test4, task, (void *)req, strlen(req), 1, &erro, &lens);
            if (NULL == rtn
                || lens != strlen(req)
                || 0 != memcmp(rtn, req, lens)) {
                LOG_WARN("syn_request error.");
            }
#else
            srey_request(test4, task, createid(), (void *)req, strlen(req), 1);
#endif 
            srey_task_release(test4);
        }
#if WITH_CORO
        syn_timeout(task, createid(), 50);
#else
        srey_timeout(task, createid(), 50);
#endif 
        break;
    }
    case MSG_TYPE_RECV: {
#if RAND_CLOSE
        if (randrange(1, 100) <= 1) {
            ev_close(&task->srey->netev, msg->fd, msg->skid);
            break;
        }
#endif
#if DELAY_SEND && WITH_CORO
        syn_sleep(task, 10);
#endif
        size_t lens;
        void *data = simple_data(msg->data, &lens);
        void *pack = simple_pack(data, lens, &lens);
        ev_send(&task->srey->netev, msg->fd, msg->skid, pack, lens, 0);
        break;
    }
    case MSG_TYPE_RESPONSE: {
        const char *resp = "this is test1 syn_request.";
        if (msg->size != strlen(resp)
            || 0 != memcmp(msg->data, resp, msg->size)) {
            LOG_WARN("srey_request error.");
        }
        break;
    }
    default:
        break;
    }
}
