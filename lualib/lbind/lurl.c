#include "lbind/lpub.h"

/// <summary>
/// 对数据进行 URL 编码
/// </summary>
/// <param name="data" type="string|lightuserdata">原始数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数；data 为字符串时不得传它，
/// 会占掉后面开关的位置，开关之后再有实参报错</param>
/// <param name="space2plus" type="integer?">非 0(默认):空格编码为 '+'(form-urlencoded)；0:空格编码为 %20(RFC 3986)</param>
/// <returns type="string">URL 编码后的字符串</returns>
static int32_t _lurl_encode(lua_State *lua) {
    size_t size;
    int32_t idx = 1;
    void *data = lpub_check_buf_idx(lua, &idx, &size, NULL);
    luaL_argcheck(lua, size <= (SIZE_MAX - 1) / 3, 1, "data too large to url encode");// 32 位上 3*size+1 会回绕
    int32_t space2plus = 0 != luaL_optinteger(lua, idx, 1);
    // 字符串 data 只占 1 格，多传的 size 会把开关顶到下一格，故开关之后再有实参就报错
    luaL_argcheck(lua, lua_isnoneornil(lua, idx + 1), idx + 1, "too many arguments");
    luaL_Buffer lbuf;
    char *out = luaL_buffinitsize(lua, &lbuf, URLEN_SIZE(size));
    luaL_pushresultsize(&lbuf, url_encode(data, size, out, space2plus));
    return 1;
}
/// <summary>
/// 对 URL 编码的数据进行解码（二进制安全）
/// </summary>
/// <param name="data" type="string|lightuserdata">URL 编码数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数；data 为字符串时不得传它，
/// 会占掉后面开关的位置，开关之后再有实参报错</param>
/// <param name="plus2space" type="integer?">非 0(默认):'+' 解码为空格(form-urlencoded)；0:'+' 保持字面(RFC 3986)</param>
/// <returns type="string">解码后的原始字符串（可含 \0）</returns>
static int32_t _lurl_decode(lua_State *lua) {
    size_t size;
    int32_t idx = 1;
    void *data = lpub_check_buf_idx(lua, &idx, &size, NULL);
    int32_t plus2space = 0 != luaL_optinteger(lua, idx, 1);
    luaL_argcheck(lua, lua_isnoneornil(lua, idx + 1), idx + 1, "too many arguments");// 同 _lurl_encode
    luaL_Buffer lbuf;
    char *out = luaL_buffinitsize(lua, &lbuf, size);
    if (0 != size) {
        memcpy(out, data, size);
    }
    size_t decoded = url_decode(out, size, plus2space);
    luaL_pushresultsize(&lbuf, decoded);
    return 1;
}
/// <summary>
/// 解析 URL 字符串
/// </summary>
/// <param name="data" type="string|lightuserdata">URL 字符串；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数；data 为字符串时不得传它，
/// 会占掉后面开关的位置，开关之后再有实参报错</param>
/// <param name="decode" type="boolean|integer?">true/非 0（默认 true）：对 path 段与 query 做 URL 解码；false/0：保留原始 percent 编码，适合后续用于 HTTP 重组</param>
/// <returns type="ParsedURL?">成功返回含 scheme/user/psw/host/port/path/query/anchor/param/segs 的表（path/query 语义随 decode 参数而定）；
/// 路径段数超 URL_MAX_PATH_DEPTH、非空查询参数超过 URL_MAX_PARAM（64）个时返回 nil；decode 模式下 URL 长度达到 1KB（URL_BUF_LENS）同样返回 nil，不解码时不限长</returns>
static int32_t _lurl_parse(lua_State *lua) {
    size_t size;
    int32_t idx = 1;
    void *data = lpub_check_buf_idx(lua, &idx, &size, NULL);
    int32_t decode = (LUA_TBOOLEAN == lua_type(lua, idx)) ? lua_toboolean(lua, idx)
                   : (LUA_TNUMBER == lua_type(lua, idx)) ? (0 != lua_tointeger(lua, idx))
                   : 1;
    luaL_argcheck(lua, lua_isnoneornil(lua, idx + 1), idx + 1, "too many arguments");// 同 _lurl_encode
    url_ctx url;
    if (ERR_OK != url_parse(&url, (const char *)data, size, '/', 0 != decode ? URL_DECODE : 0)) {
        return lpub_rtn_nil(lua, 1);
    }
    lpub_push_url_table(lua, &url);
    return 1;
}
//srey.url
LUAMOD_API int luaopen_url(lua_State *lua) {
    luaL_Reg reg[] = {
        { "encode", _lurl_encode },
        { "decode", _lurl_decode },
        { "parse", _lurl_parse },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
