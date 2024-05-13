#include "ltask/lpub.h"

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
    slog(lv, "[%s %d] %s", __FILENAME__(file), line, log);
    return 0;
}
static int32_t _lutils_ud_str(lua_State *lua) {
    void *data = lua_touserdata(lua, 1);
    size_t size = (size_t)luaL_checkinteger(lua, 2);
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

#endif
