#include "lbind/lpub.h"

#define MT_ROUTER "_router_ctx"

static int32_t _lrouter_new(lua_State *lua) {
    router_ctx **pr = lua_newuserdatauv(lua, sizeof(router_ctx *), 0);
    *pr = router_new();
    ASSOC_MTABLE(lua, MT_ROUTER);
    return 1;
}
static int32_t _lrouter_free(lua_State *lua) {
    router_ctx **pr = luaL_checkudata(lua, 1, MT_ROUTER);
    if (NULL != *pr) {
        router_free(*pr);
        *pr = NULL;
    }
    return 0;
}
/// <summary>
/// 注册路由条目并返回索引（≥0）；路径/方法非法返回 false
/// </summary>
/// <param name="self" type="userdata">router_ctx 对象</param>
/// <param name="method" type="string">HTTP 方法，如 "GET"/"POST"/"ANY"</param>
/// <param name="path" type="string">完整路由路径（调用方已拼好前缀）</param>
/// <returns type="boolean">true=注册成功；false=路径/方法非法</returns>
/// <returns type="integer?">路由索引（≥0）；仅成功时返回</returns>
static int32_t _lrouter_add(lua_State *lua) {
    router_ctx **pr = luaL_checkudata(lua, 1, MT_ROUTER);
    if (NULL == *pr) {
        return luaL_error(lua, "router freed");
    }
    size_t mlen;
    const char *method = luaL_checklstring(lua, 2, &mlen);
    size_t plen;
    const char *path = luaL_checklstring(lua, 3, &plen);
    int32_t idx = router_add_index(*pr, method, mlen, path, plen);
    if (idx < 0) {
        lua_pushboolean(lua, 0);
        return 1;
    }
    lua_pushboolean(lua, 1);
    lua_pushinteger(lua, idx);
    return 2;
}
/// <summary>
/// 匹配请求路径（不执行 handler/中间件）
/// </summary>
/// <param name="self" type="userdata">router_ctx 对象</param>
/// <param name="method" type="string">HTTP 方法字符串</param>
/// <param name="url" type="string">原始请求 URI（含查询字符串）</param>
/// <returns type="boolean">true=命中；false=失败（400 url 解析失败 / 404 无匹配路由）</returns>
/// <returns type="table?">url_table；解析成功时返回（400 时不返回）</returns>
/// <returns type="integer?">路由索引（≥0）；仅命中时返回</returns>
/// <returns type="table?">路径参数表；仅命中时返回</returns>
static int32_t _lrouter_match(lua_State *lua) {
    router_ctx **pr = luaL_checkudata(lua, 1, MT_ROUTER);
    if (NULL == *pr) {
        return luaL_error(lua, "router freed");
    }
    size_t mlen;
    const char *method = luaL_checklstring(lua, 2, &mlen);
    size_t ulen;
    const char *url = luaL_checklstring(lua, 3, &ulen);
    router_req ctx;
    ZERO(&ctx, sizeof(ctx));
    int32_t idx = router_match_index(*pr, method, mlen, url, ulen, &ctx);
    if (-2 == idx) {
        lua_pushboolean(lua, 0);
        return 1;
    }
    lua_pushboolean(lua, idx >= 0);
    lpub_push_url_table(lua, &ctx.url_storage);
    if (idx < 0) {
        return 2;
    }
    lua_pushinteger(lua, idx);
    lua_createtable(lua, 0, ctx.params_n);
    for (int32_t i = 0; i < ctx.params_n; i++) {
        lua_pushlstring(lua, ctx.params[i].key, ctx.params[i].key_len);
        lua_pushlstring(lua, ctx.params[i].val, ctx.params[i].val_len);
        lua_rawset(lua, -3);
    }
    return 4;
}
//srey.router
LUAMOD_API int luaopen_router(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lrouter_new },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "add", _lrouter_add },
        { "match", _lrouter_match },
        { "__gc", _lrouter_free },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_ROUTER, reg_new, reg_func);
    return 1;
}
