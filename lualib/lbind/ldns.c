#include "lbind/lpub.h"

/// <summary>
/// 返回配置的 DNS 服务器 IP 地址
/// </summary>
/// <param>无</param>
/// <returns type="string">DNS 服务器 IP</returns>
static int32_t _ldns_ip(lua_State *lua) {
    lua_pushstring(lua, dns_get_ip());
    return 1;
}
/// <summary>
/// 构造 UDP DNS 查询请求包（不含 2 字节长度前缀）
/// </summary>
/// <param name="domain" type="string">查询域名</param>
/// <param name="ipv6" type="integer">1 查询 AAAA 记录，0 查询 A 记录；其余值报错</param>
/// <returns type="string?">DNS 查询二进制字符串；构造失败返回 nil</returns>
/// <returns type="integer?">本次查询的事务 ID，传给 dns.unpack 回验响应；失败时同为 nil，返回值个数恒为 2</returns>
static int32_t _ldns_pack(lua_State *lua) {
    const char *domain = lpub_check_cstr(lua, 1);
    int32_t ipv6 = lpub_check_flag(lua, 2);
    char buf[ONEK];
    uint16_t id;
    size_t lens = (size_t)dns_request_pack(buf, domain, ipv6, &id);
    if (0 == lens) {
        return lpub_rtn_nil(lua, 2);
    }
    lua_pushlstring(lua, buf, lens);
    lua_pushinteger(lua, id);
    return 2;
}
/// <summary>
/// 构造 TCP DNS 查询请求包（含 2 字节大端长度前缀，RFC 1035 §4.2.2 / RFC 7766）
/// </summary>
/// <param name="domain" type="string">查询域名</param>
/// <param name="ipv6" type="integer">1 查询 AAAA 记录，0 查询 A 记录；其余值报错</param>
/// <returns type="string?">含长度前缀的 DNS 查询二进制字符串；构造失败返回 nil</returns>
/// <returns type="integer?">本次查询的事务 ID，传给 dns.unpack 回验响应；失败时同为 nil，返回值个数恒为 2</returns>
static int32_t _ldns_pack_tcp(lua_State *lua) {
    const char *domain = lpub_check_cstr(lua, 1);
    int32_t ipv6 = lpub_check_flag(lua, 2);
    char buf[ONEK];
    uint16_t id;
    size_t lens = dns_request_pack_tcp(buf, domain, ipv6, &id);
    if (0 == lens) {
        return lpub_rtn_nil(lua, 2);
    }
    lua_pushlstring(lua, buf, lens);
    lua_pushinteger(lua, id);
    return 2;
}
/// <summary>
/// 解析 DNS 响应包，提取 IP 地址列表
/// </summary>
/// <param name="pack" type="lightuserdata">DNS 响应数据指针（裸报文，不含 TCP 长度前缀）</param>
/// <param name="packlen" type="integer">响应包字节数，取值 [0, INT32_MAX]</param>
/// <param name="id" type="integer">期望的事务 ID（dns.pack/dns.pack_tcp 返回）；响应事务 ID 不匹配即视为错配/伪造返回 nil</param>
/// <returns type="string[]?">IP 字符串数组；事务 ID 不匹配、不是应答(QR 位为 0)、解析失败、RCODE 非 0 或响应被截断(TC 位置位)时均返回 nil</returns>
/// <returns type="boolean">第二返回值 nodata：true 表示服务端已给出完整答复——没有任何 A/AAAA 记录
/// （NOERROR/NODATA）或名字不存在（NXDOMAIN），换 TCP 重查也是同一结果，调用方不必再试；解析成功时恒为 false</returns>
static int32_t _ldns_unpack(lua_State *lua) {
    LPUB_LUD_ARG(lua, void, 1, pack);
    size_t packlen = lpub_check_lens(lua, 2, INT32_MAX);
    uint16_t id = lpub_check_u16(lua, 3, "transaction id out of range");
    size_t n;
    int32_t nodata = 0;
    dns_ip *ips = dns_parse_pack(pack, packlen, &n, id, &nodata);
    if (NULL == ips) {
        lua_pushnil(lua);
        lua_pushboolean(lua, 0 != nodata ? 1 : 0);
        return 2;
    }
    // 建表压串直接调用：分配失败时分配器不动返回、直接 abort（见 memory.c 的 _mem_oom），
    // 不存在能 longjmp 出去的 LUA_ERRMEM，不必套 pcall
    lua_createtable(lua, (int32_t)n, 0);
    for (size_t i = 0; i < n; i++) {
        lua_pushstring(lua, ips[i].ip);
        lua_rawseti(lua, -2, (lua_Integer)(i + 1));
    }
    FREE(ips);
    lua_pushboolean(lua, 0);
    return 2;
}
//srey.dns
LUAMOD_API int luaopen_dns(lua_State *lua) {
    luaL_Reg reg[] = {
        { "ip", _ldns_ip },
        { "pack", _ldns_pack },
        { "pack_tcp", _ldns_pack_tcp },
        { "unpack", _ldns_unpack },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
