#include "lbind/lpub.h"

// Lua 绑定：打包 harbor 跨节点消息；返回数据指针和长度
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
    default:
        data = lua_touserdata(lua, 5);
        size = (size_t)luaL_checkinteger(lua, 6);
        break;
    }
    data = harbor_pack(task, call, reqtype, key, data, size, &size);
    lua_pushlightuserdata(lua, data);
    lua_pushinteger(lua, size);
    return 2;
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
// Lua 绑定：返回本机 DNS 解析到的 IP 地址字符串
static int32_t _lprot_dns_ip(lua_State *lua) {
    lua_pushstring(lua, dns_get_ip());
    return 1;
}
// Lua 绑定：构造 DNS 查询请求包，domain 为域名，ipv6 为 1 时查询 AAAA 记录；返回二进制字符串
static int32_t _lprot_dns_pack(lua_State *lua) {
    const char *domain = luaL_checkstring(lua, 1);
    int32_t ipv6 = (int32_t)luaL_checkinteger(lua, 2);
    char buf[ONEK] = { 0 };
    size_t lens = (size_t)dns_request_pack(buf, domain, ipv6);
    lua_pushlstring(lua, buf, lens);
    return 1;
}
// Lua 绑定：解析 DNS 响应包，pack 为数据指针，packlen 为包长度，返回 IP 字符串数组 table，失败返回 nil
static int32_t _lprot_dns_unpack(lua_State *lua) {
    void *pack = lua_touserdata(lua, 1);
    size_t packlen = (size_t)luaL_checkinteger(lua, 2);
    size_t n;
    dns_ip *ips = dns_parse_pack(pack, packlen, &n);
    if (NULL == ips) {
        lua_pushnil(lua);
        return 1;
    }
    lua_createtable(lua, 0, (int32_t)n);
    for (size_t i = 0; i < n; i++) {
        lua_pushinteger(lua, i + 1);
        lua_pushstring(lua, ips[i].ip);
        lua_settable(lua, -3);
    }
    FREE(ips);
    return 1;
}
//srey.dns
LUAMOD_API int luaopen_dns(lua_State *lua) {
    luaL_Reg reg[] = {
        { "ip", _lprot_dns_ip },
        { "pack", _lprot_dns_pack },
        { "unpack", _lprot_dns_unpack },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
// Lua 绑定：打包自定义协议（custz）数据；返回数据指针和长度
static int32_t _lprot_custz_pack(lua_State *lua) {
    uint8_t pktype = (uint8_t)luaL_checkinteger(lua, 1);
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 2)) {
        data = (void *)luaL_checklstring(lua, 2, &size);
    } else {
        data = lua_touserdata(lua, 2);
        size = (size_t)luaL_checkinteger(lua, 3);
    }
    data = custz_pack(pktype, data, size, &size);
    lua_pushlightuserdata(lua, data);
    lua_pushinteger(lua, size);
    return 2;
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
// Lua 绑定：解包 WebSocket 帧，返回包含 fin/prot/secprot/secpack/data/size 字段的 table
static int32_t _lprot_websock_unpack(lua_State *lua) {
    struct websock_pack_ctx *pack = lua_touserdata(lua, 1);
    lua_createtable(lua, 0, 6);
    lua_pushstring(lua, "fin");
    lua_pushinteger(lua, websock_fin(pack));      // 是否为最终分片
    lua_settable(lua, -3);
    lua_pushstring(lua, "prot");
    lua_pushinteger(lua, websock_prot(pack));     // 帧操作码（文本/二进制/控制帧等）
    lua_settable(lua, -3);
    int32_t secprot = websock_secprot(pack);
    if (PACK_NONE != secprot) {
        lua_pushstring(lua, "secprot");
        lua_pushinteger(lua, secprot);           // 子协议类型
        lua_settable(lua, -3);
    }
    void *secpack = websock_secpack(pack);
    if (NULL != secpack) {
        lua_pushstring(lua, "secpack");
        lua_pushlightuserdata(lua, secpack);     // 子协议数据包指针
        lua_settable(lua, -3);
    }
    size_t lens;
    void *data = websock_data(pack, &lens);
    if (lens > 0) {
        lua_pushstring(lua, "data");
        lua_pushlightuserdata(lua, data);
        lua_settable(lua, -3);
    }
    lua_pushstring(lua, "size");
    lua_pushinteger(lua, lens);
    lua_settable(lua, -3);
    return 1;
}
// Lua 绑定：构造 WebSocket 握手请求包（HTTP Upgrade），返回数据指针和长度
static int32_t _lprot_websock_pack_handshake(lua_State *lua) {
    char *host = NULL;
    if (LUA_TSTRING == lua_type(lua, 1)) {
        host = (char *)luaL_checkstring(lua, 1);
    }
    char *secprot = NULL;
    if (LUA_TSTRING == lua_type(lua, 2)) {
        secprot = (char *)luaL_checkstring(lua, 2);  // Sec-WebSocket-Protocol 值
    }
    char *hspack = websock_pack_handshake(host, secprot);
    lua_pushlightuserdata(lua, hspack);
    lua_pushinteger(lua, strlen(hspack));
    return 2;
}
// Lua 绑定：构造 WebSocket Ping 帧，mask 为是否启用掩码；返回数据指针和长度
static int32_t _lprot_websock_pack_ping(lua_State *lua) {
    int32_t mask = (int32_t)luaL_checkinteger(lua, 1);
    size_t lens;
    void *pack = websock_pack_ping(mask, &lens);
    lua_pushlightuserdata(lua, pack);
    lua_pushinteger(lua, lens);
    return 2;
}
// Lua 绑定：构造 WebSocket Pong 帧；返回数据指针和长度
static int32_t _lprot_websock_pack_pong(lua_State *lua) {
    int32_t mask = (int32_t)luaL_checkinteger(lua, 1);
    size_t lens;
    void *pack = websock_pack_pong(mask, &lens);
    lua_pushlightuserdata(lua, pack);
    lua_pushinteger(lua, lens);
    return 2;
}
// Lua 绑定：构造 WebSocket Close 帧；返回数据指针和长度
static int32_t _lprot_websock_pack_close(lua_State *lua) {
    int32_t mask = (int32_t)luaL_checkinteger(lua, 1);
    size_t lens;
    void *pack = websock_pack_close(mask, &lens);
    lua_pushlightuserdata(lua, pack);
    lua_pushinteger(lua, lens);
    return 2;
}
// Lua 绑定：构造 WebSocket Text 帧，fin 为是否最终分片；返回数据指针和长度
static int32_t _lprot_websock_pack_text(lua_State *lua) {
    void *data;
    size_t dlens;
    int32_t mask = (int32_t)luaL_checkinteger(lua, 1);
    int32_t fin = (int32_t)luaL_checkinteger(lua, 2);
    if (LUA_TSTRING == lua_type(lua, 3)) {
        data = (void *)luaL_checklstring(lua, 3, &dlens);
    } else {
        data = lua_touserdata(lua, 3);
        dlens = (size_t)luaL_checkinteger(lua, 4);
    }
    void *pack = websock_pack_text(mask, fin, data, dlens, &dlens);
    lua_pushlightuserdata(lua, pack);
    lua_pushinteger(lua, dlens);
    return 2;
}
// Lua 绑定：构造 WebSocket Binary 帧，fin 为是否最终分片；返回数据指针和长度
static int32_t _lprot_websock_pack_binary(lua_State *lua) {
    void *data;
    size_t dlens;
    int32_t mask = (int32_t)luaL_checkinteger(lua, 1);
    int32_t fin = (int32_t)luaL_checkinteger(lua, 2);
    if (LUA_TSTRING == lua_type(lua, 3)) {
        data = (void *)luaL_checklstring(lua, 3, &dlens);
    } else {
        data = lua_touserdata(lua, 3);
        dlens = (size_t)luaL_checkinteger(lua, 4);
    }
    void *pack = websock_pack_binary(mask, fin, data, dlens, &dlens);
    lua_pushlightuserdata(lua, pack);
    lua_pushinteger(lua, dlens);
    return 2;
}
// Lua 绑定：构造 WebSocket Continuation 帧（续传帧），fin 为是否最终分片；返回数据指针和长度
static int32_t _lprot_websock_pack_continua(lua_State *lua) {
    void *data;
    size_t dlens;
    int32_t mask = (int32_t)luaL_checkinteger(lua, 1);
    int32_t fin = (int32_t)luaL_checkinteger(lua, 2);
    if (LUA_TSTRING == lua_type(lua, 3)) {
        data = (void *)luaL_checklstring(lua, 3, &dlens);
    } else {
        data = lua_touserdata(lua, 3);
        if (LUA_TNUMBER == lua_type(lua, 4)) {
            dlens = (size_t)luaL_checkinteger(lua, 4);
        } else {
            dlens = 0;
        }
    }
    void *pack = websock_pack_continua(mask, fin, data, dlens, &dlens);
    lua_pushlightuserdata(lua, pack);
    lua_pushinteger(lua, dlens);
    return 2;
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
// Lua 绑定：将 HTTP 状态码转换为对应的文本描述（如 200 -> "OK"）
static int32_t _lprot_http_code_status(lua_State *lua) {
    int32_t err = (int32_t)luaL_checkinteger(lua, 1);
    lua_pushstring(lua, http_code_status(err));
    return 1;
}
// Lua 绑定：返回 HTTP 包的分块传输状态（0=非分块，1=首包，2+=分块继续）
static int32_t _lprot_http_chunked(lua_State *lua) {
    struct http_pack_ctx *pack = lua_touserdata(lua, 1);
    lua_pushinteger(lua, http_chunked(pack));
    return 1;
}
// Lua 绑定：返回 HTTP 状态行数组（含版本/状态码/描述，共 3 个元素），分块中间包返回 nil
static int32_t _lprot_http_status(lua_State *lua) {
    struct http_pack_ctx *pack = lua_touserdata(lua, 1);
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
// Lua 绑定：按 key 查找 HTTP 头部字段值，分块中间包或字段不存在时返回 nil
static int32_t _lprot_http_head(lua_State *lua) {
    struct http_pack_ctx *pack = lua_touserdata(lua, 1);
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
// Lua 绑定：返回所有 HTTP 头部字段的 key->value table，分块中间包返回 nil
static int32_t _lprot_http_heads(lua_State *lua) {
    struct http_pack_ctx *pack = lua_touserdata(lua, 1);
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
// Lua 绑定：返回 HTTP body 数据指针和长度；body 为空时返回 nil 和 0
static int32_t _lprot_http_data(lua_State *lua) {
    struct http_pack_ctx *pack = lua_touserdata(lua, 1);
    size_t lens;
    void *data = http_data(pack, &lens);
    if (0 == lens) {
        lua_pushnil(lua);
        lua_pushinteger(lua, 0);
        return 2;
    }
    lua_pushlightuserdata(lua, data);
    lua_pushinteger(lua, lens);
    return 2;
}
// Lua 绑定：以 Lua 字符串形式返回 HTTP body 内容；body 为空时返回 nil
static int32_t _lprot_http_datastr(lua_State *lua) {
    struct http_pack_ctx *pack = lua_touserdata(lua, 1);
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
    lua_pushstring(lua, "resp_type");
    lua_pushstring(lua, type);
    lua_settable(lua, -3);
    lua_pushstring(lua, "resp_nelem");
    lua_pushinteger(lua, nelem);
    lua_settable(lua, -3);
}
// Lua 绑定：解析一个 Redis RESP 节点值并压栈；聚合类型返回含元素数的 table
static int32_t _lprot_redis_value(lua_State *lua) {
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
        break;
    }
    return 1;
}
// Lua 绑定：获取 Redis RESP 链表中下一个节点指针，无后续节点返回 nil
static int32_t _lprot_redis_next(lua_State *lua) {
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
// Lua 绑定：创建 SMTP 客户端上下文，初始化服务器地址、用户名和密码（不立即建立连接）
static int32_t _lprot_smtp_new(lua_State *lua) {
    const char *ip = luaL_checkstring(lua, 1);
    uint16_t port = (uint16_t)luaL_checkinteger(lua, 2);
    struct evssl_ctx *evssl = lua_touserdata(lua, 3);
    const char *user = luaL_checkstring(lua, 4);
    const char *psw = luaL_checkstring(lua, 5);
    smtp_ctx *smtp = lua_newuserdata(lua, sizeof(smtp_ctx));
    smtp_init(smtp, ip, port, evssl, user, psw);
    ASSOC_MTABLE(lua, "_smtp_ctx");
    return 1;
}
// Lua 绑定（__gc）：发送 QUIT 命令并清理 SMTP 连接上下文
static int32_t _lprot_smtp_free(lua_State *lua) {
    smtp_ctx *smtp = lua_touserdata(lua, 1);
    if (NULL != smtp->task) {
        char *cmd = smtp_pack_quit();
        ev_ud_context(&smtp->task->loader->netev, smtp->fd, smtp->skid, NULL);
        ev_send(&smtp->task->loader->netev, smtp->fd, smtp->skid, cmd, strlen(cmd), 0);
    }
    return 0;
}
// Lua 绑定：返回 SMTP 连接的 fd 和 skid
static int32_t _lprot_smtp_sock_id(lua_State *lua) {
    smtp_ctx *smtp = lua_touserdata(lua, 1);
    lua_pushinteger(lua, smtp->fd);
    lua_pushinteger(lua, smtp->skid);
    return 2;
}
// Lua 绑定：尝试建立 SMTP 连接（异步），成功返回 true，失败返回 false
static int32_t _lprot_smtp_try_connect(lua_State *lua) {
    smtp_ctx *smtp = lua_touserdata(lua, 1);
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    int32_t rtn = smtp_try_connect(task, smtp);
    if (ERR_OK == rtn) {
        lua_pushboolean(lua, 1);
    } else {
        lua_pushboolean(lua, 0);
    }
    return 1;
}
// Lua 绑定：检查 SMTP 响应包的状态码是否匹配指定 code，匹配返回 true，否则返回 false
static int32_t _lprot_smtp_check_code(lua_State *lua) {
    char *pack = (char *)lua_touserdata(lua, 2);
    const char *code = luaL_checkstring(lua, 3);
    if (ERR_OK == smtp_check_code(pack, code)) {
        lua_pushboolean(lua, 1);
    } else {
        lua_pushboolean(lua, 0);
    }
    return 1;
}
// Lua 绑定：检查 SMTP 响应包是否为 OK（2xx），是返回 true，否则返回 false
static int32_t _lprot_smtp_check_ok(lua_State *lua) {
    char *pack = (char *)lua_touserdata(lua, 2);
    if (ERR_OK == smtp_check_ok(pack)) {
        lua_pushboolean(lua, 1);
    } else {
        lua_pushboolean(lua, 0);
    }
    return 1;
}
// Lua 绑定：构造 SMTP RSET 重置命令；返回数据指针和长度
static int32_t _lprot_smtp_pack_reset(lua_State *lua) {
    char *cmd = smtp_pack_reset();
    lua_pushlightuserdata(lua, (void *)cmd);
    lua_pushinteger(lua, strlen(cmd));
    return 2;
}
// Lua 绑定：构造 SMTP MAIL FROM 命令；返回数据指针和长度
static int32_t _lprot_smtp_pack_from(lua_State *lua) {
    const char *from = luaL_checkstring(lua, 2);
    char *cmd = smtp_pack_from(from);
    lua_pushlightuserdata(lua, cmd);
    lua_pushinteger(lua, strlen(cmd));
    return 2;
}
// Lua 绑定：构造 SMTP RCPT TO 命令；返回数据指针和长度
static int32_t _lprot_smtp_pack_rcpt(lua_State *lua) {
    const char *rcpt = luaL_checkstring(lua, 2);
    char *cmd = smtp_pack_rcpt(rcpt);
    lua_pushlightuserdata(lua, cmd);
    lua_pushinteger(lua, strlen(cmd));
    return 2;
}
// Lua 绑定：构造 SMTP DATA 命令（开始传输邮件内容）；返回数据指针和长度
static int32_t _lprot_smtp_pack_data(lua_State *lua) {
    char *cmd = smtp_pack_data();
    lua_pushlightuserdata(lua, cmd);
    lua_pushinteger(lua, strlen(cmd));
    return 2;
}
// Lua 绑定：构造 SMTP QUIT 断连命令；返回数据指针和长度
static int32_t _lprot_smtp_pack_quit(lua_State *lua) {
    char *cmd = smtp_pack_quit();
    lua_pushlightuserdata(lua, cmd);
    lua_pushinteger(lua, strlen(cmd));
    return 2;
}
// Lua 绑定：构造 SMTP NOOP 心跳命令（保持连接）；返回数据指针和长度
static int32_t _lprot_smtp_pack_ping(lua_State *lua) {
    char *cmd = smtp_pack_ping();
    lua_pushlightuserdata(lua, cmd);
    lua_pushinteger(lua, strlen(cmd));
    return 2;
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
    REG_MTABLE(lua, "_smtp_ctx", reg_new, reg_func);
    return 1;
}
//srey.smtp.mail
// Lua 绑定：创建邮件上下文，用于组装邮件内容
static int32_t _lprot_mail_new(lua_State *lua) {
    mail_ctx *mail = lua_newuserdata(lua, sizeof(mail_ctx));
    mail_init(mail);
    ASSOC_MTABLE(lua, "_smtp_mail_ctx");
    return 1;
}
// Lua 绑定（__gc）：释放邮件上下文内部资源
static int32_t _lprot_mail_free(lua_State *lua) {
    mail_ctx *mail = lua_touserdata(lua, 1);
    mail_free(mail);
    return 0;
}
// Lua 绑定：设置邮件是否需要回执（reply 为 0 不需要，其他值为请求回执）
static int32_t _lprot_mail_reply(lua_State *lua) {
    mail_ctx *mail = lua_touserdata(lua, 1);
    int32_t reply;
    if (LUA_TNIL == lua_type(lua, 2)) {
        reply = 0;
    } else {
        reply = (int32_t)luaL_checkinteger(lua, 2);
    }
    mail_reply(mail, reply);
    return 0;
}
// Lua 绑定：设置邮件主题
static int32_t _lprot_mail_subject(lua_State *lua) {
    mail_ctx *mail = lua_touserdata(lua, 1);
    const char *subject = luaL_checkstring(lua, 2);
    mail_subject(mail, subject);
    return 0;
}
// Lua 绑定：设置邮件纯文本正文内容
static int32_t _lprot_mail_msg(lua_State *lua) {
    mail_ctx *mail = lua_touserdata(lua, 1);
    const char *msg = luaL_checkstring(lua, 2);
    mail_msg(mail, msg);
    return 0;
}
// Lua 绑定：设置邮件 HTML 正文内容
static int32_t _lprot_mail_html(lua_State *lua) {
    mail_ctx *mail = lua_touserdata(lua, 1);
    const char *html = luaL_checkstring(lua, 2);
    mail_html(mail, html, strlen(html));
    return 0;
}
// Lua 绑定：设置发件人姓名和邮箱地址
static int32_t _lprot_mail_from(lua_State *lua) {
    mail_ctx *mail = lua_touserdata(lua, 1);
    const char *name = luaL_checkstring(lua, 2);
    const char *email = luaL_checkstring(lua, 3);
    mail_from(mail, name, email);
    return 0;
}
// Lua 绑定：添加收件人地址，type 区分收件人类型（TO/CC/BCC）
static int32_t _lprot_mail_addrs_add(lua_State *lua) {
    mail_ctx *mail = lua_touserdata(lua, 1);
    const char *email = luaL_checkstring(lua, 2);
    mail_addr_type type = (mail_addr_type)luaL_checkinteger(lua, 3);
    mail_addrs_add(mail, email, type);
    return 0;
}
// Lua 绑定：清空所有收件人列表
static int32_t _lprot_mail_addrs_clear(lua_State *lua) {
    mail_ctx *mail = lua_touserdata(lua, 1);
    mail_addrs_clear(mail);
    return 0;
}
// Lua 绑定：添加附件文件路径
static int32_t _lprot_mail_attach_add(lua_State *lua) {
    mail_ctx *mail = lua_touserdata(lua, 1);
    const char *file = luaL_checkstring(lua, 2);
    mail_attach_add(mail, file);
    return 0;
}
// Lua 绑定：清空所有附件列表
static int32_t _lprot_mail_attach_clear(lua_State *lua) {
    mail_ctx *mail = lua_touserdata(lua, 1);
    mail_attach_clear(mail);
    return 0;
}
// Lua 绑定：清空邮件上下文中的所有内容（主题/正文/收件人/附件）
static int32_t _lprot_mail_clear(lua_State *lua) {
    mail_ctx *mail = lua_touserdata(lua, 1);
    mail_clear(mail);
    return 0;
}
// Lua 绑定：将邮件上下文序列化为 MIME 格式内容字符串；返回数据指针和长度
static int32_t _lprot_mail_pack(lua_State *lua) {
    mail_ctx *mail = lua_touserdata(lua, 1);
    char *content = mail_pack(mail);
    lua_pushlightuserdata(lua, content);
    lua_pushinteger(lua, strlen(content));
    return 2;
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
    REG_MTABLE(lua, "_smtp_mail_ctx", reg_new, reg_func);
    return 1;
}
