#include "lbind/lpub.h"

#define MT_DIGEST "_digest_ctx"
#define MT_HMAC   "_hmac_ctx"
#define MT_CIPHER "_cipher_ctx"

/// <summary>
/// 对数据进行 URL 编码
/// </summary>
/// <param name="data" type="string|lightuserdata">原始数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <param name="space2plus" type="integer?">非 0(默认):空格编码为 '+'(form-urlencoded)；0:空格编码为 %20(RFC 3986)</param>
/// <returns type="string">URL 编码后的字符串</returns>
static int32_t _lcrypt_url_encode(lua_State *lua) {
    void *data;
    size_t size;
    int32_t idx = 1;
    data = lpub_check_buf_idx(lua, &idx, &size, NULL);
    int32_t space2plus = (int32_t)luaL_optinteger(lua, idx, 1);
    luaL_Buffer lbuf;
    char *out = luaL_buffinitsize(lua, &lbuf, URLEN_SIZE(size));
    url_encode(data, size, out, space2plus);
    luaL_pushresultsize(&lbuf, strlen(out));
    return 1;
}
/// <summary>
/// 对 URL 编码的数据进行解码（二进制安全）
/// </summary>
/// <param name="data" type="string|lightuserdata">URL 编码数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <param name="plus2space" type="integer?">非 0(默认):'+' 解码为空格(form-urlencoded)；0:'+' 保持字面(RFC 3986)</param>
/// <returns type="string">解码后的原始字符串（可含 \0）</returns>
static int32_t _lcrypt_url_decode(lua_State *lua) {
    void *data;
    size_t size;
    int32_t idx = 1;
    data = lpub_check_buf_idx(lua, &idx, &size, NULL);
    int32_t plus2space = (int32_t)luaL_optinteger(lua, idx, 1);
    luaL_Buffer lbuf;
    char *out = luaL_buffinitsize(lua, &lbuf, size);
    memcpy(out, data, size);
    size_t decoded = url_decode(out, size, plus2space);
    luaL_pushresultsize(&lbuf, decoded);
    return 1;
}
/// <summary>
/// 解析 URL 字符串
/// </summary>
/// <param name="data" type="string|lightuserdata">URL 字符串；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <param name="decode" type="boolean|integer?">true/非 0（默认 true）：对 path 段与 query 做 URL 解码；false/0：保留原始 percent 编码，适合后续用于 HTTP 重组</param>
/// <returns type="ParsedURL?">成功返回含 scheme/user/psw/host/port/path/query/anchor/param/segs 的表（path/query 语义随 decode 参数而定）；URL 超 1KB 或路径段数超 URL_MAX_PATH_DEPTH 时返回 nil</returns>
static int32_t _lcrypt_url_parse(lua_State *lua) {
    void *data;
    size_t size;
    int32_t idx = 1;
    data = lpub_check_buf_idx(lua, &idx, &size, NULL);
    int32_t decode = (LUA_TBOOLEAN == lua_type(lua, idx)) ? lua_toboolean(lua, idx)
                   : (LUA_TNUMBER == lua_type(lua, idx)) ? (int32_t)lua_tointeger(lua, idx)
                   : 1;
    url_ctx url;
    if (ERR_OK != url_parse(&url, (const char *)data, size, '/', decode)) {
        lua_pushnil(lua);
        return 1;
    }
    lpub_push_url_table(lua, &url);
    return 1;
}
//srey.url
LUAMOD_API int luaopen_url(lua_State *lua) {
    luaL_Reg reg[] = {
        { "encode", _lcrypt_url_encode },
        { "decode", _lcrypt_url_decode },
        { "parse", _lcrypt_url_parse },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
/// <summary>
/// 对数据进行 Base64 编码
/// </summary>
/// <param name="data" type="string|lightuserdata">原始数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <returns type="string">Base64 编码后的字符串</returns>
static int32_t _lcrypt_bs64_encode(lua_State *lua) {
    void *data;
    size_t size;
    data = lpub_check_buf(lua, 1, &size, NULL);
    size_t lens = B64EN_SIZE(size);
    luaL_Buffer lbuf;
    char *out = luaL_buffinitsize(lua, &lbuf, lens);
    size = bs64_encode(data, size, out);
    luaL_pushresultsize(&lbuf, size);
    return 1;
}
/// <summary>
/// 对 Base64 编码数据进行解码
/// </summary>
/// <param name="data" type="string|lightuserdata">Base64 数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <returns type="string">解码后的原始二进制字符串</returns>
static int32_t _lcrypt_bs64_decode(lua_State *lua) {
    void *data;
    size_t size;
    data = lpub_check_buf(lua, 1, &size, NULL);
    size_t lens = B64DE_SIZE(size);
    luaL_Buffer lbuf;
    char *out = luaL_buffinitsize(lua, &lbuf, lens);
    size = bs64_decode(data, size, out);
    luaL_pushresultsize(&lbuf, size);
    return 1;
}
//srey.base64
LUAMOD_API int luaopen_base64(lua_State *lua) {
    luaL_Reg reg[] = {
        { "encode", _lcrypt_bs64_encode },
        { "decode", _lcrypt_bs64_decode },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
/// <summary>
/// 计算数据的 CRC16 校验值
/// </summary>
/// <param name="data" type="string|lightuserdata">数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <returns type="integer">CRC16 校验值（uint16）</returns>
static int32_t _lcrypt_crc16(lua_State *lua) {
    void *data;
    size_t size;
    data = lpub_check_buf(lua, 1, &size, NULL);
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
static int32_t _lcrypt_crc32(lua_State *lua) {
    void *data;
    size_t size;
    data = lpub_check_buf(lua, 1, &size, NULL);
    uint32_t crc = crc32(data, size);
    lua_pushinteger(lua, crc);
    return 1;
}
//srey.crc
LUAMOD_API int luaopen_crc(lua_State *lua) {
    luaL_Reg reg[] = {
        { "crc16", _lcrypt_crc16 },
        { "crc32", _lcrypt_crc32 },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
/// <summary>
/// 创建摘要（Hash）上下文
/// </summary>
/// <param name="dtype" type="integer">算法类型（MD5 / SHA1 / SHA256 等）</param>
/// <returns type="_digest_ctx">摘要对象</returns>
static int32_t _lcrypt_digest_new(lua_State *lua) {
    int32_t dtype = (int32_t)luaL_checkinteger(lua, 1);
    digest_ctx *digest = lua_newuserdata(lua, sizeof(digest_ctx));
    digest_init(digest, dtype);
    ASSOC_MTABLE(lua, MT_DIGEST);
    return 1;
}
/// <summary>
/// 返回当前摘要算法的输出长度
/// </summary>
/// <param name="self" type="userdata">摘要对象</param>
/// <returns type="integer">输出字节数</returns>
static int32_t _lcrypt_digest_size(lua_State *lua) {
    digest_ctx *digest = luaL_checkudata(lua, 1, MT_DIGEST);
    lua_pushinteger(lua, digest_size(digest));
    return 1;
}
/// <summary>
/// 重置摘要上下文，可重新开始计算
/// </summary>
/// <param name="self" type="userdata">摘要对象</param>
/// <returns>无</returns>
static int32_t _lcrypt_digest_reset(lua_State *lua) {
    digest_ctx *digest = luaL_checkudata(lua, 1, MT_DIGEST);
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
static int32_t _lcrypt_digest_update(lua_State *lua) {
    digest_ctx *digest = luaL_checkudata(lua, 1, MT_DIGEST);
    void *data;
    size_t size;
    data = lpub_check_buf(lua, 2, &size, NULL);
    digest_update(digest, data, size);
    return 0;
}
/// <summary>
/// 完成摘要计算
/// </summary>
/// <param name="self" type="userdata">摘要对象</param>
/// <returns type="string">原始二进制摘要</returns>
static int32_t _lcrypt_digest_final(lua_State *lua) {
    digest_ctx *digest = luaL_checkudata(lua, 1, MT_DIGEST);
    char out[DG_BLOCK_SIZE];
    size_t lens = digest_final(digest, out);
    lua_pushlstring(lua, out, lens);
    secure_zero(out, sizeof(out));
    return 1;
}
static int32_t _lcrypt_digest_gc(lua_State *lua) {
    digest_ctx *digest = luaL_checkudata(lua, 1, MT_DIGEST);
    secure_zero(digest, sizeof(digest_ctx));
    return 0;
}
//srey.digest
LUAMOD_API int luaopen_digest(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lcrypt_digest_new },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "size", _lcrypt_digest_size },
        { "reset", _lcrypt_digest_reset },
        { "update", _lcrypt_digest_update },
        { "final", _lcrypt_digest_final },
        { "__gc", _lcrypt_digest_gc },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_DIGEST, reg_new, reg_func);
    return 1;
}
/// <summary>
/// 创建 HMAC 上下文
/// </summary>
/// <param name="dtype" type="integer">底层 Hash 算法类型（MD5 / SHA1 / SHA256 等）</param>
/// <param name="key" type="string">密钥</param>
/// <returns type="_hmac_ctx">HMAC 对象</returns>
static int32_t _lcrypt_hmac_new(lua_State *lua) {
    size_t lens;
    int32_t dtype = (int32_t)luaL_checkinteger(lua, 1);
    const char *key = luaL_checklstring(lua, 2, &lens);
    hmac_ctx *hmac = lua_newuserdata(lua, sizeof(hmac_ctx));
    hmac_init(hmac, dtype, key, lens);
    ASSOC_MTABLE(lua, MT_HMAC);
    return 1;
}
/// <summary>
/// 返回 HMAC 输出长度
/// </summary>
/// <param name="self" type="userdata">HMAC 对象</param>
/// <returns type="integer">输出字节数</returns>
static int32_t _lcrypt_hmac_size(lua_State *lua) {
    hmac_ctx *hmac = luaL_checkudata(lua, 1, MT_HMAC);
    lua_pushinteger(lua, hmac_size(hmac));
    return 1;
}
/// <summary>
/// 重置 HMAC 上下文，可重新开始计算
/// </summary>
/// <param name="self" type="userdata">HMAC 对象</param>
/// <returns>无</returns>
static int32_t _lcrypt_hmac_reset(lua_State *lua) {
    hmac_ctx *hmac = luaL_checkudata(lua, 1, MT_HMAC);
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
static int32_t _lcrypt_hmac_update(lua_State *lua) {
    hmac_ctx *hmac = luaL_checkudata(lua, 1, MT_HMAC);
    void *data;
    size_t size;
    data = lpub_check_buf(lua, 2, &size, NULL);
    hmac_update(hmac, data, size);
    return 0;
}
/// <summary>
/// 完成 HMAC 计算
/// </summary>
/// <param name="self" type="userdata">HMAC 对象</param>
/// <returns type="string">原始二进制 HMAC 结果</returns>
static int32_t _lcrypt_hmac_final(lua_State *lua) {
    hmac_ctx *hmac = luaL_checkudata(lua, 1, MT_HMAC);
    char out[DG_BLOCK_SIZE];
    size_t lens = hmac_final(hmac, out);
    lua_pushlstring(lua, out, lens);
    secure_zero(out, sizeof(out));
    return 1;
}
static int32_t _lcrypt_hmac_gc(lua_State *lua) {
    hmac_ctx *hmac = luaL_checkudata(lua, 1, MT_HMAC);
    hmac_free(hmac);
    return 0;
}
//srey.hmac
LUAMOD_API int luaopen_hmac(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lcrypt_hmac_new },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "size", _lcrypt_hmac_size },
        { "reset", _lcrypt_hmac_reset },
        { "update", _lcrypt_hmac_update },
        { "final", _lcrypt_hmac_final },
        { "__gc", _lcrypt_hmac_gc },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_HMAC, reg_new, reg_func);
    return 1;
}
//srey.cipher
/// <summary>
/// 创建对称加解密上下文
/// </summary>
/// <param name="engine" type="integer">算法引擎（AES / DES 等）</param>
/// <param name="model" type="integer">工作模式（ECB / CBC / CFB / OFB / CTR 等）</param>
/// <param name="key" type="string">密钥</param>
/// <param name="keybits" type="integer">密钥位数（128 / 192 / 256 等）</param>
/// <param name="encrypt" type="integer">1 加密，0 解密</param>
/// <returns type="_cipher_ctx">cipher 对象</returns>
static int32_t _lcrypt_cipher_new(lua_State *lua) {
    size_t lens;
    int32_t engine = (int32_t)luaL_checkinteger(lua, 1);
    int32_t model = (int32_t)luaL_checkinteger(lua, 2);
    const char *key = luaL_checklstring(lua, 3, &lens);
    int32_t keybits = (int32_t)luaL_checkinteger(lua, 4);
    int32_t encrypt = (int32_t)luaL_checkinteger(lua, 5);
    cipher_ctx *cipher = lua_newuserdata(lua, sizeof(cipher_ctx));
    cipher_init(cipher, engine, model, key, lens, keybits, encrypt);
    ASSOC_MTABLE(lua, MT_CIPHER);
    return 1;
}
/// <summary>
/// 返回加解密分块大小
/// </summary>
/// <param name="self" type="userdata">cipher 对象</param>
/// <returns type="integer">分块字节数</returns>
static int32_t _lcrypt_cipher_size(lua_State *lua) {
    cipher_ctx *cipher = luaL_checkudata(lua, 1, MT_CIPHER);
    lua_pushinteger(lua, cipher_size(cipher));
    return 1;
}
/// <summary>
/// 设置填充模式
/// </summary>
/// <param name="self" type="userdata">cipher 对象</param>
/// <param name="padding" type="integer">填充模式（PKCS7 / ANSIX923 / ISO10126 / NOPAD 等）</param>
/// <returns>无</returns>
static int32_t _lcrypt_cipher_padding(lua_State *lua) {
    cipher_ctx *cipher = luaL_checkudata(lua, 1, MT_CIPHER);
    int32_t padding = (int32_t)luaL_checkinteger(lua, 2);
    cipher_padding(cipher, padding);
    return 0;
}
/// <summary>
/// 设置初始化向量（IV）
/// </summary>
/// <param name="self" type="userdata">cipher 对象</param>
/// <param name="iv" type="string">初始化向量</param>
/// <returns>无</returns>
static int32_t _lcrypt_cipher_iv(lua_State *lua) {
    cipher_ctx *cipher = luaL_checkudata(lua, 1, MT_CIPHER);
    size_t lens;
    const char *iv = luaL_checklstring(lua, 2, &lens);
    cipher_iv(cipher, iv, lens);
    return 0;
}
/// <summary>
/// 重置加解密上下文状态，可重新使用
/// </summary>
/// <param name="self" type="userdata">cipher 对象</param>
/// <returns>无</returns>
static int32_t _lcrypt_cipher_reset(lua_State *lua) {
    cipher_ctx *cipher = luaL_checkudata(lua, 1, MT_CIPHER);
    cipher_reset(cipher);
    return 0;
}
/// <summary>
/// 对整块数据进行加解密（不做最终 padding 处理）
/// </summary>
/// <param name="self" type="userdata">cipher 对象</param>
/// <param name="data" type="string|lightuserdata">数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <returns type="string?">加解密结果；长度不匹配 / 模式约束不满足时返回 nil</returns>
static int32_t _lcrypt_cipher_block(lua_State *lua) {
    cipher_ctx *cipher = luaL_checkudata(lua, 1, MT_CIPHER);
    void *data;
    size_t size;
    data = lpub_check_buf(lua, 2, &size, NULL);
    data = cipher_block(cipher, data, size, &size);
    //cipher_block 在长度不匹配 / 模式约束不满足时返回 NULL，需守卫避免 lua_pushlstring(NULL, n) 的 UB
    if (NULL == data) {
        lua_pushnil(lua);
        return 1;
    }
    lua_pushlstring(lua, (const char *)data, size);
    return 1;
}
/// <summary>
/// 完成加解密并处理最终 padding
/// </summary>
/// <param name="self" type="userdata">cipher 对象</param>
/// <param name="data" type="string|lightuserdata">数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <returns type="string">加解密最终结果</returns>
static int32_t _lcrypt_cipher_dofinal(lua_State *lua) {
    cipher_ctx *cipher = luaL_checkudata(lua, 1, MT_CIPHER);
    void *data;
    size_t size;
    data = lpub_check_buf(lua, 2, &size, NULL);
    size_t outlen = size + cipher_size(cipher);
    luaL_Buffer lbuf;
    char *out = luaL_buffinitsize(lua, &lbuf, outlen);
    size = cipher_dofinal(cipher, data, size, out);
    luaL_pushresultsize(&lbuf, size);
    return 1;
}
static int32_t _lcrypt_cipher_gc(lua_State *lua) {
    cipher_ctx *cipher = luaL_checkudata(lua, 1, MT_CIPHER);
    cipher_free(cipher);
    return 0;
}
LUAMOD_API int luaopen_cipher(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lcrypt_cipher_new },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "size", _lcrypt_cipher_size },
        { "padding", _lcrypt_cipher_padding },
        { "iv", _lcrypt_cipher_iv },
        { "reset", _lcrypt_cipher_reset },
        { "block", _lcrypt_cipher_block },
        { "dofinal", _lcrypt_cipher_dofinal },
        { "__gc", _lcrypt_cipher_gc },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_CIPHER, reg_new, reg_func);
    return 1;
}
