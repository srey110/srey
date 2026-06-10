#include "lbind/lpub.h"
#include <string.h>

#define MT_BSON        "_bson_ctx"
#define MT_BSON_ITER   "_bson_iter_ctx"
#define MT_BSON_OID    "_bson_oid"
#define MT_BSON_DATE   "_bson_date"
#define MT_BSON_BINARY "_bson_binary"
#define MT_BSON_INT64  "_bson_int64"

typedef struct { char data[BSON_OID_LENS]; } lbson_oid_t;
typedef struct { int64_t ms; } lbson_date_t;
typedef struct { bson_subtype subtype; size_t lens; } lbson_binary_t;
// lbson_binary_t 后紧跟 lens 字节的二进制内容
typedef struct { int64_t val; } lbson_int64_t;

// ---- bson builder ----
/// <summary>
/// 创建 bson 文档构建器
/// </summary>
/// <param name="data" type="lightuserdata?">已有 BSON 数据指针；省略时新建空文档</param>
/// <param name="size" type="integer?">data 提供时必填，表示已有数据字节数</param>
/// <returns type="_bson_ctx">bson 对象</returns>
static int32_t _lbson_new(lua_State *lua) {
    bson_ctx *bson = lua_newuserdata(lua, sizeof(bson_ctx));
    if (lua_islightuserdata(lua, 1)) {
        char *data = lua_touserdata(lua, 1);
        size_t size = (size_t)luaL_checkinteger(lua, 2);
        bson_init(bson, data, size);
    } else {
        bson_init(bson, NULL, 0);
    }
    ASSOC_MTABLE(lua, MT_BSON);
    return 1;
}
/// <summary>
/// 释放内部 doc.data（仅 owned=1 时生效；同时作为 __gc / free 调用）
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <returns>无</returns>
static int32_t _lbson_free(lua_State *lua) {
    bson_ctx *bson = luaL_checkudata(lua, 1, MT_BSON);
    if (0 != bson->doc.inc && NULL != bson->doc.data) {
        BSON_FREE(bson);
        bson->doc.data = NULL;
    }
    return 0;
}
/// <summary>
/// 开始写入嵌套文档字段（须配对调用 end()）
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名</param>
/// <returns>无</returns>
static int32_t _lbson_doc_begin(lua_State *lua) {
    bson_ctx *bson = luaL_checkudata(lua, 1, MT_BSON);
    const char *key = luaL_checkstring(lua, 2);
    bson_append_document_begain(bson, key);
    return 0;
}
/// <summary>
/// 开始写入数组字段（须配对调用 end()）
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名</param>
/// <returns>无</returns>
static int32_t _lbson_arr_begin(lua_State *lua) {
    bson_ctx *bson = luaL_checkudata(lua, 1, MT_BSON);
    const char *key = luaL_checkstring(lua, 2);
    bson_append_array_begain(bson, key);
    return 0;
}
/// <summary>
/// 结束当前嵌套层级（写入 EOD 并回填长度）
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <returns>无</returns>
static int32_t _lbson_end(lua_State *lua) {
    bson_ctx *bson = luaL_checkudata(lua, 1, MT_BSON);
    bson_append_end(bson);
    return 0;
}
/// <summary>
/// 追加 double 字段
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名</param>
/// <param name="val" type="number">double 值</param>
/// <returns>无</returns>
static int32_t _lbson_double(lua_State *lua) {
    bson_ctx *bson = luaL_checkudata(lua, 1, MT_BSON);
    const char *key = luaL_checkstring(lua, 2);
    double val = luaL_checknumber(lua, 3);
    bson_append_double(bson, key, val);
    return 0;
}
/// <summary>
/// 追加 UTF-8 字符串字段
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名</param>
/// <param name="val" type="string">UTF-8 字符串值</param>
/// <returns>无</returns>
static int32_t _lbson_utf8(lua_State *lua) {
    bson_ctx *bson = luaL_checkudata(lua, 1, MT_BSON);
    const char *key = luaL_checkstring(lua, 2);
    size_t vlen;
    const char *val = luaL_checklstring(lua, 3, &vlen);
    bson_append_utf8_n(bson, key, val, vlen);
    return 0;
}
/// <summary>
/// 追加已序列化 BSON 文档字段
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名</param>
/// <param name="doc" type="string|lightuserdata">序列化 BSON 文档；字符串时长度自动取得</param>
/// <param name="lens" type="integer?">doc 为 lightuserdata 时必填</param>
/// <returns>无</returns>
static int32_t _lbson_append_doc(lua_State *lua) {
    bson_ctx *bson = luaL_checkudata(lua, 1, MT_BSON);
    const char *key = luaL_checkstring(lua, 2);
    char *doc;
    size_t lens;
    if (LUA_TSTRING == lua_type(lua, 3)) {
        doc = (char *)luaL_checklstring(lua, 3, &lens);
    } else {
        doc = lua_touserdata(lua, 3);
        lens = (size_t)luaL_checkinteger(lua, 4);
    }
    bson_append_document(bson, key, doc, lens);
    return 0;
}
/// <summary>
/// 追加已序列化 BSON 数组字段
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名</param>
/// <param name="doc" type="string|lightuserdata">序列化 BSON 数组；字符串时长度自动取得</param>
/// <param name="lens" type="integer?">doc 为 lightuserdata 时必填</param>
/// <returns>无</returns>
static int32_t _lbson_append_arr(lua_State *lua) {
    bson_ctx *bson = luaL_checkudata(lua, 1, MT_BSON);
    const char *key = luaL_checkstring(lua, 2);
    char *doc;
    size_t lens;
    if (LUA_TSTRING == lua_type(lua, 3)) {
        doc = (char *)luaL_checklstring(lua, 3, &lens);
    } else {
        doc = lua_touserdata(lua, 3);
        lens = (size_t)luaL_checkinteger(lua, 4);
    }
    bson_append_array(bson, key, doc, lens);
    return 0;
}
/// <summary>
/// 追加二进制数据字段
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名</param>
/// <param name="subtype" type="integer">bson_subtype 枚举值</param>
/// <param name="data" type="string|lightuserdata">二进制数据；字符串时长度自动取得</param>
/// <param name="lens" type="integer?">data 为 lightuserdata 时必填</param>
/// <returns>无</returns>
static int32_t _lbson_binary(lua_State *lua) {
    bson_ctx *bson = luaL_checkudata(lua, 1, MT_BSON);
    const char *key = luaL_checkstring(lua, 2);
    bson_subtype subtype = (bson_subtype)luaL_checkinteger(lua, 3);
    char *data;
    size_t lens;
    if (LUA_TSTRING == lua_type(lua, 4)) {
        data = (char *)luaL_checklstring(lua, 4, &lens);
    } else {
        data = lua_touserdata(lua, 4);
        lens = (size_t)luaL_checkinteger(lua, 5);
    }
    bson_append_binary(bson, key, subtype, data, lens);
    return 0;
}
/// <summary>
/// 追加 ObjectId 字段
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名</param>
/// <param name="oid" type="string|lightuserdata">12 字节 ObjectId</param>
/// <returns>无</returns>
static int32_t _lbson_oid(lua_State *lua) {
    bson_ctx *bson = luaL_checkudata(lua, 1, MT_BSON);
    const char *key = luaL_checkstring(lua, 2);
    char *oid;
    if (LUA_TSTRING == lua_type(lua, 3)) {
        size_t lens;
        oid = (char *)luaL_checklstring(lua, 3, &lens);
        // bson_append_oid 固定按 BSON_OID_LENS 字节读，短串触发 OOB 读
        // 与同文件 _lbson_mkoid 校验保持一致
        luaL_argcheck(lua, lens == BSON_OID_LENS, 3, "OID must be 12 bytes");
    } else {
        LUACHECK_LUDATA(lua, 3);
        oid = lua_touserdata(lua, 3);
    }
    bson_append_oid(bson, key, oid);
    return 0;
}
/// <summary>
/// 追加布尔字段
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名</param>
/// <param name="val" type="boolean">布尔值</param>
/// <returns>无</returns>
static int32_t _lbson_bool(lua_State *lua) {
    bson_ctx *bson = luaL_checkudata(lua, 1, MT_BSON);
    const char *key = luaL_checkstring(lua, 2);
    int8_t b = (int8_t)lua_toboolean(lua, 3);
    bson_append_bool(bson, key, b);
    return 0;
}
/// <summary>
/// 追加 UTC 毫秒时间戳字段
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名</param>
/// <param name="ms" type="integer">UTC 毫秒时间戳</param>
/// <returns>无</returns>
static int32_t _lbson_date(lua_State *lua) {
    bson_ctx *bson = luaL_checkudata(lua, 1, MT_BSON);
    const char *key = luaL_checkstring(lua, 2);
    int64_t ms = (int64_t)luaL_checkinteger(lua, 3);
    bson_append_date(bson, key, ms);
    return 0;
}
/// <summary>
/// 追加 null 字段
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名</param>
/// <returns>无</returns>
static int32_t _lbson_null(lua_State *lua) {
    bson_ctx *bson = luaL_checkudata(lua, 1, MT_BSON);
    const char *key = luaL_checkstring(lua, 2);
    bson_append_null(bson, key);
    return 0;
}
/// <summary>
/// 追加正则表达式字段
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名</param>
/// <param name="pattern" type="string">正则模式</param>
/// <param name="options" type="string">选项字符串（如 "i"、"m"）</param>
/// <returns>无</returns>
static int32_t _lbson_regex(lua_State *lua) {
    bson_ctx *bson = luaL_checkudata(lua, 1, MT_BSON);
    const char *key = luaL_checkstring(lua, 2);
    const char *pattern = luaL_checkstring(lua, 3);
    const char *options = luaL_checkstring(lua, 4);
    bson_append_regex(bson, key, pattern, options);
    return 0;
}
/// <summary>
/// 追加 JavaScript 代码字段
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名</param>
/// <param name="code" type="string">JavaScript 代码</param>
/// <returns>无</returns>
static int32_t _lbson_jscode(lua_State *lua) {
    bson_ctx *bson = luaL_checkudata(lua, 1, MT_BSON);
    const char *key = luaL_checkstring(lua, 2);
    const char *code = luaL_checkstring(lua, 3);
    bson_append_jscode(bson, key, code);
    return 0;
}
/// <summary>
/// 追加 int32 字段
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名</param>
/// <param name="val" type="integer">int32 值</param>
/// <returns>无</returns>
static int32_t _lbson_int32(lua_State *lua) {
    bson_ctx *bson = luaL_checkudata(lua, 1, MT_BSON);
    const char *key = luaL_checkstring(lua, 2);
    int32_t val = (int32_t)luaL_checkinteger(lua, 3);
    bson_append_int32(bson, key, val);
    return 0;
}
/// <summary>
/// 追加 BSON Timestamp 字段
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名</param>
/// <param name="ts" type="integer">秒级时间戳</param>
/// <param name="inc" type="integer">同秒内自增量</param>
/// <returns>无</returns>
static int32_t _lbson_timestamp(lua_State *lua) {
    bson_ctx *bson = luaL_checkudata(lua, 1, MT_BSON);
    const char *key = luaL_checkstring(lua, 2);
    uint32_t ts = (uint32_t)luaL_checkinteger(lua, 3);
    uint32_t inc = (uint32_t)luaL_checkinteger(lua, 4);
    bson_append_timestamp(bson, key, ts, inc);
    return 0;
}
/// <summary>
/// 追加 int64 字段
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名</param>
/// <param name="val" type="integer">int64 值</param>
/// <returns>无</returns>
static int32_t _lbson_int64(lua_State *lua) {
    bson_ctx *bson = luaL_checkudata(lua, 1, MT_BSON);
    const char *key = luaL_checkstring(lua, 2);
    int64_t val = (int64_t)luaL_checkinteger(lua, 3);
    bson_append_int64(bson, key, val);
    return 0;
}
/// <summary>
/// 追加 MinKey 字段
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名</param>
/// <returns>无</returns>
static int32_t _lbson_minkey(lua_State *lua) {
    bson_ctx *bson = luaL_checkudata(lua, 1, MT_BSON);
    const char *key = luaL_checkstring(lua, 2);
    bson_append_minkey(bson, key);
    return 0;
}
/// <summary>
/// 追加 MaxKey 字段
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名</param>
/// <returns>无</returns>
static int32_t _lbson_maxkey(lua_State *lua) {
    bson_ctx *bson = luaL_checkudata(lua, 1, MT_BSON);
    const char *key = luaL_checkstring(lua, 2);
    bson_append_maxkey(bson, key);
    return 0;
}
/// <summary>
/// 将另一个已完成 BSON 文档的内容拼接到当前文档
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="doc" type="string|lightuserdata">已完成 BSON 文档</param>
/// <param name="size" type="integer?">doc 为 lightuserdata 时必填，表示 buffer 字节数</param>
/// <returns>无</returns>
static int32_t _lbson_cat(lua_State *lua) {
    bson_ctx *bson = luaL_checkudata(lua, 1, MT_BSON);
    char *doc;
    size_t actual_lens;
    int32_t type = lua_type(lua, 2);
    if (LUA_TSTRING == type) {
        doc = (char *)luaL_checklstring(lua, 2, &actual_lens);
    } else if (LUA_TLIGHTUSERDATA == type) {
        doc = lua_touserdata(lua, 2);
        actual_lens = (size_t)luaL_checkinteger(lua, 3);
    } else {
        return luaL_argerror(lua, 2, "string or light userdata expected");
    }
    if (actual_lens >= 4) {
        uint32_t bson_lens = (uint32_t)unpack_integer(doc, 4, 1, 0);
        if (bson_lens > (uint32_t)actual_lens) {
            return luaL_error(lua, "bson_cat: embedded length %u exceeds buffer size %zu", bson_lens, actual_lens);
        }
    }
    bson_cat(bson, doc);
    return 0;
}
/// <summary>
/// 检查文档是否已完整写入（depth 为 0）
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <returns type="boolean">完整 true，否则 false</returns>
static int32_t _lbson_complete(lua_State *lua) {
    bson_ctx *bson = luaL_checkudata(lua, 1, MT_BSON);
    lua_pushboolean(lua, bson_complete(bson));
    return 1;
}
/// <summary>
/// 返回内部 BSON 数据
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <returns type="lightuserdata">数据指针</returns>
/// <returns type="integer">字节数</returns>
static int32_t _lbson_data(lua_State *lua) {
    bson_ctx *bson = luaL_checkudata(lua, 1, MT_BSON);
    LPUB_RET_LUD(lua, BSON_DOC(bson), (lua_Integer)BSON_DOC_LENS(bson));
}
/// <summary>
/// 将当前文档转换为可读字符串
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <returns type="string?">可读字符串；转换失败返回 nil</returns>
static int32_t _lbson_tostring(lua_State *lua) {
    bson_ctx *bson = luaL_checkudata(lua, 1, MT_BSON);
    char *str = bson_tostring(bson);
    if (NULL == str) {
        lua_pushnil(lua);
        return 1;
    }
    size_t len = strlen(str);
    luaL_Buffer lbuf;
    char *p = luaL_buffinitsize(lua, &lbuf, len);
    memcpy(p, str, len);
    FREE(str);
    luaL_pushresultsize(&lbuf, len);
    return 1;
}
/// <summary>
/// 生成一个新的 ObjectId
/// </summary>
/// <param>无</param>
/// <returns type="string">12 字节 ObjectId</returns>
static int32_t _lbson_gen_oid(lua_State *lua) {
    char oid[BSON_OID_LENS];
    bson_oid(oid);
    lua_pushlstring(lua, oid, BSON_OID_LENS);
    return 1;
}
/// <summary>
/// 返回空 BSON 文档数据（指针为静态存储，勿释放）
/// </summary>
/// <param>无</param>
/// <returns type="lightuserdata">空文档数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lbson_empty(lua_State *lua) {
    size_t lens;
    const char *data = bson_empty(&lens);
    LPUB_RET_LUD(lua, (void *)data, (lua_Integer)lens);
}
/// <summary>
/// 将原始 BSON 数据转换为可读字符串
/// </summary>
/// <param name="data" type="string|lightuserdata">BSON 数据；字符串时长度自动取得</param>
/// <param name="lens" type="integer?">data 为 lightuserdata 时必填</param>
/// <returns type="string?">可读字符串；转换失败返回 nil</returns>
static int32_t _lbson_tostring2(lua_State *lua) {
    char *data;
    size_t lens;
    if (LUA_TSTRING == lua_type(lua, 1)) {
        data = (char *)luaL_checklstring(lua, 1, &lens);
    } else {
        LUACHECK_LUDATA(lua, 1);
        data = lua_touserdata(lua, 1);
        lens = (size_t)luaL_checkinteger(lua, 2);
    }
    char *str = bson_tostring2(data, lens);
    if (NULL == str) {
        lua_pushnil(lua);
        return 1;
    }
    size_t slen = strlen(str);
    luaL_Buffer lbuf;
    char *p = luaL_buffinitsize(lua, &lbuf, slen);
    memcpy(p, str, slen);
    FREE(str);
    luaL_pushresultsize(&lbuf, slen);
    return 1;
}
/// <summary>
/// 将 bson_type 枚举整数转换为可读字符串
/// </summary>
/// <param name="type" type="integer">bson_type 枚举值</param>
/// <returns type="string">可读类型名</returns>
static int32_t _lbson_type_tostring(lua_State *lua) {
    bson_type type = (bson_type)luaL_checkinteger(lua, 1);
    lua_pushstring(lua, bson_type_tostring(type));
    return 1;
}
/// <summary>
/// 将 bson_subtype 枚举整数转换为可读字符串
/// </summary>
/// <param name="type" type="integer">bson_subtype 枚举值</param>
/// <returns type="string">可读子类型名</returns>
static int32_t _lbson_subtype_tostring(lua_State *lua) {
    bson_subtype type = (bson_subtype)luaL_checkinteger(lua, 1);
    lua_pushstring(lua, bson_subtype_tostring(type));
    return 1;
}
// ---- wrapper：OID ----
/// <summary>
/// 创建 OID 包装对象
/// </summary>
/// <param name="str" type="string">12 字节 ObjectId 原始数据</param>
/// <returns type="_bson_oid">OID 包装对象</returns>
static int32_t _lbson_mkoid(lua_State *lua) {
    size_t lens;
    const char *str = luaL_checklstring(lua, 1, &lens);
    luaL_argcheck(lua, lens == BSON_OID_LENS, 1, "OID must be 12 bytes");
    lbson_oid_t *ud = lua_newuserdata(lua, sizeof(lbson_oid_t));
    memcpy(ud->data, str, BSON_OID_LENS);
    ASSOC_MTABLE(lua, MT_BSON_OID);
    return 1;
}
// 返回 12 字节 OID 原始字符串
static int32_t _lbson_mkoid_data(lua_State *lua) {
    lbson_oid_t *ud = luaL_checkudata(lua, 1, MT_BSON_OID);
    lua_pushlstring(lua, ud->data, BSON_OID_LENS);
    return 1;
}
static int32_t _lbson_mkoid_gc(lua_State *lua) {
    luaL_checkudata(lua, 1, MT_BSON_OID);
    return 0;
}
// ---- wrapper：DATE ----
/// <summary>
/// 创建 Date 包装对象
/// </summary>
/// <param name="ms" type="integer">UTC 毫秒时间戳</param>
/// <returns type="_bson_date">Date 包装对象</returns>
static int32_t _lbson_mkdate(lua_State *lua) {
    int64_t ms = (int64_t)luaL_checkinteger(lua, 1);
    lbson_date_t *ud = lua_newuserdata(lua, sizeof(lbson_date_t));
    ud->ms = ms;
    ASSOC_MTABLE(lua, MT_BSON_DATE);
    return 1;
}
// 返回 UTC 毫秒时间戳
static int32_t _lbson_mkdate_ms(lua_State *lua) {
    lbson_date_t *ud = luaL_checkudata(lua, 1, MT_BSON_DATE);
    lua_pushinteger(lua, ud->ms);
    return 1;
}
static int32_t _lbson_mkdate_gc(lua_State *lua) {
    luaL_checkudata(lua, 1, MT_BSON_DATE);
    return 0;
}
// ---- wrapper：BINARY ----
/// <summary>
/// 创建 Binary 包装对象
/// </summary>
/// <param name="subtype" type="integer">bson_subtype 枚举值</param>
/// <param name="data" type="string|lightuserdata">二进制数据；字符串时长度自动取得</param>
/// <param name="lens" type="integer?">data 为 lightuserdata 时必填</param>
/// <returns type="_bson_binary">Binary 包装对象</returns>
static int32_t _lbson_mkbinary(lua_State *lua) {
    bson_subtype subtype = (bson_subtype)luaL_checkinteger(lua, 1);
    size_t lens;
    const char *data;
    if (LUA_TSTRING == lua_type(lua, 2)) {
        data = luaL_checklstring(lua, 2, &lens);
    } else {
        LUACHECK_LUDATA(lua, 2);
        data = lua_touserdata(lua, 2);
        lens = (size_t)luaL_checkinteger(lua, 3);
    }
    lbson_binary_t *ud = lua_newuserdata(lua, sizeof(lbson_binary_t) + lens);
    ud->subtype = subtype;
    ud->lens = lens;
    memcpy(ud + 1, data, lens);
    ASSOC_MTABLE(lua, MT_BSON_BINARY);
    return 1;
}
// 返回 bson_subtype 枚举值
static int32_t _lbson_mkbinary_subtype(lua_State *lua) {
    lbson_binary_t *ud = luaL_checkudata(lua, 1, MT_BSON_BINARY);
    lua_pushinteger(lua, ud->subtype);
    return 1;
}
// 返回二进制内容（Lua 字符串）
static int32_t _lbson_mkbinary_data(lua_State *lua) {
    lbson_binary_t *ud = luaL_checkudata(lua, 1, MT_BSON_BINARY);
    lua_pushlstring(lua, (const char *)(ud + 1), ud->lens);
    return 1;
}
static int32_t _lbson_mkbinary_gc(lua_State *lua) {
    luaL_checkudata(lua, 1, MT_BSON_BINARY);
    return 0;
}
// ---- wrapper：INT64 ----
/// <summary>
/// 创建 INT64 包装对象，强制以 BSON INT64 编码
/// </summary>
/// <param name="val" type="integer">int64 值</param>
/// <returns type="_bson_int64">INT64 包装对象</returns>
static int32_t _lbson_mkint64(lua_State *lua) {
    int64_t val = (int64_t)luaL_checkinteger(lua, 1);
    lbson_int64_t *ud = lua_newuserdata(lua, sizeof(lbson_int64_t));
    ud->val = val;
    ASSOC_MTABLE(lua, MT_BSON_INT64);
    return 1;
}
// 返回 int64 整数值
static int32_t _lbson_mkint64_val(lua_State *lua) {
    lbson_int64_t *ud = luaL_checkudata(lua, 1, MT_BSON_INT64);
    lua_pushinteger(lua, ud->val);
    return 1;
}
static int32_t _lbson_mkint64_gc(lua_State *lua) {
    luaL_checkudata(lua, 1, MT_BSON_INT64);
    return 0;
}
// ---- encode 辅助 ----
// 检查 Lua table 是否为纯序列（key 全为连续整数 1..n，n>0）
static int32_t _table_is_array(lua_State *lua, int32_t idx) {
    lua_Integer n = (lua_Integer)lua_rawlen(lua, idx);
    lua_Integer count;
    if (0 == n) {
        return 0;
    }
    count = 0;
    lua_pushnil(lua);
    while (lua_next(lua, idx)) {
        lua_pop(lua, 1);
        count++;
    }
    return count == n;
}
static void _lbson_encode_value(lua_State *lua, int32_t val_idx, bson_ctx *bson, const char *key);
static void _lbson_encode_table_as_doc(lua_State *lua, int32_t idx, bson_ctx *bson) {
    luaL_checkstack(lua, 4, "bson encode");
    lua_pushnil(lua);
    while (lua_next(lua, idx)) {
        const char *key = NULL;
        char keybuf[24];
        if (LUA_TSTRING == lua_type(lua, -2)) {
            key = lua_tostring(lua, -2);
        } else if (lua_isinteger(lua, -2)) {
            snprintf(keybuf, sizeof(keybuf), "%lld", (long long)lua_tointeger(lua, -2));
            key = keybuf;
        } else {
            lua_pop(lua, 1);
            continue;
        }
        _lbson_encode_value(lua, lua_gettop(lua), bson, key);
        lua_pop(lua, 1);
    }
}
static void _lbson_encode_table_as_arr(lua_State *lua, int32_t idx, bson_ctx *bson, lua_Integer n) {
    lua_Integer i;
    char keybuf[24];
    for (i = 1; i <= n; i++) {
        snprintf(keybuf, sizeof(keybuf), "%lld", (long long)(i - 1));
        lua_rawgeti(lua, idx, i);
        _lbson_encode_value(lua, lua_gettop(lua), bson, keybuf);
        lua_pop(lua, 1);
    }
}
static void _lbson_encode_value(lua_State *lua, int32_t val_idx, bson_ctx *bson, const char *key) {
    switch (lua_type(lua, val_idx)) {
    case LUA_TNIL:
        bson_append_null(bson, key);
        break;
    case LUA_TBOOLEAN:
        bson_append_bool(bson, key, (int8_t)lua_toboolean(lua, val_idx));
        break;
    case LUA_TNUMBER:
        if (lua_isinteger(lua, val_idx)) {
            lua_Integer iv = lua_tointeger(lua, val_idx);
            if (iv >= INT32_MIN && iv <= INT32_MAX) {
                bson_append_int32(bson, key, (int32_t)iv);
            } else {
                bson_append_int64(bson, key, (int64_t)iv);
            }
        } else {
            bson_append_double(bson, key, lua_tonumber(lua, val_idx));
        }
        break;
    case LUA_TSTRING: {
        size_t lens;
        const char *s = lua_tolstring(lua, val_idx, &lens);
        bson_append_utf8_n(bson, key, s, lens);
        break;
    }
    case LUA_TTABLE:
        luaL_checkstack(lua, 4, "bson encode");
        if (_table_is_array(lua, val_idx)) {
            lua_Integer n = (lua_Integer)lua_rawlen(lua, val_idx);
            bson_append_array_begain(bson, key);
            _lbson_encode_table_as_arr(lua, val_idx, bson, n);
            bson_append_end(bson);
        } else {
            bson_append_document_begain(bson, key);
            _lbson_encode_table_as_doc(lua, val_idx, bson);
            bson_append_end(bson);
        }
        break;
    case LUA_TUSERDATA:
        if (NULL != luaL_testudata(lua, val_idx, MT_BSON_OID)) {
            lbson_oid_t *ud = lua_touserdata(lua, val_idx);
            bson_append_oid(bson, key, ud->data);
        } else if (NULL != luaL_testudata(lua, val_idx, MT_BSON_DATE)) {
            lbson_date_t *ud = lua_touserdata(lua, val_idx);
            bson_append_date(bson, key, ud->ms);
        } else if (NULL != luaL_testudata(lua, val_idx, MT_BSON_BINARY)) {
            lbson_binary_t *ud = lua_touserdata(lua, val_idx);
            bson_append_binary(bson, key, ud->subtype, (char *)(ud + 1), ud->lens);
        } else if (NULL != luaL_testudata(lua, val_idx, MT_BSON_INT64)) {
            lbson_int64_t *ud = lua_touserdata(lua, val_idx);
            bson_append_int64(bson, key, ud->val);
        }
        break;
    default:
        break;
    }
}
/// <summary>
/// 将 Lua table 编码为 BSON 文档，返回 bson_ctx userdata；顶层始终作为 DOCUMENT，嵌套纯序列 table 作为 ARRAY
/// </summary>
/// <param name="t" type="table">待编码的 Lua table</param>
/// <returns type="_bson_ctx">完整 bson 对象，可直接调用 :data() 传给 mongo API</returns>
static int32_t _lbson_encode(lua_State *lua) {
    luaL_checktype(lua, 1, LUA_TTABLE);
    bson_ctx *bson = lua_newuserdata(lua, sizeof(bson_ctx));
    bson_init(bson, NULL, 0);
    ASSOC_MTABLE(lua, MT_BSON);
    if (_table_is_array(lua, 1)) {
        _lbson_encode_table_as_arr(lua, 1, bson, (lua_Integer)lua_rawlen(lua, 1));
    } else {
        _lbson_encode_table_as_doc(lua, 1, bson);
    }
    bson_append_end(bson);
    return 1;
}
// ---- decode 辅助 ----
static void _lbson_decode_document(lua_State *lua, char *data, size_t lens, int32_t is_array, int32_t depth);
// 将迭代器当前字段解码并写入栈顶 table；null / 未知类型跳过
static void _lbson_decode_field(lua_State *lua, bson_iter *iter, int32_t is_array, int32_t depth) {
    int32_t err;
    if (is_array) {
        // BSON 数组 key 是 "0","1"...，转为 Lua 1-base 整数索引
        lua_pushinteger(lua, (lua_Integer)atoi(iter->key) + 1);
    } else {
        lua_pushstring(lua, iter->key);
    }
    switch (iter->type) {
    case BSON_DOUBLE: {
        double val = bson_iter_double(iter, &err);
        if (ERR_OK != err) {
            lua_pop(lua, 1);
            return;
        }
        lua_pushnumber(lua, val);
        break;
    }
    case BSON_UTF8: {
        const char *val = bson_iter_utf8(iter, &err);
        if (ERR_OK != err) {
            lua_pop(lua, 1);
            return;
        }
        lua_pushlstring(lua, val, iter->lens);
        break;
    }
    case BSON_JSCODE: {
        const char *val = bson_iter_jscode(iter, &err);
        if (ERR_OK != err) {
            lua_pop(lua, 1);
            return;
        }
        lua_pushlstring(lua, val, iter->lens);
        break;
    }
    case BSON_DOCUMENT: {
        size_t dlens;
        char *ddata = bson_iter_document(iter, &dlens, &err);
        if (ERR_OK != err) {
            lua_pop(lua, 1);
            return;
        }
        _lbson_decode_document(lua, ddata, dlens, 0, depth + 1);
        break;
    }
    case BSON_ARRAY: {
        size_t alens;
        char *adata = bson_iter_array(iter, &alens, &err);
        if (ERR_OK != err) {
            lua_pop(lua, 1);
            return;
        }
        _lbson_decode_document(lua, adata, alens, 1, depth + 1);
        break;
    }
    case BSON_BINARY: {
        bson_subtype subtype;
        size_t blens;
        char *bdata = bson_iter_binary(iter, &subtype, &blens, &err);
        if (ERR_OK != err) {
            lua_pop(lua, 1);
            return;
        }
        lbson_binary_t *ud = lua_newuserdata(lua, sizeof(lbson_binary_t) + blens);
        ud->subtype = subtype;
        ud->lens = blens;
        memcpy(ud + 1, bdata, blens);
        ASSOC_MTABLE(lua, MT_BSON_BINARY);
        break;
    }
    case BSON_OID: {
        char *oid = bson_iter_oid(iter, &err);
        if (ERR_OK != err) {
            lua_pop(lua, 1);
            return;
        }
        lbson_oid_t *ud = lua_newuserdata(lua, sizeof(lbson_oid_t));
        memcpy(ud->data, oid, BSON_OID_LENS);
        ASSOC_MTABLE(lua, MT_BSON_OID);
        break;
    }
    case BSON_BOOL: {
        int32_t val = bson_iter_bool(iter, &err);
        if (ERR_OK != err) {
            lua_pop(lua, 1);
            return;
        }
        lua_pushboolean(lua, val);
        break;
    }
    case BSON_DATE: {
        int64_t ms = bson_iter_date(iter, &err);
        if (ERR_OK != err) {
            lua_pop(lua, 1);
            return;
        }
        lbson_date_t *ud = lua_newuserdata(lua, sizeof(lbson_date_t));
        ud->ms = ms;
        ASSOC_MTABLE(lua, MT_BSON_DATE);
        break;
    }
    case BSON_NULL:
        lua_pop(lua, 1);
        return;
    case BSON_INT32: {
        int32_t val = bson_iter_int32(iter, &err);
        if (ERR_OK != err) {
            lua_pop(lua, 1);
            return;
        }
        lua_pushinteger(lua, val);
        break;
    }
    case BSON_INT64: {
        int64_t val = bson_iter_int64(iter, &err);
        if (ERR_OK != err) {
            lua_pop(lua, 1);
            return;
        }
        lbson_int64_t *ud = lua_newuserdata(lua, sizeof(lbson_int64_t));
        ud->val = val;
        ASSOC_MTABLE(lua, MT_BSON_INT64);
        break;
    }
    default:
        lua_pop(lua, 1);
        return;
    }
    lua_rawset(lua, -3);
}
static void _lbson_decode_document(lua_State *lua, char *data, size_t lens, int32_t is_array, int32_t depth) {
    luaL_checkstack(lua, 6, "bson decode");
    lua_newtable(lua);
    if (depth > BSON_MAX_DEPTH) {
        LOG_WARN("bson decode depth exceeded max %d.", BSON_MAX_DEPTH);
        return;
    }
    bson_ctx sub;
    bson_iter iter;
    bson_init(&sub, data, lens);
    bson_iter_init(&iter, &sub);
    while (bson_iter_next(&iter)) {
        _lbson_decode_field(lua, &iter, is_array, depth);
    }
}
/// <summary>
/// 将 BSON 数据解码为 Lua table；BSON ARRAY 字段解码为整数 key（1-base）table，DOCUMENT 解码为字符串 key table
/// </summary>
/// <param name="data" type="userdata|string|lightuserdata">bson_ctx userdata、Lua 字符串或 lightuserdata 指针</param>
/// <param name="lens" type="integer?">data 为 lightuserdata 时必填，字节数</param>
/// <returns type="table&lt;string,any&gt;|any[]">解码结果；BSON DOCUMENT 为字符串 key 表，BSON ARRAY 为整数 key（1-base）序列</returns>
static int32_t _lbson_decode(lua_State *lua) {
    char *data;
    size_t lens;
    if (LUA_TUSERDATA == lua_type(lua, 1)) {
        bson_ctx *bson = luaL_checkudata(lua, 1, MT_BSON);
        data = BSON_DOC(bson);
        lens = BSON_DOC_LENS(bson);
    } else if (LUA_TSTRING == lua_type(lua, 1)) {
        data = (char *)luaL_checklstring(lua, 1, &lens);
    } else {
        LUACHECK_LUDATA(lua, 1);
        data = lua_touserdata(lua, 1);
        lens = (size_t)luaL_checkinteger(lua, 2);
    }
    _lbson_decode_document(lua, data, lens, 0, 0);
    return 1;
}
// 注册无对外库的 wrapper 元表
static void _lbson_reg_wrapper_mt(lua_State *lua, const char *name, luaL_Reg *methods) {
    luaL_newmetatable(lua, name);
    lua_pushvalue(lua, -1);
    lua_setfield(lua, -2, "__index");
    luaL_setfuncs(lua, methods, 0);
    lua_pop(lua, 1);
}
//bson
LUAMOD_API int luaopen_bson(lua_State *lua) {
    luaL_Reg oid_mt[] = {
        { "data",  _lbson_mkoid_data },
        { "__gc",  _lbson_mkoid_gc },
        { NULL, NULL }
    };
    luaL_Reg date_mt[] = {
        { "ms",    _lbson_mkdate_ms },
        { "__gc",  _lbson_mkdate_gc },
        { NULL, NULL }
    };
    luaL_Reg binary_mt[] = {
        { "subtype", _lbson_mkbinary_subtype },
        { "data",    _lbson_mkbinary_data },
        { "__gc",    _lbson_mkbinary_gc },
        { NULL, NULL }
    };
    luaL_Reg int64_mt[] = {
        { "val",   _lbson_mkint64_val },
        { "__gc",  _lbson_mkint64_gc },
        { NULL, NULL }
    };
    _lbson_reg_wrapper_mt(lua, MT_BSON_OID,    oid_mt);
    _lbson_reg_wrapper_mt(lua, MT_BSON_DATE,   date_mt);
    _lbson_reg_wrapper_mt(lua, MT_BSON_BINARY, binary_mt);
    _lbson_reg_wrapper_mt(lua, MT_BSON_INT64,  int64_mt);
    luaL_Reg reg_new[] = {
        { "new",              _lbson_new },
        { "oid",              _lbson_gen_oid },
        { "empty",            _lbson_empty },
        { "tostring2",        _lbson_tostring2 },
        { "type_tostring",    _lbson_type_tostring },
        { "subtype_tostring", _lbson_subtype_tostring },
        { "mkoid",            _lbson_mkoid },
        { "mkdate",           _lbson_mkdate },
        { "mkbinary",         _lbson_mkbinary },
        { "mkint64",          _lbson_mkint64 },
        { "encode",           _lbson_encode },
        { "decode",           _lbson_decode },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "double",      _lbson_double },
        { "utf8",        _lbson_utf8 },
        { "doc_begin",   _lbson_doc_begin },
        { "arr_begin",   _lbson_arr_begin },
        { "end",         _lbson_end },
        { "append_doc",  _lbson_append_doc },
        { "append_arr",  _lbson_append_arr },
        { "binary",      _lbson_binary },
        { "oid",         _lbson_oid },
        { "bool",        _lbson_bool },
        { "date",        _lbson_date },
        { "null",        _lbson_null },
        { "regex",       _lbson_regex },
        { "jscode",      _lbson_jscode },
        { "int32",       _lbson_int32 },
        { "timestamp",   _lbson_timestamp },
        { "int64",       _lbson_int64 },
        { "minkey",      _lbson_minkey },
        { "maxkey",      _lbson_maxkey },
        { "cat",         _lbson_cat },
        { "complete",    _lbson_complete },
        { "data",        _lbson_data },
        { "tostring",    _lbson_tostring },
        { "free",        _lbson_free },
        { "__gc",        _lbson_free },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_BSON, reg_new, reg_func);
    return 1;
}
// ---- bson.iter ----
/// <summary>
/// 从 bson 上下文创建迭代器（以 uservalue 持有 bson 引用，防止 GC）。
/// 一次性消费契约：iter 会推进底层 bson_ctx 的 doc.offset，且 new 时强制把 offset
/// 重置到 0 以让 iter_init 正确读 doclens（encode 后 offset 在末尾，直接 init 读到 garbage）。
/// 因此对同一 bson 调用 iter.new 后，原 bson 的 :data() / :complete() 不再可靠——
/// 需要保留原始数据时请先调用 :data() 取走再创建 iter，或直接走 bson.decode() 转 Lua table
/// </summary>
/// <param name="bson" type="_bson_ctx">bson 对象</param>
/// <returns type="_bson_iter_ctx">iter 对象</returns>
static int32_t _lbson_iter_new(lua_State *lua) {
    bson_ctx *bson = luaL_checkudata(lua, 1, MT_BSON);
    // encode/write 模式下 doc.offset 在末尾，bson_iter_init 假定 offset=0 才能正确
    // 读首 4 字节 doclens；强制 reset 让 iter 在两种来源（encode / raw bytes）下行为一致
    binary_offset(&bson->doc, 0);
    bson_iter *iter = lua_newuserdata(lua, sizeof(bson_iter));
    bson_iter_init(iter, bson);
    lua_pushvalue(lua, 1);
    lua_setiuservalue(lua, -2, 1);// 持有 bson 引用，防止 GC
    ASSOC_MTABLE(lua, MT_BSON_ITER);
    return 1;
}
/// <summary>
/// 迭代器析构（绑定为 __gc）；迭代器不拥有任何堆内存，仅做类型校验
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns>无</returns>
static int32_t _lbson_iter_gc(lua_State *lua) {
    luaL_checkudata(lua, 1, MT_BSON_ITER);
    return 0;
}
/// <summary>
/// 将迭代器重置到文档起始位置
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns>无</returns>
static int32_t _lbson_iter_reset(lua_State *lua) {
    bson_iter *iter = luaL_checkudata(lua, 1, MT_BSON_ITER);
    bson_iter_reset(iter);
    return 0;
}
/// <summary>
/// 移动到下一个字段
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="boolean">有值返回 true；遍历结束返回 false</returns>
static int32_t _lbson_iter_next(lua_State *lua) {
    bson_iter *iter = luaL_checkudata(lua, 1, MT_BSON_ITER);
    lua_pushboolean(lua, bson_iter_next(iter));
    return 1;
}
/// <summary>
/// 查找指定键（支持点分多级路径），找到则移动迭代器到该位置
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <param name="keys" type="string">字段路径（如 "a.b.c"）</param>
/// <returns type="boolean">找到 true，否则 false</returns>
static int32_t _lbson_iter_find(lua_State *lua) {
    bson_iter *iter = luaL_checkudata(lua, 1, MT_BSON_ITER);
    const char *keys = luaL_checkstring(lua, 2);
    bson_iter result;
    if (ERR_OK == bson_iter_find(iter, keys, &result)) {
        *iter = result;
        lua_pushboolean(lua, 1);
    } else {
        lua_pushboolean(lua, 0);
    }
    return 1;
}
/// <summary>
/// 返回当前字段的 BSON 类型枚举整数
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="integer">bson_type 枚举值</returns>
static int32_t _lbson_iter_type(lua_State *lua) {
    bson_iter *iter = luaL_checkudata(lua, 1, MT_BSON_ITER);
    lua_pushinteger(lua, iter->type);
    return 1;
}
/// <summary>
/// 返回当前字段的键名
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="string">字段名</returns>
static int32_t _lbson_iter_key(lua_State *lua) {
    bson_iter *iter = luaL_checkudata(lua, 1, MT_BSON_ITER);
    lua_pushstring(lua, iter->key);
    return 1;
}
/// <summary>
/// 读取当前字段的 double 值
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="number?">double 值；类型不符返回 nil</returns>
static int32_t _lbson_iter_double(lua_State *lua) {
    bson_iter *iter = luaL_checkudata(lua, 1, MT_BSON_ITER);
    int32_t err;
    double val = bson_iter_double(iter, &err);
    if (ERR_OK != err) {
        lua_pushnil(lua);
        return 1;
    }
    lua_pushnumber(lua, val);
    return 1;
}
/// <summary>
/// 读取当前字段的 UTF-8 字符串
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="string?">UTF-8 字符串；类型不符返回 nil</returns>
static int32_t _lbson_iter_utf8(lua_State *lua) {
    bson_iter *iter = luaL_checkudata(lua, 1, MT_BSON_ITER);
    int32_t err;
    const char *val = bson_iter_utf8(iter, &err);
    if (ERR_OK != err) {
        lua_pushnil(lua);
        return 1;
    }
    lua_pushlstring(lua, val, iter->lens);
    return 1;
}
/// <summary>
/// 读取当前字段的嵌套文档数据
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="lightuserdata?">文档数据指针；类型不符返回 nil（仅 1 个返回值）</returns>
/// <returns type="integer?">字节数</returns>
static int32_t _lbson_iter_document(lua_State *lua) {
    bson_iter *iter = luaL_checkudata(lua, 1, MT_BSON_ITER);
    int32_t err;
    size_t lens;
    char *data = bson_iter_document(iter, &lens, &err);
    if (ERR_OK != err) {
        lua_pushnil(lua);
        return 1;
    }
    LPUB_RET_LUD(lua, data, (lua_Integer)lens);
}
/// <summary>
/// 读取当前字段的数组数据
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="lightuserdata?">数组数据指针；类型不符返回 nil（仅 1 个返回值）</returns>
/// <returns type="integer?">字节数</returns>
static int32_t _lbson_iter_array(lua_State *lua) {
    bson_iter *iter = luaL_checkudata(lua, 1, MT_BSON_ITER);
    int32_t err;
    size_t lens;
    char *data = bson_iter_array(iter, &lens, &err);
    if (ERR_OK != err) {
        lua_pushnil(lua);
        return 1;
    }
    LPUB_RET_LUD(lua, data, (lua_Integer)lens);
}
/// <summary>
/// 读取当前字段的二进制数据
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="integer?">bson_subtype 枚举值；类型不符返回 nil（仅 1 个返回值）</returns>
/// <returns type="lightuserdata?">数据指针</returns>
/// <returns type="integer?">字节数</returns>
static int32_t _lbson_iter_binary(lua_State *lua) {
    bson_iter *iter = luaL_checkudata(lua, 1, MT_BSON_ITER);
    int32_t err;
    size_t lens;
    bson_subtype subtype;
    char *data = bson_iter_binary(iter, &subtype, &lens, &err);
    if (ERR_OK != err) {
        lua_pushnil(lua);
        return 1;
    }
    lua_pushinteger(lua, subtype);
    lua_pushlightuserdata(lua, data);
    lua_pushinteger(lua, (lua_Integer)lens);
    return 3;
}
/// <summary>
/// 读取当前字段的 ObjectId
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="string?">12 字节 ObjectId；类型不符返回 nil</returns>
static int32_t _lbson_iter_oid(lua_State *lua) {
    bson_iter *iter = luaL_checkudata(lua, 1, MT_BSON_ITER);
    int32_t err;
    char *oid = bson_iter_oid(iter, &err);
    if (ERR_OK != err) {
        lua_pushnil(lua);
        return 1;
    }
    lua_pushlstring(lua, oid, BSON_OID_LENS);
    return 1;
}
/// <summary>
/// 读取当前字段的布尔值
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="boolean?">布尔值；类型不符返回 nil</returns>
static int32_t _lbson_iter_bool(lua_State *lua) {
    bson_iter *iter = luaL_checkudata(lua, 1, MT_BSON_ITER);
    int32_t err;
    int32_t val = bson_iter_bool(iter, &err);
    if (ERR_OK != err) {
        lua_pushnil(lua);
        return 1;
    }
    lua_pushboolean(lua, val);
    return 1;
}
/// <summary>
/// 读取当前字段的 UTC 毫秒时间戳
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="integer?">UTC 毫秒时间戳；类型不符返回 nil</returns>
static int32_t _lbson_iter_date(lua_State *lua) {
    bson_iter *iter = luaL_checkudata(lua, 1, MT_BSON_ITER);
    int32_t err;
    int64_t val = bson_iter_date(iter, &err);
    if (ERR_OK != err) {
        lua_pushnil(lua);
        return 1;
    }
    lua_pushinteger(lua, val);
    return 1;
}
/// <summary>
/// 读取当前字段的正则表达式
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="string?">pattern；类型不符返回 nil（仅 1 个返回值）</returns>
/// <returns type="string?">options（无 options 时为 ""）</returns>
static int32_t _lbson_iter_regex(lua_State *lua) {
    bson_iter *iter = luaL_checkudata(lua, 1, MT_BSON_ITER);
    int32_t err;
    char *options = NULL;
    const char *pattern = bson_iter_regex(iter, &options, &err);
    if (ERR_OK != err) {
        lua_pushnil(lua);
        return 1;
    }
    lua_pushstring(lua, pattern);
    lua_pushstring(lua, NULL != options ? options : "");
    return 2;
}
/// <summary>
/// 读取当前字段的 JavaScript 代码字符串
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="string?">JavaScript 代码；类型不符返回 nil</returns>
static int32_t _lbson_iter_jscode(lua_State *lua) {
    bson_iter *iter = luaL_checkudata(lua, 1, MT_BSON_ITER);
    int32_t err;
    const char *code = bson_iter_jscode(iter, &err);
    if (ERR_OK != err) {
        lua_pushnil(lua);
        return 1;
    }
    lua_pushstring(lua, code);
    return 1;
}
/// <summary>
/// 读取当前字段的 int32 值
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="integer?">int32 值；类型不符返回 nil</returns>
static int32_t _lbson_iter_int32(lua_State *lua) {
    bson_iter *iter = luaL_checkudata(lua, 1, MT_BSON_ITER);
    int32_t err;
    int32_t val = bson_iter_int32(iter, &err);
    if (ERR_OK != err) {
        lua_pushnil(lua);
        return 1;
    }
    lua_pushinteger(lua, val);
    return 1;
}
/// <summary>
/// 读取当前字段的 BSON Timestamp
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="integer?">秒级时间戳；类型不符返回 nil（仅 1 个返回值）</returns>
/// <returns type="integer?">同秒内自增量</returns>
static int32_t _lbson_iter_timestamp(lua_State *lua) {
    bson_iter *iter = luaL_checkudata(lua, 1, MT_BSON_ITER);
    int32_t err;
    uint32_t inc;
    uint32_t ts = bson_iter_timestamp(iter, &inc, &err);
    if (ERR_OK != err) {
        lua_pushnil(lua);
        return 1;
    }
    lua_pushinteger(lua, ts);
    lua_pushinteger(lua, inc);
    return 2;
}
/// <summary>
/// 读取当前字段的 int64 值
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="integer?">int64 值；类型不符返回 nil</returns>
static int32_t _lbson_iter_int64(lua_State *lua) {
    bson_iter *iter = luaL_checkudata(lua, 1, MT_BSON_ITER);
    int32_t err;
    int64_t val = bson_iter_int64(iter, &err);
    if (ERR_OK != err) {
        lua_pushnil(lua);
        return 1;
    }
    lua_pushinteger(lua, val);
    return 1;
}
/// <summary>
/// 判断当前字段是否为 null
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="boolean">字段为 null 返回 true，否则 false</returns>
static int32_t _lbson_iter_isnull(lua_State *lua) {
    bson_iter *iter = luaL_checkudata(lua, 1, MT_BSON_ITER);
    lua_pushboolean(lua, BSON_NULL == iter->type);
    return 1;
}
//bson.iter
LUAMOD_API int luaopen_bson_iter(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lbson_iter_new },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "reset",     _lbson_iter_reset },
        { "next",      _lbson_iter_next },
        { "find",      _lbson_iter_find },
        { "type",      _lbson_iter_type },
        { "key",       _lbson_iter_key },
        { "double",    _lbson_iter_double },
        { "utf8",      _lbson_iter_utf8 },
        { "document",  _lbson_iter_document },
        { "array",     _lbson_iter_array },
        { "binary",    _lbson_iter_binary },
        { "oid",       _lbson_iter_oid },
        { "bool",      _lbson_iter_bool },
        { "date",      _lbson_iter_date },
        { "regex",     _lbson_iter_regex },
        { "jscode",    _lbson_iter_jscode },
        { "int32",     _lbson_iter_int32 },
        { "timestamp", _lbson_iter_timestamp },
        { "int64",     _lbson_iter_int64 },
        { "isnull",    _lbson_iter_isnull },
        { "__gc",      _lbson_iter_gc },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_BSON_ITER, reg_new, reg_func);
    return 1;
}
