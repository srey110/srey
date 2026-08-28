#include "lbind/lpub.h"

// 编解码深度上限、稀疏数组的判定阈值
#define LYYJSON_MAX_DEPTH    1000
#define LYYJSON_SPARSE_RATIO 2
#define LYYJSON_SPARSE_SAFE  10

// 编解码上下文。出错只记文案不当场抛：在递归里 luaL_error 会 longjmp 跳过
// yyjson_doc_free，内存账立刻不平，所以一律返回 NULL/ERR_FAILED 逐层退到入口再报
typedef struct lyyjson_ctx {
    lua_State *lua;
    yyjson_mut_doc *doc;
    const char *erro;
}lyyjson_ctx;

static yyjson_mut_val *_lyyjson_pack(lyyjson_ctx *ctx, int32_t depth);

// 数组判定：键全是 >= 1 的整数才算数组，返回最大键；
// 出现其它键返回 -1 当对象编；过度稀疏返回 -2 报错，不静默转对象
static int64_t _lyyjson_arrlen(lua_State *lua) {
    int64_t max = 0;
    int64_t items = 0;
    lua_Number k;
    lua_pushnil(lua);
    while (0 != lua_next(lua, -2)) {
        if (LUA_TNUMBER == lua_type(lua, -2)) {
            k = lua_tonumber(lua, -2);
            if (floor(k) == k
                && k >= 1) {
                if (k > (lua_Number)max) {
                    max = (k >= (lua_Number)INT64_MAX) ? INT64_MAX : (int64_t)k;
                }
                items++;
                lua_pop(lua, 1);
                continue;
            }
        }
        lua_pop(lua, 2);
        return -1;
    }
    if (max > items * LYYJSON_SPARSE_RATIO
        && max > LYYJSON_SPARSE_SAFE) {
        return -2;
    }
    return max;
}
static yyjson_mut_val *_lyyjson_pack_arr(lyyjson_ctx *ctx, int64_t lens, int32_t depth) {
    yyjson_mut_val *val;
    yyjson_mut_val *arr = yyjson_mut_arr(ctx->doc);
    if (NULL == arr) {
        ctx->erro = "out of memory";
        return NULL;
    }
    for (int64_t i = 1; i <= lens; i++) {
        lua_rawgeti(ctx->lua, -1, i);
        val = _lyyjson_pack(ctx, depth);
        lua_pop(ctx->lua, 1);
        if (NULL == val) {
            return NULL;
        }
        yyjson_mut_arr_add_val(arr, val);
    }
    return arr;
}
static yyjson_mut_val *_lyyjson_pack_obj(lyyjson_ctx *ctx, int32_t depth) {
    lua_State *lua = ctx->lua;
    size_t klens;
    int32_t ktype;
    const char *kstr;
    yyjson_mut_val *key;
    yyjson_mut_val *val;
    yyjson_mut_val *obj = yyjson_mut_obj(ctx->doc);
    if (NULL == obj) {
        ctx->erro = "out of memory";
        return NULL;
    }
    lua_pushnil(lua);
    while (0 != lua_next(lua, -2)) {
        ktype = lua_type(lua, -2);
        if (LUA_TSTRING == ktype) {
            kstr = lua_tolstring(lua, -2, &klens);
        } else if (LUA_TNUMBER == ktype) {
            lua_pushvalue(lua, -2);
            kstr = lua_tolstring(lua, -1, &klens);
        } else {
            ctx->erro = "table key must be a number or string";
            lua_pop(lua, 2);
            return NULL;
        }
        key = yyjson_mut_strncpy(ctx->doc, kstr, klens);
        if (LUA_TNUMBER == ktype) {
            lua_pop(lua, 1);
        }
        if (NULL == key) {
            ctx->erro = "out of memory";
            lua_pop(lua, 2);
            return NULL;
        }
        val = _lyyjson_pack(ctx, depth);
        if (NULL == val) {
            lua_pop(lua, 2);
            return NULL;
        }
        yyjson_mut_obj_add(obj, key, val);
        lua_pop(lua, 1);
    }
    return obj;
}
static yyjson_mut_val *_lyyjson_pack_tbl(lyyjson_ctx *ctx, int32_t depth) {
    if (depth > LYYJSON_MAX_DEPTH) {
        ctx->erro = "table nested too deep";
        return NULL;
    }
    if (0 == lua_checkstack(ctx->lua, 6)) {
        ctx->erro = "lua stack overflow";
        return NULL;
    }
    int64_t lens = _lyyjson_arrlen(ctx->lua);
    if (-2 == lens) {
        ctx->erro = "excessively sparse array";
        return NULL;
    }
    return lens > 0 ? _lyyjson_pack_arr(ctx, lens, depth + 1)
                    : _lyyjson_pack_obj(ctx, depth + 1);
}
// 把栈顶的值转成 yyjson 节点。整数走 sint 通道精确写出，不退化成浮点
static yyjson_mut_val *_lyyjson_pack(lyyjson_ctx *ctx, int32_t depth) {
    lua_State *lua = ctx->lua;
    size_t lens;
    const char *str;
    switch (lua_type(lua, -1)) {
    case LUA_TNIL:
        return yyjson_mut_null(ctx->doc);
    case LUA_TBOOLEAN:
        return yyjson_mut_bool(ctx->doc, 0 != lua_toboolean(lua, -1));
    case LUA_TNUMBER:
        if (lua_isinteger(lua, -1)) {
            return yyjson_mut_sint(ctx->doc, lua_tointeger(lua, -1));
        }
        return yyjson_mut_real(ctx->doc, lua_tonumber(lua, -1));
    case LUA_TSTRING:
        str = lua_tolstring(lua, -1, &lens);
        return yyjson_mut_strncpy(ctx->doc, str, lens);
    case LUA_TTABLE:
        return _lyyjson_pack_tbl(ctx, depth);
    case LUA_TLIGHTUSERDATA:
        if (NULL == lua_touserdata(lua, -1)) {
            return yyjson_mut_null(ctx->doc);
        }
        break;
    default:
        break;
    }
    ctx->erro = "type not supported by json";
    return NULL;
}
// 把 yyjson 节点压成 Lua 值，成功后栈上恰好多一个值
static int32_t _lyyjson_push(lyyjson_ctx *ctx, yyjson_val *val, int32_t depth) {
    lua_State *lua = ctx->lua;
    size_t idx, max;
    yyjson_val *key;
    yyjson_val *sub;
    if (depth > LYYJSON_MAX_DEPTH) {
        ctx->erro = "json nested too deep";
        return ERR_FAILED;
    }
    if (0 == lua_checkstack(lua, 4)) {
        ctx->erro = "lua stack overflow";
        return ERR_FAILED;
    }
    switch (yyjson_get_type(val)) {
    case YYJSON_TYPE_NULL:
        lua_pushlightuserdata(lua, NULL);
        return ERR_OK;
    case YYJSON_TYPE_BOOL:
        lua_pushboolean(lua, yyjson_get_bool(val));
        return ERR_OK;
    case YYJSON_TYPE_NUM:
        if (yyjson_is_sint(val)) {
            lua_pushinteger(lua, (lua_Integer)yyjson_get_sint(val));
        } else if (yyjson_is_uint(val)) {
            uint64_t u = yyjson_get_uint(val);
            if (u > (uint64_t)LUA_MAXINTEGER) {
                lua_pushnumber(lua, (lua_Number)u);
            } else {
                lua_pushinteger(lua, (lua_Integer)u);
            }
        } else {
            lua_pushnumber(lua, (lua_Number)yyjson_get_real(val));
        }
        return ERR_OK;
    case YYJSON_TYPE_STR:
        lua_pushlstring(lua, yyjson_get_str(val), yyjson_get_len(val));
        return ERR_OK;
    case YYJSON_TYPE_ARR:
        lua_createtable(lua, (int32_t)yyjson_arr_size(val), 0);
        yyjson_arr_foreach(val, idx, max, sub) {
            if (ERR_OK != _lyyjson_push(ctx, sub, depth + 1)) {
                return ERR_FAILED;
            }
            lua_rawseti(lua, -2, (lua_Integer)(idx + 1));
        }
        return ERR_OK;
    case YYJSON_TYPE_OBJ:
        lua_createtable(lua, 0, (int32_t)yyjson_obj_size(val));
        yyjson_obj_foreach(val, idx, max, key, sub) {
            lua_pushlstring(lua, yyjson_get_str(key), yyjson_get_len(key));
            if (ERR_OK != _lyyjson_push(ctx, sub, depth + 1)) {
                return ERR_FAILED;
            }
            lua_rawset(lua, -3);
        }
        return ERR_OK;
    default:
        break;
    }
    ctx->erro = "unknown json type";
    return ERR_FAILED;
}
/// <summary>
/// 把 Lua 值编码成 JSON 字符串。整数精确写出（不经 %.14g），浮点按最短往返格式；
/// 键全为 >= 1 的整数才编成数组，空表编成 {}，过度稀疏的数组直接报错而非静默转对象；
/// 嵌套超过 1000 层、出现 table/number/string/boolean/nil 之外的类型都报错
/// </summary>
/// <param name="val" type="any">要编码的值</param>
/// <returns type="string">JSON 文本</returns>
static int32_t _lyyjson_encode(lua_State *lua) {
    luaL_checkany(lua, 1);
    lua_settop(lua, 1);
    lyyjson_ctx ctx;
    ctx.lua = lua;
    ctx.erro = NULL;
    ctx.doc = yyjson_mut_doc_new(NULL);
    if (NULL == ctx.doc) {
        return luaL_error(lua, "json doc create failed");
    }
    yyjson_mut_val *root = _lyyjson_pack(&ctx, 0);
    if (NULL == root) {
        yyjson_mut_doc_free(ctx.doc);
        return luaL_error(lua, "json encode: %s", NULL != ctx.erro ? ctx.erro : "failed");
    }
    yyjson_mut_doc_set_root(ctx.doc, root);
    size_t lens = 0;
    char *out = yyjson_mut_write(ctx.doc, 0, &lens);
    yyjson_mut_doc_free(ctx.doc);
    if (NULL == out) {
        return luaL_error(lua, "json encode failed");
    }
    lua_pushlstring(lua, out, lens);
    FREE(out);
    return 1;
}
/// <summary>
/// 解析 JSON 文本。JSON null 解成 yyjson.null（NULL light userdata）；
/// 超出 lua_Integer 范围的无符号整数退化成浮点；嵌套超过 1000 层报错
/// </summary>
/// <param name="data" type="string|lightuserdata">JSON 文本</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，字节数</param>
/// <returns type="any">解析结果</returns>
static int32_t _lyyjson_decode(lua_State *lua) {
    size_t lens = 0;
    char *data = lpub_check_buf(lua, 1, &lens, NULL);
    yyjson_read_err rerr;
    yyjson_doc *doc = yyjson_read_opts(data, lens, 0, NULL, &rerr);
    if (NULL == doc) {
        return luaL_error(lua, "json decode error at byte %I: %s",
                          (lua_Integer)rerr.pos, rerr.msg);
    }
    lyyjson_ctx ctx;
    ctx.lua = lua;
    ctx.doc = NULL;
    ctx.erro = NULL;
    int32_t top = lua_gettop(lua);
    int32_t rtn = _lyyjson_push(&ctx, yyjson_doc_get_root(doc), 0);
    yyjson_doc_free(doc);
    if (ERR_OK != rtn) {
        lua_settop(lua, top);
        return luaL_error(lua, "json decode: %s", NULL != ctx.erro ? ctx.erro : "failed");
    }
    return 1;
}
LUAMOD_API int luaopen_yyjson(lua_State *lua) {
    luaL_Reg reg[] = {
        { "encode", _lyyjson_encode },
        { "decode", _lyyjson_decode },
        { NULL, NULL }
    };
    luaL_newlib(lua, reg);
    lua_pushlightuserdata(lua, NULL);
    lua_setfield(lua, -2, "null");
    return 1;
}
