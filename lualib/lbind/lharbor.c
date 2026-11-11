#include "lbind/lpub.h"

/// <summary>
/// 打包 harbor 跨节点消息
/// </summary>
/// <param name="task" type="integer">目标 task name</param>
/// <param name="call" type="integer">调用类型：非0=call（单向），0=request（双向）</param>
/// <param name="reqtype" type="integer">业务请求类型，取值 [0, UINT16_MAX]，越界报错</param>
/// <param name="data" type="string|lightuserdata|nil">消息内容；nil 表示无数据</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数，取值 [0, INT32_MAX]</param>
/// <returns type="lightuserdata">打包后的数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lharbor_pack(lua_State *lua) {
    name_t task = (name_t)luaL_checkinteger(lua, 1);
    int32_t call = 0 != luaL_checkinteger(lua, 2);// 非0=call，0=request；按 64 位判，别截成 int32 再判
    subtype_t reqtype = lpub_check_u16(lua, 3, REQTYPE_OUT_OF_RANGE);
    size_t size;
    void *data = lpub_opt_buf(lua, 4, &size);
    data = harbor_pack(task, call, reqtype, data, size, &size);
    return lpub_rtn_lud(lua, data, size);
}
//srey.harbor
LUAMOD_API int luaopen_harbor(lua_State *lua) {
    luaL_Reg reg[] = {
        { "pack", _lharbor_pack },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
