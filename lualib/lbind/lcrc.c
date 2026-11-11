#include "lbind/lpub.h"

/// <summary>
/// 计算数据的 CRC16 校验值（CRC-16/ARC：反射、多项式 0x8005、初值 0、无输出异或）
/// </summary>
/// <param name="data" type="string|lightuserdata">数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <returns type="integer">CRC16 校验值（uint16）</returns>
static int32_t _lcrc_crc16(lua_State *lua) {
    size_t size;
    void *data = lpub_check_buf(lua, 1, &size, NULL);
    uint16_t crc = crc16(data, size);
    lua_pushinteger(lua, crc);
    return 1;
}
/// <summary>
/// 计算数据的 CRC32 校验值
/// </summary>
/// <param name="data" type="string|lightuserdata">数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <returns type="integer">CRC32 校验值（uint32）</returns>
static int32_t _lcrc_crc32(lua_State *lua) {
    size_t size;
    void *data = lpub_check_buf(lua, 1, &size, NULL);
    uint32_t crc = crc32(data, size);
    lua_pushinteger(lua, crc);
    return 1;
}
//srey.crc
LUAMOD_API int luaopen_crc(lua_State *lua) {
    luaL_Reg reg[] = {
        { "crc16", _lcrc_crc16 },
        { "crc32", _lcrc_crc32 },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
