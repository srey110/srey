#include "lbind/lpub.h"

#if WITH_LUA

static int32_t _lcrypt_url_encode(lua_State *lua) {
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 1)) {
        data = (void *)luaL_checklstring(lua, 1, &size);
    } else {
        data = lua_touserdata(lua, 1);
        size = (size_t)luaL_checkinteger(lua, 2);
    }
    char *out;
    MALLOC(out, URLEN_BLOCK_SIZE(size));
    url_encode(data, size, out);
    lua_pushstring(lua, out);
    FREE(out);
    return 1;
}
static int32_t _lcrypt_url_decode(lua_State *lua) {
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 1)) {
        data = (void *)luaL_checklstring(lua, 1, &size);
    } else {
        data = lua_touserdata(lua, 1);
        size = (size_t)luaL_checkinteger(lua, 2);
    }
    char *out;
    MALLOC(out, size);
    memcpy(out, data, size);
    url_decode(out, size);
    lua_pushstring(lua, out);
    FREE(out);
    return 1;
}
static int32_t _lcrypt_url_parse(lua_State *lua) {
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 1)) {
        data = (void *)luaL_checklstring(lua, 1, &size);
    } else {
        data = lua_touserdata(lua, 1);
        size = (size_t)luaL_checkinteger(lua, 2);
    }
    url_ctx url;
    url_parse(&url, data, size);
    lua_createtable(lua, 0, 8);
    if (!buf_empty(&url.scheme)) {
        lua_pushstring(lua, "scheme");
        lua_pushlstring(lua, url.scheme.data, url.scheme.lens);
        lua_settable(lua, -3);
    }
    if (!buf_empty(&url.user)) {
        lua_pushstring(lua, "user");
        lua_pushlstring(lua, url.user.data, url.user.lens);
        lua_settable(lua, -3);
    }
    if (!buf_empty(&url.psw)) {
        lua_pushstring(lua, "psw");
        lua_pushlstring(lua, url.psw.data, url.psw.lens);
        lua_settable(lua, -3);
    }
    if (!buf_empty(&url.host)) {
        lua_pushstring(lua, "host");
        lua_pushlstring(lua, url.host.data, url.host.lens);
        lua_settable(lua, -3);
    }
    if (!buf_empty(&url.port)) {
        lua_pushstring(lua, "port");
        lua_pushlstring(lua, url.port.data, url.port.lens);
        lua_settable(lua, -3);
    }
    if (!buf_empty(&url.path)) {
        lua_pushstring(lua, "path");
        lua_pushlstring(lua, url.path.data, url.path.lens);
        lua_settable(lua, -3);
    }
    if (!buf_empty(&url.anchor)) {
        lua_pushstring(lua, "anchor");
        lua_pushlstring(lua, url.anchor.data, url.anchor.lens);
        lua_settable(lua, -3);
    }
    lua_pushstring(lua, "param");
    lua_createtable(lua, 0, MAX_NPARAM);
    url_param *param;
    for (int32_t i = 0; i < MAX_NPARAM; i++) {
        param = &url.param[i];
        if (buf_empty(&param->key)) {
            break;
        }
        lua_pushlstring(lua, param->key.data, param->key.lens);
        if (buf_empty(&param->val)) {
            lua_pushstring(lua, "");
        } else {
            lua_pushlstring(lua, param->val.data, param->val.lens);
        }
        lua_settable(lua, -3);
    }
    lua_settable(lua, -3);
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
static int32_t _lcrypt_bs64_encode(lua_State *lua) {
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 1)) {
        data = (void *)luaL_checklstring(lua, 1, &size);
    } else {
        data = lua_touserdata(lua, 1);
        size = (size_t)luaL_checkinteger(lua, 2);
    }
    char *out;
    size_t lens = B64EN_BLOCK_SIZE(size);
    MALLOC(out, lens);
    size = bs64_encode(data, size, out);
    lua_pushlstring(lua, out, size);
    FREE(out);
    return 1;
}
static int32_t _lcrypt_bs64_decode(lua_State *lua) {
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 1)) {
        data = (void *)luaL_checklstring(lua, 1, &size);
    } else {
        data = lua_touserdata(lua, 1);
        size = (size_t)luaL_checkinteger(lua, 2);
    }
    char *out;
    size_t lens = B64DE_BLOCK_SIZE(size);
    MALLOC(out, lens);
    size = bs64_decode(data, size, out);
    lua_pushlstring(lua, out, size);
    FREE(out);
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
static int32_t _lcrypt_crc16(lua_State *lua) {
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 1)) {
        data = (void *)luaL_checklstring(lua, 1, &size);
    } else {
        data = lua_touserdata(lua, 1);
        size = (size_t)luaL_checkinteger(lua, 2);
    }
    uint16_t crc = crc16(data, size);
    lua_pushinteger(lua, crc);
    return 1;
}
static int32_t _lcrypt_crc32(lua_State *lua) {
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 1)) {
        data = (void *)luaL_checklstring(lua, 1, &size);
    } else {
        data = lua_touserdata(lua, 1);
        size = (size_t)luaL_checkinteger(lua, 2);
    }
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
static int32_t _lcrypt_digest_new(lua_State *lua) {
    int32_t dtype = (int32_t)luaL_checkinteger(lua, 1);
    digest_ctx *digest = lua_newuserdata(lua, sizeof(digest_ctx));
    digest_init(digest, dtype);
    ASSOC_MTABLE(lua, "_digest_ctx");
    return 1;
}
static int32_t _lcrypt_digest_size(lua_State *lua) {
    digest_ctx *digest = lua_touserdata(lua, 1);
    lua_pushinteger(lua, digest_size(digest));
    return 1;
}
static int32_t _lcrypt_digest_reset(lua_State *lua) {
    digest_ctx *digest = lua_touserdata(lua, 1);
    digest_reset(digest);
    return 0;
}
static int32_t _lcrypt_digest_update(lua_State *lua) {
    digest_ctx *digest = lua_touserdata(lua, 1);
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 2)) {
        data = (void *)luaL_checklstring(lua, 2, &size);
    } else {
        data = lua_touserdata(lua, 2);
        size = (size_t)luaL_checkinteger(lua, 3);
    }
    digest_update(digest, data, size);
    return 0;
}
static int32_t _lcrypt_digest_final(lua_State *lua) {
    digest_ctx *digest = lua_touserdata(lua, 1);
    char out[DG_BLOCK_SIZE];
    size_t lens = digest_final(digest, out);
    lua_pushlstring(lua, out, lens);
    return 1;
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
        { NULL, NULL }
    };
    REG_MTABLE(lua, "_digest_ctx", reg_new, reg_func);
    return 1;
}
static int32_t _lcrypt_hmac_new(lua_State *lua) {
    size_t lens;
    int32_t dtype = (int32_t)luaL_checkinteger(lua, 1);
    const char *key = luaL_checklstring(lua, 2, &lens);
    hmac_ctx *hmac = lua_newuserdata(lua, sizeof(hmac_ctx));
    hmac_init(hmac, dtype, key, lens);
    ASSOC_MTABLE(lua, "_hmac_ctx");
    return 1;
}
static int32_t _lcrypt_hmac_size(lua_State *lua) {
    hmac_ctx *hmac = lua_touserdata(lua, 1);
    lua_pushinteger(lua, hmac_size(hmac));
    return 1;
}
static int32_t _lcrypt_hmac_reset(lua_State *lua) {
    hmac_ctx *hmac = lua_touserdata(lua, 1);
    hmac_reset(hmac);
    return 0;
}
static int32_t _lcrypt_hmac_update(lua_State *lua) {
    hmac_ctx *hmac = lua_touserdata(lua, 1);
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 2)) {
        data = (void *)luaL_checklstring(lua, 2, &size);
    } else {
        data = lua_touserdata(lua, 2);
        size = (size_t)luaL_checkinteger(lua, 3);
    }
    hmac_update(hmac, data, size);
    return 0;
}
static int32_t _lcrypt_hmac_final(lua_State *lua) {
    hmac_ctx *hmac = lua_touserdata(lua, 1);
    char out[DG_BLOCK_SIZE];
    size_t lens = hmac_final(hmac, out);
    lua_pushlstring(lua, out, lens);
    return 1;
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
        { NULL, NULL }
    };
    REG_MTABLE(lua, "_hmac_ctx", reg_new, reg_func);
    return 1;
}
//srey.cipher
static int32_t _lcrypt_cipher_new(lua_State *lua) {
    size_t lens;
    int32_t engine = (int32_t)luaL_checkinteger(lua, 1);
    int32_t model = (int32_t)luaL_checkinteger(lua, 2);
    const char *key = luaL_checklstring(lua, 3, &lens);
    int32_t keybits = (int32_t)luaL_checkinteger(lua, 4);
    int32_t encrypt = (int32_t)luaL_checkinteger(lua, 5);
    cipher_ctx *cipher = lua_newuserdata(lua, sizeof(cipher_ctx));
    cipher_init(cipher, engine, model, key, lens, keybits, encrypt);
    ASSOC_MTABLE(lua, "_cipher_ctx");
    return 1;
}
static int32_t _lcrypt_cipher_size(lua_State *lua) {
    cipher_ctx *cipher = lua_touserdata(lua, 1);
    lua_pushinteger(lua, cipher_size(cipher));
    return 1;
}
static int32_t _lcrypt_cipher_padding(lua_State *lua) {
    cipher_ctx *cipher = lua_touserdata(lua, 1);
    int32_t padding = (int32_t)luaL_checkinteger(lua, 2);
    cipher_padding(cipher, padding);
    return 0;
}
static int32_t _lcrypt_cipher_iv(lua_State *lua) {
    cipher_ctx *cipher = lua_touserdata(lua, 1);
    size_t lens;
    const char *iv = luaL_checklstring(lua, 2, &lens);
    cipher_iv(cipher, iv, lens);
    return 0;
}
static int32_t _lcrypt_cipher_reset(lua_State *lua) {
    cipher_ctx *cipher = lua_touserdata(lua, 1);
    cipher_reset(cipher);
    return 0;
}
static int32_t _lcrypt_cipher_block(lua_State *lua) {
    cipher_ctx *cipher = lua_touserdata(lua, 1);
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 2)) {
        data = (void *)luaL_checklstring(lua, 2, &size);
    } else {
        data = lua_touserdata(lua, 2);
        size = (size_t)luaL_checkinteger(lua, 3);
    }
    data = cipher_block(cipher, data, size, &size);
    lua_pushlstring(lua, (const char *)data, size);
    return 1;
}
static int32_t _lcrypt_cipher_dofinal(lua_State *lua) {
    cipher_ctx *cipher = lua_touserdata(lua, 1);
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 2)) {
        data = (void *)luaL_checklstring(lua, 2, &size);
    } else {
        data = lua_touserdata(lua, 2);
        size = (size_t)luaL_checkinteger(lua, 3);
    }
    char *out;
    MALLOC(out, size + cipher_size(cipher));
    size = cipher_dofinal(cipher, data, size, out);
    lua_pushlstring(lua, out, size);
    FREE(out);
    return 1;
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
        { NULL, NULL }
    };
    REG_MTABLE(lua, "_cipher_ctx", reg_new, reg_func);
    return 1;
}

#endif
