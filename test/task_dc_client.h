#ifndef TASK_DC_CLIENT_H_
#define TASK_DC_CLIENT_H_

#include "lib.h"

// DataCenter (lib/services/datacenter.c) 集成测试 — 以 client 协程身份调 coro_dc_*:
//   1) set + get 同步 round-trip(返回 OK,get 拿到 value)
//   2) wait 命中(已 set):立即返回
//   3) wait 未命中:挂起 → 另一协程 set 后唤醒 + 拿到 value
//   4) multi waiter:3 个协程 wait 同 key → 1 次 set 唤醒所有 3 个
//   5) wait 超时:无人 set,超时返回 NULL
//   6) delete:set + delete + get → nil;delete 不动 pending(已挂起 waiter 继续等)
//   7) list_keys:set 3 key + list_keys → 收到 u16 长度前缀帧格式的 3 个 key
//   8) set value=NULL:get 返回 NULL(软清空)
//   9) 超长 key:client helper 校验 key 长度 >= DC_KEY_MAX 时不下发(set/get/del 均早返失败)
//  10) waiter 超时过期后迟到 set 不再唤醒它(不产生幽灵响应)
// ASan 不报 leak 即证明 dc_entry/dc_waiter/dc_pending 释放路径正确。
// dc_name 由 main.c 传入,与 dc_start 注册的 name 一致。
void task_dc_client_start(loader_ctx *loader, const char *base_name, const char *dc_name, int32_t *ok);

#endif//TASK_DC_CLIENT_H_
