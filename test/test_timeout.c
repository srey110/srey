#include "test_timeout.h"
#include "test_synsl.h"

#define TIMEOUT_TIME 50
static void test_free_cb(void *arg) {
   FREE(arg);
}
static char *test_init_arg() {
    char *arg;
    CALLOC(arg, 1, 1024);
    nowmtime("%Y-%m-%d %H:%M:%S", arg);
    return arg;
}
static void _timeout(task_ctx *task, void *arg) {
    //LOG_INFO("timeout, start at: %s", (char *)arg);
    task_ctx *test_syn = srey_task_grab(task->srey, TEST_SYN);
    if (NULL == test_syn) {
        test_synsl();
    } else {
        srey_task_ungrab(test_syn);
    }
#if WITH_CORO
    syn_timeout(task, TIMEOUT_TIME, _timeout, test_free_cb, test_init_arg());
#else
    srey_timeout(task, 0, TIMEOUT_TIME, _timeout, test_free_cb, test_init_arg());
#endif
}
static void _startup(task_ctx *task, message_ctx *msg) {
#if WITH_CORO
    syn_timeout(task, TIMEOUT_TIME, _timeout, test_free_cb, test_init_arg());
#else
    srey_timeout(task, 0, TIMEOUT_TIME, _timeout, test_free_cb, test_init_arg());
#endif
}
static void _request(task_ctx *task, message_ctx *msg) {
    if (INVALID_TNAME != msg->src) {
        task_ctx *src = srey_task_grab(task->srey, msg->src);
        if (NULL != src) {
            srey_response(src, msg->sess, ERR_OK, msg->data, msg->size, 1);
            srey_task_ungrab(src);
        }
    } else {
        const char *call = "this is srey_call.";
        if (NULL == msg->data
            || msg->size != strlen(call)
            || 0 != memcmp(msg->data, call, msg->size)) {
            LOG_WARN("srey_call error.");
        }
    }
}
void test_timeout(void) {
    task_ctx *task = srey_task_new(TTYPE_C, TEST_TIMEOUT, 0, 0, NULL, NULL);
    srey_task_regcb(task, MSG_TYPE_STARTUP, _startup);
    srey_task_regcb(task, MSG_TYPE_REQUEST, _request);
    srey_task_register(srey, task);
}
