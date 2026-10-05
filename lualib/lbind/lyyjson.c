#include "lbind/lpub.h"

// 编解码深度上限(容器层数: encode 数 table, decode 数 array/object)、稀疏数组的判定阈值。
// 深度上限与 BSON_MAX_DEPTH / REDIS_MAX_DEPTH 取同一个数, 全项目一个口径。
#define LYYJSON_MAX_DEPTH    18
#define LYYJSON_SPARSE_RATIO 2
#define LYYJSON_SPARSE_SAFE  10
// 编解码的栈上缓冲：yyjson 的全部分配（doc、节点池、输出、解析时的输入拷贝）先从这里顺序切，
// 切不下才上堆。块按 16 字节对齐
#define LYYJSON_STACK_BUF    4096
#define LYYJSON_ARENA_ALIGN  16
#define LYYJSON_READ_FLAG    YYJSON_READ_ALLOW_INVALID_UNICODE
#define LYYJSON_WRITE_FLAG   YYJSON_WRITE_ALLOW_INVALID_UNICODE

// 编解码上下文。出错只记文案不当场抛：在递归里 luaL_error 会 longjmp 跳过
// yyjson_doc_free，内存账立刻不平，所以一律返回 NULL/ERR_FAILED 逐层退到入口再报。
// 编码时字符串值与字符串键不拷贝、直接交给 yyjson，写出前不得有任何 Lua 分配：一分配就可能跑 GC，
// 弱表里的值会被回收，指针随之悬空。数字键因此用 lpub_int_str / lua_numbertocstring，不走 lua_tolstring
typedef struct lyyjson_ctx {
    lua_State *lua;
    yyjson_mut_doc *doc;
    const char *erro;
}lyyjson_ctx;
// 栈上顺序分配器，接到 yyjson_alc 上。落在 [buf, buf + size) 里的块随函数返回作废，不在的是
// 切不下时退回框架分配器的堆块，free / realloc 按地址分两路；只有最近切出的那块能就地伸缩或回退
typedef struct lyyjson_arena {
    char *buf;
    char *last;  // 最近一次切出的块
    size_t size;
    size_t used;
}lyyjson_arena;

static yyjson_mut_val *_lyyjson_pack(lyyjson_ctx *ctx, int32_t depth);

// 块是否在栈缓冲里。按整数差比：块在堆上时两个指针不属同一对象，直接比大小是未定义行为
static int32_t _lyyjson_arena_has(lyyjson_arena *arena, void *ptr) {
    return (uintptr_t)ptr - (uintptr_t)arena->buf < arena->size;
}
static void *_lyyjson_arena_malloc(void *ctx, size_t size) {
    lyyjson_arena *arena = ctx;
    size_t need = ROUND_UP(size, LYYJSON_ARENA_ALIGN);
    if (need < size
        || need > arena->size - arena->used) {
        return _malloc(size);
    }
    arena->last = arena->buf + arena->used;
    arena->used += need;
    return arena->last;
}
static void *_lyyjson_arena_realloc(void *ctx, void *ptr, size_t osize, size_t size) {
    lyyjson_arena *arena = ctx;
    if (!_lyyjson_arena_has(arena, ptr)) {
        return _realloc(ptr, size);
    }
    size_t off = (size_t)((char *)ptr - arena->buf);
    size_t need = ROUND_UP(size, LYYJSON_ARENA_ALIGN);
    if (ptr == arena->last
        && need >= size
        && need <= arena->size - off) {
        arena->used = off + need;
        return ptr;
    }
    void *nptr = _lyyjson_arena_malloc(ctx, size);
    if (NULL != nptr) {
        memcpy(nptr, ptr, osize < size ? osize : size);
    }
    return nptr;
}
static void _lyyjson_arena_free(void *ctx, void *ptr) {
    lyyjson_arena *arena = ctx;
    if (!_lyyjson_arena_has(arena, ptr)) {
        _free(ptr);
        return;
    }
    if (ptr == arena->last) {
        arena->used = (size_t)((char *)ptr - arena->buf);
        arena->last = NULL;
    }
}
static void _lyyjson_arena_init(lyyjson_arena *arena, yyjson_alc *alc, char *buf, size_t size) {
    size_t pad = ROUND_UP((uintptr_t)buf, LYYJSON_ARENA_ALIGN) - (uintptr_t)buf;
    arena->buf = buf + pad;
    arena->last = NULL;
    arena->size = size - pad;
    arena->used = 0;
    alc->malloc = _lyyjson_arena_malloc;
    alc->realloc = _lyyjson_arena_realloc;
    alc->free = _lyyjson_arena_free;
    alc->ctx = arena;
}

