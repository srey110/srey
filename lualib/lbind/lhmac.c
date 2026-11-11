#include "lbind/lpub.h"

#define MT_HMAC "_hmac_ctx"

/// <summary>
/// 创建 HMAC 上下文
/// </summary>
/// <param name="dtype" type="integer">底层 Hash 算法类型（MD5 / SHA1 / SHA256 等）；XXH32 / XXH64 不支持</param>
/// <param name="key" type="string">密钥</param>
/// <returns type="_hmac_ctx">HMAC 对象</returns>
static int32_t _lhmac_new(lua_State *lua) {
    lua_Integer dtype = luaL_checkinteger(lua, 1);
    // 上界停在 SHA512：xxhash 没有分组长度，放进来会打到 hmac_init 的 ASSERTAB 整进程中止
    luaL_argcheck(lua, dtype >= DG_MD2 && dtype <= DG_SHA512, 1, "invalid digest type");
    size_t lens;
    const char *key = luaL_checklstring(lua, 2, &lens);
    hmac_ctx *hmac = lua_newuserdata(lua, sizeof(hmac_ctx));
    hmac_init(hmac, (digest_type)dtype, key, lens);
    ASSOC_MTABLE(lua, MT_HMAC);
    return 1;
}
// 判据同 ldigest.c 的 _ldigest_check:hmac_free 整块 secure_zero,4 个内嵌 digest 的 attr 一起归零
static hmac_ctx *_lhmac_check(lua_State *lua) {
    hmac_ctx *hmac = lpub_check_udata(lua, 1, MT_HMAC);
    if (NULL == hmac->inside.attr) {
        luaL_error(lua, "hmac already freed");
    }
    return hmac;
}
/// <summary>
/// 返回 HMAC 输出长度
/// </summary>
/// <param name="self" type="userdata">HMAC 对象</param>
/// <returns type="integer">输出字节数</returns>
static int32_t _lhmac_size(lua_State *lua) {
    hmac_ctx *hmac = _lhmac_check(lua);
    lua_pushinteger(lua, hmac_size(hmac));
    return 1;
}
/// <summary>
/// 重置 HMAC 上下文，可重新开始计算
/// </summary>
/// <param name="self" type="userdata">HMAC 对象</param>
/// <returns>无</returns>
static int32_t _lhmac_reset(lua_State *lua) {
    hmac_ctx *hmac = _lhmac_check(lua);
    hmac_reset(hmac);
    return 0;
}
/// <summary>
/// 向 HMAC 上下文追加数据
/// </summary>
/// <param name="self" type="userdata">HMAC 对象</param>
/// <param name="data" type="string|lightuserdata">数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <returns>无</returns>
static int32_t _lhmac_update(lua_State *lua) {
    hmac_ctx *hmac = _lhmac_check(lua);
    size_t size;
    void *data = lpub_check_buf(lua, 2, &size, NULL);
    hmac_update(hmac, data, size);
    return 0;
}
/// <summary>
/// 完成 HMAC 计算
/// </summary>
/// <param name="self" type="userdata">HMAC 对象</param>
/// <returns type="string">原始二进制 HMAC 结果。算完对象已自动复位（密钥仍在），可直接算下一条，不必先 :reset</returns>
static int32_t _lhmac_final(lua_State *lua) {
    hmac_ctx *hmac = _lhmac_check(lua);
    char out[DG_BLOCK_SIZE];
    size_t lens = hmac_final(hmac, out);
    lua_pushlstring(lua, out, lens);
    secure_zero(out, sizeof(out));
    return 1;
}
static int32_t _lhmac_gc(lua_State *lua) {
    hmac_ctx *hmac = lpub_check_udata(lua, 1, MT_HMAC);
    hmac_free(hmac);
    return 0;
}
/// <summary>
/// 一次性 HMAC：上下文在栈上，算完当场擦除，等价于 new + update + final
/// </summary>
/// <param name="dtype" type="integer">底层 Hash 算法类型，取值同 hmac.new（XXH32 / XXH64 不支持）</param>
/// <param name="key" type="string">密钥</param>
/// <param name="data" type="string|lightuserdata">数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <returns type="string">原始二进制 HMAC 结果</returns>
static int32_t _lhmac_sum(lua_State *lua) {
    lua_Integer dtype = luaL_checkinteger(lua, 1);
    luaL_argcheck(lua, dtype >= DG_MD2 && dtype <= DG_SHA512, 1, "invalid digest type");
    size_t klens;
    const char *key = luaL_checklstring(lua, 2, &klens);
    size_t size;
    void *data = lpub_check_buf(lua, 3, &size, NULL);
    hmac_ctx hmac;
    char out[DG_BLOCK_SIZE];
    hmac_init(&hmac, (digest_type)dtype, key, klens);
    hmac_update(&hmac, data, size);
    size_t lens = hmac_final(&hmac, out);
    hmac_free(&hmac);
    lua_pushlstring(lua, out, lens);
    secure_zero(out, sizeof(out));
    return 1;
}
//srey.hmac
LUAMOD_API int luaopen_hmac(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lhmac_new },
        { "sum", _lhmac_sum },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "size", _lhmac_size },
        { "reset", _lhmac_reset },
        { "update", _lhmac_update },
        { "final", _lhmac_final },
        { "__gc", _lhmac_gc },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_HMAC, reg_new, reg_func);
    return 1;
}
