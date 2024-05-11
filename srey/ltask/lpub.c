#include "ltask/lpub.h"
#if WITH_LUA

void *global_userdata(lua_State *lua, const char *name) {
    lua_pop(lua, -1);
    if (!lua_getglobal(lua, name)) {
        return NULL;
    }
    void *data = NULL;
    if (lua_islightuserdata(lua, 1)) {
        data = lua_touserdata(lua, 1);
    }
    lua_pop(lua, -1);
    return data;
}
const char *global_string(lua_State *lua, const char *name) {
    lua_pop(lua, -1);
    if (!lua_getglobal(lua, name)) {
        return NULL;
    }
    char *data = NULL;
    if (lua_isstring(lua, 1)) {
        data = (char *)lua_tostring(lua, 1);
    }
    lua_pop(lua, -1);
    return data;
}

#endif