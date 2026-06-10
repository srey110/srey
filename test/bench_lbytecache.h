#ifndef BENCH_LBYTECACHE_H_
#define BENCH_LBYTECACHE_H_

/// <summary>
/// Lua 字节码缓存加载性能基准:循环加载一个大脚本,对比加载耗时。
/// ENABLE_LUA_BYTECACHE 开 → lbc_loadfile(首次编译入缓存,之后命中走 undump);
/// 关 → luaL_loadfile(每次重新编译)。结果经 LOG_INFO 输出。
/// 须在 hug_wait 前调用(loader / lbc 已就绪)。
/// </summary>
void bench_lbytecache(void);

#endif//BENCH_LBYTECACHE_H_
