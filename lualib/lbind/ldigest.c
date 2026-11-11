#include "lbind/lpub.h"

#define MT_DIGEST "_digest_ctx"

/// <summary>
/// 创建摘要（Hash）上下文
/// </summary>
/// <param name="dtype" type="integer">算法类型（MD5 / SHA1 / SHA256 / XXH32 / XXH64 等）</param>
/// <returns type="_digest_ctx">摘要对象</returns>
static int32_t _ldigest_new(lua_State *lua) {
    lua_Integer dtype = luaL_checkinteger(lua, 1);
    luaL_argcheck(lua, dtype >= DG_MD2 && dtype <= DG_XXH64, 1, "invalid digest type");
    digest_ctx *digest = lua_newuserdata(lua, sizeof(digest_ctx));
    digest_init(digest, (digest_type)dtype);
    ASSOC_MTABLE(lua, MT_DIGEST);
    return 1;
}
// digest / hmac 的对象按值存在 userdata 里,没有可置 NULL 的指针,故已释放的判据是 attr 归零。
// __gc 经 __index 也是个普通方法,业务一行 d:__gc() 就能提前释放,此后每个入口都得报可捕获的错
static digest_ctx *_ldigest_check(lua_State *lua) {
    digest_ctx *digest = lpub_check_udata(lua, 1, MT_DIGEST);
    if (NULL == digest->attr) {
        luaL_error(lua, "digest already freed");
    }
    return digest;
}
/// <summary>
/// 返回当前摘要算法的输出长度
/// </summary>
/// <param name="self" type="userdata">摘要对象</param>
/// <returns type="integer">输出字节数</returns>
static int32_t _ldigest_size(lua_State *lua) {
    digest_ctx *digest = _ldigest_check(lua);
    lua_pushinteger(lua, digest_size(digest));
    return 1;
}
/// <summary>
/// 重置摘要上下文，可重新开始计算
/// </summary>
/// <param name="self" type="userdata">摘要对象</param>
/// <returns>无</returns>
static int32_t _ldigest_reset(lua_State *lua) {
    digest_ctx *digest = _ldigest_check(lua);
    digest_reset(digest);
    return 0;
}
/// <summary>
/// 向摘要上下文追加数据
/// </summary>
/// <param name="self" type="userdata">摘要对象</param>
/// <param name="data" type="string|lightuserdata">数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <returns>无</returns>
static int32_t _ldigest_update(lua_State *lua) {
    digest_ctx *digest = _ldigest_check(lua);
    size_t size;
    void *data = lpub_check_buf(lua, 2, &size, NULL);
    digest_update(digest, data, size);
    return 0;
}
/// <summary>
/// 完成摘要计算
/// </summary>
/// <param name="self" type="userdata">摘要对象</param>
/// <returns type="string">原始二进制摘要。算完对象已自动复位，可直接算下一条，不必先 :reset</returns>
static int32_t _ldigest_final(lua_State *lua) {
    digest_ctx *digest = _ldigest_check(lua);
    char out[DG_BLOCK_SIZE];
    size_t lens = digest_final(digest, out);
    lua_pushlstring(lua, out, lens);
    secure_zero(out, sizeof(out));
    return 1;
}
static int32_t _ldigest_gc(lua_State *lua) {
    digest_ctx *digest = lpub_check_udata(lua, 1, MT_DIGEST);
    digest_free(digest);
    return 0;
}
/// <summary>
/// 一次性摘要：上下文在栈上，算完当场擦除，等价于 new + update + final
/// </summary>
/// <param name="dtype" type="integer">算法类型，取值同 digest.new</param>
/// <param name="data" type="string|lightuserdata">数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <returns type="string">原始二进制摘要</returns>
static int32_t _ldigest_sum(lua_State *lua) {
    lua_Integer dtype = luaL_checkinteger(lua, 1);
    luaL_argcheck(lua, dtype >= DG_MD2 && dtype <= DG_XXH64, 1, "invalid digest type");
    size_t size;
    void *data = lpub_check_buf(lua, 2, &size, NULL);
    digest_ctx digest;
    char out[DG_BLOCK_SIZE];
    digest_init(&digest, (digest_type)dtype);
    digest_update(&digest, data, size);
    size_t lens = digest_final(&digest, out);
    digest_free(&digest);
    lua_pushlstring(lua, out, lens);
    secure_zero(out, sizeof(out));
    return 1;
}
//srey.digest
LUAMOD_API int luaopen_digest(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _ldigest_new },
        { "sum", _ldigest_sum },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "size", _ldigest_size },
        { "reset", _ldigest_reset },
        { "update", _ldigest_update },
        { "final", _ldigest_final },
        { "__gc", _ldigest_gc },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_DIGEST, reg_new, reg_func);
    return 1;
}