// 数组判定：键全是 >= 1 的整数才算数组，返回最大键；
// 出现其它键返回 -1 当对象编；过度稀疏返回 -2 报错，不静默转对象
static int64_t _lyyjson_arrlen(lua_State *lua) {
    int64_t max = 0;
    int64_t items = 0;
    lua_Integer ik;
    lua_Number k;
    lua_pushnil(lua);
    while (0 != lua_next(lua, -2)) {
        if (lua_isinteger(lua, -2)) {
            ik = lua_tointeger(lua, -2);
            if (ik >= 1) {
                if (ik > max) {
                    max = ik;
                }
                items++;
                lua_pop(lua, 1);
                continue;
            }
        } else if (LUA_TNUMBER == lua_type(lua, -2)) {
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
    char kbuf[LUA_N2SBUFFSZ];
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
            key = yyjson_mut_strn(ctx->doc, kstr, klens);
        } else if (lua_isinteger(lua, -2)) {
            kstr = lpub_int_str(kbuf, sizeof(kbuf), lua_tointeger(lua, -2), &klens);
            key = yyjson_mut_strncpy(ctx->doc, kstr, klens);
        } else if (LUA_TNUMBER == ktype) {
            klens = (size_t)lua_numbertocstring(lua, -2, kbuf) - 1;
            key = yyjson_mut_strncpy(ctx->doc, kbuf, klens);
        } else {
            ctx->erro = "table key must be a number or string";
            lua_pop(lua, 2);
            return NULL;
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
    if (depth >= LYYJSON_MAX_DEPTH) {
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
        return yyjson_mut_strn(ctx->doc, str, lens);
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
// 容器进一层前的检查：层数只数容器（同 encode），栈留 4 格够放表、键和一个子值
static int32_t _lyyjson_push_check(lyyjson_ctx *ctx, int32_t depth) {
    if (depth >= LYYJSON_MAX_DEPTH) {
        ctx->erro = "json nested too deep";
        return ERR_FAILED;
    }
    if (0 == lua_checkstack(ctx->lua, 4)) {
        ctx->erro = "lua stack overflow";
        return ERR_FAILED;
    }
    return ERR_OK;
}
// 把 yyjson 节点压成 Lua 值，成功后栈上恰好多一个值。标量不查栈：根是标量时 LUA_MINSTACK 够用，
// 其余标量的位置由所在容器预留
static int32_t _lyyjson_push(lyyjson_ctx *ctx, yyjson_val *val, int32_t depth) {
    lua_State *lua = ctx->lua;
    size_t idx, max;
    yyjson_val *key;
    yyjson_val *sub;
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
        if (ERR_OK != _lyyjson_push_check(ctx, depth)) {
            return ERR_FAILED;
        }
        lua_createtable(lua, (int32_t)yyjson_arr_size(val), 0);
        yyjson_arr_foreach(val, idx, max, sub) {
            if (ERR_OK != _lyyjson_push(ctx, sub, depth + 1)) {
                return ERR_FAILED;
            }
            lua_rawseti(lua, -2, (lua_Integer)(idx + 1));
        }
        return ERR_OK;
    case YYJSON_TYPE_OBJ:
        if (ERR_OK != _lyyjson_push_check(ctx, depth)) {
            return ERR_FAILED;
        }
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
/// 数字 key 串成字符串当键名，故 [1] 与 "1" 撞成同一个键，本函数不查重（同 bson.encode），撞了就
/// 产出带重复键的 JSON、decode 回来只剩其一，调用方自己保证不撞；
/// 嵌套超过 LYYJSON_MAX_DEPTH 层、出现 table/number/string/boolean/nil 之外的类型都报错。
/// 非 UTF-8 字节原样写出不报错：Lua 字符串就是字节串，框架里 srey.ud_str 取出的
/// 二进制载荷进 JSON 是常态，卡死会让这类请求一个字节都发不出去
/// </summary>
/// <param name="val" type="any">要编码的值</param>
/// <returns type="string">JSON 文本</returns>
static int32_t _lyyjson_encode(lua_State *lua) {
    luaL_checkany(lua, 1);
    lua_settop(lua, 1);
    char abuf[LYYJSON_STACK_BUF];
    lyyjson_arena arena;
    yyjson_alc alc;
    _lyyjson_arena_init(&arena, &alc, abuf, sizeof(abuf));
    lyyjson_ctx ctx;
    ctx.lua = lua;
    ctx.erro = NULL;
    ctx.doc = yyjson_mut_doc_new(&alc);
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
    char *out = yyjson_mut_write_opts(ctx.doc, LYYJSON_WRITE_FLAG, &alc, &lens, NULL);
    yyjson_mut_doc_free(ctx.doc);
    if (NULL == out) {
        return luaL_error(lua, "json encode failed");
    }
    lua_pushlstring(lua, out, lens);
    alc.free(alc.ctx, out);
    return 1;
}
int32_t lyyjson_encode_sink(lua_State *lua, int32_t idx, lyyjson_sink sink, void *ud) {
    char abuf[LYYJSON_STACK_BUF];
    lyyjson_arena arena;
    yyjson_alc alc;
    _lyyjson_arena_init(&arena, &alc, abuf, sizeof(abuf));
    lyyjson_ctx ctx;
    ctx.lua = lua;
    ctx.erro = NULL;
    ctx.doc = yyjson_mut_doc_new(&alc);
    if (NULL == ctx.doc) {
        return ERR_FAILED;
    }
    lua_pushvalue(lua, idx);// _lyyjson_pack 只认栈顶；原值仍在 idx，弹掉不影响字符串存活
    yyjson_mut_val *root = _lyyjson_pack(&ctx, 0);
    lua_pop(lua, 1);
    if (NULL == root) {
        yyjson_mut_doc_free(ctx.doc);
        return ERR_FAILED;
    }
    yyjson_mut_doc_set_root(ctx.doc, root);
    size_t lens = 0;
    char *out = yyjson_mut_write_opts(ctx.doc, LYYJSON_WRITE_FLAG, &alc, &lens, NULL);
    yyjson_mut_doc_free(ctx.doc);
    if (NULL == out) {
        return ERR_FAILED;
    }
    sink(ud, out, lens);
    alc.free(alc.ctx, out);
    return ERR_OK;
}
/// <summary>
/// 解析 JSON 文本。JSON null 解成 yyjson.null（NULL light userdata）；
/// 超出 lua_Integer 范围的无符号整数退化成浮点；嵌套超过 LYYJSON_MAX_DEPTH 层报错。
/// 字符串里的非 UTF-8 字节原样收下不报错，与 encode 对称——第三方服务回未转码的
/// Latin-1 JSON 是常见情形，拒收等于把一条本可解析的业务响应整个丢掉
/// </summary>
/// <param name="data" type="string|lightuserdata">JSON 文本</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，字节数</param>
/// <returns type="any">解析结果</returns>
static int32_t _lyyjson_decode(lua_State *lua) {
    size_t lens = 0;
    char *data = lpub_check_buf(lua, 1, &lens, NULL);
    char abuf[LYYJSON_STACK_BUF];
    lyyjson_arena arena;
    yyjson_alc alc;
    _lyyjson_arena_init(&arena, &alc, abuf, sizeof(abuf));
    yyjson_read_err rerr;
    yyjson_doc *doc = yyjson_read_opts(data, lens, LYYJSON_READ_FLAG, &alc, &rerr);
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
    /// <field name="null" type="lightuserdata">JSON null 的哨兵，本身就是空指针。decode 解出来的
    /// 每个 JSON null 都是它，别拿去喂吃 lightuserdata 的接口</field>
    lua_pushlightuserdata(lua, NULL);
    lua_setfield(lua, -2, "null");
    return 1;
}
