#include "lbind/lpub.h"

#define MT_SOCK "_sock_ctx" // 连接标识 userdata 的元表名，只是本文件的实现细节

// 0/1 开关越界文案，只由 lpub_check_flag / lpub_opt_flag 用
#define FLAG_OUT_OF_RANGE "flag must be 0 or 1"

// 注册表里那张连接标识缓存表的键(只取地址用)。一个 lua_State 一张,按 skid 存,
// 免得每条消息取 sk 都造一个新 userdata
static const char _sk_cache_key = 0;

// 从 Lua 全局变量表中取轻量用户数据，类型不符则弹栈返回 NULL
void *global_userdata(lua_State *lua, const char *name) {
    if (LUA_TLIGHTUSERDATA != lua_getglobal(lua, name)) {
        lua_pop(lua, 1);
        return NULL;
    }
    void *data = lua_touserdata(lua, -1);
    lua_pop(lua, 1);
    return data;
}
// 从 Lua 全局变量表中取字符串并复制到 buf(NUL 结尾)。
// 复制而非返回内部指针：lua_tostring 返回的指针在 string 弹栈后,理论可被 GC 释放
int32_t global_string(lua_State *lua, const char *name, char *buf, size_t bufsize) {
    if (EMPTYPTR(buf, bufsize)) {
        return ERR_FAILED;
    }
    buf[0] = '\0';
    if (LUA_TSTRING != lua_getglobal(lua, name)) {
        lua_pop(lua, 1);
        return ERR_FAILED;
    }
    size_t lens;
    const char *data = lua_tolstring(lua, -1, &lens);
    if (lens >= bufsize) {
        lua_pop(lua, 1);
        return ERR_FAILED;
    }
    memcpy(buf, data, lens);
    buf[lens] = '\0';
    lua_pop(lua, 1);
    return ERR_OK;
}
size_t lpub_check_lens(lua_State *lua, int32_t idx, size_t max) {
    lua_Integer lens = luaL_checkinteger(lua, idx);
    luaL_argcheck(lua, lens >= 0 && (0 == max || (size_t)lens <= max), idx, LENS_RANGE);
    return (size_t)lens;
}
int64_t lpub_check_range(lua_State *lua, int32_t idx, int64_t lo, int64_t hi, const char *what) {
    lua_Integer val = luaL_checkinteger(lua, idx);
    luaL_argcheck(lua, val >= lo && val <= hi, idx, what);
    return (int64_t)val;
}
int64_t lpub_opt_range(lua_State *lua, int32_t idx, int64_t dft, int64_t lo, int64_t hi, const char *what) {
    if (lua_isnoneornil(lua, idx)) {
        return dft;
    }
    return lpub_check_range(lua, idx, lo, hi, what);
}
uint8_t lpub_check_u8(lua_State *lua, int32_t idx, const char *what) {
    return (uint8_t)lpub_check_range(lua, idx, 0, UINT8_MAX, what);
}
uint16_t lpub_check_u16(lua_State *lua, int32_t idx, const char *what) {
    return (uint16_t)lpub_check_range(lua, idx, 0, UINT16_MAX, what);
}
uint32_t lpub_check_u32(lua_State *lua, int32_t idx, const char *what) {
    return (uint32_t)lpub_check_range(lua, idx, 0, UINT32_MAX, what);
}
uint64_t lpub_check_sess(lua_State *lua, int32_t idx) {
    lua_Integer sess = luaL_checkinteger(lua, idx);
    luaL_argcheck(lua, 0 != sess, idx, "session must be non-zero");
    return (uint64_t)sess;
}
int32_t lpub_check_flag(lua_State *lua, int32_t idx) {
    return (int32_t)lpub_check_range(lua, idx, 0, 1, FLAG_OUT_OF_RANGE);
}
int32_t lpub_opt_flag(lua_State *lua, int32_t idx, int32_t dft) {
    return (int32_t)lpub_opt_range(lua, idx, dft, 0, 1, FLAG_OUT_OF_RANGE);
}
int8_t lpub_check_i8(lua_State *lua, int32_t idx, const char *what) {
    return (int8_t)lpub_check_range(lua, idx, INT8_MIN, INT8_MAX, what);
}
int16_t lpub_check_i16(lua_State *lua, int32_t idx, const char *what) {
    return (int16_t)lpub_check_range(lua, idx, INT16_MIN, INT16_MAX, what);
}
int32_t lpub_check_i32(lua_State *lua, int32_t idx, const char *what) {
    return (int32_t)lpub_check_range(lua, idx, INT32_MIN, INT32_MAX, what);
}
uint8_t lpub_opt_u8(lua_State *lua, int32_t idx, uint8_t dft, const char *what) {
    return (uint8_t)lpub_opt_range(lua, idx, dft, 0, UINT8_MAX, what);
}
void *lpub_owner_ptr(lua_State *lua, const char *omt) {
    lua_getiuservalue(lua, 1, 1);
    void **owner = luaL_testudata(lua, -1, omt);
    lua_pop(lua, 1);
    return (NULL != owner) ? *owner : NULL;
}
void **lpub_push_ud(lua_State *lua, void *ptr, const char *mt) {
    void **slot = lua_newuserdata(lua, sizeof(void *));
    *slot = ptr;
    ASSOC_MTABLE(lua, mt);
    return slot;
}
int64_t lpub_check_index0(lua_State *lua, int32_t idx, uint64_t count) {
    lua_Integer i = luaL_checkinteger(lua, idx);
    if (i < 1
        || (uint64_t)i > count) {
        return -1;
    }
    return (int64_t)(i - 1);
}
// 不写 default：新增 pack_type 时 -Wswitch 报在这里，逼着表态它能不能从 Lua 传进来。
// 只管本函数这一件事；协议分派那边的绊线在 prots.c 的 _prots_vtbl 身上
pack_type lpub_check_pktype(lua_State *lua, int32_t idx) {
    lua_Integer val = luaL_checkinteger(lua, idx);
    // 先按 lua_Integer 卡范围再收窄：pack_type 是 4 字节，先转再判的话 2^32+2 会截成 2 混成 PACK_HTTP
    if (val < 0
        || val > PACK_UDP_KCP) {
        luaL_argerror(lua, idx, "unknown pack type");
    }
    switch ((pack_type)val) {
    case PACK_NONE:
    case PACK_DNS:
    case PACK_HTTP:
    case PACK_WEBSOCK:
    case PACK_MQTT:
    case PACK_SMTP:
    case PACK_CUSTZ_FIXED:
    case PACK_CUSTZ_FLAG:
    case PACK_CUSTZ_VAR:
    case PACK_REDIS:
    case PACK_MYSQL:
    case PACK_PGSQL:
    case PACK_MONGO:
    case PACK_UDP_KCP:
        return (pack_type)val;
    }
    luaL_argerror(lua, idx, "unknown pack type");
    return PACK_NONE;// 到不了: luaL_argerror 会 longjmp
}
struct evssl_ctx *lpub_check_evssl(lua_State *lua, int32_t idx) {
    if (lua_isnoneornil(lua, idx)) {
        return NULL;
    }
    LUACHECK_LUDATA(lua, idx);
    return lua_touserdata(lua, idx);
}
void *lpub_check_buf_idx(lua_State *lua, int32_t *idx, size_t *size, int32_t *copy) {
    int32_t type = lua_type(lua, *idx);
    if (LUA_TSTRING == type) {
        const char *s = luaL_checklstring(lua, *idx, size);
        luaL_argcheck(lua, *size <= INT32_MAX, *idx, LENS_RANGE);
        if (NULL != copy) {
            *copy = 1;
        }
        *idx += 1;// string 占 1 位
        return (void *)s;
    }
    if (LUA_TLIGHTUSERDATA == type) {
        void *ud = lua_touserdata(lua, *idx);
        *size = lpub_check_lens(lua, *idx + 1, INT32_MAX);
        luaL_argcheck(lua, NULL != ud || 0 == *size, *idx, LUDATA_NONNULL);
        *idx += 2;// 先吃掉 data + size,*idx 转到 copy 位
        if (NULL != copy) {
            if (lua_isnoneornil(lua, *idx)) {
                *copy = 1;
            } else {
                *copy = lpub_check_flag(lua, *idx);
                *idx += 1;// copy 命中再 +1
            }
        }
        return ud;
    }
    luaL_argerror(lua, *idx, "string or light userdata expected");
    return NULL;// 到不了: luaL_argerror 会 longjmp
}
// idx 按值的兼容包装,丢弃推进位置
void *lpub_check_buf(lua_State *lua, int32_t idx, size_t *size, int32_t *copy) {
    return lpub_check_buf_idx(lua, &idx, size, copy);
}
void *lpub_opt_buf(lua_State *lua, int32_t idx, size_t *size) {
    void *data;
    switch (lua_type(lua, idx)) {
    case LUA_TNIL:
    case LUA_TNONE:
        *size = 0;
        return NULL;
    case LUA_TSTRING:
        data = (void *)luaL_checklstring(lua, idx, size);
        luaL_argcheck(lua, *size <= INT32_MAX, idx, LENS_RANGE);
        return data;
    case LUA_TLIGHTUSERDATA:
        *size = lpub_check_lens(lua, idx + 1, INT32_MAX);
        data = lua_touserdata(lua, idx);
        luaL_argcheck(lua, NULL != data || 0 == *size, idx, LUDATA_NONNULL);
        return data;
    default:
        break;
    }
    luaL_argerror(lua, idx, "nil, string or light userdata expected");
    return NULL;// 到不了: luaL_argerror 会 longjmp
}
name_t lpub_task_handle(lua_State *lua, int32_t idx) {
    return (LUA_TSTRING == lua_type(lua, idx))
        ? task_find_name(g_loader, lua_tostring(lua, idx))
        : (name_t)luaL_checkinteger(lua, idx);
}
int32_t lpub_rtn_bool(lua_State *lua, int32_t cond) {
    lua_pushboolean(lua, 0 != cond ? 1 : 0);
    return 1;
}
int32_t lpub_rtn_nil(lua_State *lua, int32_t n) {
    for (int32_t i = 0; i < n; i++) {
        lua_pushnil(lua);
    }
    return n;
}
int32_t lpub_rtn_lud(lua_State *lua, void *pack, size_t size) {
    if (NULL == pack) {
        return lpub_rtn_nil(lua, 2);
    }
    lua_pushlightuserdata(lua, pack);
    lua_pushinteger(lua, (lua_Integer)size);
    return 2;
}
void lpub_push_url_param(lua_State *lua, url_ctx *url) {
    lua_createtable(lua, 0, url->nparam);// 按实际参数个数建表,不按 URL_MAX_PARAM 预留
    url_param *param;
    for (int32_t i = 0; i < url->nparam; i++) {
        param = &url->param[i];
        lua_pushlstring(lua, param->key.data, param->key.lens);
        if (buf_empty(&param->val)) {
            lua_pushstring(lua, "");
        } else {
            lua_pushlstring(lua, param->val.data, param->val.lens);
        }
        lua_rawset(lua, -3);
    }
}
void lpub_push_url_table(lua_State *lua, url_ctx *url) {
    lua_createtable(lua, 0, 9);
    if (!buf_empty(&url->scheme)) {
        lua_pushlstring(lua, url->scheme.data, url->scheme.lens);
        lua_setfield(lua, -2, "scheme");
    }
    if (!buf_empty(&url->user)) {
        lua_pushlstring(lua, url->user.data, url->user.lens);
        lua_setfield(lua, -2, "user");
    }
    if (!buf_empty(&url->psw)) {
        lua_pushlstring(lua, url->psw.data, url->psw.lens);
        lua_setfield(lua, -2, "psw");
    }
    if (!buf_empty(&url->host)) {
        lua_pushlstring(lua, url->host.data, url->host.lens);
        lua_setfield(lua, -2, "host");
    }
    if (!buf_empty(&url->port)) {
        lua_pushlstring(lua, url->port.data, url->port.lens);
        lua_setfield(lua, -2, "port");
    }
    if (url->npath > 0) {
        luaL_Buffer pbuf;
        size_t pcap = url->pathlens + 1;
        char *pp = luaL_buffinitsize(lua, &pbuf, pcap);
        luaL_pushresultsize(&pbuf, url_reorg_path(url, pp, pcap));
        lua_setfield(lua, -2, "path");
    }
    lua_createtable(lua, url->npath > 0 ? url->npath : 0, 0);
    for (int32_t i = 0; i < url->npath; i++) {
        lua_pushlstring(lua, url->segs[i].data, url->segs[i].lens);
        lua_rawseti(lua, -2, i + 1);
    }
    lua_setfield(lua, -2, "segs");
    if (!buf_empty(&url->anchor)) {
        lua_pushlstring(lua, url->anchor.data, url->anchor.lens);
        lua_setfield(lua, -2, "anchor");
    }
    lpub_push_url_param(lua, url);
    lua_setfield(lua, -2, "param");
    if (url->paramlens > 0) {
        luaL_Buffer qbuf;
        size_t qcap = url->paramlens + 1;
        char *qq = luaL_buffinitsize(lua, &qbuf, qcap);
        luaL_pushresultsize(&qbuf, url_reorg_param(url, qq, qcap));
        lua_setfield(lua, -2, "query");
    }
}
// 连接标识的 __index：只有 fd / skid / valid 三个可读键。
// valid 让"连接是否失效"只有一处判据，业务不必自己比 INVALID_SOCK，口径同 C 侧 sock_is_invalid
static int32_t _sock_index(lua_State *lua) {
    sock_ctx *sk = lua_touserdata(lua, 1);
    const char *k = lua_tostring(lua, 2);
    if (NULL == sk || NULL == k) {
        lua_pushnil(lua);
        return 1;
    }
    if (0 == strcmp(k, "fd")) {
        lua_pushinteger(lua, (lua_Integer)sk->fd);
    } else if (0 == strcmp(k, "skid")) {
        lua_pushinteger(lua, (lua_Integer)sk->skid);
    } else if (0 == strcmp(k, "valid")) {
        lua_pushboolean(lua, !sock_is_invalid(sk));
    } else {
        lua_pushnil(lua);
    }
    return 1;
}
// 按值比较两个连接标识。两侧都要 testudata 而非 checkudata：两个 full userdata 相比时
// Lua 不看元表是否相同,只要一侧挂了 __eq 就派发到这里,拿 sock 比别的 userdata 会抛参数错
static int32_t _sock_eq(lua_State *lua) {
    sock_ctx *a = luaL_testudata(lua, 1, MT_SOCK);
    sock_ctx *b = luaL_testudata(lua, 2, MT_SOCK);
    lua_pushboolean(lua, NULL != a && NULL != b
                         && a->fd == b->fd && a->skid == b->skid);
    return 1;
}
static int32_t _sock_tostring(lua_State *lua) {
    sock_ctx *sk = luaL_checkudata(lua, 1, MT_SOCK);
    lua_pushfstring(lua, "sock(%d,%I)", (int)sk->fd, (lua_Integer)sk->skid);
    return 1;
}
void lpub_reg_sock(lua_State *lua) {
    luaL_newmetatable(lua, MT_SOCK);
    lua_pushcfunction(lua, _sock_index);
    lua_setfield(lua, -2, "__index");
    lua_pushcfunction(lua, _sock_eq);
    lua_setfield(lua, -2, "__eq");
    lua_pushcfunction(lua, _sock_tostring);
    lua_setfield(lua, -2, "__tostring");
    lua_pushstring(lua, MT_SOCK);
    lua_setfield(lua, -2, "__metatable");
    lua_pop(lua, 1);
}
// 取缓存表压栈,不存在就建。建的是弱值表：缓存只为同一 skid 复用同一 userdata 省一次分配,
// 摘除点却只有 CLOSE 分发那一处——连接被 bind_task 转交后 CLOSE 落在别的 task,
// 原 task 这条 skid 再没有路径删得掉。弱值让条目随 userdata 一起被回收,不必依赖摘除点
static void _sk_cache_push(lua_State *lua) {
    if (LUA_TTABLE != lua_rawgetp(lua, LUA_REGISTRYINDEX, &_sk_cache_key)) {
        lua_pop(lua, 1);
        lua_newtable(lua);
        lua_newtable(lua);
        lua_pushstring(lua, "v");
        lua_setfield(lua, -2, "__mode");
        lua_setmetatable(lua, -2);
        lua_pushvalue(lua, -1);
        lua_rawsetp(lua, LUA_REGISTRYINDEX, &_sk_cache_key);
    }
}
void lpub_push_sock(lua_State *lua, sock_ctx *sk) {
    sock_ctx *ud = (sock_ctx *)lua_newuserdatauv(lua, sizeof(sock_ctx), 0);
    *ud = *sk;
    ASSOC_MTABLE(lua, MT_SOCK);
}
// 消息分发专用:按 skid 复用同一个 userdata,免得每条消息都造一个。
// 命中必须刷新值——teardown 会先把 fd 复位再发 CLOSE,拿旧快照会让 valid 恒真。
// 只给这条路用:别处(sock_id / connect / udp)走上面那个不入缓存的,否则连接关掉之后
// 再取一次就会把死 skid 写回表里,而 uncache 只在 CLOSE 分发后触发一次,再没人摘得掉
void lpub_push_sock_msg(lua_State *lua, sock_ctx *sk) {
    if (0 == sk->skid) {
        lpub_push_sock(lua, sk);
        return;
    }
    _sk_cache_push(lua);
    if (LUA_TUSERDATA == lua_rawgeti(lua, -1, (lua_Integer)sk->skid)) {
        *(sock_ctx *)lua_touserdata(lua, -1) = *sk;
        lua_remove(lua, -2);
        return;
    }
    lua_pop(lua, 1);
    sock_ctx *ud = (sock_ctx *)lua_newuserdatauv(lua, sizeof(sock_ctx), 0);
    *ud = *sk;
    ASSOC_MTABLE(lua, MT_SOCK);
    lua_pushvalue(lua, -1);
    lua_rawseti(lua, -3, (lua_Integer)sk->skid);
    lua_remove(lua, -2);
}
void lpub_sock_uncache(lua_State *lua, uint64_t skid) {
    if (0 == skid) {
        return;
    }
    _sk_cache_push(lua);
    lua_pushnil(lua);
    lua_rawseti(lua, -2, (lua_Integer)skid);
    lua_pop(lua, 1);
}
sock_ctx *lpub_check_sock(lua_State *lua, int32_t idx) {
    return (sock_ctx *)luaL_checkudata(lua, idx, MT_SOCK);
}
int32_t lpub_push_sock_invalid(lua_State *lua) {
    sock_ctx sk;
    sock_set_invalid(&sk);
    sk.skid = 0;
    lpub_push_sock(lua, &sk);
    return 1;
}
int32_t lpub_is_sock(lua_State *lua, int32_t idx) {
    return NULL != luaL_testudata(lua, idx, MT_SOCK);
}
