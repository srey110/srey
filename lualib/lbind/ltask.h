#ifndef LTASK_H_
#define LTASK_H_

#include "lbind/lpub.h"

/// <summary>
/// 初始化 Lua 运行时并执行启动脚本（startup.lua），
/// 用于在主进程启动阶段加载 Lua 配置/初始化逻辑。
/// </summary>
/// <param name="script">Lua 脚本所在子目录名（相对于程序根路径）</param>
/// <returns>成功返回 ERR_OK，失败返回 ERR_FAILED</returns>
int32_t ltask_startup(const char *script);

#endif//LTASK_H_
