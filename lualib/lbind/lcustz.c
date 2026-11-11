#include "lbind/lpub.h"

/// <summary>
/// 打包自定义协议（custz）数据
/// </summary>
/// <param name="pktype" type="integer">协议子类型，只接受 PACK_CUSTZ_FIXED / FLAG / VAR，其余报错</param>
/// <param name="data" type="string|lightuserdata">载荷数据；字符串时长度自动取得。空载荷报错：接收侧把零长包当协议错误断开</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数，须大于 0</param>
/// <returns type="lightuserdata?">打包后的数据指针；载荷超出所选头部能表达的范围时返回 nil</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lcustz_pack(lua_State *lua) {
    // 只认三个 custz 子类型：custz_pack 的 default 是 ASSERTAB(0)，传别的进去当场 abort 整个进程
    pack_type pktype = lpub_check_pktype(lua, 1);
    luaL_argcheck(lua, PACK_CUSTZ_FIXED == pktype
                  || PACK_CUSTZ_FLAG == pktype
                  || PACK_CUSTZ_VAR == pktype,
                  1, "expected a PACK_CUSTZ_* pack type");
    size_t size;
    void *data = lpub_check_buf(lua, 2, &size, NULL);
    luaL_argcheck(lua, 0 != size, 2, "empty payload");
    size_t lens;
    void *pack = custz_pack(pktype, data, size, &lens);
    return lpub_rtn_lud(lua, pack, lens);
}
//srey.custz
LUAMOD_API int luaopen_custz(lua_State *lua) {
    luaL_Reg reg[] = {
        { "pack", _lcustz_pack },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
