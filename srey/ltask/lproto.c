#include "ltask/lpub.h"
#include "tasks/harbor.h"

#if WITH_LUA

static int32_t _lproto_harbor_pack(lua_State *lua) {
    name_t task = (name_t)luaL_checkinteger(lua, 1);
    int32_t call = (int32_t)luaL_checkinteger(lua, 2);
    uint8_t reqtype = (uint8_t)luaL_checkinteger(lua, 3);
    const char *key = luaL_checkstring(lua, 4);
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
        { "pack", _lproto_harbor_pack },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
static int32_t _lproto_dns_ip(lua_State *lua) {
    lua_pushstring(lua, dns_get_ip());
    return 1;
}
static int32_t _lproto_dns_pack(lua_State *lua) {
    const char *domain = luaL_checkstring(lua, 1);
    int32_t ipv6 = (int32_t)luaL_checkinteger(lua, 2);
    char buf[ONEK] = { 0 };
    size_t lens = (size_t)dns_request_pack(buf, domain, ipv6);
    lua_pushlstring(lua, buf, lens);
    return 1;
}
static int32_t _lproto_dns_unpack(lua_State *lua) {
    void *pack = lua_touserdata(lua, 1);
    size_t n;
    dns_ip *ips = dns_parse_pack(pack, &n);
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
        { "ip", _lproto_dns_ip },
        { "pack", _lproto_dns_pack },
        { "unpack", _lproto_dns_unpack },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
static int32_t _lproto_custz_pack(lua_State *lua) {
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 1)) {
        data = (void *)luaL_checklstring(lua, 1, &size);
    } else {
        data = lua_touserdata(lua, 1);
        size = (size_t)luaL_checkinteger(lua, 2);
    }
    data = (void *)custz_pack(data, size, &size);
    lua_pushlightuserdata(lua, data);
    lua_pushinteger(lua, size);
    return 2;
}
static int32_t _lproto_custz_unpack(lua_State *lua) {
    struct custz_pack_ctx *pack = lua_touserdata(lua, 1);
    size_t lens;
    void *data = custz_data(pack, &lens);
    lua_pushlightuserdata(lua, data);
    lua_pushinteger(lua, lens);
    return 2;
}
//srey.custz
LUAMOD_API int luaopen_custz(lua_State *lua) {
    luaL_Reg reg[] = {
        { "pack", _lproto_custz_pack },
        { "unpack", _lproto_custz_unpack },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
static int32_t _lproto_websock_handshake_pack(lua_State *lua) {
    char *host = NULL;
    if (LUA_TSTRING == lua_type(lua, 1)) {
        host = (char *)luaL_checkstring(lua, 1);
    }
    char *hspack = websock_handshake_pack(host);
    lua_pushlightuserdata(lua, hspack);
    lua_pushinteger(lua, strlen(hspack));
    return 2;
}
static int32_t _lproto_websock_unpack(lua_State *lua) {
    struct websock_pack_ctx *pack = lua_touserdata(lua, 1);
    lua_createtable(lua, 0, 4);
    lua_pushstring(lua, "proto");
    lua_pushinteger(lua, websock_pack_proto(pack));
    lua_settable(lua, -3);
    lua_pushstring(lua, "fin");
    lua_pushinteger(lua, websock_pack_fin(pack));
    lua_settable(lua, -3);
    size_t lens;
    void *data = websock_pack_data(pack, &lens);
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
static int32_t _lproto_websock_ping(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    int32_t mask = (int32_t)luaL_checkinteger(lua, 3);
    websock_ping(&g_scheduler->netev, fd, skid, mask);
    return 0;
}
static int32_t _lproto_websock_pong(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    int32_t client = (int32_t)luaL_checkinteger(lua, 3);
    websock_pong(&g_scheduler->netev, fd, skid, client);
    return 0;
}
static int32_t _lproto_websock_close(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    int32_t mask = (int32_t)luaL_checkinteger(lua, 3);
    websock_close(&g_scheduler->netev, fd, skid, mask);
    return 0;
}
static int32_t _lproto_websock_text(lua_State *lua) {
    void *data;
    size_t dlens;
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    int32_t mask = (int32_t)luaL_checkinteger(lua, 3);
    int32_t fin = (int32_t)luaL_checkinteger(lua, 4);
    if (LUA_TSTRING == lua_type(lua, 5)) {
        data = (void *)luaL_checklstring(lua, 5, &dlens);
    } else {
        data = lua_touserdata(lua, 5);
        dlens = (size_t)luaL_checkinteger(lua, 6);
    }
    websock_text(&g_scheduler->netev, fd, skid, mask, fin, data, dlens);
    return 0;
}
static int32_t _lproto_websock_binary(lua_State *lua) {
    void *data;
    size_t dlens;
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    int32_t mask = (int32_t)luaL_checkinteger(lua, 3);
    int32_t fin = (int32_t)luaL_checkinteger(lua, 4);
    if (LUA_TSTRING == lua_type(lua, 5)) {
        data = (void *)luaL_checklstring(lua, 5, &dlens);
    } else {
        data = lua_touserdata(lua, 5);
        dlens = (size_t)luaL_checkinteger(lua, 6);
    }
    websock_binary(&g_scheduler->netev, fd, skid, mask, fin, data, dlens);
    return 0;
}
static int32_t _lproto_websock_continuation(lua_State *lua) {
    void *data;
    size_t dlens;
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    int32_t mask = (int32_t)luaL_checkinteger(lua, 3);
    int32_t fin = (int32_t)luaL_checkinteger(lua, 4);
    if (LUA_TSTRING == lua_type(lua, 5)) {
        data = (void *)luaL_checklstring(lua, 5, &dlens);
    } else {
        data = lua_touserdata(lua, 5);
        if (LUA_TNUMBER == lua_type(lua, 6)) {
            dlens = (size_t)luaL_checkinteger(lua, 6);
        } else {
            dlens = 0;
        }
    }
    websock_continuation(&g_scheduler->netev, fd, skid, mask, fin, data, dlens);
    return 0;
}
//srey.websock
LUAMOD_API int luaopen_websock(lua_State *lua) {
    luaL_Reg reg[] = {
        { "handshake_pack", _lproto_websock_handshake_pack },
        { "unpack", _lproto_websock_unpack },
        { "ping", _lproto_websock_ping },
        { "pong", _lproto_websock_pong },
        { "close", _lproto_websock_close },
        { "text", _lproto_websock_text },
        { "binary", _lproto_websock_binary },
        { "continuation", _lproto_websock_continuation },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
static int32_t _lproto_http_code_status(lua_State *lua) {
    int32_t err = (int32_t)luaL_checkinteger(lua, 1);
    lua_pushstring(lua, http_code_status(err));
    return 1;
}
static int32_t _lproto_http_chunked(lua_State *lua) {
    struct http_pack_ctx *pack = lua_touserdata(lua, 1);
    lua_pushinteger(lua, http_chunked(pack));
    return 1;
}
static int32_t _lproto_http_status(lua_State *lua) {
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
static int32_t _lproto_http_head(lua_State *lua) {
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
static int32_t _lproto_http_heads(lua_State *lua) {
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
static int32_t _lproto_http_data(lua_State *lua) {
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
static int32_t _lproto_http_datastr(lua_State *lua) {
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
        { "code_status", _lproto_http_code_status },
        { "chunked", _lproto_http_chunked },
        { "status", _lproto_http_status },
        { "head", _lproto_http_head },
        { "heads", _lproto_http_heads },
        { "data", _lproto_http_data },
        { "datastr", _lproto_http_datastr },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
static void _lproto_redis_agg(lua_State *lua, const char *type, int64_t nelem) {
    lua_createtable(lua, 0, 2);
    lua_pushstring(lua, "resp_type");
    lua_pushstring(lua, type);
    lua_settable(lua, -3);
    lua_pushstring(lua, "resp_nelem");
    lua_pushinteger(lua, nelem);
    lua_settable(lua, -3);
}
static int32_t _lproto_redis_value(lua_State *lua) {
    redis_pack_ctx *pk = lua_touserdata(lua, 1);
    if (NULL == pk) {
        lua_pushnil(lua);
        return 1;
    }
    switch (pk->proto) {
    case RESP_STRING:
    case RESP_ERROR:
    case RESP_BSTRING:
    case RESP_BERROR:
    case RESP_VERB:
        if (pk->len < 0) {
            lua_pushnil(lua);
        } else if (0 == pk->len) {
            lua_pushstring(lua, "");
        } else {
            lua_pushlstring(lua, pk->data, pk->len);
        }
        break;
    case RESP_INTEGER:
    case RESP_BIGNUM:
        lua_pushinteger(lua, pk->ival);
        break;
    case RESP_NIL:
        lua_pushnil(lua);
        break;
    case RESP_BOOL:
        lua_pushboolean(lua, (int32_t)pk->ival);
        break;
    case RESP_DOUBLE:
        lua_pushnumber(lua, pk->dval);
        break;
    case RESP_ARRAY:
        _lproto_redis_agg(lua, "array", pk->nelem);
        break;
    case RESP_SET:
        _lproto_redis_agg(lua, "set", pk->nelem);
        break;
    case RESP_PUSHE:
        _lproto_redis_agg(lua, "push", pk->nelem);
        break;
    case RESP_MAP:
        _lproto_redis_agg(lua, "map", pk->nelem);
        break;
    case RESP_ATTR:
        _lproto_redis_agg(lua, "attr", pk->nelem);
        break;
    default:
        break;
    }
    return 1;
}
static int32_t _lproto_redis_next(lua_State *lua) {
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
        { "value", _lproto_redis_value },
        { "next", _lproto_redis_next },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}

#endif
