#include "lbind/lpub.h"

#define SERI_PACK_MAX_DEPTH 32

static int32_t _lseri_pack_one(lua_State *lua, binary_ctx *bw, int32_t idx, int32_t depth);
static void _lseri_push_item(lua_State *lua, seri_iter *iter, const seri_item *item, int32_t depth);

// 递归 pack 一个 table：先数组段，再 hash 段（跳过已在数组段的整数 key），尾随 array_end
static int32_t _lseri_pack_table(lua_State *lua, binary_ctx *bw, int32_t idx, int32_t depth) {
    if (depth > SERI_PACK_MAX_DEPTH) {
        lua_pushfstring(lua, "seri pack: table nested too deep (>%d)", SERI_PACK_MAX_DEPTH);
        return ERR_FAILED;
    }
    if (idx < 0) {
        idx = lua_gettop(lua) + idx + 1;
    }
    if (0 == lua_checkstack(lua, LUA_MINSTACK)) {
        lua_pushliteral(lua, "seri pack: stack overflow");
        return ERR_FAILED;
    }
    lua_Integer alen = (lua_Integer)lua_rawlen(lua, idx);
    seri_append_array_start(bw, (uint32_t)alen);
    lua_Integer i;
    for (i = 1; i <= alen; i++) {
        lua_rawgeti(lua, idx, i);
        if (ERR_OK != _lseri_pack_one(lua, bw, -1, depth)) {
            return ERR_FAILED;
        }
        lua_pop(lua, 1);
    }
    lua_Integer ik;
    lua_pushnil(lua);
    while (0 != lua_next(lua, idx)) {
        if (LUA_TNUMBER == lua_type(lua, -2) && lua_isinteger(lua, -2)) {
            ik = lua_tointeger(lua, -2);
            if (ik > 0 && ik <= alen) {
                lua_pop(lua, 1);
                continue;
            }
        }
        if (ERR_OK != _lseri_pack_one(lua, bw, -2, depth)) {
            return ERR_FAILED;
        }
        if (ERR_OK != _lseri_pack_one(lua, bw, -1, depth)) {
            return ERR_FAILED;
        }
        lua_pop(lua, 1);
    }
    seri_append_array_end(bw);
    return ERR_OK;
}
static int32_t _lseri_pack_one(lua_State *lua, binary_ctx *bw, int32_t idx, int32_t depth) {
    int32_t type = lua_type(lua, idx);
    switch (type) {
    case LUA_TNIL:
        seri_append_nil(bw);
        break;
    case LUA_TBOOLEAN:
        seri_append_bool(bw, lua_toboolean(lua, idx));
        break;
    case LUA_TNUMBER:
        if (lua_isinteger(lua, idx)) {
            seri_append_int(bw, (int64_t)lua_tointeger(lua, idx));
        } else {
            seri_append_real(bw, (double)lua_tonumber(lua, idx));
        }
        break;
    case LUA_TSTRING: {
        size_t len;
        const char *s = lua_tolstring(lua, idx, &len);
        seri_append_string(bw, s, len);
        break;
    }
    case LUA_TLIGHTUSERDATA:
        seri_append_userdata(bw, lua_touserdata(lua, idx));
        break;
    case LUA_TTABLE:
        if (idx < 0) {
            idx = lua_gettop(lua) + idx + 1;
        }
        return _lseri_pack_table(lua, bw, idx, depth + 1);
    default:
        lua_pushfstring(lua, "seri pack: unsupported type %s", lua_typename(lua, type));
        return ERR_FAILED;
    }
    return ERR_OK;
}
/// <summary>
/// 序列化任意 Lua 值为二进制 buffer。
/// 支持 nil/boolean/integer/number/string/table/lightuserdata；
/// 不支持 function/thread/userdata（重型），遇到会 raise error。
/// table 嵌套深度上限 32。
/// 返回的 lightuserdata 由调用方负责释放：可用 utils.ud_free，
/// 也可直接作为 srey.request/call/response 的 data 参数并传 copy=0 转移所有权。
/// </summary>
/// <param name="..." type="any">任意多个待序列化的值</param>
/// <returns type="lightuserdata">序列化后的 buffer 指针</returns>
/// <returns type="integer">buffer 字节数</returns>
static int32_t _lseri_pack(lua_State *lua) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    int32_t top = lua_gettop(lua);
    int32_t i;
    for (i = 1; i <= top; i++) {
        if (ERR_OK != _lseri_pack_one(lua, &bw, i, 0)) {
            binary_free(&bw);
            return lua_error(lua);
        }
    }
    void *buf = bw.data;
    size_t size = bw.offset;
    bw.data = NULL;
    bw.size = 0;
    bw.offset = 0;
    binary_free(&bw);
    LPUB_RET_LUD(lua, buf, (lua_Integer)size);
}
static void _lseri_unpack_one(lua_State *lua, seri_iter *iter, int32_t depth) {
    seri_item item;
    int32_t rc = seri_iter_next(iter, &item);
    if (1 != rc) {
        luaL_error(lua, "seri unpack: malformed stream");
    }
    _lseri_push_item(lua, iter, &item, depth);
}
// 递归 unpack 一个 table：先数组段，再 hash 段直到 SERI_ITEM_NIL 终止
static void _lseri_unpack_table(lua_State *lua, seri_iter *iter, uint32_t array_n, int32_t depth) {
    if (depth > SERI_PACK_MAX_DEPTH) {
        luaL_error(lua, "seri unpack: table nested too deep (>%d)", SERI_PACK_MAX_DEPTH);
    }
    luaL_checkstack(lua, LUA_MINSTACK, NULL);
    // array_n 来自不可信流,预分配上限卡在剩余字节数(每元素 ≥1 字节 tag),防恶意流以极小数据撑爆预分配
    size_t remain = iter->len - iter->offset;
    uint32_t prealloc = ((size_t)array_n <= remain) ? array_n : (uint32_t)remain;
    lua_createtable(lua, (int32_t)prealloc, 0);
    uint32_t i;
    for (i = 1; i <= array_n; i++) {
        _lseri_unpack_one(lua, iter, depth);
        lua_rawseti(lua, -2, (lua_Integer)i);
    }
    seri_item item;
    int32_t rc;
    for (;;) {
        rc = seri_iter_next(iter, &item);
        if (1 != rc) {
            luaL_error(lua, "seri unpack: malformed stream in hash section");
        }
        if (SERI_ITEM_NIL == item.type) {
            return;
        }
        _lseri_push_item(lua, iter, &item, depth);
        _lseri_unpack_one(lua, iter, depth);
        lua_rawset(lua, -3);
    }
}
static void _lseri_push_item(lua_State *lua, seri_iter *iter, const seri_item *item, int32_t depth) {
    switch (item->type) {
    case SERI_ITEM_NIL:
        lua_pushnil(lua);
        break;
    case SERI_ITEM_BOOL:
        lua_pushboolean(lua, item->v.b);
        break;
    case SERI_ITEM_INT:
        lua_pushinteger(lua, (lua_Integer)item->v.i);
        break;
    case SERI_ITEM_REAL:
        lua_pushnumber(lua, (lua_Number)item->v.r);
        break;
    case SERI_ITEM_STRING:
        lua_pushlstring(lua, item->v.s.p, item->v.s.len);
        break;
    case SERI_ITEM_USERDATA:
        lua_pushlightuserdata(lua, item->v.ud);
        break;
    case SERI_ITEM_ARRAY_BEGIN:
        _lseri_unpack_table(lua, iter, item->v.array_n, depth + 1);
        break;
    default:
        luaL_error(lua, "seri unpack: invalid item type %d", (int32_t)item->type);
    }
}
/// <summary>
/// 反序列化 seri.pack 输出的二进制 buffer 为多个 Lua 值。
/// 支持 lightuserdata+size 或 string 单参形式；返回顺序与 pack 输入相同。
/// </summary>
/// <param name="data" type="string|lightuserdata">序列化数据</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <returns type="...">解码后的 Lua 值（多返回值）</returns>
static int32_t _lseri_unpack(lua_State *lua) {
    const char *buf;
    size_t size;
    int32_t type = lua_type(lua, 1);
    if (LUA_TSTRING == type) {
        buf = luaL_checklstring(lua, 1, &size);
    } else if (LUA_TLIGHTUSERDATA == type) {
        buf = (const char *)lua_touserdata(lua, 1);
        size = (size_t)luaL_checkinteger(lua, 2);
        if (NULL == buf) {
            return 0;
        }
    } else {
        return luaL_argerror(lua, 1, "string or light userdata expected");
    }
    if (0 == size) {
        return 0;
    }
    seri_iter iter;
    seri_iter_init(&iter, buf, size);
    int32_t base = lua_gettop(lua);
    seri_item item;
    int32_t rc;
    for (;;) {
        rc = seri_iter_next(&iter, &item);
        if (0 == rc) {
            break;
        }
        if (rc < 0) {
            return luaL_error(lua, "seri unpack: malformed stream");
        }
        luaL_checkstack(lua, 1, NULL);
        _lseri_push_item(lua, &iter, &item, 0);
    }
    return lua_gettop(lua) - base;
}
// srey.seri
LUAMOD_API int luaopen_seri(lua_State *lua) {
    luaL_Reg reg[] = {
        { "pack",   _lseri_pack   },
        { "unpack", _lseri_unpack },
        { NULL, NULL }
    };
    luaL_newlib(lua, reg);
    return 1;
}
