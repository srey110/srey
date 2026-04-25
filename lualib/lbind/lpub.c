#include "lbind/lpub.h"

// 从 Lua 全局变量表中取轻量用户数据，类型不符则弹栈返回 NULL
void *global_userdata(lua_State *lua, const char *name) {
    if (LUA_TLIGHTUSERDATA != lua_getglobal(lua, name)) {
        lua_pop(lua, 1);
        return NULL;
    }
    void *data = lua_touserdata(lua, -1);
    lua_pop(lua, 1);
    return data;
}

// 从 Lua 全局变量表中取字符串，类型不符则弹栈返回 NULL
const char *global_string(lua_State *lua, const char *name) {
    if (LUA_TSTRING != lua_getglobal(lua, name)) {
        lua_pop(lua, 1);
        return NULL;
    }
    const char *data = lua_tostring(lua, -1);
    lua_pop(lua, 1);
    return data;
}
