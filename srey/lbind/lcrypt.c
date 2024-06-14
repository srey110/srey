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

static int32_t _lcrypt_md5_new(lua_State *lua) {
    lua_newuserdata(lua, sizeof(md5_ctx));
    ASSOC_MTABLE(lua, "_md5_ctx");
    return 1;
}
static int32_t _lcrypt_md5_init(lua_State *lua) {
    md5_ctx *md5 = lua_touserdata(lua, 1);
    md5_init(md5);
    return 0;
}
static int32_t _lcrypt_md5_update(lua_State *lua) {
    md5_ctx *md5 = lua_touserdata(lua, 1);
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 2)) {
        data = (void *)luaL_checklstring(lua, 2, &size);
    } else {
        data = lua_touserdata(lua, 2);
        size = (size_t)luaL_checkinteger(lua, 3);
    }
    md5_update(md5, data, size);
    return 0;
}
static int32_t _lcrypt_md5_final(lua_State *lua) {
    md5_ctx *md5 = lua_touserdata(lua, 1);
    char out[MD5_BLOCK_SIZE];
    md5_final(md5, out);
    lua_pushlstring(lua, out, sizeof(out));
    return 1;
}
//srey.md5
LUAMOD_API int luaopen_md5(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lcrypt_md5_new },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "init", _lcrypt_md5_init },
        { "update", _lcrypt_md5_update },
        { "final", _lcrypt_md5_final },
        { NULL, NULL }
    };
    REG_MTABLE(lua, "_md5_ctx", reg_new, reg_func);
    return 1;
}
static int32_t _lcrypt_sha1_new(lua_State *lua) {
    lua_newuserdata(lua, sizeof(sha1_ctx));
    ASSOC_MTABLE(lua, "_sha1_ctx");
    return 1;
}
static int32_t _lcrypt_sha1_init(lua_State *lua) {
    sha1_ctx *sha1 = lua_touserdata(lua, 1);
    sha1_init(sha1);
    return 0;
}
static int32_t _lcrypt_sha1_update(lua_State *lua) {
    sha1_ctx *sha1 = lua_touserdata(lua, 1);
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 2)) {
        data = (void *)luaL_checklstring(lua, 2, &size);
    } else {
        data = lua_touserdata(lua, 2);
        size = (size_t)luaL_checkinteger(lua, 3);
    }
    sha1_update(sha1, data, size);
    return 0;
}
static int32_t _lcrypt_sha1_final(lua_State *lua) {
    sha1_ctx *sha1 = lua_touserdata(lua, 1);
    char out[SHA1_BLOCK_SIZE];
    sha1_final(sha1, out);
    lua_pushlstring(lua, out, sizeof(out));
    return 1;
}
//srey.sha1
LUAMOD_API int luaopen_sha1(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lcrypt_sha1_new },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "init", _lcrypt_sha1_init },
        { "update", _lcrypt_sha1_update },
        { "final", _lcrypt_sha1_final },
        { NULL, NULL }
    };
    REG_MTABLE(lua, "_sha1_ctx", reg_new, reg_func);
    return 1;
}
static int32_t _lcrypt_sha256_new(lua_State *lua) {
    lua_newuserdata(lua, sizeof(sha256_ctx));
    ASSOC_MTABLE(lua, "_sha256_ctx");
    return 1;
}
static int32_t _lcrypt_sha256_init(lua_State *lua) {
    sha256_ctx *sha256 = lua_touserdata(lua, 1);
    sha256_init(sha256);
    return 0;
}
static int32_t _lcrypt_sha256_update(lua_State *lua) {
    sha256_ctx *sha256 = lua_touserdata(lua, 1);
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 2)) {
        data = (void *)luaL_checklstring(lua, 2, &size);
    } else {
        data = lua_touserdata(lua, 2);
        size = (size_t)luaL_checkinteger(lua, 3);
    }
    sha256_update(sha256, data, size);
    return 0;
}
static int32_t _lcrypt_sha256_final(lua_State *lua) {
    sha256_ctx *sha256 = lua_touserdata(lua, 1);
    char out[SHA256_BLOCK_SIZE];
    sha256_final(sha256, out);
    lua_pushlstring(lua, out, sizeof(out));
    return 1;
}
//srey.sha256
LUAMOD_API int luaopen_sha256(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lcrypt_sha256_new },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "init", _lcrypt_sha256_init },
        { "update", _lcrypt_sha256_update },
        { "final", _lcrypt_sha256_final },
        { NULL, NULL }
    };
    REG_MTABLE(lua, "_sha256_ctx", reg_new, reg_func);
    return 1;
}
static int32_t _lcrypt_hmac_sha256_new(lua_State *lua) {
    size_t lens;
    const char *key = luaL_checklstring(lua, 1, &lens);
    hmac_sha256_ctx *mac256 = lua_newuserdata(lua, sizeof(hmac_sha256_ctx));
    hmac_sha256_key(mac256, key, lens);
    ASSOC_MTABLE(lua, "_hmac_sha256_ctx");
    return 1;
}
static int32_t _lcrypt_hmac_sha256_init(lua_State *lua) {
    hmac_sha256_ctx *mac256 = lua_touserdata(lua, 1);
    hmac_sha256_init(mac256);
    return 0;
}
static int32_t _lcrypt_hmac_sha256_update(lua_State *lua) {
    hmac_sha256_ctx *mac256 = lua_touserdata(lua, 1);
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 2)) {
        data = (void *)luaL_checklstring(lua, 2, &size);
    } else {
        data = lua_touserdata(lua, 2);
        size = (size_t)luaL_checkinteger(lua, 3);
    }
    hmac_sha256_update(mac256, data, size);
    return 0;
}
static int32_t _lcrypt_hmac_sha256_final(lua_State *lua) {
    hmac_sha256_ctx *mac256 = lua_touserdata(lua, 1);
    char out[SHA256_BLOCK_SIZE];
    hmac_sha256_final(mac256, out);
    lua_pushlstring(lua, (char *)out, sizeof(out));
    return 1;
}
//srey.hmac256
LUAMOD_API int luaopen_hmac256(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lcrypt_hmac_sha256_new },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "init", _lcrypt_hmac_sha256_init },
        { "update", _lcrypt_hmac_sha256_update },
        { "final", _lcrypt_hmac_sha256_final },
        { NULL, NULL }
    };
    REG_MTABLE(lua, "_hmac_sha256_ctx", reg_new, reg_func);
    return 1;
}

#endif
