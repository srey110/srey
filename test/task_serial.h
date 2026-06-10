#ifndef TASK_SERIAL_H_
#define TASK_SERIAL_H_

#include "lib.h"

// coro_serial_new / coro_serial_free / coro_serial_call 单元测试：
//   1) 单协程进入：验证 func 被调用 + arg 透传
//   2) 同协程嵌套 cs：cs 内再调 cs 不死锁，ref 计数正确
//   3) 跨协程串行 + FIFO：A 持锁 coro_sleep，B/C 排队等待，顺序 A→B→C
//   4) 不在协程上下文调用：返回 ERR_FAILED
//   5) 多 serial 实例独立：cs1 不阻塞 cs2
//   6) cs 内 yield 互斥：任意时刻最多 1 个协程在 cs 内（peak == 1）
// 全部 case 通过后将 *ok 置 1。
void task_serial_start(loader_ctx *loader, const char *name, int32_t *ok);

#endif//TASK_SERIAL_H_
