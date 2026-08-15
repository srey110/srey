#ifndef TASK_V6ONLY_H_
#define TASK_V6ONLY_H_

#include "lib.h"

// IPV6_V6ONLY 强制生效回归测试：
//   监听 "::" 后，"::1" 必须连得上、"127.0.0.1" 必须连不上——
//   证明 sock_v6only 真的设上了，而不是落回平台默认
//   （Linux/macOS 默认收 v4-mapped，不设就会让 IPv4 连进来）。
// 本机无 IPv6 回环时视为通过(只跳过断言)，唯一置 ok=0 的是"IPv4 连进 :: 监听"这条真回归。
void task_v6only_start(loader_ctx *loader, const char *name, uint16_t port, int32_t *ok);

#endif//TASK_V6ONLY_H_
