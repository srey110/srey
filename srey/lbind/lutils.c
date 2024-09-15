#include "lbind/lpub.h"

#if WITH_LUA

static int32_t _lutils_log_setlv(lua_State *lua) {
    LOG_LEVEL lv = (LOG_LEVEL)luaL_checkinteger(lua, 1);
    log_setlv(lv);
    return 0;
}
static int32_t _lutils_log(lua_State *lua) {
    LOG_LEVEL lv = (LOG_LEVEL)luaL_checkinteger(lua, 1);
    const char *file = luaL_checkstring(lua, 2);
    int32_t line = (int32_t)luaL_checkinteger(lua, 3);
    const char *log = luaL_checkstring(lua, 4);
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (NULL == task) {
        slog(lv, "[%s %d] %s", __FILENAME__(file), line, log);
    } else {
        slog(lv, "[%s %d][%d] %s", __FILENAME__(file), line, task->name, log);
    }
    return 0;
}
static int32_t _lutils_ud_str(lua_State *lua) {
    void *data = lua_touserdata(lua, 1);
    size_t size = (size_t)luaL_checkinteger(lua, 2);
    if (NULL == data) {
        lua_pushnil(lua);
        return 1;
    }
    lua_pushlstring(lua, data, size);
    return 1;
}
static int32_t _lutils_hex(lua_State *lua) {
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 1)) {
        data = (void *)luaL_checklstring(lua, 1, &size);
    } else {
        data = lua_touserdata(lua, 1);
        size = (size_t)luaL_checkinteger(lua, 2);
    }
    char *out;
    MALLOC(out, HEX_ENSIZE(size));
    tohex(data, size, out);
    lua_pushstring(lua, out);
    FREE(out);
    return 1;
}
static int32_t _lutils_id(lua_State *lua) {
    lua_pushinteger(lua, createid());
    return 1;
}
static int32_t _lutils_remote_addr(lua_State *lua) {
    netaddr_ctx addr;
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    if (-1 == fd) {
        lua_pushnil(lua);
        return 1;
    }
    if (ERR_OK != netaddr_remote(&addr, fd)) {
        lua_pushnil(lua);
        return 1;
    }
    char ip[IP_LENS];
    if (ERR_OK != netaddr_ip(&addr, ip)) {
        lua_pushnil(lua);
        return 1;
    }
    uint16_t port = netaddr_port(&addr);
    lua_pushstring(lua, ip);
    lua_pushinteger(lua, port);
    return 2;
}
//srey.utils
LUAMOD_API int luaopen_utils(lua_State *lua) {
    luaL_Reg reg[] = {
        { "log_setlv", _lutils_log_setlv },
        { "log", _lutils_log },
        { "ud_str", _lutils_ud_str },
        { "hex", _lutils_hex },
        { "id", _lutils_id },
        { "remote_addr", _lutils_remote_addr },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
static int32_t _lhash_ring_new(lua_State *lua) {
    hash_ring_ctx *ring = lua_newuserdata(lua, sizeof(hash_ring_ctx));
    hash_ring_init(ring);
    ASSOC_MTABLE(lua, "_hash_ring_ctx");
    return 1;
}
static int32_t _lhash_ring_free(lua_State *lua) {
    hash_ring_ctx *ring = lua_touserdata(lua, 1);
    hash_ring_free(ring);
    return 0;
}
static int32_t _lhash_ring_add(lua_State *lua) {
    hash_ring_ctx *ring = lua_touserdata(lua, 1);
    uint32_t nreplicas = (uint32_t)luaL_checkinteger(lua, 2);
    size_t lens;
    void *name = NULL;
    if (LUA_TSTRING == lua_type(lua, 3)) {
        name = (void *)luaL_checklstring(lua, 3, &lens);
    } else {
        name = lua_touserdata(lua, 3);
        lens = (size_t)luaL_checkinteger(lua, 4);
    }
    if (ERR_OK == hash_ring_add(ring, name, lens, nreplicas)) {
        lua_pushboolean(lua, 1);
    } else {
        lua_pushboolean(lua, 0);
    }
    return 1;
}
static int32_t _lhash_ring_remove(lua_State *lua) {
    hash_ring_ctx *ring = lua_touserdata(lua, 1);
    size_t lens;
    void *name = NULL;
    if (LUA_TSTRING == lua_type(lua, 2)) {
        name = (void *)luaL_checklstring(lua, 2, &lens);
    } else {
        name = lua_touserdata(lua, 2);
        lens = (size_t)luaL_checkinteger(lua, 3);
    }
    hash_ring_remove(ring, name, lens);
    return 0;
}
static int32_t _lhash_ring_find(lua_State *lua) {
    hash_ring_ctx *ring = lua_touserdata(lua, 1);
    size_t lens;
    void *key = NULL;
    if (LUA_TSTRING == lua_type(lua, 2)) {
        key = (void *)luaL_checklstring(lua, 2, &lens);
    } else {
        key = lua_touserdata(lua, 2);
        lens = (size_t)luaL_checkinteger(lua, 3);
    }
    hash_ring_node *node = hash_ring_find(ring, key, lens);
    if (NULL == node) {
        lua_pushnil(lua);
    } else {
        lua_pushlstring(lua, node->name, node->lens);
    }
    return 1;
}
static int32_t _lhash_ring_print(lua_State *lua) {
    hash_ring_ctx *ring = lua_touserdata(lua, 1);
    hash_ring_print(ring);
    return 0;
}
//srey.hashring
LUAMOD_API int luaopen_hashring(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lhash_ring_new },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "add", _lhash_ring_add },
        { "remove", _lhash_ring_remove },
        { "find", _lhash_ring_find },
        { "print", _lhash_ring_print },
        { "__gc", _lhash_ring_free },
        { NULL, NULL }
    };
    REG_MTABLE(lua, "_hash_ring_ctx", reg_new, reg_func);
    return 1;
}

#endif
