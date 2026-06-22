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
void lpub_push_url_table(lua_State *lua, url_ctx *url) {
    lua_createtable(lua, 0, 9);
    if (!buf_empty(&url->scheme)) {
        lua_pushlstring(lua, url->scheme.data, url->scheme.lens);
        lua_setfield(lua, -2, "scheme");
    }
    if (!buf_empty(&url->user)) {
        lua_pushlstring(lua, url->user.data, url->user.lens);
        lua_setfield(lua, -2, "user");
    }
    if (!buf_empty(&url->psw)) {
        lua_pushlstring(lua, url->psw.data, url->psw.lens);
        lua_setfield(lua, -2, "psw");
    }
    if (!buf_empty(&url->host)) {
        lua_pushlstring(lua, url->host.data, url->host.lens);
        lua_setfield(lua, -2, "host");
    }
    if (!buf_empty(&url->port)) {
        lua_pushlstring(lua, url->port.data, url->port.lens);
        lua_setfield(lua, -2, "port");
    }
    if (url->npath > 0) {
        char pathbuf[URL_BUF_LENS];
        size_t plen = url_reorg_path(url, pathbuf, sizeof(pathbuf));
        lua_pushlstring(lua, pathbuf, plen);
        lua_setfield(lua, -2, "path");
    }
    lua_createtable(lua, url->npath > 0 ? url->npath : 0, 0);
    for (int32_t i = 0; i < url->npath; i++) {
        lua_pushlstring(lua, url->segs[i].data, url->segs[i].lens);
        lua_rawseti(lua, -2, i + 1);
    }
    lua_setfield(lua, -2, "segs");
    if (!buf_empty(&url->anchor)) {
        lua_pushlstring(lua, url->anchor.data, url->anchor.lens);
        lua_setfield(lua, -2, "anchor");
    }
    lua_createtable(lua, 0, URL_MAX_PARAM);
    url_param *param;
    for (int32_t i = 0; i < URL_MAX_PARAM; i++) {
        param = &url->param[i];
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
    lua_setfield(lua, -2, "param");
    if (!buf_empty(&url->param[0].key)) {
        char querybuf[URL_BUF_LENS];
        size_t qlen = url_reorg_param(url, querybuf, sizeof(querybuf));
        if (qlen > 0) {
            lua_pushlstring(lua, querybuf, qlen);
            lua_setfield(lua, -2, "query");
        }
    }
}
