#ifndef TASK_DBREFCNT_H_
#define TASK_DBREFCNT_H_

#include "lib.h"

// DB 绑定(mysql/pgsql/mongo/smtp)引用计数配对回归。
// 模拟 Lua handle 反复连非法地址触发 ev_connect 同步失败，验证 acquire(try_connect +1)
// 与 udfree(-1) 严格配对：失败路径不会把仍被持有的 ctx 块误 free(修复前的 UAF/double-free)。
void task_dbrefcnt_start(loader_ctx *loader, const char *name, int32_t *ok);

#endif//TASK_DBREFCNT_H_
