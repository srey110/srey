#ifndef TASK_CLOSE_FLUSH_H_
#define TASK_CLOSE_FLUSH_H_

#include "lib.h"

// ev_close 的"关闭前冲刷一次"契约回归测试,两段:
// 1) 小包:每轮 coro_connect → ev_send(SMALL) → ev_close,一次冲刷必然写进内核,
//    server 端累计应精确等于发送总量,close_cb 触发 ROUNDS 次
// 2) 大包:一轮 ev_send(BIG) → ev_close,不断言送达量(超出内核发送缓冲的按契约丢弃),
//    只断言 close_cb 照样触发——即关闭有界,不会因对端来不及读而滞留
// 两段都通过则 *ok=1。
void task_close_flush_start(loader_ctx *loader, const char *name, uint16_t port, int32_t *ok);

#endif//TASK_CLOSE_FLUSH_H_
