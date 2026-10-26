#ifndef TASK_POOL_H_
#define TASK_POOL_H_

#include "lib.h"

// db_pool_ctx 单元测试（假连接，不碰网络）：
//   1) db_pool_add 容量/重复指针校验
//   2) 懒建连：第一次借出才 connect，归还后再借不重连
//   3) 借出期间断连：归还后下一次借出自动重连
//   4) 建连失败：按 backoff 冷却，冷却期内不再尝试
//   5) cap=2、4 协程并发：同时在用不超过 2
//   6) cap=1、3 协程：按 FIFO 交接
//   7) 空闲探活：超过 ping_idle 先 ping；ping 失败冷却后重连
//   8) 等连接超时：DB_POOL_TIMEDOUT，连接归还后可继续借
//   9) 关闭唤醒等待者，借出连接在 put 时断开
// 全部 case 通过后将 *ok 置 1。
void task_pool_start(loader_ctx *loader, const char *name, int32_t *ok);

#endif//TASK_POOL_H_
