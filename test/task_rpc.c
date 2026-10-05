#include "task_rpc.h"

static int32_t _prt = 0;

// 整数加法，供 type1 请求调用
static int32_t _add(int32_t a, int32_t b) {
    return a + b;
}
// 处理来自 task_timeout 的 RPC 请求，src 为 INVALID_TNAME 时表示 fire-and-forget
static void _requested(task_ctx *task, subtype_t reqtype, uint64_t sess, name_t src, void *data, size_t size) {
    switch (reqtype) {
    case 100: {
        // 整数加法：读取两个 int32（网络字节序），返回和（网络字节序）
        binary_ctx breader;
        binary_init_read(&breader, data, size);
        int32_t a = (int32_t)binary_get_integer(&breader, 4, 0);
        int32_t b = (int32_t)binary_get_integer(&breader, 4, 0);
        int sum = _add(a, b);
        if (INVALID_TNAME != src) {
            task_ctx *resp = task_grab(task->loader, src);
            if (NULL != resp) {
                int32_t rst = htonl(sum);
                task_response(resp, reqtype, sess, ERR_OK, &rst, sizeof(rst), 1);
                task_ungrab(resp);
            } else {
                LOG_WARN("grab task %"PRIu64" error.", src);
            }
        } else {
            // task_call fire-and-forget，无需回复
            if (_prt) {
                LOG_INFO("this is task call, sum: %d", sum);
            }
        }
        break;
    }
    case 101: {
        // 字节串回显：原样返回请求数据，覆盖变长数据路径；按句柄投递（task_response_to），不先 grab
        if (INVALID_TNAME != src
            && ERR_OK != task_response_to(task->loader, src, reqtype, sess, ERR_OK, data, size, 1)) {
            LOG_WARN("response to task %"PRIu64" error.", src);
        }
        break;
    }
    default:
        break;
    }
}
// 按句柄投递的失败契约：目标不在返回 ERR_FAILED，copy=1 时内部副本自己放、copy=0 时载荷没碰过仍归调用方；
// 投给自己的单向加法走成功路径。泄漏与重复释放由退出时的内存检查兜
static void _check_post_to(task_ctx *task) {
    int32_t pair[2];
    name_t gone = createid();// 从没注册过的句柄
    void *buf;
    pair[0] = (int32_t)htonl(1);
    pair[1] = (int32_t)htonl(2);
    ASSERTAB(ERR_FAILED == task_call_to(task->loader, gone, 100, pair, sizeof(pair), 1),
             "task_call_to unknown handle must fail");
    ASSERTAB(ERR_FAILED == task_response_to(task->loader, INVALID_TNAME, 100, 1, ERR_OK, pair, sizeof(pair), 1),
             "task_response_to invalid handle must fail");
    buf = dup_zero(pair, sizeof(pair));
    ASSERTAB(ERR_FAILED == task_request_to(task->loader, gone, NULL, 100, 0, buf, sizeof(pair), 0),
             "task_request_to unknown handle must fail");
    FREE(buf);// copy=0 投递失败，载荷仍归调用方
    ASSERTAB(ERR_OK == task_call_to(task->loader, task->handle, 100, pair, sizeof(pair), 1),
             "task_call_to self must succeed");
}
static void _startup(task_ctx *task) {
    task_requested(task, _requested);
    _check_post_to(task);
}
void task_rpc_start(loader_ctx *loader, const char *name, int32_t pt) {
    _prt = pt;
    coro_task_register(loader, name, 0, _startup, NULL, NULL, NULL);
}
