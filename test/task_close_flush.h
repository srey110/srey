#ifndef TASK_CLOSE_FLUSH_H_
#define TASK_CLOSE_FLUSH_H_

#include "lib.h"

// ev_close 的"发完即关、关闭有界"契约回归测试,两段:
// 1) 小包:每轮 coro_connect → ev_send(SMALL) → ev_close,server 端累计应精确等于发送总量,
//    close_cb 触发 ROUNDS 次。注意这一段走的是普通直发路径:CMD_SEND 排在 CMD_DISCONNECT
//    之前,4KB 当场就写进内核了,轮到冲刷时队列已空、直接早退
// 2) 大包:一轮 ev_send(BIG) → ev_close,不断言送达量(写不进内核的按契约丢弃),
//    只断言 close_cb 照样触发——即关闭有界,不会因对端来不及读而滞留
// 两段都通过则 *ok=1。
// 冲刷本身(_evpub_close_flush_tcp)的送达与守卫由 test_event.c 直接单测:队列非空是它干活的
// 前提,而 unix 侧只要 socket 可写正常写路径就已抽干队列,集成场景观察不到它送出字节
void task_close_flush_start(loader_ctx *loader, const char *name, uint16_t port, int32_t *ok);

#endif//TASK_CLOSE_FLUSH_H_
