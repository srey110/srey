#include "bench_lbytecache.h"
#include "lib.h"
#if WITH_LUA && ENABLE_LUA_BYTECACHE
#include "lua/lauxlib.h"
#include "lua/lualib.h"
#include "lbind/lbytecache.h"

// 生成脚本的函数个数(越多单次编译开销越大)
#define BENCH_NFUNC 500
// 计时循环加载次数
#define BENCH_LOOPS 5000
// require 用的模块名(对应 procpath 下 <BENCH_MOD>.lua)
#define BENCH_MOD "_lbc_bench"

// 生成含 BENCH_NFUNC 个函数的 Lua 脚本到 path,令编译有可观开销
static void _bench_gen(const char *path) {
    FILE *fp = fopen(path, "wb");
    if (NULL == fp) {
        return;
    }
    fprintf(fp, "local M = {}\n");
    for (int32_t i = 0; i < BENCH_NFUNC; i++) {
        fprintf(fp,
            "function M.f%d(a, b, c) local x = a + b * %d local y = x * x - c "
            "local t = {x, y, a - b, b - c} return x, y, t end\n", i, i);
    }
    fprintf(fp, "return M\n");
    fclose(fp);
}
// 直接 loadfile 循环计时:use_lbc!=0 走 lbc_loadfile(undump),否则 luaL_loadfile(compile);含一次预热
static uint64_t _bench_loadfile(lua_State *lua, const char *path, int32_t use_lbc) {
    int32_t st = (0 != use_lbc) ? lbc_loadfile(lua, path) : luaL_loadfile(lua, path);
    if (LUA_OK == st) {
        lua_pop(lua, 1);
    }
    uint64_t t0 = nowms();
    for (int32_t i = 0; i < BENCH_LOOPS; i++) {
        if (LUA_OK == ((0 != use_lbc) ? lbc_loadfile(lua, path) : luaL_loadfile(lua, path))) {
            lua_pop(lua, 1);
        }
    }
    return nowms() - t0;
}
// 设 package.path 为 dir/?.lua,令 require 能按模块名定位脚本
static void _bench_setpath(lua_State *lua, const char *dir) {
    lua_getglobal(lua, "package");
    lua_pushfstring(lua, "%s%s?.lua", dir, PATH_SEPARATORSTR);
    lua_setfield(lua, -2, "path");
    lua_pop(lua, 1);
}
// require(BENCH_MOD) 一次,随后清 package.loaded[BENCH_MOD] 令下次重新加载
static void _bench_require_once(lua_State *lua) {
    lua_getglobal(lua, "require");
    lua_pushstring(lua, BENCH_MOD);
    lua_pcall(lua, 1, 1, 0);
    lua_pop(lua, 1);
    lua_getglobal(lua, "package");
    lua_getfield(lua, -1, "loaded");
    lua_pushnil(lua);
    lua_setfield(lua, -2, BENCH_MOD);
    lua_pop(lua, 2);
}
// require 路径循环计时(含模块执行);state 须已 openlibs + setpath(+可选 install_searcher);含一次预热
static uint64_t _bench_require(lua_State *lua) {
    _bench_require_once(lua);
    uint64_t t0 = nowms();
    for (int32_t i = 0; i < BENCH_LOOPS; i++) {
        _bench_require_once(lua);
    }
    return nowms() - t0;
}
void bench_lbytecache(void) {
    char dir[PATH_LENS];
    char path[PATH_LENS];
    SNPRINTF(dir, sizeof(dir), "%s", procpath());
    SNPRINTF(path, sizeof(path), "%s%s%s.lua", dir, PATH_SEPARATORSTR, BENCH_MOD);
    _bench_gen(path);

    // 组1:直接 loadfile —— luaL_loadfile(compile) vs lbc_loadfile(undump),同一 state、不执行
    lua_State *l1 = luaL_newstate();
    if (NULL == l1) {
        remove(path);
        return;
    }
    uint64_t compile_ms = _bench_loadfile(l1, path, 0);
    uint64_t undump_ms = _bench_loadfile(l1, path, 1);
    lua_close(l1);

    // 组2:require 路径 —— 标准 searcher(compile) vs lbc_install_searcher(undump),含模块执行
    lua_State *ls = luaL_newstate();
    luaL_openlibs(ls);
    _bench_setpath(ls, dir);
    uint64_t req_std_ms = _bench_require(ls);
    lua_close(ls);

    lua_State *lc = luaL_newstate();
    luaL_openlibs(lc);
    _bench_setpath(lc, dir);
    lbc_install_searcher(lc);
    uint64_t req_lbc_ms = _bench_require(lc);
    lua_close(lc);

    remove(path);

    double lf_speedup = (undump_ms > 0) ? (double)compile_ms / (double)undump_ms : 0.0;
    double rq_speedup = (req_lbc_ms > 0) ? (double)req_std_ms / (double)req_lbc_ms : 0.0;
    LOG_INFO("[bench_lbytecache] nfunc=%d loops=%d", BENCH_NFUNC, BENCH_LOOPS);
    LOG_INFO("[bench_lbytecache] loadfile: luaL(compile)=%llums(%.4fms) lbc(undump)=%llums(%.4fms) speedup=%.2fx",
             (unsigned long long)compile_ms, (double)compile_ms / (double)BENCH_LOOPS,
             (unsigned long long)undump_ms, (double)undump_ms / (double)BENCH_LOOPS, lf_speedup);
    LOG_INFO("[bench_lbytecache] require : std(compile)=%llums(%.4fms) lbc(searcher/undump)=%llums(%.4fms) speedup=%.2fx",
             (unsigned long long)req_std_ms, (double)req_std_ms / (double)BENCH_LOOPS,
             (unsigned long long)req_lbc_ms, (double)req_lbc_ms / (double)BENCH_LOOPS, rq_speedup);
}
#else
void bench_lbytecache(void) {}
#endif
