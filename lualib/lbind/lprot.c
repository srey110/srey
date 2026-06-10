#include "lbind/lpub.h"

#define MT_SMTP      "_smtp_ctx"
#define MT_SMTP_MAIL "_smtp_mail_ctx"

/// <summary>
/// 打包 harbor 跨节点消息
/// </summary>
/// <param name="task" type="integer">目标 task name</param>
/// <param name="call" type="integer">调用类型：0=call（单向），1=request（双向）</param>
/// <param name="reqtype" type="integer">业务请求类型</param>
/// <param name="key" type="string">路由 key（用于一致性哈希定位节点）</param>
/// <param name="data" type="string|lightuserdata|nil">消息内容；nil 表示无数据</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <returns type="lightuserdata">打包后的数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lprot_harbor_pack(lua_State *lua) {
    name_t task = (name_t)luaL_checkinteger(lua, 1);
    int32_t call = (int32_t)luaL_checkinteger(lua, 2);      // 0=call，1=request
    uint8_t reqtype = (uint8_t)luaL_checkinteger(lua, 3);
    const char *key = luaL_checkstring(lua, 4);              // 路由 key
    void *data;
    size_t size;
    switch (lua_type(lua, 5)) {
    case LUA_TNIL:
    case LUA_TNONE:
        data = NULL;
        size = 0;
        break;
    case LUA_TSTRING:
        data = (void *)luaL_checklstring(lua, 5, &size);
        break;
    case LUA_TLIGHTUSERDATA:
        data = lua_touserdata(lua, 5);
        size = (size_t)luaL_checkinteger(lua, 6);
        break;
    default:
        return luaL_argerror(lua, 5, "nil, string or light userdata expected");
    }
    data = harbor_pack(task, call, reqtype, key, data, size, &size);
    if (NULL == data) {
        return luaL_error(lua, "harbor_pack: signature generation failed.");
    }
    LPUB_RET_LUD(lua, data, size);
}
//srey.harbor
LUAMOD_API int luaopen_harbor(lua_State *lua) {
    luaL_Reg reg[] = {
        { "pack", _lprot_harbor_pack },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
/// <summary>
/// 返回配置的 DNS 服务器 IP 地址
/// </summary>
/// <param>无</param>
/// <returns type="string">DNS 服务器 IP</returns>
static int32_t _lprot_dns_ip(lua_State *lua) {
    lua_pushstring(lua, dns_get_ip());
    return 1;
}
/// <summary>
/// 构造 UDP DNS 查询请求包（不含 2 字节长度前缀）
/// </summary>
/// <param name="domain" type="string">查询域名</param>
/// <param name="ipv6" type="integer">1 查询 AAAA 记录，0 查询 A 记录</param>
/// <returns type="string">DNS 查询二进制字符串</returns>
static int32_t _lprot_dns_pack(lua_State *lua) {
    const char *domain = luaL_checkstring(lua, 1);
    int32_t ipv6 = (int32_t)luaL_checkinteger(lua, 2);
    char buf[ONEK];
    size_t lens = (size_t)dns_request_pack(buf, domain, ipv6);
    lua_pushlstring(lua, buf, lens);
    return 1;
}
/// <summary>
/// 构造 TCP DNS 查询请求包（含 2 字节大端长度前缀，RFC 1035 §4.2.2 / RFC 7766）
/// </summary>
/// <param name="domain" type="string">查询域名</param>
/// <param name="ipv6" type="integer">1 查询 AAAA 记录，0 查询 A 记录</param>
/// <returns type="string?">含长度前缀的 DNS 查询二进制字符串；构造失败返回 nil</returns>
static int32_t _lprot_dns_pack_tcp(lua_State *lua) {
    const char *domain = luaL_checkstring(lua, 1);
    int32_t ipv6 = (int32_t)luaL_checkinteger(lua, 2);
    char buf[ONEK];
    size_t lens = dns_request_pack_tcp(buf, domain, ipv6);
    if (0 == lens) {
        lua_pushnil(lua);
        return 1;
    }
    lua_pushlstring(lua, buf, lens);
    return 1;
}
/// <summary>
/// 解析 DNS 响应包，提取 IP 地址列表
/// </summary>
/// <param name="pack" type="lightuserdata">DNS 响应数据指针（裸报文，不含 TCP 长度前缀）</param>
/// <param name="packlen" type="integer">响应包字节数</param>
/// <returns type="string[]?">IP 字符串数组；解析失败或 RCODE 非 0 时返回 nil</returns>
static int32_t _lprot_dns_unpack(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    void *pack = lua_touserdata(lua, 1);
    size_t packlen = (size_t)luaL_checkinteger(lua, 2);
    size_t n;
    dns_ip *ips = dns_parse_pack(pack, packlen, &n);
    if (NULL == ips) {
        lua_pushnil(lua);
        return 1;
    }
    lua_createtable(lua, (int32_t)n, 0);
    for (size_t i = 0; i < n; i++) {
        ips[i].ip[IP_LENS - 1] = '\0';
        lua_pushstring(lua, ips[i].ip);
        lua_rawseti(lua, -2, (lua_Integer)(i + 1));
    }
    FREE(ips);
    return 1;
}
//srey.dns
LUAMOD_API int luaopen_dns(lua_State *lua) {
    luaL_Reg reg[] = {
        { "ip", _lprot_dns_ip },
        { "pack", _lprot_dns_pack },
        { "pack_tcp", _lprot_dns_pack_tcp },
        { "unpack", _lprot_dns_unpack },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
/// <summary>
/// 打包自定义协议（custz）数据
/// </summary>
/// <param name="pktype" type="integer">协议子类型</param>
/// <param name="data" type="string|lightuserdata">载荷数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <returns type="lightuserdata">打包后的数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lprot_custz_pack(lua_State *lua) {
    uint8_t pktype = (uint8_t)luaL_checkinteger(lua, 1);
    void *data;
    size_t size;
    data = lpub_check_buf(lua, 2, &size, NULL);
    data = custz_pack(pktype, data, size, &size);
    if (NULL == data) {
        lua_pushnil(lua);
        return 1;
    }
    LPUB_RET_LUD(lua, data, size);
}
//srey.custz
LUAMOD_API int luaopen_custz(lua_State *lua) {
    luaL_Reg reg[] = {
        { "pack", _lprot_custz_pack },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
/// <summary>
/// 解包 WebSocket 帧
/// </summary>
/// <param name="pack" type="lightuserdata">websock_pack_ctx 指针</param>
/// <returns type="WebSocketFrame">含 fin / prot / secprot / secpack / data / size 字段的表；secprot/secpack/data 仅在存在时填充</returns>
static int32_t _lprot_websock_unpack(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    struct websock_pack_ctx *pack = (struct websock_pack_ctx *)lua_touserdata(lua, 1);
    lua_createtable(lua, 0, 6);
    lua_pushinteger(lua, websock_fin(pack));      // 是否为最终分片
    lua_setfield(lua, -2, "fin");
    lua_pushinteger(lua, websock_prot(pack));     // 帧操作码（文本/二进制/控制帧等）
    lua_setfield(lua, -2, "prot");
    int32_t secprot = websock_secprot(pack);
    if (PACK_NONE != secprot) {
        lua_pushinteger(lua, secprot);            // 子协议类型
        lua_setfield(lua, -2, "secprot");
    }
    void *secpack = websock_secpack(pack);
    if (NULL != secpack) {
        lua_pushlightuserdata(lua, secpack);      // 子协议数据包指针
        lua_setfield(lua, -2, "secpack");
    }
    size_t lens;
    void *data = websock_data(pack, &lens);
    if (lens > 0) {
        lua_pushlightuserdata(lua, data);
        lua_setfield(lua, -2, "data");
    }
    lua_pushinteger(lua, lens);
    lua_setfield(lua, -2, "size");
    return 1;
}
/// <summary>
/// 构造 WebSocket 握手请求包（HTTP Upgrade）
/// </summary>
/// <param name="host" type="string?">Host 头字段；nil 表示省略</param>
/// <param name="secprot" type="string?">Sec-WebSocket-Protocol 字段；nil 表示省略</param>
/// <returns type="lightuserdata">握手包数据指针（业务通过 srey.send/connect copy=0 接管或 utils.ud_free 释放）</returns>
/// <returns type="integer">数据长度</returns>
/// <returns type="lightuserdata">signkey 指针,用于校验对端 Sec-WebSocket-Accept;业务用完必须 utils.ud_free 释放</returns>
static int32_t _lprot_websock_pack_handshake(lua_State *lua) {
    char *host = NULL;
    if (LUA_TSTRING == lua_type(lua, 1)) {
        host = (char *)luaL_checkstring(lua, 1);
    }
    char *secprot = NULL;
    if (LUA_TSTRING == lua_type(lua, 2)) {
        secprot = (char *)luaL_checkstring(lua, 2);  // Sec-WebSocket-Protocol 值
    }
    char *signkey;
    MALLOC(signkey, WS_SIGN_KEY_LENS);
    char *hspack = websock_pack_handshake(host, secprot, signkey);
    lua_pushlightuserdata(lua, hspack);
    lua_pushinteger(lua, strlen(hspack));
    lua_pushlightuserdata(lua, signkey);
    return 3;
}
/// <summary>
/// 构造 WebSocket Ping 控制帧
/// </summary>
/// <param name="mask" type="integer">是否启用掩码（客户端发送 1，服务端 0）</param>
/// <returns type="lightuserdata">数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lprot_websock_pack_ping(lua_State *lua) {
    int32_t mask = (int32_t)luaL_checkinteger(lua, 1);
    size_t lens;
    void *pack = websock_pack_ping(mask, &lens);
    LPUB_RET_LUD(lua, pack, lens);
}
/// <summary>
/// 构造 WebSocket Pong 控制帧
/// </summary>
/// <param name="mask" type="integer">是否启用掩码（客户端 1，服务端 0）</param>
/// <returns type="lightuserdata">数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lprot_websock_pack_pong(lua_State *lua) {
    int32_t mask = (int32_t)luaL_checkinteger(lua, 1);
    size_t lens;
    void *pack = websock_pack_pong(mask, &lens);
    LPUB_RET_LUD(lua, pack, lens);
}
/// <summary>
/// 构造 WebSocket Close 控制帧
/// </summary>
/// <param name="mask" type="integer">是否启用掩码（客户端 1，服务端 0）</param>
/// <returns type="lightuserdata">数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lprot_websock_pack_close(lua_State *lua) {
    int32_t mask = (int32_t)luaL_checkinteger(lua, 1);
    size_t lens;
    void *pack = websock_pack_close(mask, &lens);
    LPUB_RET_LUD(lua, pack, lens);
}
/// <summary>
/// 构造 WebSocket 文本帧（首帧）
/// </summary>
/// <param name="mask" type="integer">是否启用掩码（客户端 1，服务端 0）</param>
/// <param name="fin" type="integer">1 表示完整消息，0 表示后续有 continuation 帧</param>
/// <param name="data" type="string|lightuserdata">载荷数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <returns type="lightuserdata">数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lprot_websock_pack_text(lua_State *lua) {
    void *data;
    size_t dlens;
    int32_t mask = (int32_t)luaL_checkinteger(lua, 1);
    int32_t fin = (int32_t)luaL_checkinteger(lua, 2);
    data = lpub_check_buf(lua, 3, &dlens, NULL);
    void *pack = websock_pack_text(mask, fin, data, dlens, &dlens);
    LPUB_RET_LUD(lua, pack, dlens);
}
/// <summary>
/// 构造 WebSocket 二进制帧（首帧）
/// </summary>
/// <param name="mask" type="integer">是否启用掩码（客户端 1，服务端 0）</param>
/// <param name="fin" type="integer">1 表示完整消息，0 表示后续有 continuation 帧</param>
/// <param name="data" type="string|lightuserdata">载荷数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <returns type="lightuserdata">数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lprot_websock_pack_binary(lua_State *lua) {
    void *data;
    size_t dlens;
    int32_t mask = (int32_t)luaL_checkinteger(lua, 1);
    int32_t fin = (int32_t)luaL_checkinteger(lua, 2);
    data = lpub_check_buf(lua, 3, &dlens, NULL);
    void *pack = websock_pack_binary(mask, fin, data, dlens, &dlens);
    LPUB_RET_LUD(lua, pack, dlens);
}
/// <summary>
/// 构造 WebSocket Continuation 帧（分片消息的中间或最后帧）
/// </summary>
/// <param name="mask" type="integer">是否启用掩码（客户端 1，服务端 0）</param>
/// <param name="fin" type="integer">1 表示最后帧（PROT_SLICE_END），0 表示中间帧</param>
/// <param name="data" type="string|lightuserdata">载荷数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时可选，缺省按 0 处理</param>
/// <returns type="lightuserdata">数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lprot_websock_pack_continua(lua_State *lua) {
    void *data;
    size_t dlens;
    int32_t mask = (int32_t)luaL_checkinteger(lua, 1);
    int32_t fin = (int32_t)luaL_checkinteger(lua, 2);
    int32_t type = lua_type(lua, 3);
    if (LUA_TSTRING == type) {
        data = (void *)luaL_checklstring(lua, 3, &dlens);
    } else if (LUA_TLIGHTUSERDATA == type) {
        data = lua_touserdata(lua, 3);
        if (LUA_TNUMBER == lua_type(lua, 4)) {
            dlens = (size_t)luaL_checkinteger(lua, 4);
        } else {
            dlens = 0;
        }
    } else {
        return luaL_argerror(lua, 3, "string or light userdata expected");
    }
    void *pack = websock_pack_continua(mask, fin, data, dlens, &dlens);
    LPUB_RET_LUD(lua, pack, dlens);
}
//srey.websock
LUAMOD_API int luaopen_websock(lua_State *lua) {
    luaL_Reg reg[] = {
        { "unpack", _lprot_websock_unpack },
        { "pack_handshake", _lprot_websock_pack_handshake },
        { "pack_ping", _lprot_websock_pack_ping },
        { "pack_pong", _lprot_websock_pack_pong },
        { "pack_close", _lprot_websock_pack_close },
        { "pack_text", _lprot_websock_pack_text },
        { "pack_binary", _lprot_websock_pack_binary },
        { "pack_continua", _lprot_websock_pack_continua },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
/// <summary>
/// 将 HTTP 状态码转换为对应的文本描述（如 200 → "OK"）
/// </summary>
/// <param name="code" type="integer">HTTP 状态码</param>
/// <returns type="string">文本描述</returns>
static int32_t _lprot_http_code_status(lua_State *lua) {
    int32_t err = (int32_t)luaL_checkinteger(lua, 1);
    lua_pushstring(lua, http_code_status(err));
    return 1;
}
/// <summary>
/// 返回 HTTP 包的分块传输状态
/// </summary>
/// <param name="pack" type="lightuserdata">http_pack_ctx 指针</param>
/// <returns type="integer">0=非分块；1=首包（含 header）；2+ 分块中间/结束块</returns>
static int32_t _lprot_http_chunked(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    struct http_pack_ctx *pack = (struct http_pack_ctx *)lua_touserdata(lua, 1);
    lua_pushinteger(lua, http_chunked(pack));
    return 1;
}
/// <summary>
/// 返回 HTTP 状态行 / 请求行三元组
/// </summary>
/// <param name="pack" type="lightuserdata">http_pack_ctx 指针</param>
/// <returns type="string[]?">3 元数组：响应为 {version, code, message}，请求为 {method, uri, version}；分块中间包返回 nil</returns>
static int32_t _lprot_http_status(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    struct http_pack_ctx *pack = (struct http_pack_ctx *)lua_touserdata(lua, 1);
    int32_t chunck = http_chunked(pack);
    if (0 != chunck
        && 1 != chunck) {
        lua_pushnil(lua);
        return 1;
    }
    buf_ctx *buf = http_status(pack);
    lua_createtable(lua, 3, 0);
    for (int32_t i = 0; i < 3; i++) {
        lua_pushlstring(lua, buf[i].data, buf[i].lens);
        lua_rawseti(lua, -2, i + 1);
    }
    return 1;
}
/// <summary>
/// 按 key 查找 HTTP 头部字段值
/// </summary>
/// <param name="pack" type="lightuserdata">http_pack_ctx 指针</param>
/// <param name="key" type="string">header 名（大小写不敏感）</param>
/// <returns type="string?">header 值；分块中间包或字段不存在时返回 nil</returns>
static int32_t _lprot_http_head(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    struct http_pack_ctx *pack = (struct http_pack_ctx *)lua_touserdata(lua, 1);
    const char *key = luaL_checkstring(lua, 2);
    int32_t chunck = http_chunked(pack);
    if (0 != chunck
        && 1 != chunck) {
        lua_pushnil(lua);
        return 1;
    }
    size_t vlens;
    char *val = http_header(pack, key, &vlens);
    if (NULL == val) {
        lua_pushnil(lua);
        return 1;
    }
    lua_pushlstring(lua, val, vlens);
    return 1;
}
/// <summary>
/// 返回所有 HTTP 头部字段
/// </summary>
/// <param name="pack" type="lightuserdata">http_pack_ctx 指针</param>
/// <returns type="table&lt;string,string&gt;?">key→value 表；分块中间包返回 nil</returns>
static int32_t _lprot_http_heads(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    struct http_pack_ctx *pack = (struct http_pack_ctx *)lua_touserdata(lua, 1);
    int32_t chunck = http_chunked(pack);
    if (0 != chunck
        && 1 != chunck) {
        lua_pushnil(lua);
        return 1;
    }
    uint32_t nhead = http_nheader(pack);
    lua_createtable(lua, 0, (int32_t)nhead);
    http_header_ctx *header;
    for (uint32_t i = 0; i < nhead; i++) {
        header = http_header_at(pack, i);
        lua_pushlstring(lua, header->key.data, header->key.lens);
        lua_pushlstring(lua, header->value.data, header->value.lens);
        lua_settable(lua, -3);
    }
    return 1;
}
/// <summary>
/// 返回 HTTP body 数据指针和长度
/// </summary>
/// <param name="pack" type="lightuserdata">http_pack_ctx 指针</param>
/// <returns type="lightuserdata?">body 数据指针；空时返回 nil</returns>
/// <returns type="integer">body 字节数；空时为 0</returns>
static int32_t _lprot_http_data(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    struct http_pack_ctx *pack = (struct http_pack_ctx *)lua_touserdata(lua, 1);
    size_t lens;
    void *data = http_data(pack, &lens);
    if (0 == lens) {
        lua_pushnil(lua);
        lua_pushinteger(lua, 0);
        return 2;
    }
    LPUB_RET_LUD(lua, data, lens);
}
/// <summary>
/// 以 Lua 字符串形式返回 HTTP body 内容
/// </summary>
/// <param name="pack" type="lightuserdata">http_pack_ctx 指针</param>
/// <returns type="string?">body 内容；空时返回 nil</returns>
static int32_t _lprot_http_datastr(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    struct http_pack_ctx *pack = (struct http_pack_ctx *)lua_touserdata(lua, 1);
    size_t lens;
    void *data = http_data(pack, &lens);
    if (0 == lens) {
        lua_pushnil(lua);
        return 1;
    }
    lua_pushlstring(lua, data, lens);
    return 1;
}
//srey.http
LUAMOD_API int luaopen_http(lua_State *lua) {
    luaL_Reg reg[] = {
        { "code_status", _lprot_http_code_status },
        { "chunked", _lprot_http_chunked },
        { "status", _lprot_http_status },
        { "head", _lprot_http_head },
        { "heads", _lprot_http_heads },
        { "data", _lprot_http_data },
        { "datastr", _lprot_http_datastr },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
// 内部辅助：构造 Redis 聚合类型（array/set/map/push/attr）的 table，含 resp_type 和 resp_nelem 字段
static void _lprot_redis_agg(lua_State *lua, const char *type, int64_t nelem) {
    lua_createtable(lua, 0, 2);
    lua_pushstring(lua, type);
    lua_setfield(lua, -2, "resp_type");
    lua_pushinteger(lua, nelem);
    lua_setfield(lua, -2, "resp_nelem");
}
/// <summary>
/// 解析一个 Redis RESP 节点的值
/// </summary>
/// <param name="pk" type="lightuserdata">redis_pack_ctx 节点指针；nil 时返回 nil</param>
/// <returns type="string|integer|number|boolean|nil|RedisAggValue">标量直接返回；聚合类型（array/set/map/push/attr）返回 RedisAggValue</returns>
static int32_t _lprot_redis_value(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    redis_pack_ctx *pk = lua_touserdata(lua, 1);
    if (NULL == pk) {
        lua_pushnil(lua);
        return 1;
    }
    switch (pk->prot) {
    case RESP_STRING:   // 简单字符串
    case RESP_ERROR:    // 错误字符串
    case RESP_BSTRING:  // 批量字符串
    case RESP_BERROR:   // 批量错误
    case RESP_VERB:     // 带类型的字符串
        if (pk->len < 0) {
            lua_pushnil(lua);
        } else if (0 == pk->len) {
            lua_pushstring(lua, "");
        } else {
            lua_pushlstring(lua, pk->data, (size_t)pk->len);
        }
        break;
    case RESP_INTEGER:  // 整数
    case RESP_BIGNUM:   // 大整数
        lua_pushinteger(lua, pk->ival);
        break;
    case RESP_NIL:      // Null 值
        lua_pushnil(lua);
        break;
    case RESP_BOOL:     // 布尔值
        lua_pushboolean(lua, (int32_t)pk->ival);
        break;
    case RESP_DOUBLE:   // 浮点数
        lua_pushnumber(lua, pk->dval);
        break;
    case RESP_ARRAY:    // 数组
        _lprot_redis_agg(lua, "array", pk->nelem);
        break;
    case RESP_SET:      // 集合
        _lprot_redis_agg(lua, "set", pk->nelem);
        break;
    case RESP_PUSHE:    // 推送消息
        _lprot_redis_agg(lua, "push", pk->nelem);
        break;
    case RESP_MAP:      // 映射
        _lprot_redis_agg(lua, "map", pk->nelem);
        break;
    case RESP_ATTR:     // 属性
        _lprot_redis_agg(lua, "attr", pk->nelem);
        break;
    default:
        lua_pushnil(lua);
        break;
    }
    return 1;
}
/// <summary>
/// 获取 Redis RESP 链表中下一个节点指针
/// </summary>
/// <param name="pk" type="lightuserdata">redis_pack_ctx 节点指针</param>
/// <returns type="lightuserdata?">下一个节点指针；无后续节点返回 nil</returns>
static int32_t _lprot_redis_next(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    redis_pack_ctx *pk = lua_touserdata(lua, 1);
    if (NULL == pk
        || NULL == pk->next) {
        lua_pushnil(lua);
        return 1;
    }
    lua_pushlightuserdata(lua, pk->next);
    return 1;
}
//srey.redis
LUAMOD_API int luaopen_redis(lua_State *lua) {
    luaL_Reg reg[] = {
        { "value", _lprot_redis_value },
        { "next", _lprot_redis_next },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
//srey.smtp
/// <summary>
/// 创建 SMTP 客户端上下文（不立即建立连接）
/// </summary>
/// <param name="ip" type="string">SMTP 服务器 IP</param>
/// <param name="port" type="integer">SMTP 服务器端口</param>
/// <param name="evssl" type="lightuserdata|nil">SSL 上下文；nil 表示明文</param>
/// <param name="user" type="string">认证用户名</param>
/// <param name="psw" type="string">认证密码</param>
/// <returns type="_smtp_ctx">SMTP 对象</returns>
static int32_t _lprot_smtp_new(lua_State *lua) {
    const char *ip = luaL_checkstring(lua, 1);
    uint16_t port = (uint16_t)luaL_checkinteger(lua, 2);
    struct evssl_ctx *evssl = NULL;
    if (LUA_TNIL != lua_type(lua, 3)) {
        LUACHECK_LUDATA(lua, 3);
        evssl = lua_touserdata(lua, 3);
    }
    const char *user = luaL_checkstring(lua, 4);
    const char *psw = luaL_checkstring(lua, 5);
    smtp_ctx *smtp = lua_newuserdata(lua, sizeof(smtp_ctx));
    smtp_init(smtp, ip, port, evssl, user, psw);
    ASSOC_MTABLE(lua, MT_SMTP);
    return 1;
}
/// <summary>
/// 发送 QUIT 命令并清理 SMTP 连接上下文（绑定为 __gc，由 Lua GC 自动调用）
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <returns>无</returns>
static int32_t _lprot_smtp_free(lua_State *lua) {
    smtp_ctx *smtp = luaL_checkudata(lua, 1, MT_SMTP);
    if (NULL != smtp->task && INVALID_SOCK != smtp->fd) {
        char *cmd = smtp_pack_quit();
        ev_ud_context(&smtp->task->loader->netev, smtp->fd, smtp->skid, NULL);
        ev_send(&smtp->task->loader->netev, smtp->fd, smtp->skid, cmd, strlen(cmd), 0);
    }
    secure_zero(smtp->psw, sizeof(smtp->psw));
    return 0;
}
/// <summary>
/// 返回 SMTP 连接的 fd 和 skid
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <returns type="integer">socket fd</returns>
/// <returns type="integer">skid</returns>
static int32_t _lprot_smtp_sock_id(lua_State *lua) {
    smtp_ctx *smtp = luaL_checkudata(lua, 1, MT_SMTP);
    lua_pushinteger(lua, smtp->fd);
    lua_pushinteger(lua, smtp->skid);
    return 2;
}
/// <summary>
/// 尝试建立 SMTP 连接（异步）
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <returns type="boolean">发起成功 true，失败 false</returns>
static int32_t _lprot_smtp_try_connect(lua_State *lua) {
    smtp_ctx *smtp = luaL_checkudata(lua, 1, MT_SMTP);
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    int32_t rtn = smtp_try_connect(task, smtp);
    if (ERR_OK == rtn) {
        lua_pushboolean(lua, 1);
    } else {
        lua_pushboolean(lua, 0);
    }
    return 1;
}
/// <summary>
/// 检查 SMTP 响应包的状态码是否匹配指定 code
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <param name="pack" type="lightuserdata">SMTP 响应包指针</param>
/// <param name="code" type="string">期望状态码（如 "220"、"250"）</param>
/// <returns type="boolean">匹配 true，否则 false</returns>
static int32_t _lprot_smtp_check_code(lua_State *lua) {
    luaL_checkudata(lua, 1, MT_SMTP);
    LUACHECK_LUDATA(lua, 2);
    char *pack = (char *)lua_touserdata(lua, 2);
    const char *code = luaL_checkstring(lua, 3);
    if (ERR_OK == smtp_check_code(pack, code)) {
        lua_pushboolean(lua, 1);
    } else {
        lua_pushboolean(lua, 0);
    }
    return 1;
}
/// <summary>
/// 检查 SMTP 响应包是否为 OK（2xx）
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <param name="pack" type="lightuserdata">SMTP 响应包指针</param>
/// <returns type="boolean">2xx 返回 true，否则 false</returns>
static int32_t _lprot_smtp_check_ok(lua_State *lua) {
    luaL_checkudata(lua, 1, MT_SMTP);
    LUACHECK_LUDATA(lua, 2);
    char *pack = (char *)lua_touserdata(lua, 2);
    if (ERR_OK == smtp_check_ok(pack)) {
        lua_pushboolean(lua, 1);
    } else {
        lua_pushboolean(lua, 0);
    }
    return 1;
}
/// <summary>
/// 构造 SMTP RSET 重置命令
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lprot_smtp_pack_reset(lua_State *lua) {
    luaL_checkudata(lua, 1, MT_SMTP);
    char *cmd = smtp_pack_reset();
    LPUB_RET_LUD(lua, (void *)cmd, strlen(cmd));
}
/// <summary>
/// 构造 SMTP MAIL FROM 命令（CRLF 注入防御：地址含 CRLF 时返回 nil）
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <param name="from" type="string">发件人邮箱地址</param>
/// <returns type="lightuserdata?">命令数据指针；地址含 CRLF 时返回 nil</returns>
/// <returns type="integer">数据长度；地址含 CRLF 时为 0</returns>
static int32_t _lprot_smtp_pack_from(lua_State *lua) {
    luaL_checkudata(lua, 1, MT_SMTP);
    const char *from = luaL_checkstring(lua, 2);
    char *cmd = smtp_pack_from(from);
    if (NULL == cmd) {
        lua_pushnil(lua);
        lua_pushinteger(lua, 0);
        return 2;
    }
    LPUB_RET_LUD(lua, cmd, strlen(cmd));
}
/// <summary>
/// 构造 SMTP RCPT TO 命令（CRLF 注入防御：地址含 CRLF 时返回 nil）
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <param name="rcpt" type="string">收件人邮箱地址</param>
/// <returns type="lightuserdata?">命令数据指针；地址含 CRLF 时返回 nil</returns>
/// <returns type="integer">数据长度；地址含 CRLF 时为 0</returns>
static int32_t _lprot_smtp_pack_rcpt(lua_State *lua) {
    luaL_checkudata(lua, 1, MT_SMTP);
    const char *rcpt = luaL_checkstring(lua, 2);
    char *cmd = smtp_pack_rcpt(rcpt);
    if (NULL == cmd) {
        lua_pushnil(lua);
        lua_pushinteger(lua, 0);
        return 2;
    }
    LPUB_RET_LUD(lua, cmd, strlen(cmd));
}
/// <summary>
/// 构造 SMTP DATA 命令（开始传输邮件内容）
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lprot_smtp_pack_data(lua_State *lua) {
    luaL_checkudata(lua, 1, MT_SMTP);
    char *cmd = smtp_pack_data();
    LPUB_RET_LUD(lua, cmd, strlen(cmd));
}
/// <summary>
/// 构造 SMTP QUIT 断连命令
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lprot_smtp_pack_quit(lua_State *lua) {
    luaL_checkudata(lua, 1, MT_SMTP);
    char *cmd = smtp_pack_quit();
    LPUB_RET_LUD(lua, cmd, strlen(cmd));
}
/// <summary>
/// 构造 SMTP NOOP 心跳命令（保持连接）
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lprot_smtp_pack_ping(lua_State *lua) {
    luaL_checkudata(lua, 1, MT_SMTP);
    char *cmd = smtp_pack_ping();
    LPUB_RET_LUD(lua, cmd, strlen(cmd));
}
LUAMOD_API int luaopen_smtp(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lprot_smtp_new },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "try_connect", _lprot_smtp_try_connect },
        { "check_code",_lprot_smtp_check_code },
        { "check_ok", _lprot_smtp_check_ok },
        { "pack_reset", _lprot_smtp_pack_reset },
        { "pack_from", _lprot_smtp_pack_from },
        { "pack_rcpt", _lprot_smtp_pack_rcpt },
        { "pack_data", _lprot_smtp_pack_data },
        { "pack_quit", _lprot_smtp_pack_quit },
        { "pack_ping", _lprot_smtp_pack_ping },
        { "sock_id", _lprot_smtp_sock_id },
        { "__gc", _lprot_smtp_free },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_SMTP, reg_new, reg_func);
    return 1;
}
//srey.smtp.mail
/// <summary>
/// 创建邮件上下文，用于组装邮件内容
/// </summary>
/// <param>无</param>
/// <returns type="_smtp_mail_ctx">邮件对象</returns>
static int32_t _lprot_mail_new(lua_State *lua) {
    mail_ctx *mail = lua_newuserdata(lua, sizeof(mail_ctx));
    mail_init(mail);
    ASSOC_MTABLE(lua, MT_SMTP_MAIL);
    return 1;
}
/// <summary>
/// 释放邮件上下文内部资源（绑定为 __gc，由 Lua GC 自动调用）
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <returns>无</returns>
static int32_t _lprot_mail_free(lua_State *lua) {
    mail_ctx *mail = luaL_checkudata(lua, 1, MT_SMTP_MAIL);
    mail_free(mail);
    return 0;
}
/// <summary>
/// 设置邮件是否需要回执
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <param name="reply" type="integer?">0 不需要，其他值请求回执；nil 视为 0</param>
/// <returns>无</returns>
static int32_t _lprot_mail_reply(lua_State *lua) {
    mail_ctx *mail = luaL_checkudata(lua, 1, MT_SMTP_MAIL);
    int32_t reply;
    if (LUA_TNIL == lua_type(lua, 2)) {
        reply = 0;
    } else {
        reply = (int32_t)luaL_checkinteger(lua, 2);
    }
    mail_reply(mail, reply);
    return 0;
}
/// <summary>
/// 设置邮件主题
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <param name="subject" type="string">邮件主题</param>
/// <returns>无</returns>
static int32_t _lprot_mail_subject(lua_State *lua) {
    mail_ctx *mail = luaL_checkudata(lua, 1, MT_SMTP_MAIL);
    const char *subject = luaL_checkstring(lua, 2);
    mail_subject(mail, subject);
    return 0;
}
/// <summary>
/// 设置邮件纯文本正文内容
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <param name="msg" type="string">纯文本正文</param>
/// <returns>无</returns>
static int32_t _lprot_mail_msg(lua_State *lua) {
    mail_ctx *mail = luaL_checkudata(lua, 1, MT_SMTP_MAIL);
    const char *msg = luaL_checkstring(lua, 2);
    mail_msg(mail, msg);
    return 0;
}
/// <summary>
/// 设置邮件 HTML 正文内容
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <param name="html" type="string">HTML 正文</param>
/// <returns>无</returns>
static int32_t _lprot_mail_html(lua_State *lua) {
    mail_ctx *mail = luaL_checkudata(lua, 1, MT_SMTP_MAIL);
    const char *html = luaL_checkstring(lua, 2);
    mail_html(mail, html, strlen(html));
    return 0;
}
/// <summary>
/// 设置发件人姓名和邮箱地址
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <param name="name" type="string">发件人显示名</param>
/// <param name="email" type="string">发件人邮箱地址</param>
/// <returns>无</returns>
static int32_t _lprot_mail_from(lua_State *lua) {
    mail_ctx *mail = luaL_checkudata(lua, 1, MT_SMTP_MAIL);
    const char *name = luaL_checkstring(lua, 2);
    const char *email = luaL_checkstring(lua, 3);
    mail_from(mail, name, email);
    return 0;
}
/// <summary>
/// 添加收件人地址
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <param name="email" type="string">收件人邮箱</param>
/// <param name="type" type="integer">收件人类型（TO / CC / BCC，对应 mail_addr_type 枚举）</param>
/// <returns>无</returns>
static int32_t _lprot_mail_addrs_add(lua_State *lua) {
    mail_ctx *mail = luaL_checkudata(lua, 1, MT_SMTP_MAIL);
    const char *email = luaL_checkstring(lua, 2);
    mail_addr_type type = (mail_addr_type)luaL_checkinteger(lua, 3);
    mail_addrs_add(mail, email, type);
    return 0;
}
/// <summary>
/// 清空所有收件人列表
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <returns>无</returns>
static int32_t _lprot_mail_addrs_clear(lua_State *lua) {
    mail_ctx *mail = luaL_checkudata(lua, 1, MT_SMTP_MAIL);
    mail_addrs_clear(mail);
    return 0;
}
/// <summary>
/// 添加附件文件
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <param name="file" type="string">附件文件路径</param>
/// <returns>无</returns>
static int32_t _lprot_mail_attach_add(lua_State *lua) {
    mail_ctx *mail = luaL_checkudata(lua, 1, MT_SMTP_MAIL);
    const char *file = luaL_checkstring(lua, 2);
    mail_attach_add(mail, file);
    return 0;
}
/// <summary>
/// 清空所有附件列表
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <returns>无</returns>
static int32_t _lprot_mail_attach_clear(lua_State *lua) {
    mail_ctx *mail = luaL_checkudata(lua, 1, MT_SMTP_MAIL);
    mail_attach_clear(mail);
    return 0;
}
/// <summary>
/// 清空邮件上下文中的所有内容（主题 / 正文 / 收件人 / 附件）
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <returns>无</returns>
static int32_t _lprot_mail_clear(lua_State *lua) {
    mail_ctx *mail = luaL_checkudata(lua, 1, MT_SMTP_MAIL);
    mail_clear(mail);
    return 0;
}
/// <summary>
/// 将邮件上下文序列化为 MIME 格式内容字符串
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <returns type="lightuserdata">MIME 字符串指针</returns>
/// <returns type="integer">字符串长度</returns>
static int32_t _lprot_mail_pack(lua_State *lua) {
    mail_ctx *mail = luaL_checkudata(lua, 1, MT_SMTP_MAIL);
    char *content = mail_pack(mail);
    LPUB_RET_LUD(lua, content, strlen(content));
}
LUAMOD_API int luaopen_mail(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lprot_mail_new },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "reply", _lprot_mail_reply },
        { "subject",  _lprot_mail_subject },
        { "msg",  _lprot_mail_msg },
        { "html",  _lprot_mail_html },
        { "from",  _lprot_mail_from },
        { "addrs_add",  _lprot_mail_addrs_add },
        { "addrs_clear",  _lprot_mail_addrs_clear },
        { "attach_add",  _lprot_mail_attach_add },
        { "attach_clear",  _lprot_mail_attach_clear },
        { "clear",  _lprot_mail_clear },
        { "pack",  _lprot_mail_pack },
        { "__gc", _lprot_mail_free },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_SMTP_MAIL, reg_new, reg_func);
    return 1;
}
