#ifndef TASK_CORO_EXTRA_H_
#define TASK_CORO_EXTRA_H_

#include "lib.h"

// 协程 API 边界/失败路径补充覆盖：
//   coro_sleep 1500ms（穿越 tv1 → tv2 时间轮 cascade）
//   coro_connect 拒绝（127.0.0.1:1 不可达端口）
//   coro_send 在已 ev_close 的 fd 上调用（peer-disconnect 路径）
//   dns_lookup 解析 example.com（DNS 协程路径）
//   coro_request 未知 rtype 触发请求超时
//   task_register 同名第二次被拒，已注册那个的名字索引不受影响
// 另含 coro_sess 契约的七条（与 bin/script/test/unit_coro.lua 测同一组场景，两侧各自断言）：
//   CLOSE 广播排空期间重新注册同一 sess，新等待者不被收尾误删
//   同一 skid 上并发 coro_sendto，N 个等待者与 N 个响应逐一配对不丢
//   同 sess 同 mtype 的两个等待者按注册顺序唤醒（FIFO）
//   队头 mtype 不匹配即视为无等待者，不越过队头去找
//   keep=true 的条目摘空 waiters 后仍留着，直到 CLOSE 清零才可删
//   超时路径无视 keep，摘空即删
//   超时摘的是到期者本身（可能不是队头），还剩等待者时条目留着
// 全部 case 通过后将 *ok 置 1；任何步骤失败立即 LOG_ERROR 并返回（不置位）。
// httpport 用于 send-after-close 与 close-reregister；udpport 是 udp echo server 的端口；
// rpcname 用于 coro_request 目标（任意已注册的 task）。
void task_coro_extra_start(loader_ctx *loader, const char *name, uint16_t httpport, uint16_t udpport,
                           const char *rpcname, int32_t *ok);

#endif//TASK_CORO_EXTRA_H_
