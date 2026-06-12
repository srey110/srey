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
void *lpub_check_buf_idx(lua_State *lua, int32_t *idx, size_t *size, int32_t *copy) {
    int32_t type = lua_type(lua, *idx);
    if (LUA_TSTRING == type) {
        const char *s = luaL_checklstring(lua, *idx, size);
        if (NULL != copy) {
            *copy = 1;
        }
        *idx += 1;// string 占 1 位
        return (void *)s;
    }
    if (LUA_TLIGHTUSERDATA == type) {
        void *ud = lua_touserdata(lua, *idx);
        *size = (size_t)luaL_checkinteger(lua, *idx + 1);
        *idx += 2;// 先吃掉 data + size,*idx 转到 copy 位
        if (NULL != copy) {
            if (lua_isinteger(lua, *idx)) {
                *copy = (int32_t)luaL_checkinteger(lua, *idx);
                *idx += 1;// copy 命中再 +1
            } else {
                *copy = 1;
            }
        }
        return ud;
    }
    luaL_argerror(lua, *idx, "string or light userdata expected");
    return NULL;//unreachable: luaL_argerror longjmp
}
// idx 按值的兼容包装,丢弃推进位置;旧调用方无需改动
void *lpub_check_buf(lua_State *lua, int32_t idx, size_t *size, int32_t *copy) {
    return lpub_check_buf_idx(lua, &idx, size, copy);
}
