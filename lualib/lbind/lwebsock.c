#include "lbind/lpub.h"

/// <summary>
/// 解包 WebSocket 帧
/// </summary>
/// <param name="pack" type="lightuserdata">websock_pack_ctx 指针</param>
/// <returns type="WebSocketFrame">含 fin / prot / secprot / secpack / data / size 字段的表；
///   secprot / secpack 仅在存在时填充，data 恒有：它是包尾柔性数组的地址，
///   零长帧下仍是有效指针（size 为 0），可直接连同 size 转手给取 buf 的接口</returns>
static int32_t _lwebsock_unpack(lua_State *lua) {
    LPUB_LUD_ARG(lua, struct websock_pack_ctx, 1, pack);
    lua_createtable(lua, 0, 6);
    lua_pushinteger(lua, websock_fin(pack));// 是否为最终分片
    lua_setfield(lua, -2, "fin");
    lua_pushinteger(lua, websock_prot(pack));// 帧操作码（文本/二进制/控制帧等）
    lua_setfield(lua, -2, "prot");
    int32_t secprot = websock_secprot(pack);
    if (PACK_NONE != secprot) {
        lua_pushinteger(lua, secprot);// 子协议类型
        lua_setfield(lua, -2, "secprot");
    }
    void *secpack = websock_secpack(pack);
    if (NULL != secpack) {
        lua_pushlightuserdata(lua, secpack);// 子协议数据包指针
        lua_setfield(lua, -2, "secpack");
    }
    size_t lens;
    void *data = websock_data(pack, &lens);
    // 零长帧也照推：websock_data 恒非 NULL，不设字段的话上层拿到 nil，再喂给 lpub_check_buf 会抛错
    lua_pushlightuserdata(lua, data);
    lua_setfield(lua, -2, "data");
    lua_pushinteger(lua, lens);
    lua_setfield(lua, -2, "size");
    return 1;
}
/// <summary>
/// 解包 WebSocket 帧，同 unpack 但按多返回值给出、不建表
/// </summary>
/// <param name="pack" type="lightuserdata">websock_pack_ctx 指针</param>
/// <returns type="integer">fin：是否为最终分片</returns>
/// <returns type="integer">prot：帧操作码</returns>
/// <returns type="integer?">secprot：子协议类型；没有子协议时为 nil</returns>
/// <returns type="lightuserdata?">secpack：子协议数据包指针；没有时为 nil</returns>
/// <returns type="lightuserdata">data：载荷指针，恒非 nil，理由同 unpack</returns>
/// <returns type="integer">size：载荷字节数</returns>
static int32_t _lwebsock_frame(lua_State *lua) {
    LPUB_LUD_ARG(lua, struct websock_pack_ctx, 1, pack);
    lua_pushinteger(lua, websock_fin(pack));
    lua_pushinteger(lua, websock_prot(pack));
    int32_t secprot = websock_secprot(pack);
    if (PACK_NONE != secprot) {
        lua_pushinteger(lua, secprot);
    } else {
        lua_pushnil(lua);
    }
    void *secpack = websock_secpack(pack);
    if (NULL != secpack) {
        lua_pushlightuserdata(lua, secpack);
    } else {
        lua_pushnil(lua);
    }
    size_t lens;
    lua_pushlightuserdata(lua, websock_data(pack, &lens));
    lua_pushinteger(lua, lens);
    return 6;
}
/// <summary>
/// 构造 WebSocket 握手请求包（HTTP Upgrade）
/// </summary>
/// <param name="host" type="string?">Host 头字段；nil 表示省略</param>
/// <param name="uri" type="string?">HTTP request-target（path?query）；nil 或空字符串时使用 "/"</param>
/// <param name="secprot" type="string?">Sec-WebSocket-Protocol 字段；nil 表示省略</param>
/// <returns type="lightuserdata?">握手包数据指针；secprot 校验失败、host/uri/secprot 任一含 CR/LF（会拆出额外的 HTTP 头）、
/// 或取不到 CSPRNG 熵生成 nonce 时返回 nil。
/// 业务通过 srey.send copy=0 接管或 utils.ud_free 释放</returns>
/// <returns type="integer?">数据长度</returns>
/// <returns type="lightuserdata?">握手上下文 hsctx。传给 srey.connect 当 extra 的那一刻所有权才转交框架，此后业务不得 ud_free；
/// 没走到 srey.connect（打完包就不连了、中途出错）时它仍归业务，必须自己 ud_free。
/// 失败时三个返回值都是 nil，个数恒为 3</returns>
static int32_t _lwebsock_pack_handshake(lua_State *lua) {
    luaL_argcheck(lua, lua_isnoneornil(lua, 1) || LUA_TSTRING == lua_type(lua, 1), 1, "host must be a string");
    luaL_argcheck(lua, lua_isnoneornil(lua, 2) || LUA_TSTRING == lua_type(lua, 2), 2, "uri must be a string");
    luaL_argcheck(lua, lua_isnoneornil(lua, 3) || LUA_TSTRING == lua_type(lua, 3), 3, "secprot must be a string");
    const char *host = lua_isnoneornil(lua, 1) ? NULL : lpub_check_cstr(lua, 1);
    const char *uri = lua_isnoneornil(lua, 2) ? NULL : lpub_check_cstr(lua, 2);
    const char *secprot = lua_isnoneornil(lua, 3) ? NULL : lpub_check_cstr(lua, 3);
    ws_hs_ctx *hsctx;
    char *hspack = websock_pack_handshake(host, uri, secprot, &hsctx);
    if (NULL == hspack) {
        return lpub_rtn_nil(lua, 3);
    }
    lua_pushlightuserdata(lua, hspack);
    lua_pushinteger(lua, strlen(hspack));
    lua_pushlightuserdata(lua, hsctx);
    return 3;
}
/// <summary>
/// 构造 WebSocket Ping 控制帧
/// </summary>
/// <param name="mask" type="integer">是否启用掩码（客户端发送 1，服务端 0）；只收 0/1，其余报错</param>
/// <param name="data" type="string|lightuserdata?">载荷，可省；不超过 125 字节</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <returns type="lightuserdata?">数据指针；载荷超过 125 字节，或 mask 非 0 且取不到 CSPRNG 熵生成掩码 key 时返回 nil</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lwebsock_pack_ping(lua_State *lua) {
    int32_t mask = (int32_t)lpub_check_flag(lua, 1);
    size_t dlens, lens;
    void *data = lpub_opt_buf(lua, 2, &dlens);
    void *pack = websock_pack_ping(mask, data, dlens, &lens);
    return lpub_rtn_lud(lua, pack, lens);
}
/// <summary>
/// 构造 WebSocket Pong 控制帧；回应 PING 时须原样带上那个 PING 的载荷
/// </summary>
/// <param name="mask" type="integer">是否启用掩码（客户端 1，服务端 0）；只收 0/1，其余报错</param>
/// <param name="data" type="string|lightuserdata?">载荷，可省；不超过 125 字节</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <returns type="lightuserdata?">数据指针；返回 nil 的情形同 pack_ping</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lwebsock_pack_pong(lua_State *lua) {
    int32_t mask = (int32_t)lpub_check_flag(lua, 1);
    size_t dlens, lens;
    void *data = lpub_opt_buf(lua, 2, &dlens);
    void *pack = websock_pack_pong(mask, data, dlens, &lens);
    return lpub_rtn_lud(lua, pack, lens);
}
/// <summary>
/// 构造 WebSocket Close 控制帧。对端发来的 CLOSE 由协议层当场回应，只在本端发起关闭时用它
/// </summary>
/// <param name="mask" type="integer">是否启用掩码（客户端 1，服务端 0）；只收 0/1，其余报错</param>
/// <param name="code" type="integer?">状态码（如 1000 正常关闭）；省略或 0 表示不带，取值 0~65535</param>
/// <param name="reason" type="string?">关闭原因（UTF-8），带原因时 code 不能为 0；加上 2 字节状态码不超过 125 字节</param>
/// <returns type="lightuserdata?">数据指针；code 为 0 却带了原因、长度超限，或取不到掩码熵时返回 nil</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lwebsock_pack_close(lua_State *lua) {
    int32_t mask = (int32_t)lpub_check_flag(lua, 1);
    uint16_t code = (uint16_t)lpub_opt_range(lua, 2, 0, 0, UINT16_MAX, "close code out of range");
    size_t rlens, lens;
    const char *reason = luaL_optlstring(lua, 3, NULL, &rlens);
    void *pack = websock_pack_close(mask, code, reason, rlens, &lens);
    return lpub_rtn_lud(lua, pack, lens);
}
/// <summary>
/// 构造 WebSocket 文本帧（首帧）
/// </summary>
/// <param name="mask" type="integer">是否启用掩码（客户端 1，服务端 0）；只收 0/1，其余报错</param>
/// <param name="fin" type="integer">1 表示完整消息，0 表示后续有 continuation 帧；只收 0/1，其余报错</param>
/// <param name="data" type="string|lightuserdata">载荷数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <returns type="lightuserdata?">数据指针；mask 非 0 且取不到 CSPRNG 熵生成掩码 key 时返回 nil</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lwebsock_pack_text(lua_State *lua) {
    int32_t mask = (int32_t)lpub_check_flag(lua, 1);
    int32_t fin = (int32_t)lpub_check_flag(lua, 2);
    size_t dlens, lens;
    void *data = lpub_check_buf(lua, 3, &dlens, NULL);
    void *pack = websock_pack_text(mask, fin, data, dlens, &lens);
    return lpub_rtn_lud(lua, pack, lens);
}
/// <summary>
/// 构造 WebSocket 二进制帧（首帧）
/// </summary>
/// <param name="mask" type="integer">是否启用掩码（客户端 1，服务端 0）；只收 0/1，其余报错</param>
/// <param name="fin" type="integer">1 表示完整消息，0 表示后续有 continuation 帧；只收 0/1，其余报错</param>
/// <param name="data" type="string|lightuserdata">载荷数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <returns type="lightuserdata?">数据指针；返回 nil 的情形同 pack_text</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lwebsock_pack_binary(lua_State *lua) {
    int32_t mask = (int32_t)lpub_check_flag(lua, 1);
    int32_t fin = (int32_t)lpub_check_flag(lua, 2);
    size_t dlens, lens;
    void *data = lpub_check_buf(lua, 3, &dlens, NULL);
    void *pack = websock_pack_binary(mask, fin, data, dlens, &lens);
    return lpub_rtn_lud(lua, pack, lens);
}
/// <summary>
/// 构造 WebSocket Continuation 帧（分片消息的中间或最后帧）
/// </summary>
/// <param name="mask" type="integer">是否启用掩码（客户端 1，服务端 0）；只收 0/1，其余报错</param>
/// <param name="fin" type="integer">1 表示最后帧（PROT_SLICE_END），0 表示中间帧；只收 0/1，其余报错</param>
/// <param name="data" type="string|lightuserdata">载荷数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <returns type="lightuserdata?">数据指针；返回 nil 的情形同 pack_text</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lwebsock_pack_continua(lua_State *lua) {
    int32_t mask = (int32_t)lpub_check_flag(lua, 1);
    int32_t fin = (int32_t)lpub_check_flag(lua, 2);
    size_t dlens, lens;
    void *data = lpub_check_buf(lua, 3, &dlens, NULL);
    void *pack = websock_pack_continua(mask, fin, data, dlens, &lens);
    return lpub_rtn_lud(lua, pack, lens);
}
/// <summary>
/// 解析 HANDSHAKED 交付的 ws_secprots_ctx，返回匹配到的子协议下标与全部子协议名列表
/// </summary>
/// <param name="spctx" type="lightuserdata?">握手交付的 ws_secprots_ctx 指针；仅本协程下次挂起前有效，勿保留。
/// 其余类型报错</param>
/// <returns type="integer?">匹配到的子协议下标(0 起，-1 表示无)；spctx 为 nil 或空指针返回 nil</returns>
/// <returns type="string[]?">全部子协议名列表（1 起）；spctx 为空时同为 nil，返回值个数恒为 2
/// （wbsk.connect 明说了 spctx 可能为 nil，业务确实会走到这条）</returns>
static int32_t _lwebsock_secprots(lua_State *lua) {
    if (lua_isnoneornil(lua, 1)) {
        return lpub_rtn_nil(lua, 2);
    }
    LUACHECK_LUDATA_OPT(lua, 1);
    ws_secprots_ctx *spctx = lua_touserdata(lua, 1);
    if (NULL == spctx) {
        return lpub_rtn_nil(lua, 2);
    }
    lua_pushinteger(lua, spctx->index);
    lua_createtable(lua, spctx->cnt, 0);
    for (int32_t i = 0; i < spctx->cnt; i++) {
        lua_pushlstring(lua, spctx->prots[i].data, spctx->prots[i].lens);
        lua_rawseti(lua, -2, i + 1);
    }
    return 2;
}
//srey.websock
LUAMOD_API int luaopen_websock(lua_State *lua) {
    luaL_Reg reg[] = {
        { "unpack", _lwebsock_unpack },
        { "frame", _lwebsock_frame },
        { "pack_handshake", _lwebsock_pack_handshake },
        { "pack_ping", _lwebsock_pack_ping },
        { "pack_pong", _lwebsock_pack_pong },
        { "pack_close", _lwebsock_pack_close },
        { "pack_text", _lwebsock_pack_text },
        { "pack_binary", _lwebsock_pack_binary },
        { "pack_continua", _lwebsock_pack_continua },
        { "secprots", _lwebsock_secprots },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
