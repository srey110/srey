#include "lbind/lpub.h"

#define MT_LOAD_TREND "_load_trend_ctx"

/// <summary>
/// 创建负载趋势检测器（trend.new）
/// </summary>
/// <returns type="_load_trend_ctx">负载趋势对象</returns>
static int32_t _ltrend_new(lua_State *lua) {
    load_trend_ctx *trend = lua_newuserdata(lua, sizeof(load_trend_ctx));
    load_trend_init(trend);
    ASSOC_MTABLE(lua, MT_LOAD_TREND);
    return 1;
}
/// <summary>
/// 采样当前值并基于趋势判断负载是否繁忙。阈值缺省取 utils/pool.h 的 SHRINK_BUSY，
/// 与 C 侧各池的收缩判据同源——调用方不必也不该再抄一份字面量
/// </summary>
/// <param name="self" type="userdata">负载趋势对象</param>
/// <param name="cur" type="integer">当前采样值，取值 [0, UINT32_MAX]</param>
/// <param name="busy_num" type="integer?">繁忙阈值分子；缺省与 busy_den 一并取 SHRINK_BUSY</param>
/// <param name="busy_den" type="integer?">繁忙阈值分母</param>
/// <returns type="boolean">繁忙 true；不忙 false</returns>
static int32_t _ltrend_busy(lua_State *lua) {
    load_trend_ctx *trend = lpub_check_udata(lua, 1, MT_LOAD_TREND);
    // 采样值不是长度，不走 lpub_check_lens（那句报的是 "length out of range"）；
    // 负数与超 uint32 都会让忙闲判定给出垃圾结论，两头都挡
    size_t cur = (size_t)lpub_check_range(lua, 2, 0, UINT32_MAX, "sample out of range");
    if (lua_isnoneornil(lua, 3)) {
        return lpub_rtn_bool(lua, load_trend_busy(trend, cur, SHRINK_BUSY));
    }
    uint32_t busy_num = lpub_check_u32(lua, 3, "busy_num out of range");
    uint32_t busy_den = lpub_check_u32(lua, 4, "busy_den out of range");
    return lpub_rtn_bool(lua, load_trend_busy(trend, cur, busy_num, busy_den));
}
//srey.trend
LUAMOD_API int luaopen_trend(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _ltrend_new },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "busy", _ltrend_busy },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_LOAD_TREND, reg_new, reg_func);
    /// <field name="shrink_time" type="integer">池收缩检测周期（毫秒），即 utils/pool.h 的 SHRINK_TIME</field>
    lua_pushinteger(lua, (lua_Integer)SHRINK_TIME);
    lua_setfield(lua, -2, "shrink_time");
    return 1;
}
