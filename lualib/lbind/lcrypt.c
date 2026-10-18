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
    size_t size;
    int32_t idx = 1;
    void *data = lpub_check_buf_idx(lua, &idx, &size, NULL);
    luaL_argcheck(lua, size <= (SIZE_MAX - 1) / 3, 1, "data too large to url encode");// 32 位上 3*size+1 会回绕
    int32_t space2plus = 0 != luaL_optinteger(lua, idx, 1);
    luaL_Buffer lbuf;
    char *out = luaL_buffinitsize(lua, &lbuf, URLEN_SIZE(size));
    luaL_pushresultsize(&lbuf, url_encode(data, size, out, space2plus));
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
    size_t size;
    int32_t idx = 1;
    void *data = lpub_check_buf_idx(lua, &idx, &size, NULL);
    int32_t plus2space = 0 != luaL_optinteger(lua, idx, 1);
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
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <param name="decode" type="boolean|integer?">true/非 0（默认 true）：对 path 段与 query 做 URL 解码；false/0：保留原始 percent 编码，适合后续用于 HTTP 重组</param>
/// <returns type="ParsedURL?">成功返回含 scheme/user/psw/host/port/path/query/anchor/param/segs 的表（path/query 语义随 decode 参数而定）；
/// 路径段数超 URL_MAX_PATH_DEPTH、非空查询参数超过 URL_MAX_PARAM（64）个时返回 nil；decode 模式下 URL 长度达到 1KB（URL_BUF_LENS）同样返回 nil，不解码时不限长</returns>
static int32_t _lcrypt_url_parse(lua_State *lua) {
    size_t size;
    int32_t idx = 1;
    void *data = lpub_check_buf_idx(lua, &idx, &size, NULL);
    int32_t decode = (LUA_TBOOLEAN == lua_type(lua, idx)) ? lua_toboolean(lua, idx)
                   : (LUA_TNUMBER == lua_type(lua, idx)) ? (0 != lua_tointeger(lua, idx))
                   : 1;
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
static int32_t _lcrypt_only_crlf(const char *data, size_t size) {
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
static int32_t _lcrypt_bs64_decode(lua_State *lua) {
    size_t size;
    void *data = lpub_check_buf(lua, 1, &size, NULL);
    size_t lens = B64DE_SIZE(size);
    luaL_Buffer lbuf;
    char *out = luaL_buffinitsize(lua, &lbuf, lens);
    size_t declens = bs64_decode(data, size, out);
    // bs64_decode 返 0 时分不出畸形与空内容(见 base64.h)：空输入、只含 CR/LF 的算空串，其余(含只有 '=')算畸形。
    // 不区分的话损坏数据也得到空串，调用方那句 if d then use(d) end 会把它当成合法凭据收下
    int32_t bad = (0 == declens && 0 != size && !_lcrypt_only_crlf(data, size));
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
static int32_t _lcrypt_crc32(lua_State *lua) {
    size_t size;
    void *data = lpub_check_buf(lua, 1, &size, NULL);
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
/// <param name="dtype" type="integer">算法类型（MD5 / SHA1 / SHA256 / XXH32 / XXH64 等）</param>
/// <returns type="_digest_ctx">摘要对象</returns>
static int32_t _lcrypt_digest_new(lua_State *lua) {
    lua_Integer dtype = luaL_checkinteger(lua, 1);
    luaL_argcheck(lua, dtype >= DG_MD2 && dtype <= DG_XXH64, 1, "invalid digest type");
    digest_ctx *digest = lua_newuserdata(lua, sizeof(digest_ctx));
    digest_init(digest, (digest_type)dtype);
    ASSOC_MTABLE(lua, MT_DIGEST);
    return 1;
}
// digest / hmac 的对象按值存在 userdata 里,没有可置 NULL 的指针,故已释放的判据是 attr 归零。
// __gc 经 __index 也是个普通方法,业务一行 d:__gc() 就能提前释放,此后每个入口都得报可捕获的错
static digest_ctx *_lcrypt_check_digest(lua_State *lua) {
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
static int32_t _lcrypt_digest_size(lua_State *lua) {
    digest_ctx *digest = _lcrypt_check_digest(lua);
    lua_pushinteger(lua, digest_size(digest));
    return 1;
}
/// <summary>
/// 重置摘要上下文，可重新开始计算
/// </summary>
/// <param name="self" type="userdata">摘要对象</param>
/// <returns>无</returns>
static int32_t _lcrypt_digest_reset(lua_State *lua) {
    digest_ctx *digest = _lcrypt_check_digest(lua);
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
    digest_ctx *digest = _lcrypt_check_digest(lua);
    size_t size;
    void *data = lpub_check_buf(lua, 2, &size, NULL);
    digest_update(digest, data, size);
    return 0;
}
/// <summary>
/// 完成摘要计算
/// </summary>
/// <param name="self" type="userdata">摘要对象</param>
/// <returns type="string">原始二进制摘要</returns>
static int32_t _lcrypt_digest_final(lua_State *lua) {
    digest_ctx *digest = _lcrypt_check_digest(lua);
    char out[DG_BLOCK_SIZE];
    size_t lens = digest_final(digest, out);
    lua_pushlstring(lua, out, lens);
    secure_zero(out, sizeof(out));
    return 1;
}
static int32_t _lcrypt_digest_gc(lua_State *lua) {
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
static int32_t _lcrypt_digest_sum(lua_State *lua) {
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
        { "new", _lcrypt_digest_new },
        { "sum", _lcrypt_digest_sum },
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
/// <param name="dtype" type="integer">底层 Hash 算法类型（MD5 / SHA1 / SHA256 等）；XXH32 / XXH64 不支持</param>
/// <param name="key" type="string">密钥</param>
/// <returns type="_hmac_ctx">HMAC 对象</returns>
static int32_t _lcrypt_hmac_new(lua_State *lua) {
    size_t lens;
    lua_Integer dtype = luaL_checkinteger(lua, 1);
    // 上界停在 SHA512：xxhash 没有分组长度，放进来会打到 hmac_init 的 ASSERTAB 整进程中止
    luaL_argcheck(lua, dtype >= DG_MD2 && dtype <= DG_SHA512, 1, "invalid digest type");
    const char *key = luaL_checklstring(lua, 2, &lens);
    hmac_ctx *hmac = lua_newuserdata(lua, sizeof(hmac_ctx));
    hmac_init(hmac, (digest_type)dtype, key, lens);
    ASSOC_MTABLE(lua, MT_HMAC);
    return 1;
}
// 判据同 _lcrypt_check_digest:hmac_free 整块 secure_zero,4 个内嵌 digest 的 attr 一起归零
static hmac_ctx *_lcrypt_check_hmac(lua_State *lua) {
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
static int32_t _lcrypt_hmac_size(lua_State *lua) {
    hmac_ctx *hmac = _lcrypt_check_hmac(lua);
    lua_pushinteger(lua, hmac_size(hmac));
    return 1;
}
/// <summary>
/// 重置 HMAC 上下文，可重新开始计算
/// </summary>
/// <param name="self" type="userdata">HMAC 对象</param>
/// <returns>无</returns>
static int32_t _lcrypt_hmac_reset(lua_State *lua) {
    hmac_ctx *hmac = _lcrypt_check_hmac(lua);
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
    hmac_ctx *hmac = _lcrypt_check_hmac(lua);
    size_t size;
    void *data = lpub_check_buf(lua, 2, &size, NULL);
    hmac_update(hmac, data, size);
    return 0;
}
/// <summary>
/// 完成 HMAC 计算
/// </summary>
/// <param name="self" type="userdata">HMAC 对象</param>
/// <returns type="string">原始二进制 HMAC 结果</returns>
static int32_t _lcrypt_hmac_final(lua_State *lua) {
    hmac_ctx *hmac = _lcrypt_check_hmac(lua);
    char out[DG_BLOCK_SIZE];
    size_t lens = hmac_final(hmac, out);
    lua_pushlstring(lua, out, lens);
    secure_zero(out, sizeof(out));
    return 1;
}
static int32_t _lcrypt_hmac_gc(lua_State *lua) {
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
static int32_t _lcrypt_hmac_sum(lua_State *lua) {
    size_t klens;
    size_t size;
    lua_Integer dtype = luaL_checkinteger(lua, 1);
    luaL_argcheck(lua, dtype >= DG_MD2 && dtype <= DG_SHA512, 1, "invalid digest type");
    const char *key = luaL_checklstring(lua, 2, &klens);
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
        { "new", _lcrypt_hmac_new },
        { "sum", _lcrypt_hmac_sum },
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
/// 创建对称加解密上下文；缺省不填充（NoPadding），要填充用 :padding 设置
/// </summary>
/// <param name="engine" type="integer">算法引擎（CIPHER_TYPE：DES / DES3 / AES）</param>
/// <param name="model" type="integer">工作模式（ECB / CBC / CFB / OFB / CTR 等）</param>
/// <param name="key" type="string">密钥。AES 按 keybits 取 16 / 24 / 32 字节、DES 取 8 字节、DES3 取 24 字节，
/// 不足补 0，超长只取前缀、多出的静默丢弃；DES3 给 17~23 字节时补 0 到 24、三段独立，
/// 9~16 字节时补 0 到 16、第三段取第一段，不超过 8 字节时三段相同，强度退化为单重 DES</param>
/// <param name="keybits" type="integer">密钥位数（128 / 192 / 256 等）</param>
/// <param name="encrypt" type="integer">1 加密，0 解密</param>
/// <returns type="_cipher_ctx">cipher 对象</returns>
static int32_t _lcrypt_cipher_new(lua_State *lua) {
    size_t lens;
    int32_t engine = (int32_t)lpub_check_range(lua, 1, DES, AES, "invalid cipher engine");
    int32_t model = (int32_t)lpub_check_range(lua, 2, ECB, CTR, "invalid cipher model");
    const char *key = luaL_checklstring(lua, 3, &lens);
    lua_Integer keybits = luaL_checkinteger(lua, 4);
    int32_t encrypt = lpub_check_flag(lua, 5);
    // keybits 只有 AES 用得上,DES/DES3 忽略该形参;越界会打到 aes_init 的 ASSERTAB 上整进程 abort
    luaL_argcheck(lua, AES != engine || 128 == keybits || 192 == keybits || 256 == keybits,
                  4, "invalid aes key bits");
    cipher_ctx *cipher = lua_newuserdata(lua, sizeof(cipher_ctx));
    cipher_init(cipher, engine, model, key, lens, (int32_t)keybits, encrypt);
    ASSOC_MTABLE(lua, MT_CIPHER);
    return 1;
}
// 判据同 _lcrypt_check_digest：cipher_free 整块 secure_zero，引擎回调随之归零
static cipher_ctx *_lcrypt_check_cipher(lua_State *lua) {
    cipher_ctx *cipher = lpub_check_udata(lua, 1, MT_CIPHER);
    if (NULL == cipher->_cipher) {
        luaL_error(lua, "cipher already freed");
    }
    return cipher;
}
/// <summary>
/// 返回加解密分块大小
/// </summary>
/// <param name="self" type="userdata">cipher 对象</param>
/// <returns type="integer">分块字节数</returns>
static int32_t _lcrypt_cipher_size(lua_State *lua) {
    cipher_ctx *cipher = _lcrypt_check_cipher(lua);
    lua_pushinteger(lua, cipher_size(cipher));
    return 1;
}
/// <summary>
/// 设置填充模式；取值须落在 padding_model 枚举内，越界报错。cipher.new 出来缺省为 NoPadding
/// </summary>
/// <param name="self" type="userdata">cipher 对象</param>
/// <param name="padding" type="integer">填充模式（PADDING_MODEL：NoPadding / ZeroPadding / PKCS57 / ISO10126 / ANSIX923）</param>
/// <returns>无</returns>
static int32_t _lcrypt_cipher_padding(lua_State *lua) {
    cipher_ctx *cipher = _lcrypt_check_cipher(lua);
    lua_Integer padding = luaL_checkinteger(lua, 2);
    luaL_argcheck(lua, padding >= NoPadding && padding <= ANSIX923, 2, "invalid padding model");
    cipher_padding(cipher, (padding_model)padding);
    return 0;
}
/// <summary>
/// 设置初始化向量（IV）。CTR / CFB / OFB 跨消息复用同一对象时每条消息前都须设新 IV，见 dofinal
/// </summary>
/// <param name="self" type="userdata">cipher 对象</param>
/// <param name="iv" type="string">初始化向量</param>
/// <returns>无</returns>
static int32_t _lcrypt_cipher_iv(lua_State *lua) {
    cipher_ctx *cipher = _lcrypt_check_cipher(lua);
    size_t lens;
    const char *iv = luaL_checklstring(lua, 2, &lens);
    cipher_iv(cipher, iv, lens);
    return 0;
}
/// <summary>
/// 重置加解密上下文状态，可重新使用：当前 IV 回到 :iv 设的初始 IV，流模式的密钥流续用位置清零
/// </summary>
/// <param name="self" type="userdata">cipher 对象</param>
/// <returns>无</returns>
static int32_t _lcrypt_cipher_reset(lua_State *lua) {
    cipher_ctx *cipher = _lcrypt_check_cipher(lua);
    cipher_reset(cipher);
    return 0;
}
/// <summary>
/// 加解密一段数据，每段不超过分组长度（:size）。CFB / OFB / CTR 按字节续用密钥流：未设填充时可按任意长度
/// 切段逐次调用，结果与整段一次 :dofinal 逐字节相同，空串不消耗密钥流；设了填充时加密侧不足一组的段会补成整组，
/// 只能放在最后。续用位置由 :reset / :iv / :dofinal 清零
/// </summary>
/// <param name="self" type="userdata">cipher 对象</param>
/// <param name="data" type="string|lightuserdata">数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <returns type="string?">加解密结果。返回 nil 的情形：超过分组长度；不足一组时 ECB/CBC 加密未设填充、
/// ECB/CBC 解密、CFB/OFB/CTR 解密且设了填充；ISO10126 取随机字节失败</returns>
static int32_t _lcrypt_cipher_block(lua_State *lua) {
    cipher_ctx *cipher = _lcrypt_check_cipher(lua);
    size_t size;
    void *data = lpub_check_buf(lua, 2, &size, NULL);
    data = cipher_block(cipher, data, size, &size);
    //cipher_block 在长度不匹配 / 模式约束不满足时返回 NULL，需守卫避免 lua_pushlstring(NULL, n) 的 UB
    if (NULL == data) {
        return lpub_rtn_nil(lua, 1);
    }
    lua_pushlstring(lua, (const char *)data, size);
    return 1;
}
/// <summary>
/// 完成加解密并处理最终 padding。每次进入都先把当前 IV 复位成 :iv 设的初始 IV（同 :reset）；
/// CTR / CFB / OFB 跨消息复用同一对象时每条消息前须先 :iv 设新 IV，否则各条消息的密钥流相同
/// </summary>
/// <param name="self" type="userdata">cipher 对象</param>
/// <param name="data" type="string|lightuserdata">数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <returns type="string?">加解密最终结果。返回 nil 的情形：输入不是分组整数倍时 ECB/CBC 未设填充或在解密、
/// CFB/OFB/CTR 在解密且设了填充；解密设了填充而输入为空或填充校验不通过；ISO10126 取随机字节失败。
/// 不能拿空串当失败判据——加密空明文再解回来本就是空串</returns>
static int32_t _lcrypt_cipher_dofinal(lua_State *lua) {
    cipher_ctx *cipher = _lcrypt_check_cipher(lua);
    size_t size;
    void *data = lpub_check_buf(lua, 2, &size, NULL);
    size_t outlen = size + cipher_size(cipher);
    luaL_Buffer lbuf;
    char *out = luaL_buffinitsize(lua, &lbuf, outlen);
    int32_t rtn = cipher_dofinal(cipher, data, size, out, &size);
    // 失败也先收 luaL_Buffer 再 pop，理由见 _lcrypt_bs64_decode
    luaL_pushresultsize(&lbuf, ERR_OK == rtn ? size : 0);
    secure_zero(&lbuf, sizeof(lbuf));// 理由同 _lcrypt_bs64_decode
    if (ERR_OK != rtn) {
        lua_pop(lua, 1);
        lua_pushnil(lua);
    }
    return 1;
}
static int32_t _lcrypt_cipher_gc(lua_State *lua) {
    cipher_ctx *cipher = lpub_check_udata(lua, 1, MT_CIPHER);
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
