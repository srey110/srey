#include "lbind/lpub.h"

void *global_userdata(lua_State *lua, const char *name) {
    if (LUA_TLIGHTUSERDATA != lua_getglobal(lua, name)) {
        lua_pop(lua, 1);
        return NULL;
    }
    void *data = lua_touserdata(lua, -1);
    lua_pop(lua, 1);
    return data;
}
const char *global_string(lua_State *lua, const char *name) {
    if (LUA_TSTRING != lua_getglobal(lua, name)) {
        lua_pop(lua, 1);
        return NULL;
    }
    const char *data = lua_tostring(lua, -1);
    lua_pop(lua, 1);
    return data;
}
