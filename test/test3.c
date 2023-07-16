#include "test3.h"
#include "test4.h"

void test3_run(task_ctx *task, message_ctx *msg) {
    switch (msg->mtype) {
    case MSG_TYPE_STARTUP: {
        uint64_t skid;
        srey_udp(task, "0.0.0.0", 15002, &skid);
#if WITH_CORO
        syn_timeout(task, createid(), 50);
#else
        srey_timeout(task, createid(), 50);
#endif 
        break;
    }
    case MSG_TYPE_TIMEOUT: {
        task_ctx *test4 = srey_task_grab(task->srey, TEST4);
        if (NULL == test4) {
#if WITH_CORO
            if (ERR_OK != syn_task_new(task, TTYPE_CORO, TEST4, 0, 0, NULL, test4_run, NULL, NULL, NULL)){
                LOG_WARN("syn_task_new test4 error.");
            }
#else
            if (ERR_OK != srey_task_new(task->srey, TTYPE_DEF, TEST4, 0, 0, INVALID_TNAME, 0, NULL, test4_run, NULL, NULL, NULL)) {
                LOG_WARN("syn_task_new test4 error.");
            }
#endif 
        } else {
            const char *call = "this is srey_call.";
            srey_call(test4, (void *)call, strlen(call), 1);
            const char *req = "this is test3 syn_request.";
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
    case MSG_TYPE_RESPONSE: {
        const char *resp = "this is test3 syn_request.";
        if (msg->size != strlen(resp)
            || 0 != memcmp(msg->data, resp, msg->size)) {
            LOG_WARN("srey_request error.");
        }
        break;
    }
    case MSG_TYPE_RECVFROM: {
        char ip[IP_LENS];
        netaddr_ctx *addr = msg->data;
        netaddr_ip(addr, ip);
        uint16_t port = netaddr_port(addr);
        ev_sendto(&task->srey->netev, msg->fd, msg->skid, ip, port, ((char *)msg->data) + sizeof(netaddr_ctx), msg->size);
        break;
    }
    default:
        break;
    }
}
