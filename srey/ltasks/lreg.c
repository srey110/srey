#include "lib.h"
#if WITH_LUA
#include "lua/lauxlib.h"

#define MSG_PUSH_NETPUB(msg)\
lua_pushinteger(lua, msg->pktype);\
lua_pushinteger(lua, msg->fd);\
lua_pushinteger(lua, msg->skid)

static int32_t _lreg_setlog(lua_State *lua) {
    if (LUA_TNIL != lua_type(lua, 1)) {
        LOG_LEVEL lv = (LOG_LEVEL)luaL_checkinteger(lua, 1);
        SETLOGLV(lv);
    }
    if (LUA_TNIL != lua_type(lua, 2)) {
        int32_t prt = (int32_t)luaL_checkinteger(lua, 2);
        SETLOGPRT(prt);
    }
    return 0;
}
static int32_t _lreg_log(lua_State *lua) {
    LOG_LEVEL lv = (LOG_LEVEL)luaL_checkinteger(lua, 1);
    const char *file = luaL_checkstring(lua, 2);
    int32_t line = (int32_t)luaL_checkinteger(lua, 3);
    const char *log = luaL_checkstring(lua, 4);
    loger_log(&g_logerctx, lv, "[%s][%s %d]%s", _getlvstr(lv), __FILENAME__(file), line, log);
    return 0;
}
static int32_t _lreg_remoteaddr(lua_State *lua) {
    netaddr_ctx addr;
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    if (-1 == fd) {
        return 0;
    }
    if (ERR_OK != netaddr_remoteaddr(&addr, fd, sock_family(fd))) {
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
#if WITH_SSL
static int32_t _lreg_evssl_new(lua_State *lua) {
    const char *name = luaL_checkstring(lua, 1);
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
    certs_register(srey, name, ssl);
    lua_pushlightuserdata(lua, ssl);
    return 1;
}
static int32_t _lreg_evssl_p12new(lua_State *lua) {
    const char *name = luaL_checkstring(lua, 1);
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
    certs_register(srey, name, ssl);
    lua_pushlightuserdata(lua, ssl);
    return 1;
}
static int32_t _lreg_evssl_qury(lua_State *lua) {
    const char *name = luaL_checkstring(lua, 1);
    struct evssl_ctx *ssl = certs_qury(srey, name);
    if (NULL == ssl) {
        return 0;
    }
    lua_pushlightuserdata(lua, ssl);
    return 1;
}
#endif
static int32_t _lreg_task_qury(lua_State *lua) {
    int32_t name = (int32_t)luaL_checkinteger(lua, 1);
    task_ctx *task = srey_taskqury(srey, name);
    if (NULL == task) {
        return 0;
    }
    lua_pushlightuserdata(lua, task);
    return 1;
}
static int32_t _lreg_task_name(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    lua_pushinteger(lua, task_name(task));
    return 1;
}
static int32_t _lreg_task_session(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    lua_pushinteger(lua, task_session(task));
    return 1;
}
static int32_t _lreg_task_call(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 2)) {
        data = (void *)luaL_checklstring(lua, 2, &size);
    } else {
        data = lua_touserdata(lua, 2);
        size = (size_t)luaL_checkinteger(lua, 3);
    }
    task_call(task, data, size, 1);
    return 0;
}
static int32_t _lreg_task_request(lua_State *lua) {
    task_ctx *dst = lua_touserdata(lua, 1);
    task_ctx *src = lua_touserdata(lua, 2);
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 3)) {
        data = (void *)luaL_checklstring(lua, 3, &size);
    } else {
        data = lua_touserdata(lua, 3);
        size = (size_t)luaL_checkinteger(lua, 4);
    }
    size_t lens;
    void *resp = task_request(dst, src, data, size, 1, &lens);
    if (NULL == resp) {
        return 0;
    }
    lua_pushlightuserdata(lua, resp);
    lua_pushinteger(lua, lens);
    return 2;
}
static int32_t _lreg_task_response(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    uint64_t sess = (uint64_t)luaL_checkinteger(lua, 2);
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 3)) {
        data = (void *)luaL_checklstring(lua, 3, &size);
    } else {
        data = lua_touserdata(lua, 3);
        size = (size_t)luaL_checkinteger(lua, 4);
    }
    task_response(task, sess, data, size, 1);
    return 0;
}
static int32_t _lreg_setud_typstat(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    int8_t pktype = -1;
    int8_t status = -1;
    if (LUA_TNIL != lua_type(lua, 3)) {
        pktype = (int8_t)luaL_checkinteger(lua, 3);
    }
    if (LUA_TNIL != lua_type(lua, 4)) {
        status = (int8_t)luaL_checkinteger(lua, 4);
    }
    ev_setud_typstat(srey_netev(srey), fd, skid, pktype, status);
    return 0;
}
static int32_t _lreg_setud_data(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    void *task = lua_touserdata(lua, 3);
    ev_setud_data(srey_netev(srey), fd, skid, task);
    return 0;
}
static int32_t _lreg_sleep(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    uint32_t time = (uint32_t)luaL_checkinteger(lua, 2);
    task_sleep(task, time);
    return 0;
}
static int32_t _lreg_timeout(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    uint64_t session = (uint64_t)luaL_checkinteger(lua, 2);
    uint32_t time = (uint32_t)luaL_checkinteger(lua, 3);
    task_timeout(task, session, time);
    return 0;
}
static int32_t _lreg_connect(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    int32_t ptype = (int32_t)luaL_checkinteger(lua, 2);
    struct evssl_ctx *evssl = lua_touserdata(lua, 3);
    const char *ip = luaL_checkstring(lua, 4);
    uint16_t port = (uint16_t)luaL_checkinteger(lua, 5);
    int32_t sendev = (int32_t)luaL_checkinteger(lua, 6);
    uint64_t skid;
    SOCKET fd = task_netconnect(task, ptype, evssl, ip, port, sendev, &skid);
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
    if (ERR_OK == task_netlisten(task, ptype, evssl, ip, port, sendev)) {
        lua_pushboolean(lua, 1);
    } else {
        lua_pushboolean(lua, 0);
    }
    return 1;
}
static int32_t _lreg_udp(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    const char *ip = luaL_checkstring(lua, 2);
    uint16_t port = (uint16_t)luaL_checkinteger(lua, 3);
    uint64_t skid;
    SOCKET fd = task_netudp(task, ip, port, &skid);
    if (INVALID_SOCK == fd) {
        lua_pushinteger(lua, -1);
        return 1;
    }
    lua_pushinteger(lua, fd);
    lua_pushinteger(lua, skid);
    return 2;
}
static int32_t _lreg_synsend(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 2);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 3);
    pack_type ptype = (pack_type)luaL_checkinteger(lua, 4);
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 5)) {
        data = (void *)luaL_checklstring(lua, 5, &size);
    } else {
        data = lua_touserdata(lua, 5);
        size = (size_t)luaL_checkinteger(lua, 6);
    }
    size_t lens;
    void *resp = task_synsend(task, fd, skid, data, size, &lens, ptype);
    if (NULL == resp) {
        return 0;
    }
    lua_pushlightuserdata(lua, resp);
    lua_pushinteger(lua, lens);
    return 2;
}
static int32_t _lreg_send(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 2);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 3);
    pack_type ptype = (pack_type)luaL_checkinteger(lua, 4);
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 5)) {
        data = (void *)luaL_checklstring(lua, 5, &size);
    } else {
        data = lua_touserdata(lua, 5);
        size = (size_t)luaL_checkinteger(lua, 6);
    }
    if (ERR_OK == task_netsend(task, fd, skid, data, size, 0, ptype)) {
        lua_pushboolean(lua, 1);
    } else {
        lua_pushboolean(lua, 0);
    }
    return 1;
}
static int32_t _lreg_synsendto(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 2);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 3);
    const char *ip = luaL_checkstring(lua, 4);
    uint16_t port = (uint16_t)luaL_checkinteger(lua, 5);
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 6)) {
        data = (void *)luaL_checklstring(lua, 6, &size);
    } else {
        data = lua_touserdata(lua, 6);
        size = (size_t)luaL_checkinteger(lua, 7);
    }
    size_t lens;
    void *resp = task_synsendto(task, fd, skid, ip, port, data, size, &lens);
    if (NULL == resp) {
        return 0;
    }
    lua_pushlightuserdata(lua, resp);
    lua_pushinteger(lua, lens);
    return 2;
}
static int32_t _lreg_sendto(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 2);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 3);
    const char *ip = luaL_checkstring(lua, 4);
    uint16_t port = (uint16_t)luaL_checkinteger(lua, 5);
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 6)) {
        data = (void *)luaL_checklstring(lua, 6, &size);
    } else {
        data = lua_touserdata(lua, 6);
        size = (size_t)luaL_checkinteger(lua, 7);
    }
    if (ERR_OK == task_sendto(task, fd, skid, ip, port, data, size, 0)) {
        lua_pushboolean(lua, 1);
    } else {
        lua_pushboolean(lua, 0);
    }
    return 1;
}
static int32_t _lreg_close(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    ev_close(srey_netev(srey), fd, skid);
    return 0;
}
static int32_t _lreg_simple_pack(lua_State *lua) {
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
    for (int32_t i = 0; i < 3; i++) {
        lua_pushlstring(lua, buf[i].data, buf[i].lens);
    }
    return 3;
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
        return 0;
    }
    lua_pushlightuserdata(lua, data);
    lua_pushinteger(lua, lens);
    return 2;
}
static int32_t _lreg_websock_connect(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    const char *host = luaL_checkstring(lua, 2);
    uint16_t port = (uint16_t)luaL_checkinteger(lua, 3);
    struct evssl_ctx *evssl = lua_touserdata(lua, 4);
    uint64_t skid;
    SOCKET fd = websock_connect(task, host, port, evssl, &skid);
    if (INVALID_SOCK == fd) {
        lua_pushinteger(lua, -1);
        return 1;
    }
    lua_pushinteger(lua, fd);
    lua_pushinteger(lua, skid);
    return 2;
}
static int32_t _lreg_websock_pack(lua_State *lua) {
    void *pack = lua_touserdata(lua, 1);
    lua_pushinteger(lua, websock_pack_proto(pack));
    lua_pushinteger(lua, websock_pack_fin(pack));
    size_t lens;
    void *data = websock_pack_data(pack, &lens);
    if (lens > 0){
        lua_pushlightuserdata(lua, data);
        lua_pushinteger(lua, lens);
        return 4;
    }
    return 2;
}
static int32_t _lreg_websock_ping(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    websock_ping(srey_netev(srey), fd, skid);
    return 0;
}
static int32_t _lreg_websock_pong(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    websock_pong(srey_netev(srey), fd, skid);
    return 0;
}
static int32_t _lreg_websock_close(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    websock_close(srey_netev(srey), fd, skid);
    return 0;
}
static int32_t _lreg_websock_text(lua_State *lua) {
    void *data;
    size_t dlens;
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    if (LUA_TSTRING == lua_type(lua, 3)) {
        data = (void *)luaL_checklstring(lua, 3, &dlens);
    } else {
        data = lua_touserdata(lua, 3);
        dlens = (size_t)luaL_checkinteger(lua, 4);
    }
    if (LUA_TSTRING == lua_type(lua, 5)) {
        size_t klens;
        const char *key = luaL_checklstring(lua, 5, &klens);
        websock_text(srey_netev(srey), fd, skid, (char *)key, data, dlens);
    } else {
        websock_text(srey_netev(srey), fd, skid, NULL, data, dlens);
    }
    return 0;
}
static int32_t _lreg_websock_binary(lua_State *lua) {
    void *data;
    size_t dlens;
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    if (LUA_TSTRING == lua_type(lua, 3)) {
        data = (void *)luaL_checklstring(lua, 3, &dlens);
    } else {
        data = lua_touserdata(lua, 3);
        dlens = (size_t)luaL_checkinteger(lua, 4);
    }
    if (LUA_TSTRING == lua_type(lua, 5)) {
        size_t klens;
        const char *key = luaL_checklstring(lua, 5, &klens);
        websock_binary(srey_netev(srey), fd, skid, (char *)key, data, dlens);
    } else {
        websock_binary(srey_netev(srey), fd, skid, NULL, data, dlens);
    }
    return 0;
}
static int32_t _lreg_websock_continuation(lua_State *lua) {
    void *data;
    size_t dlens;
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    int32_t fin = (int32_t)luaL_checkinteger(lua, 3);
    if (LUA_TSTRING == lua_type(lua, 4)) {
        data = (void *)luaL_checklstring(lua, 4, &dlens);
    } else {
        data = lua_touserdata(lua, 4);
        dlens = (size_t)luaL_checkinteger(lua, 5);
    }
    if (LUA_TSTRING == lua_type(lua, 6)) {
        size_t klens;
        const char *key = luaL_checklstring(lua, 6, &klens);
        websock_continuation(srey_netev(srey), fd, skid, fin, (char *)key, data, dlens);
    } else {
        websock_continuation(srey_netev(srey), fd, skid, fin, NULL, data, dlens);
    }
    return 0;
}
static int32_t _lreg_msg_info(lua_State *lua) {
    message_ctx *msg = lua_touserdata(lua, 1);
    int32_t argc = 0;
    switch (msg->msgtype) {
    case MSG_TYPE_STARTED:
        break;
    case MSG_TYPE_CLOSING:
        break;
    case MSG_TYPE_TIMEOUT://sess
        lua_pushinteger(lua, msg->session);
        argc++;
        break;
    case MSG_TYPE_CONNECT://pktype fd skid err
        MSG_PUSH_NETPUB(msg);
        argc += 3;
        lua_pushinteger(lua, msg->erro);
        argc++;
        break;
    case MSG_TYPE_HANDSHAKED://pktype fd skid
        MSG_PUSH_NETPUB(msg);
        argc += 3;
        break;
    case MSG_TYPE_ACCEPT://pktype fd skid
        MSG_PUSH_NETPUB(msg);
        argc += 3;
        break;
    case MSG_TYPE_SEND://pktype fd skid size
        MSG_PUSH_NETPUB(msg);
        argc += 3;
        lua_pushinteger(lua, msg->size);
        argc++;
        break;
    case MSG_TYPE_CLOSE://pktype fd skid
        MSG_PUSH_NETPUB(msg);
        argc += 3;
        break;
    case MSG_TYPE_RECV://pktype fd skid data size
        MSG_PUSH_NETPUB(msg);
        argc += 3;
        lua_pushlightuserdata(lua, msg->data);
        argc++;
        lua_pushinteger(lua, msg->size);
        argc++;
        break;
    case MSG_TYPE_RECVFROM: {//fd skid data size ip port
        char ip[IP_LENS];
        udp_msg_ctx *umsg = msg->data;
        netaddr_ip(&umsg->addr, ip);
        lua_pushinteger(lua, msg->fd);
        argc++;
        lua_pushinteger(lua, msg->skid);
        argc++;
        lua_pushlightuserdata(lua, umsg->data);
        argc++;
        lua_pushinteger(lua, msg->size);
        argc++;
        lua_pushstring(lua, ip);
        argc++;
        lua_pushinteger(lua, netaddr_port(&umsg->addr));
        argc++;
        break;
    }
    case MSG_TYPE_REQUEST://sess src data size
        lua_pushinteger(lua, msg->session);
        argc++;
        lua_pushinteger(lua, msg->src);
        argc++;
        lua_pushlightuserdata(lua, msg->data);
        argc++;
        lua_pushinteger(lua, msg->size);
        argc++;
        break;
    default:
        break;
    }
    return argc;
}
LUAMOD_API int luaopen_srey_utils(lua_State *lua) {
    luaL_Reg reg[] = {
        { "log", _lreg_log },//(LOG_LEVEL, file, line, log)
        { "setlog", _lreg_setlog },//(LOG_LEVEL, int32_t)

        { "remoteaddr", _lreg_remoteaddr },//(SOCKET) ip, port
        { "utostr", _lreg_udtostr },//(data, size) str

        { "md5", _lreg_md5 },//(data, size) str
        { "sha1_b64encode", _lreg_sha1_b64encode },//(data, size) str
        { "b64encode", _lreg_b64encode },//(data, size) str
        { "b64decode", _lreg_b64decode },//(data, size) str
        { "urlencode", _lreg_urlencode },//(data) str
#if WITH_SSL
        { "evssl_new", _lreg_evssl_new },//(name, ca, cert, key, filetype, verify) evssl_ctx
        { "evssl_p12new", _lreg_evssl_p12new },//(name, p12, pwd, verify) evssl_ctx
        { "evssl_qury", _lreg_evssl_qury },//(name) evssl_ctx
#endif
        { "task_qury", _lreg_task_qury },//(name) task_ctx
        { "task_name", _lreg_task_name },//(task) name
        { "task_session", _lreg_task_session },//(task) session
        { "task_call", _lreg_task_call },//(dst, data, size)
        { "task_request", _lreg_task_request },//(dst, src, data, size) resp lens
        { "task_response", _lreg_task_response },//(task, sess, data, size)

        { "setud_typstat", _lreg_setud_typstat },//(fd, skid, pktype, status)
        { "setud_data", _lreg_setud_data },//(fd, skid, data)

        { "sleep", _lreg_sleep },//(task, time)
        { "timeout", _lreg_timeout },//(task, sess, time)
        { "connect", _lreg_connect },//(task, pktype, evssl, host, port, sendev) fd skid
        { "listen", _lreg_listen },//(task, pktype, evssl, host, port, sendev) bool
        { "udp", _lreg_udp },//(task, host, port) fd skid
        { "synsend", _lreg_synsend },//(task, fd, skid, pktype, data, size) resp lens
        { "send", _lreg_send },//(fd, skid, pktype, data, size)
        { "synsendto", _lreg_synsendto },//(task, fd, skid, host, port, data, size) resp lens
        { "sendto", _lreg_sendto },//(fd, skid, host, port, data, size)
        { "close", _lreg_close },//(fd, skid)

        { "simple_pack", _lreg_simple_pack },//(pack) data size

        { "http_chunked", _lreg_http_chunked },//(pack) chunked
        { "http_status", _lreg_http_status },//(pack) str str str
        { "http_head", _lreg_http_head },//(pack, key) str
        { "http_heads", _lreg_http_heads },//(pack) table
        { "http_data", _lreg_http_data },//(pack) data size

        { "websock_connect", _lreg_websock_connect },//(task, ip, port, evssl) fd skid
        { "websock_pack", _lreg_websock_pack },//(pack) proto fin data size
        { "websock_ping", _lreg_websock_ping },//(fd, skid)
        { "websock_pong", _lreg_websock_pong },//(fd, skid)
        { "websock_close", _lreg_websock_close },//(fd, skid)
        { "websock_text", _lreg_websock_text },//(fd, skid, data, size, key)
        { "websock_binary", _lreg_websock_binary },//(fd, skid, data, size, key)
        { "websock_continuation", _lreg_websock_continuation },//(fd, skid, fin, data, size, key)

        { "msg_info", _lreg_msg_info },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}

#endif
