#include "lbind/lpub.h"

#define MT_CIPHER "_cipher_ctx"

//srey.cipher
/// <summary>
/// 创建对称加解密上下文；缺省不填充（NoPadding），要填充用 :padding 设置；缺省 IV 全零，非 ECB 模式应再调 :iv
/// </summary>
/// <param name="engine" type="integer">算法引擎（CIPHER_TYPE：DES / DES3 / AES）</param>
/// <param name="model" type="integer">工作模式（CIPHER_MODEL：ECB / CBC / CFB / OFB / CTR）</param>
/// <param name="key" type="string">密钥。AES 按 keybits 取 16 / 24 / 32 字节、DES 取 8 字节、DES3 取 24 字节，
/// 不足补 0，超长只取前缀、多出的静默丢弃；DES3 给 17~23 字节时补 0 到 24、三段独立，
/// 9~16 字节时补 0 到 16、第三段取第一段，不超过 8 字节时三段相同，强度退化为单重 DES</param>
/// <param name="keybits" type="integer">密钥位数：AES 只收 128 / 192 / 256，DES / DES3 忽略</param>
/// <param name="encrypt" type="integer">1 加密，0 解密</param>
/// <returns type="_cipher_ctx">cipher 对象</returns>
static int32_t _lcipher_new(lua_State *lua) {
    int32_t engine = (int32_t)lpub_check_range(lua, 1, DES, AES, "invalid cipher engine");
    int32_t model = (int32_t)lpub_check_range(lua, 2, ECB, CTR, "invalid cipher model");
    size_t lens;
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
// 判据同 ldigest.c 的 _ldigest_check：cipher_free 整块 secure_zero，引擎回调随之归零
static cipher_ctx *_lcipher_check(lua_State *lua) {
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
static int32_t _lcipher_size(lua_State *lua) {
    cipher_ctx *cipher = _lcipher_check(lua);
    lua_pushinteger(lua, cipher_size(cipher));
    return 1;
}
/// <summary>
/// 设置填充模式；取值须落在 padding_model 枚举内，越界报错。cipher.new 出来缺省为 NoPadding。
/// ZeroPadding 不对称：解密不剥离补的 0，:dofinal 返回含补零的完整长度；要还原原始长度用 PKCS57 / ANSIX923 / ISO10126
/// </summary>
/// <param name="self" type="userdata">cipher 对象</param>
/// <param name="padding" type="integer">填充模式（PADDING_MODEL：NoPadding / ZeroPadding / PKCS57 / ISO10126 / ANSIX923）</param>
/// <returns>无</returns>
static int32_t _lcipher_padding(lua_State *lua) {
    cipher_ctx *cipher = _lcipher_check(lua);
    lua_Integer padding = luaL_checkinteger(lua, 2);
    luaL_argcheck(lua, padding >= NoPadding && padding <= ANSIX923, 2, "invalid padding model");
    cipher_padding(cipher, (padding_model)padding);
    return 0;
}
/// <summary>
/// 设置初始化向量（IV）。CTR / CFB / OFB 跨消息复用同一对象时每条消息前都须设新 IV，见 dofinal
/// </summary>
/// <param name="self" type="userdata">cipher 对象</param>
/// <param name="iv" type="string">初始化向量；短于分组长度（:size）补 0，长于只取前缀，ECB 下忽略</param>
/// <returns>无</returns>
static int32_t _lcipher_iv(lua_State *lua) {
    cipher_ctx *cipher = _lcipher_check(lua);
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
static int32_t _lcipher_reset(lua_State *lua) {
    cipher_ctx *cipher = _lcipher_check(lua);
    cipher_reset(cipher);
    return 0;
}
/// <summary>
/// 加解密一段数据，每段不超过分组长度（:size）。CFB / OFB / CTR 按字节续用密钥流：未设填充时可按任意长度
/// 切段逐次调用，结果与整段一次 :dofinal 逐字节相同，空串不消耗密钥流；设了填充时加密侧不足一组的段会补成整组，
/// 只能放在最后。PKCS57 / ISO10126 / ANSIX923 下总长恰为整组时不会追加整块填充，要与 :dofinal 一致须最后再送一个空串；
/// 解密侧不校验也不剥离填充，带填充的整条消息用 :dofinal。续用位置由 :reset / :iv / :dofinal 清零
/// </summary>
/// <param name="self" type="userdata">cipher 对象</param>
/// <param name="data" type="string|lightuserdata">数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <returns type="string?">加解密结果。返回 nil 的情形：超过分组长度；不足一组时 ECB/CBC 加密未设填充、
/// ECB/CBC 解密、CFB/OFB/CTR 解密且设了填充；ISO10126 取随机字节失败</returns>
static int32_t _lcipher_block(lua_State *lua) {
    cipher_ctx *cipher = _lcipher_check(lua);
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
static int32_t _lcipher_dofinal(lua_State *lua) {
    cipher_ctx *cipher = _lcipher_check(lua);
    size_t size;
    void *data = lpub_check_buf(lua, 2, &size, NULL);
    size_t outlen = size + cipher_size(cipher);
    luaL_Buffer lbuf;
    char *out = luaL_buffinitsize(lua, &lbuf, outlen);
    int32_t rtn = cipher_dofinal(cipher, data, size, out, &size);
    // 失败也先收 luaL_Buffer 再 pop，理由见 lbase64.c 的 _lbase64_decode
    luaL_pushresultsize(&lbuf, ERR_OK == rtn ? size : 0);
    secure_zero(&lbuf, sizeof(lbuf));// 理由同 lbase64.c 的 _lbase64_decode
    if (ERR_OK != rtn) {
        lua_pop(lua, 1);
        lua_pushnil(lua);
    }
    return 1;
}
static int32_t _lcipher_gc(lua_State *lua) {
    cipher_ctx *cipher = lpub_check_udata(lua, 1, MT_CIPHER);
    cipher_free(cipher);
    return 0;
}
LUAMOD_API int luaopen_cipher(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lcipher_new },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "size", _lcipher_size },
        { "padding", _lcipher_padding },
        { "iv", _lcipher_iv },
        { "reset", _lcipher_reset },
        { "block", _lcipher_block },
        { "dofinal", _lcipher_dofinal },
        { "__gc", _lcipher_gc },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_CIPHER, reg_new, reg_func);
    return 1;
}
