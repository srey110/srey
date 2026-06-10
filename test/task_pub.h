#ifndef TASK_PUB_H_
#define TASK_PUB_H_

#include "lib.h"

// 名称-整型值映射项，用于在测试任务间传递端口号、测试结果等配置
typedef struct name_val_ctx {
    const char *name;   // 键名，NULL 表示数组终止哨兵
    int32_t val;        // 对应的整型值（端口号 / 测试结果标志等）
}name_val_ctx;

// TCP 自定义协议测试指令
typedef enum tcp_test_prot {
    TEST_ECHO = 0x01,       // 回显：服务端原样返回收到的数据
    TEST_SSL_CHANGE,        // SSL 升级：服务端先回显再切换为 SSL 模式
    TEST_PKTYPE_CHANGE,     // 协议切换：数据体第 2 字节指定新 pack_type
    TEST_RPC_ECHO          
}tcp_test_prot;

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
    return NULL;
}

#endif//TASK_PUB_H_
