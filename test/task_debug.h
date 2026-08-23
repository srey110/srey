#ifndef TASK_DEBUG_H_
#define TASK_DEBUG_H_

#include "lib.h"

// debug 命令链路集成测试（不依赖外部服务，另建两个靶子 task）：
//   A. _debug_request（C task 的 REQ_DEBUG 处理）经 coro_request 直发
//      1) stat 回统计文本（表头 + TOTAL），coros 回协程转储
//      2) loglv 三档：合法值回 "log level => N"、缺参、越界
//      3) mem/gc/inject/hotfix/未知命令/非位置化载荷一律透传业务——
//         C 侧那张 Lua 专属命令名字表已删，判定移到发起方，所以直发时它们与未知命令同路。
//         目标未注册 on_requested 时框架回 ERR_FAILED + "not register..."；
//         注册了则回业务自己的文案（debug_console 的 503 能说真话就靠这一点）
//   B. debug_console 的 HTTP 面（需 main.c 先起 console）
//      4) GET /{handle}/mem 打到 C task：发起方按目标 task 类型就地挡下，回 200 + 不支持
//      5) GET /{handle}/stat：needlua=0 正常透传，回统计文本
//      6) GET /0/mem 广播：每个 C task 都是同一句不支持
// 未覆盖：门的放行侧（needlua 且目标为 TASK_LUA）—— 本二进制里没有 Lua task
// 全部通过后置 *ok = 1
void task_debug_start(loader_ctx *loader, const char *name, uint16_t port, int32_t *ok);

#endif//TASK_DEBUG_H_
