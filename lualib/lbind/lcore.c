#include "lbind/lpub.h"

// 辅助宏：若栈上指定位置为整数则取其值，否则默认为 1（复制语义）
#define COPY_TYPE(lua, idx) (lua_isinteger(lua, idx) ? (int32_t)luaL_checkinteger(lua, idx) : 1)

// Lua 绑定：向当前 task 注册一个超时事件，sess 为会话 id，time 为延迟毫秒数
static int32_t _lcore_timeout(lua_State *lua) {
    uint64_t sess = (uint64_t)luaL_checkinteger(lua, 1);
    uint32_t time = (uint32_t)luaL_checkinteger(lua, 2);
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    task_timeout(task, sess, time, NULL);
    return 0;
}
// Lua 绑定：向目标 task 发送单向调用消息（无响应），data 可为字符串或 userdata
static int32_t _lcore_call(lua_State *lua) {
    task_ctx *dst = lua_touserdata(lua, 1);
    uint8_t reqtype = (uint8_t)luaL_checkinteger(lua, 2);
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 3)) {
        data = (void *)luaL_checklstring(lua, 3, &size);
    } else {
        data = lua_touserdata(lua, 3);
        size = (size_t)luaL_checkinteger(lua, 4);
    }
    int32_t copy = COPY_TYPE(lua, 5);
    task_call(dst, reqtype, data, size, copy);
    return 0;
}
// Lua 绑定：向目标 task 发送请求消息，携带 sess 会话 id 以便对方响应
static int32_t _lcore_request(lua_State *lua) {
    task_ctx *dst = lua_touserdata(lua, 1);
    uint8_t reqtype = (uint8_t)luaL_checkinteger(lua, 2);
    uint64_t sess = (uint64_t)luaL_checkinteger(lua, 3);
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 4)) {
        data = (void *)luaL_checklstring(lua, 4, &size);
    } else {
        data = lua_touserdata(lua, 4);
        size = (size_t)luaL_checkinteger(lua, 5);
    }
    int32_t copy = COPY_TYPE(lua, 6);
    task_ctx *src = global_userdata(lua, CUR_TASK_NAME);
    task_request(dst, src, reqtype, sess, data, size, copy);
    return 0;
}
// Lua 绑定：向请求方 task 回复响应消息，携带错误码 erro 及可选数据
static int32_t _lcore_response(lua_State *lua) {
    task_ctx *dst = lua_touserdata(lua, 1);
    uint64_t sess = (uint64_t)luaL_checkinteger(lua, 2);
    int32_t erro = (int32_t)luaL_checkinteger(lua, 3);
    void *data;
    size_t size;
    int32_t type = lua_type(lua, 4);
    if (LUA_TNIL == type
        || LUA_TNONE == type) {
        data = NULL;
        size = 0;
    } else {
        if (LUA_TSTRING == type) {
            data = (void *)luaL_checklstring(lua, 4, &size);
        } else {
            data = lua_touserdata(lua, 4);
            size = (size_t)luaL_checkinteger(lua, 5);
        }
    }
    int32_t copy = COPY_TYPE(lua, 6);
    task_response(dst, sess, erro, data, size, copy);
    return 0;
}
// Lua 绑定：在当前 task 上监听 TCP/UDP 端口，返回监听 id 或 -1（失败）
static int32_t _lcore_listen(lua_State *lua) {
    pack_type pktype = (pack_type)luaL_checkinteger(lua, 1);
    struct evssl_ctx *evssl = NULL;
    if (LUA_TNIL != lua_type(lua, 2)) {
        evssl = lua_touserdata(lua, 2);
    }
    const char *ip = luaL_checkstring(lua, 3);
    uint16_t port = (uint16_t)luaL_checkinteger(lua, 4);
    int32_t netev = lua_isinteger(lua, 5) ? (int32_t)luaL_checkinteger(lua, 5) : NETEV_NONE;
    uint64_t id;
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (ERR_OK != task_listen(task, pktype, evssl, ip, port, &id, netev)) {
        lua_pushinteger(lua, -1);
    } else {
        lua_pushinteger(lua, id);
    }
    return 1;
}
// Lua 绑定：取消指定监听 id 的监听
static int32_t _lcore_unlisten(lua_State *lua) {
    uint64_t id = (uint64_t)luaL_checkinteger(lua, 1);
    ev_unlisten(&g_loader->netev, id);
    return 0;
}
// Lua 绑定：发起 TCP 连接，成功返回 fd 和 skid，失败返回 INVALID_SOCK
static int32_t _lcore_connect(lua_State *lua) {
    pack_type pktype = (pack_type)luaL_checkinteger(lua, 1);
    struct evssl_ctx *evssl = NULL;
    if (LUA_TNIL != lua_type(lua, 2)) {
        evssl = lua_touserdata(lua, 2);
    }
    const char *ip = luaL_checkstring(lua, 3);
    uint16_t port = (uint16_t)luaL_checkinteger(lua, 4);
    int32_t netev = lua_isinteger(lua, 5) ? (int32_t)luaL_checkinteger(lua, 5) : NETEV_NONE;
    SOCKET fd;
    uint64_t skid;
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (ERR_OK != task_connect(task, pktype, evssl, ip, port, netev, NULL, &fd, &skid)) {
        lua_pushinteger(lua, INVALID_SOCK);
        return 1;
    }
    lua_pushinteger(lua, fd);
    lua_pushinteger(lua, skid);
    return 2;
}
// Lua 绑定：对已有连接进行 SSL 升级握手，成功返回 true，失败返回 false
static int32_t _lcore_ssl_exchange(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    int32_t client = (int32_t)luaL_checkinteger(lua, 3);
    struct evssl_ctx *evssl = lua_touserdata(lua, 4);
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (ERR_OK == ev_ssl(&task->loader->netev, fd, skid, client, evssl)) {
        lua_pushboolean(lua, 1);
    } else {
        lua_pushboolean(lua, 0);
    }
    return 1;
}
// Lua 绑定：创建 UDP 套接字并绑定地址，成功返回 fd 和 skid，失败返回 INVALID_SOCK
static int32_t _lcore_udp(lua_State *lua) {
    const char *ip = luaL_checkstring(lua, 1);
    uint16_t port = (uint16_t)luaL_checkinteger(lua, 2);
    SOCKET fd;
    uint64_t skid;
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (ERR_OK != task_udp(task, ip, port, &fd, &skid)) {
        lua_pushinteger(lua, INVALID_SOCK);
        return 1;
    }
    lua_pushinteger(lua, fd);
    lua_pushinteger(lua, skid);
    return 2;
}
// Lua 绑定：向指定 fd/skid 发送 TCP 数据，成功返回 true，失败返回 false
static int32_t _lcore_send(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 3)) {
        data = (void *)luaL_checklstring(lua, 3, &size);
    } else {
        data = lua_touserdata(lua, 3);
        size = (size_t)luaL_checkinteger(lua, 4);
    }
    int32_t copy = COPY_TYPE(lua, 5);
    if (ERR_OK == ev_send(&g_loader->netev, fd, skid, data, size, copy)) {
        lua_pushboolean(lua, 1);
    } else {
        lua_pushboolean(lua, 0);
    }
    return 1;
}
// Lua 绑定：向指定 ip:port 发送 UDP 数据，成功返回 true，失败返回 false
static int32_t _lcore_sendto(lua_State *lua) {
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
    int32_t copy = COPY_TYPE(lua, 7);
    if (ERR_OK == ev_sendto(&g_loader->netev, fd, skid, ip, port, data, size, copy)) {
        lua_pushboolean(lua, 1);
    } else {
        lua_pushboolean(lua, 0);
    }
    return 1;
}
// Lua 绑定：主动关闭指定 fd/skid 的网络连接
static int32_t _lcore_close(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    ev_close(&g_loader->netev, fd, skid);
    return 0;
}
// Lua 绑定：动态修改指定连接的封包类型（pktype）
static int32_t _lcore_pack_type(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    pack_type pktype = (pack_type)luaL_checkinteger(lua, 3);
    ev_ud_pktype(&g_loader->netev, fd, skid, pktype);
    return 0;
}
// Lua 绑定：设置指定连接的用户自定义状态值
static int32_t _lcore_status(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    int8_t status = (int8_t)luaL_checkinteger(lua, 3);
    ev_ud_status(&g_loader->netev, fd, skid, status);
    return 0;
}
// Lua 绑定：将指定连接绑定到目标 task（消息将投递到对应 task）
static int32_t _lcore_bind_task(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    name_t name = (name_t)luaL_checkinteger(lua, 3);
    ev_ud_name(&g_loader->netev, fd, skid, name);
    return 0;
}
// Lua 绑定：为指定连接设置会话 id（sess），用于关联请求与响应
static int32_t _lcore_session(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    uint64_t sess = (uint64_t)luaL_checkinteger(lua, 3);
    ev_ud_sess(&g_loader->netev, fd, skid, sess);
    return 0;
}
// Lua 绑定：检查指定封包是否允许恢复（协议层分片重组判断），返回 true/false
static int32_t _lcore_may_resume(lua_State *lua) {
    pack_type pktype = (pack_type)luaL_checkinteger(lua, 1);
    void *data = lua_touserdata(lua, 2);
    if (ERR_OK == prots_may_resume(pktype, data)) {
        lua_pushboolean(lua, 1);
    } else {
        lua_pushboolean(lua, 0);
    }
    return 1;
}
// Lua 绑定：加载 PEM/DER 格式的 CA、证书和私钥，注册 SSL 上下文，返回 ssl 指针或 nil
static int32_t _lcore_cert_register(lua_State *lua) {
#if WITH_SSL
    name_t name = (name_t)luaL_checkinteger(lua, 1);
    const char *ca = luaL_checkstring(lua, 2);
    const char *cert = luaL_checkstring(lua, 3);
    const char *key = luaL_checkstring(lua, 4);
    int32_t keytype;
    int32_t type = (int32_t)lua_type(lua, 5);
    if (LUA_TNUMBER == type) {
        keytype = (int32_t)luaL_checkinteger(lua, 5);
    } else {
        keytype = SSL_FILETYPE_PEM; // 默认使用 PEM 格式
    }
    char capath[PATH_LENS];
    char certpath[PATH_LENS];
    char keypath[PATH_LENS];
    const char *propath = global_string(lua, PATH_NAME);
    if (0 != strlen(ca)) {
        SNPRINTF(capath, sizeof(capath), "%s%s%s%s%s",
            propath, PATH_SEPARATORSTR, CERT_FOLDER, PATH_SEPARATORSTR, ca);
    }
    if (0 != strlen(cert)) {
        SNPRINTF(certpath, sizeof(certpath), "%s%s%s%s%s",
            propath, PATH_SEPARATORSTR, CERT_FOLDER, PATH_SEPARATORSTR, cert);
    }
    if (0 != strlen(key)) {
        SNPRINTF(keypath, sizeof(keypath), "%s%s%s%s%s",
            propath, PATH_SEPARATORSTR, CERT_FOLDER, PATH_SEPARATORSTR, key);
    }
    evssl_ctx *ssl = evssl_new(capath, certpath, keypath, keytype);
    if (NULL != ssl) {
        if (ERR_OK != evssl_register(name, ssl)) {
            evssl_free(ssl);
            lua_pushnil(lua);
        } else {
            lua_pushlightuserdata(lua, ssl);
        }
    } else {
        lua_pushnil(lua);
    }
#else
    lua_pushnil(lua);
#endif
    return 1;
}
// Lua 绑定：加载 PKCS12 格式证书文件，注册 SSL 上下文，返回 ssl 指针或 nil
static int32_t _lcore_p12_register(lua_State *lua) {
#if WITH_SSL
    name_t name = (name_t)luaL_checkinteger(lua, 1);
    const char *p12 = luaL_checkstring(lua, 2);
    const char *pwd = luaL_checkstring(lua, 3);
    char p12path[PATH_LENS];
    if (0 != strlen(p12)) {
        const char *propath = global_string(lua, PATH_NAME);
        SNPRINTF(p12path, sizeof(p12path), "%s%s%s%s%s",
            propath, PATH_SEPARATORSTR, CERT_FOLDER, PATH_SEPARATORSTR, p12);
    }
    evssl_ctx *ssl = evssl_p12_new(p12path, pwd);
    if (NULL != ssl) {
        if (ERR_OK != evssl_register(name, ssl)) {
            evssl_free(ssl);
            lua_pushnil(lua);
        } else {
            lua_pushlightuserdata(lua, ssl);
        }
    } else {
        lua_pushnil(lua);
    }
#else
    lua_pushnil(lua);
#endif
    return 1;
}
// Lua 绑定：按 name 查询已注册的 SSL 上下文，返回 ssl 指针或 nil
static int32_t _lcore_ssl_qury(lua_State *lua) {
#if WITH_SSL
    name_t name = (name_t)luaL_checkinteger(lua, 1);
    struct evssl_ctx *ssl = evssl_qury(name);
    if (NULL != ssl) {
        lua_pushlightuserdata(lua, ssl);
    } else {
        lua_pushnil(lua);
    }
#else
    lua_pushnil(lua);
#endif
    return 1;
}
//srey.core
LUAMOD_API int luaopen_core(lua_State *lua) {
    luaL_Reg reg[] = {
        { "timeout", _lcore_timeout },
        { "call", _lcore_call },
        { "request", _lcore_request },
        { "response", _lcore_response },
        { "listen", _lcore_listen },
        { "unlisten", _lcore_unlisten },
        { "connect", _lcore_connect },
        { "ssl_exchange", _lcore_ssl_exchange },
        { "udp", _lcore_udp },

        { "send", _lcore_send },
        { "sendto", _lcore_sendto },
        { "close", _lcore_close },

        { "pack_type", _lcore_pack_type },
        { "status", _lcore_status },
        { "bind_task", _lcore_bind_task },
        { "session", _lcore_session },

        { "may_resume", _lcore_may_resume },

        { "cert_register", _lcore_cert_register },
        { "p12_register", _lcore_p12_register },
        { "ssl_qury", _lcore_ssl_qury },

        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
