#ifndef LBYTECACHE_H_
#define LBYTECACHE_H_

#include "lbind/lpub.h"
#if ENABLE_LUA_BYTECACHE

// mtime 失效开关:1 每次加载校验文件 mtime(开发期改脚本重启 task 自动失效);0 关闭(省一次 stat)
#define LBC_CHECK_MTIME 1

/// <summary>
/// 初始化进程级 Lua 字节码缓存;须在首个业务 task 加载脚本前调用一次。
/// </summary>
/// <param name="lck">rwlock_distr_ctx 分布式读写锁</param>
void lbc_init(rwlock_distr_ctx *lck);
/// <summary>
/// 释放字节码缓存;须在所有 worker 停止(loader_free)之后调用。
/// </summary>
void lbc_free(void);
/// <summary>
/// 判断 file 是不是普通文件,顺手取缓存校验要用的 mtime;找文件时用它代替 isfile,加载时就不必再 stat 一次。
/// </summary>
/// <param name="file">文件路径</param>
/// <param name="mt">出参:文件修改时间;LBC_CHECK_MTIME=0 时恒 0</param>
/// <returns>普通文件 ERR_OK,否则 ERR_FAILED(此时 mt 未写)</returns>
int32_t lbc_stat(const char *file, uint64_t *mt);
/// <summary>
/// 缓存版脚本加载,栈效果等同 luaL_loadfile:成功后栈顶为可调用 chunk。
/// </summary>
/// <param name="lua">lua_State</param>
/// <param name="path">脚本文件路径</param>
/// <param name="mt">lbc_stat 对同一路径取得的 mtime</param>
/// <returns>LUA_OK 或 Lua 错误码</returns>
int32_t lbc_loadfile(lua_State *lua, const char *path, uint64_t mt);
/// <summary>
/// 将指定 lua_State 的 package Lua 文件 searcher 替换为缓存版(require 走缓存)。
/// </summary>
/// <param name="lua">lua_State</param>
void lbc_install_searcher(lua_State *lua);
/// <summary>
/// 失效缓存:path 为 NULL 清空全部,否则按路径清除单条(热更新/重载脚本用)。
/// </summary>
/// <param name="path">脚本文件路径或 NULL</param>
void lbc_clear(const char *path);

#endif
#endif//LBYTECACHE_H_
