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
/// 注册路由条目并返回索引（≥0）；注册失败返回 false 加 C 侧的失败码
/// </summary>
/// <param name="self" type="userdata">router_ctx 对象</param>
/// <param name="method" type="string">HTTP 方法，如 "GET"/"POST"/"ANY"</param>
/// <param name="path" type="string">完整路由路径（调用方已拼好前缀）</param>
/// <returns type="boolean">true=注册成功；false=被拒</returns>
/// <returns type="integer">成功时是路由索引（≥0）；失败时是 router_add_index 的失败码
/// （-1 路径非法或方法未知，-2 已有等价路由把它遮住），调用方据此给出不同提示</returns>
static int32_t _lrouter_add(lua_State *lua) {
    LPUB_UD_ARG(lua, router_ctx, MT_ROUTER, pr, "router freed");
    size_t mlen;
    const char *method = luaL_checklstring(lua, 2, &mlen);
    size_t plen;
    const char *path = luaL_checklstring(lua, 3, &plen);
    int32_t idx = router_add_index(*pr, method, mlen, path, plen);
    lua_pushboolean(lua, idx >= 0);
    lua_pushinteger(lua, idx);// 成功是索引，失败是失败码；两条路径返回值个数一致
    return 2;
}
// dispatch 只用 path 与 param 两个字段（见 router.lua 的 _make_ctx），这里就只压这两个。
// 不复用 lpub_push_url_table：那个是 url.parse 的通用序列化器，每次会额外造出 segs 数组
// 加每段一个 Lua 字符串，还要重组一遍 query 串——对每个 HTTP 请求都白付一次
static void _lrouter_push_url(lua_State *lua, url_ctx *url) {
    lua_createtable(lua, 0, 2);
    // "/" 与 "//" 的段全是空段，被 router_match_index 剔光后 npath 归零，
    // 此时 url_reorg_path 只会吐出空串，直接给 "/"，别让 path 字段缺席
    if (url->npath > 0) {
        luaL_Buffer pbuf;
        size_t pcap = url->pathlens + 1;
        char *pp = luaL_buffinitsize(lua, &pbuf, pcap);
        luaL_pushresultsize(&pbuf, url_reorg_path(url, pp, pcap));
    } else {
        lua_pushliteral(lua, "/");
    }
    lua_setfield(lua, -2, "path");
    // 按实际参数个数建表，不按 URL_MAX_PARAM 预留
    lua_createtable(lua, 0, url->nparam);
    url_param *param;
    for (int32_t i = 0; i < url->nparam; i++) {
        param = &url->param[i];
        lua_pushlstring(lua, param->key.data, param->key.lens);
        if (buf_empty(&param->val)) {
            lua_pushstring(lua, "");
        } else {
            lua_pushlstring(lua, param->val.data, param->val.lens);
        }
        lua_settable(lua, -3);
    }
    lua_setfield(lua, -2, "param");
}
/// <summary>
/// 匹配请求路径（不执行 handler/中间件）
/// </summary>
/// <param name="self" type="userdata">router_ctx 对象</param>
/// <param name="method" type="string">HTTP 方法字符串</param>
/// <param name="url" type="string">原始请求 URI（含查询字符串）</param>
/// <returns type="boolean">true=命中；false=失败</returns>
/// <returns type="integer">HTTP 状态码：200 命中 / 400 url 解析失败 / 404 无匹配路由 / 405 方法不在已知列表</returns>
/// <returns type="table?">{ path, param } —— 仅命中时返回；未命中一律只返前两个值</returns>
/// <returns type="integer?">路由索引（≥0）；仅命中时返回</returns>
/// <returns type="table?">路径参数表；仅命中时返回</returns>
static int32_t _lrouter_match(lua_State *lua) {
    LPUB_UD_ARG(lua, router_ctx, MT_ROUTER, pr, "router freed");
    size_t mlen;
    const char *method = luaL_checklstring(lua, 2, &mlen);
    size_t ulen;
    const char *url = luaL_checklstring(lua, 3, &ulen);
    url_ctx urlstorage;
    router_req ctx;
    ZERO(&ctx, sizeof(ctx));
    ctx.url = &urlstorage;
    int32_t idx = router_match_index(*pr, method, mlen, url, ulen, &ctx);
    int32_t code = router_match_code(idx);
    lua_pushboolean(lua, idx >= 0);
    lua_pushinteger(lua, code);
    // 未命中(400/405/404)时不建 url 表：dispatch 那边 if not ok 就直接回响应了，建了也没人看
    if (idx < 0) {
        return 2;
    }
    _lrouter_push_url(lua, ctx.url);
    lua_pushinteger(lua, idx);
    lua_createtable(lua, 0, ctx.params_n);
    for (int32_t i = 0; i < ctx.params_n; i++) {
        lua_pushlstring(lua, ctx.params[i].key, ctx.params[i].key_len);
        lua_pushlstring(lua, ctx.params[i].val, ctx.params[i].val_len);
        lua_rawset(lua, -3);
    }
    return 5;
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
