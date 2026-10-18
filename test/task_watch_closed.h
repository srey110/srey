#ifndef TASK_WATCH_CLOSED_H_
#define TASK_WATCH_CLOSED_H_

#include "lib.h"

// task_watch_closed 的契约回归：普通 task 与协程 task 各一个，各登记两个观察者 A、B 再设业务关闭回调。
// 先连 127.0.0.1:1(必拒)：失败那条连接的合成 CLOSE(NEVERCONN)观察者与业务回调都不该收到；
// 再连 httpport 并当场关掉：这条 CLOSE 须按 A → B → 业务回调的顺序各调一次，三处拿到的 sess 都等于该连接的 skid。
// 两个 task 各自通过时把 *ok_normal / *ok_coro 置 1
void task_watch_closed_start(loader_ctx *loader, uint16_t httpport, int32_t *ok_normal, int32_t *ok_coro);

#endif//TASK_WATCH_CLOSED_H_
