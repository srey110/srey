#include "lbind/lpub.h"

#define MT_ROUTER "_router_ctx"

/// <summary>
/// 创建一个路由表
/// </summary>
/// <returns type="_router_ctx">router 对象</returns>
static int32_t _lrouter_new(lua_State *lua) {
    router_ctx *r = router_new();
    router_set_own_chain(r, 1);// 执行链由 Lua runner 跑，C 的 16 层上限管不到它
    lpub_push_ud(lua, r, MT_ROUTER);
    return 1;
}
static int32_t _lrouter_free(lua_State *lua) {
    router_ctx **pr = lpub_check_udata(lua, 1, MT_ROUTER);
    if (NULL != *pr) {
        router_free(*pr);
        *pr = NULL;
    }
    return 0;
}
// Lua 回调在 C 链里只是占位; 真正的函数由 Lua 侧 runner 按 arg 下标取出。
static void _lrouter_marker(router_req *ctx, void *arg) {
    (void)ctx;
    (void)arg;
}
// 流式路由的 C 链占位; 真正回调由 Lua 侧按 arg 下标取出，不在这里执行。
static void _lrouter_stream_marker(router_req *ctx, uint8_t slice, void *data, size_t lens, void *arg) {
    (void)ctx;
    (void)slice;
    (void)data;
    (void)lens;
    (void)arg;
}
static void _lrouter_push_segs(lua_State *lua, router_ctx *r, int32_t idx);
// 把 Lua _fns 下标装进 C 的 void *arg; 不解引用，只原样交回 Lua。
static void *_lrouter_arg(lua_Integer idx) {
    return (void *)(intptr_t)idx;
}
/// <summary>
/// 注册一条带 Lua 回调索引的路由；同 _lrouter_add，但 handler/mws 由 C 执行链保存。
/// </summary>
/// <param name="self" type="userdata">router_ctx 对象</param>
/// <param name="method" type="string">HTTP 方法，如 "GET"/"POST"/"ANY"</param>
/// <param name="path" type="string">完整路由路径（调用方已拼好前缀）</param>
/// <param name="handler_idx" type="integer">handler 在 Lua _fns 表里的下标（≥1）</param>
/// <param name="mws" type="integer[]">路由级中间件在 _fns 表里的下标数组；空表或 nil 表示无中间件</param>
/// <param name="stream" type="boolean">true=流式路由，false=普通路由</param>
/// <returns type="boolean">true=注册成功；false=被拒</returns>
/// <returns type="integer">成功时是路由索引（≥0）；失败时是 -1</returns>
/// <returns type="(string|RouterSeg)[]?">成功时是段序列；失败时为 nil</returns>
static int32_t _lrouter_add_steps(lua_State *lua) {
    LPUB_UD_ARG(lua, router_ctx, MT_ROUTER, pr, "router freed");
    size_t mlen;
    const char *method = luaL_checklstring(lua, 2, &mlen);
    size_t plen;
    const char *path = luaL_checklstring(lua, 3, &plen);
    lua_Integer hidx = luaL_checkinteger(lua, 4);
    int32_t mws_n = 0;
    if (lua_istable(lua, 5)) {
        mws_n = (int32_t)luaL_len(lua, 5);
    }
    int32_t stream = lua_toboolean(lua, 6);
    router_method mask = router_method_mask(method, mlen);
    if (0 == mask || hidx <= 0 || mws_n < 0) {
        lua_pushboolean(lua, 0);
        lua_pushinteger(lua, -1);
        lua_pushnil(lua);
        return 3;
    }
    router_step *mws = NULL;
    if (mws_n > 0) {
        MALLOC(mws, sizeof(router_step) * (size_t)mws_n);
    }
    int32_t isnum;
    lua_Integer idx;
    for (int32_t i = 0; i < mws_n; i++) {
        isnum = 0;
        lua_rawgeti(lua, 5, i + 1);
        idx = lua_tointegerx(lua, -1, &isnum);
        lua_pop(lua, 1);
        if (!isnum || idx <= 0) {
            FREE(mws);
            lua_pushboolean(lua, 0);
            lua_pushinteger(lua, -1);
            lua_pushnil(lua);
            return 3;
        }
        mws[i].cb = _lrouter_marker;
        mws[i].arg = _lrouter_arg(idx);
    }
    char *pathz = dup_zero(path, plen);
    router_entry *e;
    if (stream) {
        e = router_add_stream_steps(*pr, NULL, mask, pathz,
                                    _lrouter_stream_marker, _lrouter_arg(hidx), mws, mws_n);
    } else {
        e = router_add_steps(*pr, NULL, mask, pathz,
                             _lrouter_marker, _lrouter_arg(hidx), mws, mws_n);
    }
    FREE(pathz);
    FREE(mws);
    if (NULL == e) {
        lua_pushboolean(lua, 0);
        lua_pushinteger(lua, -1);
        lua_pushnil(lua);
        return 3;
    }
    int32_t route_id = router_entry_id(e);
    lua_pushboolean(lua, 1);
    lua_pushinteger(lua, route_id);
    _lrouter_push_segs(lua, *pr, route_id);
    return 3;
}
/// <summary>
/// 注册一个全局中间件回调，顺序按调用顺序追加到 C 执行链头。
/// </summary>
/// <param name="self" type="userdata">router_ctx 对象</param>
/// <param name="idx" type="integer">中间件在 Lua _fns 表里的下标（≥1）</param>
/// <returns type="boolean">true=C 侧注册成功；false=下标非法</returns>
static int32_t _lrouter_use_step(lua_State *lua) {
    LPUB_UD_ARG(lua, router_ctx, MT_ROUTER, pr, "router freed");
    lua_Integer idx = luaL_checkinteger(lua, 2);
    int32_t rtn = idx <= 0 ? ERR_FAILED : router_use_fn(*pr, _lrouter_marker, _lrouter_arg(idx));
    lua_pushboolean(lua, ERR_OK == rtn);
    return 1;
}
/// <summary>
/// 取 C 执行链的函数下标数组，供 Lua 侧通用 runner 执行；流式路由不含准入哨兵。
/// </summary>
/// <param name="self" type="userdata">router_ctx 对象</param>
/// <param name="idx" type="integer">路由索引（≥0，add_steps 的返回）</param>
/// <returns type="integer[]?">函数下标数组；路由索引越界时为 nil</returns>
static int32_t _lrouter_chain(lua_State *lua) {
    LPUB_UD_ARG(lua, router_ctx, MT_ROUTER, pr, "router freed");
    int32_t idx = (int32_t)luaL_checkinteger(lua, 2);
    int32_t n = router_chain_count(*pr, idx);
    if (n < 0) {
        lua_pushnil(lua);
        return 1;
    }
    lua_createtable(lua, n, 0);
    router_step step;
    for (int32_t k = 0; k < n; k++) {
        if (ERR_OK == router_chain_at(*pr, idx, k, &step)
            && step.cb == _lrouter_marker) {
            lua_pushinteger(lua, (lua_Integer)(intptr_t)step.arg);
        } else {
            lua_pushnil(lua);
        }
        lua_rawseti(lua, -2, k + 1);
    }
    return 1;
}
// 把注册时解析好的段序列交回 Lua：字面量段压字符串，占位符段压 { key, opt }。
// 段全部取完(router_seg_index 报越界)才收手，故根路径这种零段路由得到一张空表
static void _lrouter_push_segs(lua_State *lua, router_ctx *r, int32_t idx) {
    router_seg_type t;
    const char *str;
    uint32_t str_len;
    int32_t k = 0;
    lua_newtable(lua);
    while (ERR_OK == router_seg_index(r, idx, k, &t, &str, &str_len)) {
        if (ROUTER_SEG_LIT == t) {
            lua_pushlstring(lua, str, str_len);
        } else {
            lua_createtable(lua, 0, 2);
            if (ROUTER_SEG_WILD == t) {
                lua_pushliteral(lua, "*");
            } else {
                lua_pushlstring(lua, str, str_len);
            }
            lua_setfield(lua, -2, "key");
            lua_pushboolean(lua, ROUTER_SEG_PARAM != t);
            lua_setfield(lua, -2, "opt");
        }
        lua_rawseti(lua, -2, k + 1);
        k++;
    }
}
/// <summary>
/// 注册一条路由
/// </summary>
/// <param name="self" type="userdata">router_ctx 对象</param>
/// <param name="method" type="string">HTTP 方法，如 "GET"/"POST"/"ANY"</param>
/// <param name="path" type="string">完整路由路径（调用方已拼好前缀）</param>
/// <returns type="boolean">true=注册成功；false=被拒</returns>
/// <returns type="integer">成功时是路由索引（≥0）；失败时是 router_add_index 的失败码
/// （-1 路径非法或方法未知，-2 全部变体都被已有的等价路由遮住），调用方据此给出不同提示</returns>
/// <returns type="(string|RouterSeg)[]?">段序列，注册失败时为 nil：字面量段是字符串，
/// 占位符段是 { key = 名字, opt = 是否可选 }（末尾通配的 key 为 "*"；占位符名字只认 \w，
/// 故与它撞不上）。反向生成 URL 用它，别再照路径模板自己解析一遍 —— 段语法只有 C 侧一份</returns>
static int32_t _lrouter_add(lua_State *lua) {
    LPUB_UD_ARG(lua, router_ctx, MT_ROUTER, pr, "router freed");
    size_t mlen;
    const char *method = luaL_checklstring(lua, 2, &mlen);
    size_t plen;
    const char *path = luaL_checklstring(lua, 3, &plen);
    int32_t idx = router_add_index(*pr, method, mlen, path, plen);
    lua_pushboolean(lua, idx >= 0);
    lua_pushinteger(lua, idx);// 成功是索引，失败是失败码
    if (idx < 0) {
        lua_pushnil(lua);// 补齐到与成功路径同样 3 个返回值，规矩见 lpub_rtn_nil
        return 3;
    }
    _lrouter_push_segs(lua, *pr, idx);
    return 3;
}
// 压规范化后的 path。"/" 与 "//" 的段全是空段，被 router_match_index 剔光后 npath 归零，
// 此时 url_reorg_path 只会吐出空串，直接给 "/"，别让 path 缺席
static void _lrouter_push_path(lua_State *lua, url_ctx *url) {
    if (url->npath > 0) {
        luaL_Buffer pbuf;
        size_t pcap = url->pathlens + 1;
        char *pp = luaL_buffinitsize(lua, &pbuf, pcap);
        luaL_pushresultsize(&pbuf, url_reorg_path(url, pp, pcap));
    } else {
        lua_pushliteral(lua, "/");
    }
}
// 查询参数表；没有查询参数压 nil，不建空表
static void _lrouter_push_query(lua_State *lua, url_ctx *url) {
    if (0 == url->nparam) {
        lua_pushnil(lua);
    } else {
        lpub_push_url_param(lua, url);
    }
}
// router_match_index 未命中时 url 是否已解析：400 与未知方法没解析，404 / 405 / OPTIONS 自动应答已解析
static inline int32_t _lrouter_url_parsed(int32_t idx) {
    return -2 != idx && -3 != idx;
}
// match 的 url 表只压 path 与 param 两个字段
static void _lrouter_push_url(lua_State *lua, url_ctx *url) {
    lua_createtable(lua, 0, 2);
    _lrouter_push_path(lua, url);
    lua_setfield(lua, -2, "path");
    lpub_push_url_param(lua, url);
    lua_setfield(lua, -2, "param");
}
// 压 Allow 头的值；allow 为 0（不是 405 / OPTIONS 自动应答）压 nil
static void _lrouter_push_allow(lua_State *lua, router_method allow) {
    char buf[ROUTER_ALLOW_LENS];
    if (0 == allow) {
        lua_pushnil(lua);
        return;
    }
    lua_pushlstring(lua, buf, router_allow_value(allow, buf));
}
// 压路径参数表 { 名字 = 取值 }
static void _lrouter_push_params(lua_State *lua, router_req *ctx) {
    lua_createtable(lua, 0, ctx->params_n);
    for (int32_t i = 0; i < ctx->params_n; i++) {
        lua_pushlstring(lua, ctx->params[i].key, ctx->params[i].key_len);
        lua_pushlstring(lua, ctx->params[i].val, ctx->params[i].val_len);
        lua_rawset(lua, -3);
    }
}
/// <summary>
/// 匹配请求路径（不执行 handler/中间件）
/// </summary>
/// <param name="self" type="userdata">router_ctx 对象</param>
/// <param name="method" type="string">HTTP 方法字符串</param>
/// <param name="url" type="string">原始请求 URI（含查询字符串）</param>
/// <returns type="boolean">true=命中；false=失败</returns>
/// <returns type="integer">HTTP 状态码，取值见 C 层 router_match_code（未命中的 200 是 OPTIONS 自动应答）</returns>
/// <returns type="RouterMatchedURL|string?">命中时是规范化后的 path 与 query 表；未命中时是 Allow 头的值
/// （405 与 OPTIONS 自动应答才有，其余为 nil）</returns>
/// <returns type="integer|RouterMatchedURL?">命中时是路由索引（≥0）；404 / 405 / OPTIONS 自动应答时是 path 与 query 表，
/// 400 与未知方法没有这一位、只返到 Allow 为止</returns>
/// <returns type="table&lt;string,string&gt;?">路径参数表；仅命中时返回</returns>
static int32_t _lrouter_match(lua_State *lua) {
    LPUB_UD_ARG(lua, router_ctx, MT_ROUTER, pr, "router freed");
    size_t mlen;
    const char *method = luaL_checklstring(lua, 2, &mlen);
    size_t ulen;
    const char *url = luaL_checklstring(lua, 3, &ulen);
    url_ctx urlstorage;
    router_req ctx;
    ZERO(&ctx, offsetof(router_req, chain));
    ctx.url = &urlstorage;
    int32_t idx = router_match_index(*pr, method, mlen, url, ulen, &ctx);
    int32_t code = router_match_code(idx);
    lua_pushboolean(lua, idx >= 0);
    lua_pushinteger(lua, code);
    if (idx < 0) {
        _lrouter_push_allow(lua, ctx.allow);
        if (_lrouter_url_parsed(idx)) {
            _lrouter_push_url(lua, ctx.url);
            return 4;
        }
        return 3;
    }
    _lrouter_push_url(lua, ctx.url);
    lua_pushinteger(lua, idx);
    _lrouter_push_params(lua, &ctx);
    return 5;
}
/// <summary>
/// 同 match，但方法与 URI 直接取自请求包的请求行，返回值摊平、不建中间表
/// </summary>
/// <param name="self" type="userdata">router_ctx 对象</param>
/// <param name="pack" type="lightuserdata">http_pack_ctx 指针</param>
/// <returns type="boolean?">true=命中；false=失败；不带请求行的包（分块中间包）为 nil，且只此一个返回值</returns>
/// <returns type="integer">HTTP 状态码，取值同 match</returns>
/// <returns type="string">请求方法</returns>
/// <returns type="string">请求行第三段（HTTP 版本）</returns>
/// <returns type="boolean">回完响应是否该关连接，判据见 C 层 http_want_close</returns>
/// <returns type="string?">命中时是规范化后的 path；未命中时是 Allow 头的值（405 与 OPTIONS 自动应答才有，
/// 其余为 nil）</returns>
/// <returns type="string|table&lt;string,string&gt;?">命中时是查询参数表，没有查询参数时为 nil（不建空表）；
/// 404 / 405 / OPTIONS 自动应答时是规范化后的 path，400 与未知方法只返到 Allow 为止</returns>
/// <returns type="integer|table&lt;string,string&gt;?">命中时是路由索引（≥0）；404 / 405 / OPTIONS 自动应答时是
/// 查询参数表（没有时为 nil），且未命中只返到这里为止</returns>
/// <returns type="table&lt;string,string&gt;?">路径参数表；仅命中时返回，没有路径参数时为 nil（不建空表）；
/// 末尾通配吃下的段在键 "*" 下</returns>
static int32_t _lrouter_match_pack(lua_State *lua) {
    LPUB_UD_ARG(lua, router_ctx, MT_ROUTER, pr, "router freed");
    LPUB_LUD_ARG(lua, struct http_pack_ctx, 2, pack);
    buf_ctx *status = http_status(pack);
    if (NULL == status) {
        return lpub_rtn_nil(lua, 1);
    }
    url_ctx urlstorage;
    router_req ctx;
    ZERO(&ctx, offsetof(router_req, chain));
    ctx.url = &urlstorage;
    int32_t idx = router_match_index(*pr, status[0].data, status[0].lens,
                                     status[1].data, status[1].lens, &ctx);
    lua_pushboolean(lua, idx >= 0);
    lua_pushinteger(lua, router_match_code(idx));
    lua_pushlstring(lua, status[0].data, status[0].lens);
    lua_pushlstring(lua, status[2].data, status[2].lens);
    lua_pushboolean(lua, http_want_close(pack));
    if (idx < 0) {
        _lrouter_push_allow(lua, ctx.allow);
        if (!_lrouter_url_parsed(idx)) {
            return 6;
        }
        _lrouter_push_path(lua, ctx.url);
        _lrouter_push_query(lua, ctx.url);
        return 8;
    }
    _lrouter_push_path(lua, ctx.url);
    _lrouter_push_query(lua, ctx.url);
    lua_pushinteger(lua, idx);
    if (0 == ctx.params_n) {
        lua_pushnil(lua);
    } else {
        _lrouter_push_params(lua, &ctx);
    }
    return 9;
}
/// <summary>
/// 请求带 Expect: 100-continue 时回一条 100 Continue，让客户端开始发请求体；流式路由准入通过后调。
/// 判据见 C 层 http_expect_continue（HTTP/1.0 不回）
/// </summary>
/// <param name="sk" type="userdata">连接标识</param>
/// <param name="pack" type="lightuserdata">首帧的 http_pack_ctx 指针</param>
static int32_t _lrouter_expect_continue(lua_State *lua) {
    sock_ctx *sk = lpub_check_sock(lua, 1);
    LPUB_LUD_ARG(lua, struct http_pack_ctx, 2, pack);
    if (http_expect_continue(pack)) {
        ev_send(&g_loader->netev, sk, (void *)HTTP_CONTINUE_RESP, sizeof(HTTP_CONTINUE_RESP) - 1, 1);
    }
    return 0;
}
//srey.router
LUAMOD_API int luaopen_router(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lrouter_new },
        { "expect_continue", _lrouter_expect_continue },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "add", _lrouter_add },
        { "add_steps", _lrouter_add_steps },
        { "use_step", _lrouter_use_step },
        { "chain", _lrouter_chain },
        { "match", _lrouter_match },
        { "match_pack", _lrouter_match_pack },
        { "__gc", _lrouter_free },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_ROUTER, reg_new, reg_func);
    return 1;
}
