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
// 从 Lua 全局变量表中取字符串并复制到 buf(NUL 结尾)。
// 复制而非返回内部指针：lua_tostring 返回的指针在 string 弹栈后,理论可被 GC 释放
int32_t global_string(lua_State *lua, const char *name, char *buf, size_t bufsize) {
    if (EMPTYPTR(buf, bufsize)) {
        return ERR_FAILED;
    }
    buf[0] = '\0';
    if (LUA_TSTRING != lua_getglobal(lua, name)) {
        lua_pop(lua, 1);
        return ERR_FAILED;
    }
    size_t lens;
    const char *data = lua_tolstring(lua, -1, &lens);
    if (lens >= bufsize) {
        lua_pop(lua, 1);
        return ERR_FAILED;
    }
    memcpy(buf, data, lens);
    buf[lens] = '\0';
    lua_pop(lua, 1);
    return ERR_OK;
}
void *lpub_check_buf(lua_State *lua, int32_t idx, size_t *size, int32_t *copy) {
    int32_t type = lua_type(lua, idx);
    if (LUA_TSTRING == type) {
        if (NULL != copy) {
            *copy = 1;
        }
        return (void *)luaL_checklstring(lua, idx, size);
    }
    if (LUA_TLIGHTUSERDATA == type) {
        *size = (size_t)luaL_checkinteger(lua, idx + 1);
        if (NULL != copy) {
            *copy = lua_isinteger(lua, idx + 2) ? (int32_t)luaL_checkinteger(lua, idx + 2) : 1;
        }
        return lua_touserdata(lua, idx);
    }
    luaL_argerror(lua, idx, "string or light userdata expected");
    return NULL;//unreachable: luaL_argerror longjmp
}
