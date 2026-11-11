#include "lbind/lpub.h"

#define LOGLV_OUT_OF_RANGE "log level out of range (0-4)"

/// <summary>
/// 设置全局日志输出等级
/// </summary>
/// <param name="lv" type="integer">日志等级，取值 [0, 4]（LOGLV_FATAL..LOGLV_DEBUG），越界报错</param>
/// <returns>无</returns>
static int32_t _llog_setlv(lua_State *lua) {
    log_level lv = (log_level)lpub_check_range(lua, 1, LOGLV_FATAL, LOGLV_DEBUG, LOGLV_OUT_OF_RANGE);
    log_setlv(lv);
    return 0;
}
/// <summary>
/// 获取当前全局日志输出等级
/// </summary>
/// <param>无</param>
/// <returns type="integer">当前日志等级，参考 log_level</returns>
static int32_t _llog_getlv(lua_State *lua) {
    lua_pushinteger(lua, (lua_Integer)log_getlv());
    return 1;
}
/// <summary>
/// 输出一条日志，自动附带调用位置（文件名、行号）、task 名称（若存在）。
/// 调用位置由本函数按栈层号自己取，不经 debug.getinfo 建表
/// </summary>
/// <param name="lv" type="integer">日志等级，取值 [0, 4]（LOGLV_FATAL..LOGLV_DEBUG），越界报错</param>
/// <param name="level" type="integer">调用位置所在的栈层，含义同 debug.getinfo 的层号（0 为本函数自身）；该层不存在时不输出，超出 int32 范围报错</param>
/// <param name="log" type="string">日志正文</param>
/// <returns>无</returns>
static int32_t _llog_log(lua_State *lua) {
    log_level lv = (log_level)lpub_check_range(lua, 1, LOGLV_FATAL, LOGLV_DEBUG, LOGLV_OUT_OF_RANGE);
    int32_t level = lpub_check_i32(lua, 2, "level out of range");
    size_t mlen;
    const char *log = luaL_checklstring(lua, 3, &mlen);
    lua_Debug ar;
    if (0 == lua_getstack(lua, level, &ar)
        || 0 == lua_getinfo(lua, "Sl", &ar)) {
        return 0;
    }
    task_ctx *task = lpub_cur_task(lua);
    const char *fname = _filename(ar.short_src);
    char num[24];
    size_t nlen;
    const char *nstr = lpub_int_str(num, sizeof(num), ar.currentline, &nlen);
    if (NULL == task) {
        const char *parts[] = { "[", fname, " ", nstr, "] ", log };
        size_t lens[] = { 1, strlen(fname), 1, nlen, 2, mlen };
        slog_parts(lv, parts, lens, 6);
    } else {
        const char *tname = _NAME_OR(task->name);
        const char *parts[] = { "[", fname, " ", nstr, "][", tname, "] ", log };
        size_t lens[] = { 1, strlen(fname), 1, nlen, 2, strlen(tname), 2, mlen };
        slog_parts(lv, parts, lens, 8);
    }
    return 0;
}
//srey.log
LUAMOD_API int luaopen_log(lua_State *lua) {
    luaL_Reg reg[] = {
        { "setlv", _llog_setlv },
        { "getlv", _llog_getlv },
        { "log", _llog_log },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
