#include "lib.h"
#if WITH_LUA
#include "lua/lapi.h"
#include "lua/lauxlib.h"
#include "lua/lstring.h"

#define MSG_PUSH_NETPUB(msg)\
lua_pushinteger(lua, msg->pktype);\
lua_pushinteger(lua, msg->fd);\
lua_pushinteger(lua, msg->skid)

static int32_t _lreg_log_setlv(lua_State *lua) {
    LOG_LEVEL lv = (LOG_LEVEL)luaL_checkinteger(lua, 1);
    log_setlv(lv);
    return 0;
}
static int32_t _lreg_log(lua_State *lua) {
    LOG_LEVEL lv = (LOG_LEVEL)luaL_checkinteger(lua, 1);
    const char *file = luaL_checkstring(lua, 2);
    int32_t line = (int32_t)luaL_checkinteger(lua, 3);
    const char *log = luaL_checkstring(lua, 4);
    slog(lv, "[%s %d] %s", __FILENAME__(file), line, log);
    return 0;
}
static int32_t _lreg_remoteaddr(lua_State *lua) {
    netaddr_ctx addr;
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    if (-1 == fd) {
        return 0;
    }
    if (ERR_OK != netaddr_remote(&addr, fd)) {
        return 0;
    }
    char ip[IP_LENS];
    if (ERR_OK != netaddr_ip(&addr, ip)) {
        return 0;
    }
    uint16_t port = netaddr_port(&addr);
    lua_pushstring(lua, ip);
    lua_pushinteger(lua, port);
    return 2;
}
static int32_t _lreg_udtostr(lua_State *lua) {
    void *data = lua_touserdata(lua, 1);
    size_t size = (size_t)luaL_checkinteger(lua, 2);
    lua_pushlstring(lua, data, size);
    return 1;
}
static int32_t _lreg_getid(lua_State *lua) {
    lua_pushinteger(lua, createid());
    return 1;
}
static int32_t _lreg_md5(lua_State *lua) {
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 1)) {
        data = (void *)luaL_checklstring(lua, 1, &size);
    } else {
        data = lua_touserdata(lua, 1);
        size = (size_t)luaL_checkinteger(lua, 2);
    }
    char strmd5[33];
    md5((const char*)data, size, strmd5);
    lua_pushstring(lua, strmd5);
    return 1;
}
static int32_t _lreg_sha1_b64encode(lua_State *lua) {
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 1)) {
        data = (void *)luaL_checklstring(lua, 1, &size);
    } else {
        data = lua_touserdata(lua, 1);
        size = (size_t)luaL_checkinteger(lua, 2);
    }
    char sha1str[20];
    sha1(data, size, sha1str);
    char *b64 = b64encode(sha1str, sizeof(sha1str), &size);
    lua_pushlstring(lua, b64, size);
    FREE(b64);
    return 1;
}
static int32_t _lreg_b64encode(lua_State *lua) {
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 1)) {
        data = (void *)luaL_checklstring(lua, 1, &size);
    } else {
        data = lua_touserdata(lua, 1);
        size = (size_t)luaL_checkinteger(lua, 2);
    }
    size_t blens;
    char *b64 = b64encode(data, size, &blens);
    lua_pushlstring(lua, b64, blens);
    FREE(b64);
    return 1;
}
static int32_t _lreg_b64decode(lua_State *lua) {
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 1)) {
        data = (void *)luaL_checklstring(lua, 1, &size);
    } else {
        data = lua_touserdata(lua, 1);
        size = (size_t)luaL_checkinteger(lua, 2);
    }
    size_t blens;
    char *b64 = b64decode(data, size, &blens);
    lua_pushlstring(lua, b64, blens);
    FREE(b64);
    return 1;
}
static int32_t _lreg_urlencode(lua_State *lua) {
    size_t size;
    size_t lens;
    const char *data = luaL_checklstring(lua, 1, &size);
    char *encode = urlencode(data, size, &lens);
    lua_pushlstring(lua, encode, lens);
    FREE(encode);
    return 1;
}
static int32_t _lreg_evssl_new(lua_State *lua) {
#if WITH_SSL
    name_t name = (name_t)luaL_checkinteger(lua, 1);
    const char *ca = luaL_checkstring(lua, 2);
    const char *cert = luaL_checkstring(lua, 3);
    const char *key = luaL_checkstring(lua, 4);
    int32_t keytype = (int32_t)luaL_checkinteger(lua, 5);
    int32_t verify = (int32_t)luaL_checkinteger(lua, 6);
    lua_pop(lua, 6);
    lua_getglobal(lua, "_propath");
    const char *propath = lua_tostring(lua, 1);
    lua_pop(lua, 1);
    char capath[PATH_LENS] = { 0 };
    char certpath[PATH_LENS] = { 0 };
    char keypath[PATH_LENS] = { 0 };
    if (0 != strlen(ca)) {
        SNPRINTF(capath, sizeof(capath) - 1, "%s%s%s%s%s",
            propath, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, ca);
    }
    if (0 != strlen(cert)) {
        SNPRINTF(certpath, sizeof(certpath) - 1, "%s%s%s%s%s",
            propath, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, cert);
    }
    if (0 != strlen(key)) {
        SNPRINTF(keypath, sizeof(keypath) - 1, "%s%s%s%s%s",
            propath, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, key);
    }
    evssl_ctx *ssl = evssl_new(capath, certpath, keypath, keytype, verify);
    if (ERR_OK != srey_ssl_register(srey, name, ssl)) {
        evssl_free(ssl);
        return 0;
    }
    lua_pushlightuserdata(lua, ssl);
    return 1;
#else
    return 0;
#endif
}
static int32_t _lreg_evssl_p12new(lua_State *lua) {
#if WITH_SSL
    name_t name = (name_t)luaL_checkinteger(lua, 1);
    const char *p12 = luaL_checkstring(lua, 2);
    const char *pwd = luaL_checkstring(lua, 3);
    int32_t verify = (int32_t)luaL_checkinteger(lua, 4);
    char p12path[PATH_LENS] = { 0 };
    if (0 != strlen(p12)) {
        lua_pop(lua, 4);
        lua_getglobal(lua, "_propath");
        const char *propath = lua_tostring(lua, 1);
        lua_pop(lua, 1);
        SNPRINTF(p12path, sizeof(p12path) - 1, "%s%s%s%s%s",
            propath, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, p12);
    }
    evssl_ctx *ssl = evssl_p12_new(p12path, pwd, verify);
    if (ERR_OK != srey_ssl_register(srey, name, ssl)) {
        evssl_free(ssl);
        return 0;
    }
    lua_pushlightuserdata(lua, ssl);
    return 1;
#else
    return 0;
#endif
}
static int32_t _lreg_evssl_qury(lua_State *lua) {
#if WITH_SSL
    name_t name = (name_t)luaL_checkinteger(lua, 1);
    struct evssl_ctx *ssl = srey_ssl_qury(srey, name);
    if (NULL == ssl) {
        return 0;
    }
    lua_pushlightuserdata(lua, ssl);
    return 1;
#else
    return 0;
#endif
}
static int32_t _lreg_task_grab(lua_State *lua) {
    name_t name = (name_t)luaL_checkinteger(lua, 1);
    task_ctx *task = srey_task_grab(srey, name);
    if (NULL == task) {
        return 0;
    }
    lua_pushlightuserdata(lua, task);
    return 1;
}
static int32_t _lreg_task_incref(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    srey_task_incref(task);
    return 0;
}
static int32_t _lreg_task_ungrab(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    srey_task_ungrab(task);
    return 0;
}
static int32_t _lreg_task_close(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    srey_task_close(task);
    return 0;
}
static int32_t _lreg_task_name(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    lua_pushinteger(lua, task->name);
    return 1;
}
static int32_t _lreg_call(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 2)) {
        data = (void *)luaL_checklstring(lua, 2, &size);
    } else {
        data = lua_touserdata(lua, 2);
        size = (size_t)luaL_checkinteger(lua, 3);
    }
    int32_t copy = (int32_t)luaL_checkinteger(lua, 4);
    srey_call(task, data, size, copy);
    return 0;
}
static int32_t _lreg_request(lua_State *lua) {
    task_ctx *dst = lua_touserdata(lua, 1);
    task_ctx *src = lua_touserdata(lua, 2);
    uint64_t sess = (uint64_t)luaL_checkinteger(lua, 3);
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 4)) {
        data = (void *)luaL_checklstring(lua, 4, &size);
    } else {
        data = lua_touserdata(lua, 4);
        size = (size_t)luaL_checkinteger(lua, 5);
    }
    int32_t copy = (int32_t)luaL_checkinteger(lua, 6);
    srey_request(dst, src, sess, data, size, copy);
    return 0;
}
static int32_t _lreg_response(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    uint64_t sess = (uint64_t)luaL_checkinteger(lua, 2);
    int32_t erro = (int32_t)luaL_checkinteger(lua, 3);
    void *data;
    size_t size;
    int32_t ttype = lua_type(lua, 4);
    if (LUA_TNIL == ttype) {
        data = NULL;
        size = 0;
    } else {
        if (LUA_TSTRING == ttype) {
            data = (void *)luaL_checklstring(lua, 4, &size);
        } else {
            data = lua_touserdata(lua, 4);
            size = (size_t)luaL_checkinteger(lua, 5);
        }
    }
    int32_t copy = (int32_t)luaL_checkinteger(lua, 6);
    srey_response(task, sess, erro, data, size, copy);
    return 0;
}
static int32_t _lreg_setud_pktype(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    int8_t pktype = (int8_t)luaL_checkinteger(lua, 3);
    ev_ud_pktype(&srey->netev, fd, skid, pktype);
    return 0;
}
static int32_t _lreg_setud_status(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    int8_t status = (int8_t)luaL_checkinteger(lua, 3);
    ev_ud_status(&srey->netev, fd, skid, status);
    return 0;
}
static int32_t _lreg_setud_name(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    name_t name = (name_t)luaL_checkinteger(lua, 3);
    ev_ud_name(&srey->netev, fd, skid, name);
    return 0;
}
static int32_t _lreg_setud_sess(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    uint64_t sess = (uint64_t)luaL_checkinteger(lua, 3);
    ev_ud_sess(&srey->netev, fd, skid, sess);
    return 0;
}
static int32_t _lreg_timeout(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    uint64_t sess = (uint64_t)luaL_checkinteger(lua, 2);
    uint32_t time = (uint32_t)luaL_checkinteger(lua, 3);
    srey_timeout(task, sess, time, NULL, NULL, NULL);
    return 0;
}
static int32_t _lreg_connect(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    uint64_t sess = (uint64_t)luaL_checkinteger(lua, 2);
    int32_t ptype = (int32_t)luaL_checkinteger(lua, 3);
    struct evssl_ctx *evssl = lua_touserdata(lua, 4);
    const char *ip = luaL_checkstring(lua, 5);
    uint16_t port = (uint16_t)luaL_checkinteger(lua, 6);
    int32_t sendev = (int32_t)luaL_checkinteger(lua, 7);
    uint64_t skid;
    SOCKET fd = srey_connect(task, sess, ptype, evssl, ip, port, sendev, &skid);
    if (INVALID_SOCK == fd) {
        lua_pushinteger(lua, -1);
        return 1;
    }
    lua_pushinteger(lua, fd);
    lua_pushinteger(lua, skid);
    return 2;
}
static int32_t _lreg_listen(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    int32_t ptype = (int32_t)luaL_checkinteger(lua, 2);
    struct evssl_ctx *evssl = NULL;
    if (LUA_TNIL != lua_type(lua, 3)) {
        evssl = lua_touserdata(lua, 3);
    }
    const char *ip = luaL_checkstring(lua, 4);
    uint16_t port = (uint16_t)luaL_checkinteger(lua, 5);
    int32_t sendev = (int32_t)luaL_checkinteger(lua, 6);
    uint64_t id;
    if (ERR_OK != srey_listen(task, ptype, evssl, ip, port, sendev, &id)) {
        return 0;
    }
    lua_pushinteger(lua, id);
    return 1;
}
static int32_t _lreg_unlisten(lua_State *lua) {
    uint64_t id = (uint64_t)luaL_checkinteger(lua, 1);
    ev_unlisten(&srey->netev, id);
    return 0;
}
static int32_t _lreg_udp(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    const char *ip = luaL_checkstring(lua, 2);
    uint16_t port = (uint16_t)luaL_checkinteger(lua, 3);
    uint64_t skid;
    SOCKET fd = srey_udp(task, ip, port, &skid);
    if (INVALID_SOCK == fd) {
        lua_pushinteger(lua, -1);
        return 1;
    }
    lua_pushinteger(lua, fd);
    lua_pushinteger(lua, skid);
    return 2;
}
static int32_t _lreg_send(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 2);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 3);
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 4)) {
        data = (void *)luaL_checklstring(lua, 4, &size);
    } else {
        data = lua_touserdata(lua, 4);
        size = (size_t)luaL_checkinteger(lua, 5);
    }
    int32_t copy = (int32_t)luaL_checkinteger(lua, 6);
    ev_send(&task->srey->netev, fd, skid, data, size, copy);
    return 0;
}
static int32_t _lreg_sendto(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    const char *ip = luaL_checkstring(lua, 3);
    uint16_t port = (uint16_t)luaL_checkinteger(lua, 4);
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 5)) {
        data = (void *)luaL_checklstring(lua, 5, &size);
    } else {
        data = lua_touserdata(lua, 5);
        size = (size_t)luaL_checkinteger(lua, 6);
    }
    if (ERR_OK == ev_sendto(&srey->netev, fd, skid, ip, port, data, size)) {
        lua_pushboolean(lua, 1);
    } else {
        lua_pushboolean(lua, 0);
    }
    return 1;
}
static int32_t _lreg_close(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    ev_close(&srey->netev, fd, skid);
    return 0;
}
static int32_t _lreg_dns_pack(lua_State *lua) {
    const char *domain = luaL_checkstring(lua, 1);
    int32_t ipv6 = (int32_t)luaL_checkinteger(lua, 2);
    char buf[ONEK] = { 0 };
    size_t lens = (size_t)dns_request_pack(buf, domain, ipv6);
    lua_pushlstring(lua, buf, lens);
    return 1;
}
static int32_t _lreg_dns_unpack(lua_State *lua) {
    void *pack = lua_touserdata(lua, 1);
    size_t n;
    dns_ip *ips = dns_parse_pack(pack, &n);
    lua_createtable(lua, 0, (int32_t)n);
    if (NULL == ips) {
        return 1;
    }
    for (size_t i = 0; i < n; i++) {
        lua_pushinteger(lua, i + 1);
        lua_pushstring(lua, ips[i].ip);
        lua_settable(lua, -3);
    }
    FREE(ips);
    return 1;
}
static int32_t _lreg_simple_pack(lua_State *lua) {
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 1)) {
        data = (void *)luaL_checklstring(lua, 1, &size);
    } else {
        data = lua_touserdata(lua, 1);
        size = (size_t)luaL_checkinteger(lua, 2);
    }
    data = simple_pack(data, size, &size);
    lua_pushlightuserdata(lua, data);
    lua_pushinteger(lua, size);
    return 2;
}
static int32_t _lreg_simple_unpack(lua_State *lua) {
    void *pack = lua_touserdata(lua, 1);
    size_t lens;
    void *data = simple_data(pack, &lens);
    lua_pushlightuserdata(lua, data);
    lua_pushinteger(lua, lens);
    return 2;
}
static int32_t _lreg_http_chunked(lua_State *lua) {
    void *pack = lua_touserdata(lua, 1);
    lua_pushinteger(lua, http_chunked(pack));
    return 1;
}
static int32_t _lreg_http_status(lua_State *lua) {
    void *pack = lua_touserdata(lua, 1);
    int32_t chunck = http_chunked(pack);
    if (0 != chunck
        && 1 != chunck) {
        return 0;
    }
    buf_ctx *buf = http_status(pack);
    lua_createtable(lua, 3, 0);
    for (int32_t i = 0; i < 3; i++) {
        lua_pushlstring(lua, buf[i].data, buf[i].lens);
        lua_rawseti(lua, -2, i + 1);
    }
    return 1;
}
static int32_t _lreg_http_head(lua_State *lua) {
    void *pack = lua_touserdata(lua, 1);
    const char *key = luaL_checkstring(lua, 2);
    int32_t chunck = http_chunked(pack);
    if (0 != chunck
        && 1 != chunck) {
        return 0;
    }
    size_t vlens;
    char *val = http_header(pack, key, &vlens);
    if (NULL == val) {
        return 0;
    }
    lua_pushlstring(lua, val, vlens);
    return 1;
}
static int32_t _lreg_http_heads(lua_State *lua) {
    void *pack = lua_touserdata(lua, 1);
    int32_t chunck = http_chunked(pack);
    if (0 != chunck
        && 1 != chunck) {
        return 0;
    }
    size_t nhead = http_nheader(pack);
    lua_createtable(lua, 0, (int32_t)nhead);
    http_header_ctx *header;
    for (size_t i = 0; i < nhead; i++) {
        header = http_header_at(pack, i);
        lua_pushlstring(lua, header->key.data, header->key.lens);
        lua_pushlstring(lua, header->value.data, header->value.lens);
        lua_settable(lua, -3);
    }
    return 1;
}
static int32_t _lreg_http_data(lua_State *lua) {
    void *pack = lua_touserdata(lua, 1);
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
static int32_t _lreg_websock_hspack(lua_State *lua) {
    char *host = NULL;
    if (LUA_TSTRING == lua_type(lua, 1)) {
        host = (char *)luaL_checkstring(lua, 1);
    }
    char *hspack = websock_handshake_pack(host);
    lua_pushstring(lua, hspack);
    FREE(hspack);
    return 1;
}
static int32_t _lreg_websock_unpack(lua_State *lua) {
    void *pack = lua_touserdata(lua, 1);
    lua_createtable(lua, 0, 4);
    lua_pushstring(lua, "proto");
    lua_pushinteger(lua, websock_pack_proto(pack));
    lua_settable(lua, -3);
    lua_pushstring(lua, "fin");
    lua_pushboolean(lua, websock_pack_fin(pack));
    lua_settable(lua, -3);
    size_t lens;
    void *data = websock_pack_data(pack, &lens);
    if (lens > 0){
        lua_pushstring(lua, "data");
        lua_pushlightuserdata(lua, data);
        lua_settable(lua, -3);
        lua_pushstring(lua, "size");
        lua_pushinteger(lua, lens);
        lua_settable(lua, -3);
    }
    return 1;
}
static int32_t _lreg_websock_ping(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    int32_t mask = (int32_t)luaL_checkinteger(lua, 3);
    websock_ping(&srey->netev, fd, skid, mask);
    return 0;
}
static int32_t _lreg_websock_pong(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    int32_t client = (int32_t)luaL_checkinteger(lua, 3);
    websock_pong(&srey->netev, fd, skid, client);
    return 0;
}
static int32_t _lreg_websock_close(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    int32_t mask = (int32_t)luaL_checkinteger(lua, 3);
    websock_close(&srey->netev, fd, skid, mask);
    return 0;
}
static int32_t _lreg_websock_text(lua_State *lua) {
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
    websock_text(&srey->netev, fd, skid, mask, fin, data, dlens);
    return 0;
}
static int32_t _lreg_websock_binary(lua_State *lua) {
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
    websock_binary(&srey->netev, fd, skid, mask, fin, data, dlens);
    return 0;
}
static int32_t _lreg_websock_continuation(lua_State *lua) {
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
    websock_continuation(&srey->netev, fd, skid, mask, fin, data, dlens);
    return 0;
}
static int32_t _lreg_msg_clean(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    msg_type mtype = (msg_type)luaL_checkinteger(lua, 2);
    pack_type pktype = PACK_NONE;
    if (LUA_TNUMBER == lua_type(lua, 3)) {
        pktype = (pack_type)luaL_checkinteger(lua, 3);
    }
    void *data = lua_touserdata(lua, 4);
    message_clean(task, mtype, pktype, data);
    return 0;
}
LUAMOD_API int luaopen_srey_utils(lua_State *lua) {
    luaL_Reg reg[] = {
        { "log_setlv", _lreg_log_setlv },
        { "log", _lreg_log },

        { "remoteaddr", _lreg_remoteaddr },
        { "utostr", _lreg_udtostr },
        { "getid", _lreg_getid },

        { "md5", _lreg_md5 },
        { "sha1_b64encode", _lreg_sha1_b64encode },
        { "b64encode", _lreg_b64encode },
        { "b64decode", _lreg_b64decode },
        { "urlencode", _lreg_urlencode },

        { "evssl_new", _lreg_evssl_new },
        { "evssl_p12new", _lreg_evssl_p12new },
        { "evssl_qury", _lreg_evssl_qury },

        { "task_grab", _lreg_task_grab },
        { "task_incref", _lreg_task_incref },
        { "task_ungrab", _lreg_task_ungrab },
        { "task_close", _lreg_task_close },
        { "task_name", _lreg_task_name },
        { "task_call", _lreg_call },
        { "task_request", _lreg_request },
        { "task_response", _lreg_response },

        { "setud_pktype", _lreg_setud_pktype },
        { "setud_status", _lreg_setud_status },
        { "setud_name", _lreg_setud_name },
        { "setud_sess",_lreg_setud_sess },

        { "timeout", _lreg_timeout },
        { "connect", _lreg_connect },
        { "listen", _lreg_listen },
        { "unlisten", _lreg_unlisten },
        { "udp", _lreg_udp },
        { "send", _lreg_send },
        { "sendto", _lreg_sendto },
        { "close", _lreg_close },

        { "dns_pack", _lreg_dns_pack },
        { "dns_unpack", _lreg_dns_unpack },

        { "simple_pack", _lreg_simple_pack },
        { "simple_unpack", _lreg_simple_unpack },

        { "http_chunked", _lreg_http_chunked },
        { "http_status", _lreg_http_status },
        { "http_head", _lreg_http_head },
        { "http_heads", _lreg_http_heads },
        { "http_data", _lreg_http_data },

        { "websock_hspack", _lreg_websock_hspack },
        { "websock_unpack", _lreg_websock_unpack },
        { "websock_ping", _lreg_websock_ping },
        { "websock_pong", _lreg_websock_pong },
        { "websock_close", _lreg_websock_close },
        { "websock_text", _lreg_websock_text },
        { "websock_binary", _lreg_websock_binary },
        { "websock_continuation", _lreg_websock_continuation },

        { "msg_clean", _lreg_msg_clean },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}

#endif
