#include "lbind/lpub.h"

/// <summary>
/// 将 C userdata 指针按指定长度转换为 Lua 字符串
/// </summary>
/// <param name="data" type="lightuserdata?">C 指针；为 nil 时函数返回 nil</param>
/// <param name="size" type="integer">数据字节数，取值 [0, INT32_MAX]</param>
/// <returns type="string?">转换后的字符串；data 为 nil，或为空指针而 size 大于 0 时返回 nil</returns>
static int32_t _lutils_ud_str(lua_State *lua) {
    int32_t type = lua_type(lua, 1);
    if (LUA_TNIL == type || LUA_TNONE == type) {
        return lpub_rtn_nil(lua, 1);
    }
    LUACHECK_LUDATA_OPT(lua, 1);
    void *data = lua_touserdata(lua, 1);
    size_t size = lpub_check_lens(lua, 2, INT32_MAX);
    if (NULL == data && size > 0) {
        return lpub_rtn_nil(lua, 1);
    }
    lua_pushlstring(lua, data, size);
    return 1;
}
/// <summary>
/// 释放 C 堆分配的 light userdata 指针；非 light userdata 时不操作（full userdata 由 GC 管理）
/// </summary>
/// <param name="data" type="lightuserdata">待释放的 C 堆指针</param>
/// <returns>无</returns>
static int32_t _lutils_ud_free(lua_State *lua) {
    //只释放 light userdata；full userdata 由 GC 管理，不得手动 free，否则 double-free 崩溃
    if (!lua_islightuserdata(lua, 1)) {
        return 0;
    }
    void *data = lua_touserdata(lua, 1);
    FREE(data);
    return 0;
}
/// <summary>
/// 将二进制数据编码为十六进制字符串
/// </summary>
/// <param name="data" type="string|lightuserdata">待编码数据；为字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数；
/// data 为字符串时不得传它——lower 紧跟在 data 之后，多传一个会占掉 lower 的位置，传了报错</param>
/// <param name="lower" type="boolean?">true 输出小写；缺省或 false 输出大写</param>
/// <returns type="string">十六进制字符串</returns>
static int32_t _lutils_hex(lua_State *lua) {
    size_t size;
    int32_t idx = 1;
    // data 为字符串时只占 1 个栈位,为 lightuserdata 时占 2 个(data+size);
    // lower 是可变位置的下一个参数,须用 _idx 版本取推进后的准确栈位,不能固定写死
    void *data = lpub_check_buf_idx(lua, &idx, &size, NULL);
    // 卡死类型:字符串多传一个 size 就把它顶到 lower 位上,而 Lua 里任何数字都为真,
    // 结果是"缺省该大写"的调用悄悄输出小写
    luaL_argcheck(lua, lua_isnoneornil(lua, idx) || lua_isboolean(lua, idx), idx, "boolean expected");
    int32_t lower = lua_toboolean(lua, idx);
    luaL_Buffer lbuf;
    char *out = luaL_buffinitsize(lua, &lbuf, HEX_ENSIZE(size));
    tohex(data, size, out, lower);
    luaL_pushresultsize(&lbuf, size * 2);
    return 1;
}
/// <summary>
/// 生成全局唯一 id（基于 createid 的 64 位整数；同一线程内递增，跨线程不保证先后）
/// </summary>
/// <param>无</param>
/// <returns type="integer">全局唯一 id</returns>
static int32_t _lutils_id(lua_State *lua) {
    lua_pushinteger(lua, createid());
    return 1;
}
/// <summary>
/// 从 createid 生成的 ID 中解析出服务器 id（高 16 位）
/// </summary>
/// <param name="id" type="integer">createid 返回的 ID</param>
/// <returns type="integer">服务器 id（与 serviceid 设置值一致，范围 0..0x7FFF）</returns>
static int32_t _lutils_parse_svid(lua_State *lua) {
    uint64_t id = (uint64_t)luaL_checkinteger(lua, 1);
    lua_pushinteger(lua, parse_svid(id));
    return 1;
}
/// <summary>
/// 生成 N 字节密码学安全随机数据（Windows BCryptGenRandom / macOS 与 BSD arc4random / Linux getrandom，被拦或内核不支持时读 /dev/urandom / 其余 Unix /dev/urandom）
/// </summary>
/// <param name="lens" type="integer">随机字节数，取值 [0, INT32_MAX]</param>
/// <returns type="string?">随机字节字符串；失败返回 nil</returns>
static int32_t _lutils_csprng_rand(lua_State *lua) {
    size_t n = lpub_check_lens(lua, 1, INT32_MAX);
    if (0 == n) {
        lua_pushlstring(lua, "", 0);
        return 1;
    }
    luaL_Buffer lbuf;
    char *buf = luaL_buffinitsize(lua, &lbuf, n);
    int32_t bad = (ERR_OK != csprng_rand(buf, n));
    luaL_pushresultsize(&lbuf, bad ? 0 : n);
    secure_zero(&lbuf, sizeof(lbuf));// 理由同 lbase64.c 的 _lbase64_decode
    if (bad) {
        lua_pop(lua, 1);
        return lpub_rtn_nil(lua, 1);
    }
    return 1;
}
/// <summary>
/// 把 on_accepted / on_recvedfrom 交出的对端地址转成 IP 与端口。地址是网络线程收到时当场取的，不会因 fd 复用张冠李戴；
/// 只在交出它的那个回调期间有效，要留就当场转好自己存
/// </summary>
/// <param name="addr" type="lightuserdata?">on_accepted 的第 3 个参数(listen 没订阅 NET_EV.PEERADDR 或取失败时为 nil)，
/// 或 on_recvedfrom 的第 3 个参数</param>
/// <returns type="string?">对端 IP，IPv6 链路本地地址带 "%网卡号"；addr 为 nil 时返回 nil</returns>
/// <returns type="integer?">对端端口；仅在第一个返回值非 nil 时有效</returns>
static int32_t _lutils_remote_addr(lua_State *lua) {
    if (lua_isnoneornil(lua, 1)) {
        return lpub_rtn_nil(lua, 2);
    }
    LUACHECK_LUDATA(lua, 1);
    netaddr_ctx *addr = lua_touserdata(lua, 1);
    char ip[IP_LENS];
    if (ERR_OK != netaddr_ip(addr, ip)) {
        return lpub_rtn_nil(lua, 2);
    }
    uint16_t port = netaddr_port(addr);
    lua_pushstring(lua, ip);
    lua_pushinteger(lua, port);
    return 2;
}
//srey.utils
LUAMOD_API int luaopen_utils(lua_State *lua) {
    luaL_Reg reg[] = {
        { "ud_free", _lutils_ud_free },
        { "ud_str", _lutils_ud_str },
        { "hex", _lutils_hex },
        { "id", _lutils_id },
        { "parse_svid", _lutils_parse_svid },
        { "csprng_rand", _lutils_csprng_rand },
        { "remote_addr", _lutils_remote_addr },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
