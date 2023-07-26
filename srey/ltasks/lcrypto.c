#include "lib.h"
#if WITH_LUA
#include "lua/lapi.h"
#include "lua/lauxlib.h"
#include "lua/lstring.h"

static int32_t _lcrypto_b64_encode(lua_State *lua) {
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 1)) {
        data = (void *)luaL_checklstring(lua, 1, &size);
    } else {
        data = lua_touserdata(lua, 1);
        size = (size_t)luaL_checkinteger(lua, 2);
    }
    char *out;
    size_t lens = B64_ENSIZE(size);
    MALLOC(out, lens);
    size = b64_encode(data, size, out);
    lua_pushlstring(lua, out, size);
    FREE(out);
    return 1;
}
static int32_t _lcrypto_b64_decode(lua_State *lua) {
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 1)) {
        data = (void *)luaL_checklstring(lua, 1, &size);
    } else {
        data = lua_touserdata(lua, 1);
        size = (size_t)luaL_checkinteger(lua, 2);
    }
    char *out;
    size_t lens = B64_DESIZE(size);
    MALLOC(out, lens);
    size = b64_decode(data, size, out);
    lua_pushlstring(lua, out, size);
    FREE(out);
    return 1;
}
static int32_t _lcrypto_crc16(lua_State *lua) {
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
static int32_t _lcrypto_crc32(lua_State *lua) {
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
static int32_t _lcrypto_md5_init(lua_State *lua) {
    md5_ctx *md5 = lua_touserdata(lua, 1);
    if (NULL == md5) {
        md5 = lua_newuserdata(lua, sizeof(md5_ctx));
    }
    md5_init(md5);
    return 1;
}
static int32_t _lcrypto_md5_update(lua_State *lua) {
    md5_ctx *md5 = lua_touserdata(lua, 1);
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 2)) {
        data = (void *)luaL_checklstring(lua, 2, &size);
    } else {
        data = lua_touserdata(lua, 3);
        size = (size_t)luaL_checkinteger(lua, 3);
    }
    md5_update(md5, data, size);
    return 0;
}
static int32_t _lcrypto_md5_final(lua_State *lua) {
    md5_ctx *md5 = lua_touserdata(lua, 1);
    char out[16];
    md5_final(md5, out);
    lua_pushlstring(lua, out, sizeof(out));
    return 1;
}
static int32_t _lcrypto_sha1_init(lua_State *lua) {
    sha1_ctx *sha1 = lua_touserdata(lua, 1);
    if (NULL == sha1) {
        sha1 = lua_newuserdata(lua, sizeof(sha1_ctx));
    }
    sha1_init(sha1);
    return 1;
}
static int32_t _lcrypto_sha1_update(lua_State *lua) {
    sha1_ctx *sha1 = lua_touserdata(lua, 1);
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 2)) {
        data = (void *)luaL_checklstring(lua, 2, &size);
    } else {
        data = lua_touserdata(lua, 3);
        size = (size_t)luaL_checkinteger(lua, 3);
    }
    sha1_update(sha1, data, size);
    return 0;
}
static int32_t _lcrypto_sha1_final(lua_State *lua) {
    sha1_ctx *sha1 = lua_touserdata(lua, 1);
    char out[20];
    sha1_final(sha1, out);
    lua_pushlstring(lua, out, sizeof(out));
    return 1;
}
static int32_t _lcrypto_sha256_init(lua_State *lua) {
    sha256_ctx *sha256 = lua_touserdata(lua, 1);
    if (NULL == sha256) {
        sha256 = lua_newuserdata(lua, sizeof(sha256_ctx));
    }
    sha256_init(sha256);
    return 1;
}
static int32_t _lcrypto_sha256_update(lua_State *lua) {
    sha256_ctx *sha256 = lua_touserdata(lua, 1);
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 2)) {
        data = (void *)luaL_checklstring(lua, 2, &size);
    } else {
        data = lua_touserdata(lua, 3);
        size = (size_t)luaL_checkinteger(lua, 3);
    }
    sha256_update(sha256, data, size);
    return 0;
}
static int32_t _lcrypto_sha256_final(lua_State *lua) {
    sha256_ctx *sha256 = lua_touserdata(lua, 1);
    char out[32];
    sha256_final(sha256, out);
    lua_pushlstring(lua, out, sizeof(out));
    return 1;
}
static int32_t _lcrypto_url_encode(lua_State *lua) {
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 1)) {
        data = (void *)luaL_checklstring(lua, 1, &size);
    } else {
        data = lua_touserdata(lua, 2);
        size = (size_t)luaL_checkinteger(lua, 2);
    }
    char *out;
    MALLOC(out, URL_ENSIZE(size));
    url_encode(data, size, out);
    lua_pushstring(lua, out);
    FREE(out);
    return 1;
}
LUAMOD_API int luaopen_crypto(lua_State *lua) {
    luaL_Reg reg[] = {
        { "b64_encode", _lcrypto_b64_encode },
        { "b64_decode", _lcrypto_b64_decode },

        { "crc16", _lcrypto_crc16 },
        { "crc32", _lcrypto_crc32 },

        { "md5_init", _lcrypto_md5_init },
        { "md5_update", _lcrypto_md5_update },
        { "md5_final", _lcrypto_md5_final },

        { "sha1_init", _lcrypto_sha1_init },
        { "sha1_update", _lcrypto_sha1_update },
        { "sha1_final", _lcrypto_sha1_final },

        { "sha256_init", _lcrypto_sha256_init },
        { "sha256_update", _lcrypto_sha256_update },
        { "sha256_final", _lcrypto_sha256_final },

        { "url_encode", _lcrypto_url_encode },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}

#endif
