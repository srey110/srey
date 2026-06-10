#ifndef DEBUG_CONSOLE_H_
#define DEBUG_CONSOLE_H_

#include "srey/coro.h"

/// <summary>
/// 启动 HTTP 调试控制台 task service。浏览器访问 / 打开调试 UI 页面；
/// curl /{handle}/{cmd} 向目标 task 发 REQ_DEBUG 调试命令；/0/{cmd} 广播到所有 task。
/// 命令 help/mem/gc/stat/coros/loglv/inject/hotfix 由目标 task 自身的 REQ_DEBUG handler 处理
/// （C task 走 lib/srey/debug_request.c，Lua task 走 lib.debug_request）。
/// </summary>
/// <param name="loader">loader_ctx</param>
/// <param name="name">字符串任务名；NULL 或空串表示不启动</param>
/// <param name="ip">监听 IP；NULL 返回失败</param>
/// <param name="port">监听端口；0 表示不启动</param>
/// <returns>ERR_OK 成功（name/port 为空时跳过也返回 ERR_OK）；ERR_FAILED 启动失败</returns>
int32_t debug_console_start(loader_ctx *loader, const char *name, const char *ip, uint16_t port);

#endif//DEBUG_CONSOLE_H_
