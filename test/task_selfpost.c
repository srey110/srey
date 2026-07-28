#include "task_selfpost.h"

// 故意设小的 qumsg 容量, 使自投递立刻越过快路径进入溢出层。
// 取 2 而非更大值有第二个理由: task_new 按 fsqu_capacity / QUEUE_OVERLOAD_RATIO 推导过载告警阈值,
// 2/3 == 0 而 tda_check 把 0 视为禁用, 于是不会每次跑测试都打一条与真实背压无法区分的 overload 警告。
// 不可再小: mpq_init 有 ASSERTAB(capacity >= 2)
#define SELFPOST_QUECAP  2
// 自投递条数, 须远大于 SELFPOST_QUECAP
#define SELFPOST_CNT     64
// 自投递使用的请求类型
#define SELFPOST_REQTYPE 100

typedef struct task_selfpost_args {
    int32_t *ok;
}task_selfpost_args;

static atomic_t _nrecved;

static void _requested(task_ctx *task, subtype_t reqtype, uint64_t sess, name_t src, void *data, size_t size) {
    (void)sess;
    (void)src;
    (void)data;
    (void)size;
    if (SELFPOST_REQTYPE != reqtype) {
        return;
    }
    // 同 task 消息串行处理, 用原子仅为跨 worker 迁移时的可见性
    if (SELFPOST_CNT != ATOMIC_ADD(&_nrecved, 1) + 1) {
        return;
    }
    task_selfpost_args *arg = (task_selfpost_args *)coro_get_arg(task);
    *(arg->ok) = 1;
#if FSQU_MPQ
    LOG_INFO("selfpost tested, %d messages received.", SELFPOST_CNT);
#else
    LOG_INFO("selfpost tested, %d messages received (FSQU_MPQ=0: fsqu_push is unbounded, mpq overflow fallback not covered).",
             SELFPOST_CNT);
#endif
}
static void _startup(task_ctx *task) {
    task_requested(task, _requested);
    // 在自身 dispatch 内向自己投递: 唯一消费者是执行本函数的 worker,
    // 入队若阻塞则无人排空, 此处必挂死(见头文件说明)
    int32_t i;
    for (i = 0; i < SELFPOST_CNT; i++) {
        task_call(task, SELFPOST_REQTYPE, NULL, 0, 1);
    }
}
void task_selfpost_start(loader_ctx *loader, const char *name, int32_t *ok) {
    if (NULL == ok) {
        return;
    }
    ATOMIC_SET(&_nrecved, 0);
    task_selfpost_args *arg;
    CALLOC(arg, 1, sizeof(task_selfpost_args));
    arg->ok = ok;
    coro_task_register(loader, name, SELFPOST_QUECAP, _startup, NULL, _free, arg);
}
