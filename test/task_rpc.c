#include "task_rpc.h"

static int32_t _prt = 0;

// 整数加法，供 type1 请求调用
static int32_t _add(int32_t a, int32_t b) {
    return a + b;
}
// 处理来自 task_timeout 的 RPC 请求，src 为 INVALID_TNAME 时表示 fire-and-forget
static void _requested(task_ctx *task, uint8_t reqtype, uint64_t sess, name_t src, void *data, size_t size) {
    switch (reqtype) {
    case 100: {
        // 整数加法：读取两个 int32（网络字节序），返回和（网络字节序）
        binary_ctx breader;
        binary_init(&breader, data, size, 0);
        int32_t a = (int32_t)binary_get_integer(&breader, 4, 0);
        int32_t b = (int32_t)binary_get_integer(&breader, 4, 0);
        int sum = _add(a, b);
        if (INVALID_TNAME != src) {
            task_ctx *resp = task_grab(task->loader, src);
            if (NULL != resp) {
                int32_t rst = htonl(sum);
                task_response(resp, sess, ERR_OK, &rst, sizeof(rst), 1);
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
        // 字节串回显：原样返回请求数据，覆盖变长数据路径
        if (INVALID_TNAME != src) {
            task_ctx *resp = task_grab(task->loader, src);
            if (NULL != resp) {
                task_response(resp, sess, ERR_OK, data, size, 1);
                task_ungrab(resp);
            } else {
                LOG_WARN("grab task %"PRIu64" error.", src);
            }
        }
        break;
    }
    default:
        break;
    }
}
static void _startup(task_ctx *task) {
    task_requested(task, _requested);
}
void task_rpc_start(loader_ctx *loader, const char *name, int32_t pt) {
    _prt = pt;
    coro_task_register(loader, name, 0, _startup, NULL, NULL, NULL);
}
