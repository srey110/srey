#include "test_timeout.h"
#include "test_synsl.h"

#define TIMEOUT_TIME 50
static void test_free_cb(void *arg) {
   FREE(arg);
}
static char *test_init_arg() {
    char *arg;
    CALLOC(arg, 1, 1024);
    mstostr(nowms(), "%Y-%m-%d %H:%M:%S", arg);
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
    syn_timeout(task, TIMEOUT_TIME, _timeout, test_free_cb, test_init_arg());
}
static void _startup(task_ctx *task, message_ctx *msg) {
    uint64_t bg = nowsec();
    syn_sleep(task, 1000);
    if (nowsec() - bg != 1) {
        LOG_WARN("syn_sleep error.");
    }
    syn_timeout(task, TIMEOUT_TIME, _timeout, test_free_cb, test_init_arg());
}
static void _request(task_ctx *task, message_ctx *msg) {
    if (INVALID_TNAME != msg->src) {
        task_ctx *src = srey_task_grab(task->srey, msg->src);
        if (NULL != src) {
            //syn_sleep(task, 2000);
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
static cJSON *test_void(task_ctx *task, cJSON *args) {
    /*char *info = cJSON_PrintUnformatted(args);
    FREE(info);*/
    return cJSON_CreateObject();
}
static cJSON *test_add(task_ctx *task, cJSON *args) {
    int32_t n = cJSON_GetArraySize(args);
    if (2 != n) {
        return NULL;
    }
    cJSON *val1 = cJSON_GetArrayItem(args, 0);
    cJSON *val2 = cJSON_GetArrayItem(args, 1);
    int32_t sum = (int32_t)(val1->valuedouble + val2->valuedouble);
    return rpc_args_format("i", sum);
}
void test_timeout(void) {
    task_ctx *task = srey_task_new(TTYPE_C, TEST_TIMEOUT, NULL, NULL);
    srey_task_regcb(task, MSG_TYPE_STARTUP, _startup);
    srey_task_regcb(task, MSG_TYPE_REQUEST, _request);
    rpc_register(task, "test_void", test_void);
    rpc_register(task, "test_add", test_add);
    srey_task_register(srey, task);
}
