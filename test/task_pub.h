#ifndef TASK_PUB_H_
#define TASK_PUB_H_

#include "lib.h"

// 名称-整型值映射项，用于在测试任务间传递端口号、测试结果等配置
typedef struct name_val_ctx {
    const char *name;   // 键名，NULL 表示数组终止哨兵
    // 端口号，或测试结果：0 没连上（依赖外部服务的用例允许如此）/ 1 通过 /
    // -1 连上了、还没走完 / -2 正在连（依赖外部服务的用例与 mqtt 客户端发起连接前置上，连不上回 0）/
    // -3 已失败(终态，见 test_fail)。
    // 0 与 -1 分开是为了不让 main.c 的 optional 白名单把"服务起着但结果不对"吞成 network error；
    // 0 同时代表 docker 没起与密码/权限错：建连、握手、认证在 *_connect 里是一次调用，测试这层分不开。
    // 各 task 在自己的线程里写、汇总 task 在别的线程里读，读写一律 ATOMIC_GET / ATOMIC_SET((atomic_t *)槽, v)。
    // 各 task_*_start 的 *ok 出参一律按本字段解释，不再逐个重述
    int32_t val;
}name_val_ctx;

// TCP 自定义协议测试指令
typedef enum tcp_test_prot {
    TEST_ECHO = 0x01,       // 回显：服务端原样返回收到的数据
    TEST_SSL_CHANGE,        // SSL 升级：服务端先回显再切换为 SSL 模式
    TEST_PKTYPE_CHANGE,     // 协议切换：数据体第 2 字节指定新 pack_type
    TEST_RPC_ECHO
}tcp_test_prot;

// 用例的失败出口写终态 -3：汇总 task 见到就不再等满超时，直接计失败
static inline void test_fail(int32_t *ok) {
    ATOMIC_SET((atomic_t *)ok, -3);
}

// 给"整个用例在 _startup 里顺序跑完、最后才写 1"的用例用：跑完还没置 1 就是中途 return 了，
// 落终态 -3 免得汇总等满超时。startup 返回后还在异步跑、或槽由别的 task 写的别套。
// 关闭期返回的不算：那是超时/中断收尾，用例本来就要被打断
static inline void test_fail_unfinished(task_ctx *task, int32_t *ok) {
    if (!task_isclosing(task) && 1 != (int32_t)ATOMIC_GET((atomic_t *)ok)) {
        test_fail(ok);
    }
}

// 在 name_val_ctx 数组中按名称查找对应值的指针，找不到返回 NULL
static inline int32_t *_get_name_val(name_val_ctx *list, const char *name) {
    for (int32_t i = 0; ; i++) {
        if (NULL == list[i].name) {
            return NULL;
        }
        if (0 == strcmp(name, list[i].name)) {
            return &list[i].val;
        }
    }
}

#endif//TASK_PUB_H_
