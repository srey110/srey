#include "lbind/lpub.h"

/// <summary>
/// 对数据进行 Base64 编码
/// </summary>
/// <param name="data" type="string|lightuserdata">原始数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <returns type="string">Base64 编码后的字符串</returns>
static int32_t _lbase64_encode(lua_State *lua) {
    size_t size;
    void *data = lpub_check_buf(lua, 1, &size, NULL);
    size_t lens = B64EN_SIZE(size);
    luaL_Buffer lbuf;
    char *out = luaL_buffinitsize(lua, &lbuf, lens);
    size = bs64_encode(data, size, out);
    luaL_pushresultsize(&lbuf, size);
    return 1;
}
// 只含 CR / LF：换行折过的空内容
static int32_t _lbase64_only_crlf(const char *data, size_t size) {
    for (size_t i = 0; i < size; i++) {
        if ('\r' != data[i] && '\n' != data[i]) {
            return 0;
        }
    }
    return 1;
}
/// <summary>
/// 对 Base64 编码数据进行解码
/// </summary>
/// <param name="data" type="string|lightuserdata">Base64 数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <returns type="string?">解码后的原始二进制字符串；输入含非法字符或填充被伪造截断时返回 nil。
/// 空输入与只含 CR/LF 的输入合法解出空串，不能拿空串当失败信号</returns>
static int32_t _lbase64_decode(lua_State *lua) {
    size_t size;
    void *data = lpub_check_buf(lua, 1, &size, NULL);
    size_t lens = B64DE_SIZE(size);
    luaL_Buffer lbuf;
    char *out = luaL_buffinitsize(lua, &lbuf, lens);
    size_t declens = bs64_decode(data, size, out);
    // bs64_decode 返 0 时分不出畸形与空内容(见 base64.h)：空输入、只含 CR/LF 的算空串，其余(含只有 '=')算畸形。
    // 不区分的话损坏数据也得到空串，调用方那句 if d then use(d) end 会把它当成合法凭据收下
    int32_t bad = (0 == declens && 0 != size && !_lbase64_only_crlf(data, size));
    // 失败也得先 luaL_pushresultsize 再 pop：luaL_buffinitsize 会在栈上留一个中间对象，
    // 不收就直接压 nil 的话它还压在返回值下面
    luaL_pushresultsize(&lbuf, declens);
    secure_zero(&lbuf, sizeof(lbuf));// 结果小时落在 lbuf 自带的 C 栈缓冲里，交出去后擦掉(同 digest 的 out)
    if (bad) {
        lua_pop(lua, 1);
        lua_pushnil(lua);
    }
    return 1;
}
//srey.base64
LUAMOD_API int luaopen_base64(lua_State *lua) {
    luaL_Reg reg[] = {
        { "encode", _lbase64_encode },
        { "decode", _lbase64_decode },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
