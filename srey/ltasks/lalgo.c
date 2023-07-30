#include "lib.h"
#if WITH_LUA
#include "lua/lapi.h"
#include "lua/lauxlib.h"
#include "lua/lstring.h"

static int32_t _lalgo_b64_encode(lua_State *lua) {
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
static int32_t _lalgo_b64_decode(lua_State *lua) {
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
static int32_t _lalgo_crc16(lua_State *lua) {
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
static int32_t _lalgo_crc32(lua_State *lua) {
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
static int32_t _lalgo_md5_new(lua_State *lua) {
    lua_newuserdata(lua, sizeof(md5_ctx));
    luaL_getmetatable(lua, "_md5_ctx");
    lua_setmetatable(lua, -2);
    return 1;
}
static int32_t _lalgo_md5_init(lua_State *lua) {
    md5_ctx *md5 = lua_touserdata(lua, 1);
    md5_init(md5);
    return 0;
}
static int32_t _lalgo_md5_update(lua_State *lua) {
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
static int32_t _lalgo_md5_final(lua_State *lua) {
    md5_ctx *md5 = lua_touserdata(lua, 1);
    unsigned char out[MD5_BLOCK_SIZE];
    md5_final(md5, out);
    lua_pushlstring(lua, (char *)out, sizeof(out));
    return 1;
}
static void _reg_class(lua_State *lua, const char *cname, const char *mname,
    luaL_Reg *reg, lua_CFunction _new, lua_CFunction _del) {
    lua_pushcfunction(lua, _new);
    lua_setglobal(lua, cname);
    luaL_newmetatable(lua, mname);
    lua_pushvalue(lua, -1);
    if (NULL != _del) {
        lua_pushstring(lua, "__gc");
        lua_pushcfunction(lua, _del);
        lua_settable(lua, -3);
    }
    lua_pushstring(lua, "__index");
    lua_pushvalue(lua, -2);
    lua_settable(lua, -3);
    luaL_setfuncs(lua, reg, 0);
}
static int32_t _luaopen_srey_md5(lua_State *lua) {
    luaL_Reg reg[] = {
        { "init", _lalgo_md5_init },
        { "update", _lalgo_md5_update },
        { "final", _lalgo_md5_final },
        { NULL, NULL }
    };
    _reg_class(lua, "md5_new", "_md5_ctx", reg, _lalgo_md5_new, NULL);
    return 0;
}
static int32_t _lalgo_sha1_new(lua_State *lua) {
    lua_newuserdata(lua, sizeof(sha1_ctx));
    luaL_getmetatable(lua, "_sha1_ctx");
    lua_setmetatable(lua, -2);
    return 1;
}
static int32_t _lalgo_sha1_init(lua_State *lua) {
    sha1_ctx *sha1 = lua_touserdata(lua, 1);
    sha1_init(sha1);
    return 0;
}
static int32_t _lalgo_sha1_update(lua_State *lua) {
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
static int32_t _lalgo_sha1_final(lua_State *lua) {
    sha1_ctx *sha1 = lua_touserdata(lua, 1);
    unsigned char out[SHA1_BLOCK_SIZE];
    sha1_final(sha1, out);
    lua_pushlstring(lua, (char *)out, sizeof(out));
    return 1;
}
static int32_t _luaopen_srey_sha1(lua_State *lua) {
    luaL_Reg reg[] = {
        { "init", _lalgo_sha1_init },
        { "update", _lalgo_sha1_update },
        { "final", _lalgo_sha1_final },
        { NULL, NULL }
    };
    _reg_class(lua, "sha1_new", "_sha1_ctx", reg, _lalgo_sha1_new, NULL);
    return 0;
}
static int32_t _lalgo_sha256_new(lua_State *lua) {
    lua_newuserdata(lua, sizeof(sha256_ctx));
    luaL_getmetatable(lua, "_sha256_ctx");
    lua_setmetatable(lua, -2);
    return 1;
}
static int32_t _lalgo_sha256_init(lua_State *lua) {
    sha256_ctx *sha256 = lua_touserdata(lua, 1);
    sha256_init(sha256);
    return 0;
}
static int32_t _lalgo_sha256_update(lua_State *lua) {
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
static int32_t _lalgo_sha256_final(lua_State *lua) {
    sha256_ctx *sha256 = lua_touserdata(lua, 1);
    unsigned char out[SHA256_BLOCK_SIZE];
    sha256_final(sha256, out);
    lua_pushlstring(lua, (char *)out, sizeof(out));
    return 1;
}
static int32_t _luaopen_srey_sha256(lua_State *lua) {
    luaL_Reg reg[] = {
        { "init", _lalgo_sha256_init },
        { "update", _lalgo_sha256_update },
        { "final", _lalgo_sha256_final },
        { NULL, NULL }
    };
    _reg_class(lua, "sha256_new", "_sha256_ctx", reg, _lalgo_sha256_new, NULL);
    return 0;
}
static int32_t _lalgo_hmac_sha256_new(lua_State *lua) {
    size_t lens;
    const char *key = luaL_checklstring(lua, 1, &lens);
    hmac_sha256_ctx *mac256 = lua_newuserdata(lua, sizeof(hmac_sha256_ctx));
    hmac_sha256_key(mac256, (uint8_t *)key, lens);
    luaL_getmetatable(lua, "_hmac_sha256_ctx");
    lua_setmetatable(lua, -2);
    return 1;
}
static int32_t _lalgo_hmac_sha256_init(lua_State *lua) {
    hmac_sha256_ctx *mac256 = lua_touserdata(lua, 1);
    hmac_sha256_init(mac256);
    return 0;
}
static int32_t _lalgo_hmac_sha256_update(lua_State *lua) {
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
static int32_t _lalgo_hmac_sha256_final(lua_State *lua) {
    hmac_sha256_ctx *mac256 = lua_touserdata(lua, 1);
    unsigned char out[SHA256_BLOCK_SIZE];
    hmac_sha256_final(mac256, out);
    lua_pushlstring(lua, (char *)out, sizeof(out));
    return 1;
}
static int32_t _luaopen_srey_hmac_sha256(lua_State *lua) {
    luaL_Reg reg[] = {
        { "init", _lalgo_hmac_sha256_init },
        { "update", _lalgo_hmac_sha256_update },
        { "final", _lalgo_hmac_sha256_final },
        { NULL, NULL }
    };
    _reg_class(lua, "hmac_sha256_new", "_hmac_sha256_ctx", reg, _lalgo_hmac_sha256_new, NULL);
    return 0;
}
static int32_t _lalgo_hash_ring_new(lua_State *lua) {
    uint32_t n = (uint32_t)luaL_checkinteger(lua, 1);
    HASH_FUNCTION func = (HASH_FUNCTION)luaL_checkinteger(lua, 2);
    hash_ring_t *ring = hash_ring_create(n, func);
    if (NULL == ring) {
        return 0;
    }
    *(hash_ring_t**)lua_newuserdata(lua, sizeof(hash_ring_t*)) = ring;
    luaL_getmetatable(lua, "_hash_ring");
    lua_setmetatable(lua, -2);
    return 1;
}
static int32_t _lalgo_hash_ring_del(lua_State *lua) {
    hash_ring_free(*(hash_ring_t**)lua_touserdata(lua, 1));
    return 0;
}
static int32_t _lalgo_hash_ring_mode(lua_State *lua) {
    hash_ring_t *ring = *(hash_ring_t**)lua_touserdata(lua, 1);
    HASH_MODE mode = (HASH_MODE)luaL_checkinteger(lua, 2);
    if (ERR_OK != hash_ring_set_mode(ring, mode)) {
        return luaL_error(lua, "hash_ring_set_mode failed.");
    }
    return 0;
}
static int32_t _lalgo_hash_ring_print(lua_State *lua) {
    hash_ring_t *ring = *(hash_ring_t**)lua_touserdata(lua, 1);
    hash_ring_print(ring);
    return 0;
}
static int32_t _lalgo_hash_ring_add_node(lua_State *lua) {
    hash_ring_t *ring = *(hash_ring_t**)lua_touserdata(lua, 1);
    void *name;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 2)) {
        name = (void *)luaL_checklstring(lua, 2, &size);
    } else {
        name = lua_touserdata(lua, 2);
        size = (size_t)luaL_checkinteger(lua, 3);
    }
    if (ERR_OK != hash_ring_add_node(ring, name, size)) {
        return luaL_error(lua, "hash_ring_add_node failed.");
    }
    return 0;
}
static int32_t _lalgo_hash_ring_remove_node(lua_State *lua) {
    hash_ring_t *ring = *(hash_ring_t**)lua_touserdata(lua, 1);
    void *name;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 2)) {
        name = (void *)luaL_checklstring(lua, 2, &size);
    } else {
        name = lua_touserdata(lua, 2);
        size = (size_t)luaL_checkinteger(lua, 3);
    }
    hash_ring_remove_node(ring, name, size);
    return 0;
}
static int32_t _lalgo_hash_ring_get_node(lua_State *lua) {
    hash_ring_t *ring = *(hash_ring_t**)lua_touserdata(lua, 1);
    void *name;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 2)) {
        name = (void *)luaL_checklstring(lua, 2, &size);
    } else {
        name = lua_touserdata(lua, 2);
        size = (size_t)luaL_checkinteger(lua, 3);
    }
    hash_ring_node_t *node = hash_ring_get_node(ring, name, size);
    if (NULL == node) {
        lua_pushnil(lua);
    } else {
        lua_pushlstring(lua, (const char *)node->name, node->nameLen);
    }
    return 1;
}
static int32_t _lalgo_hash_ring_find_node(lua_State *lua) {
    hash_ring_t *ring = *(hash_ring_t**)lua_touserdata(lua, 1);
    void *key;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 2)) {
        key = (void *)luaL_checklstring(lua, 2, &size);
    } else {
        key = lua_touserdata(lua, 2);
        size = (size_t)luaL_checkinteger(lua, 3);
    }
    hash_ring_node_t *node = hash_ring_find_node(ring, key, size);
    if (NULL == node) {
        lua_pushnil(lua);
    } else {
        lua_pushlstring(lua, (const char *)node->name, node->nameLen);
    }
    return 1;
}
static int32_t _lalgo_hash_ring_find_nodes(lua_State *lua) {
    hash_ring_t *ring = *(hash_ring_t**)lua_touserdata(lua, 1);
    uint32_t n = (uint32_t)luaL_checkinteger(lua, 2);
    void *key;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 3)) {
        key = (void *)luaL_checklstring(lua, 3, &size);
    } else {
        key = lua_touserdata(lua, 3);
        size = (size_t)luaL_checkinteger(lua, 4);
    }
    hash_ring_node_t *nodes;
    MALLOC(nodes, sizeof(hash_ring_node_t *) * n);
    int32_t nfind = hash_ring_find_nodes(ring, key, size, (hash_ring_node_t **)nodes, n);
    if (ERR_FAILED == nfind) {
        FREE(nodes);
        return 0;
    }
    lua_createtable(lua, 0, nfind);
    for (int32_t i = 0; i < nfind; i++) {
        lua_pushinteger(lua, i + 1);
        lua_pushlstring(lua, (const char *)nodes[i].name, nodes[i].nameLen);
        lua_settable(lua, -3);
    }
    FREE(nodes);
    return 1;
}
static int32_t _luaopen_srey_hash_ring(lua_State *lua) {
    luaL_Reg reg[] = {
        { "mode", _lalgo_hash_ring_mode },
        { "print", _lalgo_hash_ring_print },
        { "add", _lalgo_hash_ring_add_node },
        { "remove", _lalgo_hash_ring_remove_node },
        { "get", _lalgo_hash_ring_get_node },
        { "find", _lalgo_hash_ring_find_node },
        { "finds", _lalgo_hash_ring_find_nodes },
        { NULL, NULL }
    };
    _reg_class(lua, "hash_ring_new", "_hash_ring", reg, _lalgo_hash_ring_new, _lalgo_hash_ring_del);
    return 0;
}
static int32_t _lalgo_sha1_b64(lua_State *lua) {
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 1)) {
        data = (void *)luaL_checklstring(lua, 1, &size);
    } else {
        data = lua_touserdata(lua, 1);
        size = (size_t)luaL_checkinteger(lua, 2);
    }
    sha1_ctx sha1;
    unsigned char outsh[SHA1_BLOCK_SIZE];
    sha1_init(&sha1);
    sha1_update(&sha1, data, size);
    sha1_final(&sha1, outsh);

    char b64[B64EN_BLOCK_SIZE(sizeof(outsh))];
    bs64_encode((const char *)outsh, sizeof(outsh), b64);
    lua_pushstring(lua, b64);
    return 1;
}
static int32_t _lalgo_url_encode(lua_State *lua) {
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
LUAMOD_API int luaopen_srey_algo(lua_State *lua) {
    _luaopen_srey_md5(lua);
    _luaopen_srey_sha1(lua);
    _luaopen_srey_sha256(lua);
    _luaopen_srey_hash_ring(lua);
    _luaopen_srey_hmac_sha256(lua);

    luaL_Reg reg[] = {
        { "b64_encode", _lalgo_b64_encode },
        { "b64_decode", _lalgo_b64_decode },

        { "crc16", _lalgo_crc16 },
        { "crc32", _lalgo_crc32 },

        { "md5_update", _lalgo_md5_update },
        { "md5_final", _lalgo_md5_final },

        { "url_encode", _lalgo_url_encode },

        { "sha1_b64",_lalgo_sha1_b64 },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}

#endif
