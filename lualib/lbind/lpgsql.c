#include "lbind/lpub.h"

#define MT_PGSQL_BIND   "_pgsql_bind_ctx"
#define MT_PGSQL_READER "_pgsql_reader_ctx"
#define MT_PGSQL        "_pgsql_ctx"

/// <summary>
/// 创建 pgsql 参数绑定上下文
/// </summary>
/// <param name="nparam" type="integer">预期绑定参数数量</param>
/// <returns type="_pgsql_bind_ctx">bind 对象</returns>
static int32_t _lpgsql_bind_new(lua_State *lua) {
    uint16_t nparam = (uint16_t)luaL_checkinteger(lua, 1);
    pgsql_bind_ctx *bind = lua_newuserdata(lua, sizeof(pgsql_bind_ctx));
    pgsql_bind_init(bind, nparam);
    ASSOC_MTABLE(lua, MT_PGSQL_BIND);
    return 1;
}
/// <summary>
/// 释放绑定上下文内部缓冲区（绑定为 __gc）
/// </summary>
/// <param name="self" type="userdata">bind 对象</param>
/// <returns>无</returns>
static int32_t _lpgsql_bind_free(lua_State *lua) {
    pgsql_bind_ctx *bind = luaL_checkudata(lua, 1, MT_PGSQL_BIND);
    pgsql_bind_free(bind);
    return 0;
}
/// <summary>
/// 清空已绑定参数，重置缓冲区偏移量以便复用
/// </summary>
/// <param name="self" type="userdata">bind 对象</param>
/// <returns>无</returns>
static int32_t _lpgsql_bind_clear(lua_State *lua) {
    pgsql_bind_ctx *bind = luaL_checkudata(lua, 1, MT_PGSQL_BIND);
    pgsql_bind_clear(bind);
    return 0;
}
/// <summary>
/// 绑定 NULL 参数
/// </summary>
/// <param name="self" type="userdata">bind 对象</param>
/// <returns>无</returns>
static int32_t _lpgsql_bind_null(lua_State *lua) {
    pgsql_bind_ctx *bind = luaL_checkudata(lua, 1, MT_PGSQL_BIND);
    pgsql_bind_null(bind);
    return 0;
}
/// <summary>
/// 绑定 bool 类型参数（二进制格式；非 boolean/number 时绑定 NULL）
/// </summary>
/// <param name="self" type="userdata">bind 对象</param>
/// <param name="val" type="boolean|number|nil">布尔值</param>
/// <returns>无</returns>
static int32_t _lpgsql_bind_bool(lua_State *lua) {
    pgsql_bind_ctx *bind = luaL_checkudata(lua, 1, MT_PGSQL_BIND);
    int32_t type = lua_type(lua, 2);
    if (LUA_TBOOLEAN != type && LUA_TNUMBER != type) {
        pgsql_bind_null(bind);
        return 0;
    }
    pgsql_bind_bool(bind, (int8_t)lua_toboolean(lua, 2));
    return 0;
}
/// <summary>
/// 绑定 int16 参数（大端二进制；非 number 时绑定 NULL）
/// </summary>
/// <param name="self" type="userdata">bind 对象</param>
/// <param name="val" type="integer|nil">int16 值</param>
/// <returns>无</returns>
static int32_t _lpgsql_bind_int16(lua_State *lua) {
    pgsql_bind_ctx *bind = luaL_checkudata(lua, 1, MT_PGSQL_BIND);
    if (LUA_TNUMBER != lua_type(lua, 2)) {
        pgsql_bind_null(bind);
        return 0;
    }
    pgsql_bind_int16(bind, (int16_t)luaL_checkinteger(lua, 2));
    return 0;
}
/// <summary>
/// 绑定 int32 参数（大端二进制；非 number 时绑定 NULL）
/// </summary>
/// <param name="self" type="userdata">bind 对象</param>
/// <param name="val" type="integer|nil">int32 值</param>
/// <returns>无</returns>
static int32_t _lpgsql_bind_int32(lua_State *lua) {
    pgsql_bind_ctx *bind = luaL_checkudata(lua, 1, MT_PGSQL_BIND);
    if (LUA_TNUMBER != lua_type(lua, 2)) {
        pgsql_bind_null(bind);
        return 0;
    }
    pgsql_bind_int32(bind, (int32_t)luaL_checkinteger(lua, 2));
    return 0;
}
/// <summary>
/// 绑定 int64 参数（大端二进制；非 number 时绑定 NULL）
/// </summary>
/// <param name="self" type="userdata">bind 对象</param>
/// <param name="val" type="integer|nil">int64 值</param>
/// <returns>无</returns>
static int32_t _lpgsql_bind_int64(lua_State *lua) {
    pgsql_bind_ctx *bind = luaL_checkudata(lua, 1, MT_PGSQL_BIND);
    if (LUA_TNUMBER != lua_type(lua, 2)) {
        pgsql_bind_null(bind);
        return 0;
    }
    pgsql_bind_int64(bind, luaL_checkinteger(lua, 2));
    return 0;
}
/// <summary>
/// 绑定 float 参数（大端二进制；非 number 时绑定 NULL）
/// </summary>
/// <param name="self" type="userdata">bind 对象</param>
/// <param name="val" type="number|nil">float 值</param>
/// <returns>无</returns>
static int32_t _lpgsql_bind_float(lua_State *lua) {
    pgsql_bind_ctx *bind = luaL_checkudata(lua, 1, MT_PGSQL_BIND);
    if (LUA_TNUMBER != lua_type(lua, 2)) {
        pgsql_bind_null(bind);
        return 0;
    }
    pgsql_bind_float(bind, (float)luaL_checknumber(lua, 2));
    return 0;
}
/// <summary>
/// 绑定 double 参数（大端二进制；非 number 时绑定 NULL）
/// </summary>
/// <param name="self" type="userdata">bind 对象</param>
/// <param name="val" type="number|nil">double 值</param>
/// <returns>无</returns>
static int32_t _lpgsql_bind_double(lua_State *lua) {
    pgsql_bind_ctx *bind = luaL_checkudata(lua, 1, MT_PGSQL_BIND);
    if (LUA_TNUMBER != lua_type(lua, 2)) {
        pgsql_bind_null(bind);
        return 0;
    }
    pgsql_bind_double(bind, luaL_checknumber(lua, 2));
    return 0;
}
/// <summary>
/// 绑定文本参数（TEXT/VARCHAR/BPCHAR；nil 时绑定 NULL）
/// </summary>
/// <param name="self" type="userdata">bind 对象</param>
/// <param name="data" type="string|userdata|lightuserdata|nil">文本数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 userdata/lightuserdata 时必填</param>
/// <returns>无</returns>
static int32_t _lpgsql_bind_text(lua_State *lua) {
    pgsql_bind_ctx *bind = luaL_checkudata(lua, 1, MT_PGSQL_BIND);
    size_t size = 0;
    const char *data = NULL;
    switch (lua_type(lua, 2)) {
    case LUA_TSTRING:
        data = luaL_checklstring(lua, 2, &size);
        break;
    case LUA_TUSERDATA:
    case LUA_TLIGHTUSERDATA:
        data = lua_touserdata(lua, 2);
        size = (size_t)luaL_checkinteger(lua, 3);
        break;
    default:
        break;
    }
    if (NULL == data) {
        pgsql_bind_null(bind);
    } else {
        pgsql_bind_text(bind, data, size);
    }
    return 0;
}
/// <summary>
/// 绑定 BYTEA 参数（二进制原始字节；nil 时绑定 NULL）
/// </summary>
/// <param name="self" type="userdata">bind 对象</param>
/// <param name="data" type="string|userdata|lightuserdata|nil">二进制数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 userdata/lightuserdata 时必填</param>
/// <returns>无</returns>
static int32_t _lpgsql_bind_bytea(lua_State *lua) {
    pgsql_bind_ctx *bind = luaL_checkudata(lua, 1, MT_PGSQL_BIND);
    size_t size = 0;
    const char *data = NULL;
    switch (lua_type(lua, 2)) {
    case LUA_TSTRING:
        data = luaL_checklstring(lua, 2, &size);
        break;
    case LUA_TUSERDATA:
    case LUA_TLIGHTUSERDATA:
        data = lua_touserdata(lua, 2);
        size = (size_t)luaL_checkinteger(lua, 3);
        break;
    default:
        break;
    }
    if (NULL == data) {
        pgsql_bind_null(bind);
    } else {
        pgsql_bind_bytea(bind, data, size);
    }
    return 0;
}
/// <summary>
/// 绑定 TIMESTAMP 参数（非 number 时绑定 NULL）
/// </summary>
/// <param name="self" type="userdata">bind 对象</param>
/// <param name="usec" type="integer|nil">相对 PG 纪元（2000-01-01）的微秒数</param>
/// <returns>无</returns>
static int32_t _lpgsql_bind_timestamp(lua_State *lua) {
    pgsql_bind_ctx *bind = luaL_checkudata(lua, 1, MT_PGSQL_BIND);
    if (LUA_TNUMBER != lua_type(lua, 2)) {
        pgsql_bind_null(bind);
        return 0;
    }
    pgsql_bind_timestamp(bind, luaL_checkinteger(lua, 2));
    return 0;
}
/// <summary>
/// 绑定 TIMESTAMPTZ 参数（非 number 时绑定 NULL）
/// </summary>
/// <param name="self" type="userdata">bind 对象</param>
/// <param name="usec" type="integer|nil">相对 PG UTC 纪元的微秒数</param>
/// <returns>无</returns>
static int32_t _lpgsql_bind_timestamptz(lua_State *lua) {
    pgsql_bind_ctx *bind = luaL_checkudata(lua, 1, MT_PGSQL_BIND);
    if (LUA_TNUMBER != lua_type(lua, 2)) {
        pgsql_bind_null(bind);
        return 0;
    }
    pgsql_bind_timestamptz(bind, luaL_checkinteger(lua, 2));
    return 0;
}
/// <summary>
/// 绑定 DATE 参数（非 number 时绑定 NULL）
/// </summary>
/// <param name="self" type="userdata">bind 对象</param>
/// <param name="days" type="integer|nil">相对 PG 纪元（2000-01-01）的天数</param>
/// <returns>无</returns>
static int32_t _lpgsql_bind_date(lua_State *lua) {
    pgsql_bind_ctx *bind = luaL_checkudata(lua, 1, MT_PGSQL_BIND);
    if (LUA_TNUMBER != lua_type(lua, 2)) {
        pgsql_bind_null(bind);
        return 0;
    }
    pgsql_bind_date(bind, (int32_t)luaL_checkinteger(lua, 2));
    return 0;
}
/// <summary>
/// 绑定 UUID 参数（接受长度恰好为 16 字节的二进制字符串；其他绑定 NULL）
/// </summary>
/// <param name="self" type="userdata">bind 对象</param>
/// <param name="data" type="string|nil">16 字节 UUID 二进制串</param>
/// <returns>无</returns>
static int32_t _lpgsql_bind_uuid(lua_State *lua) {
    pgsql_bind_ctx *bind = luaL_checkudata(lua, 1, MT_PGSQL_BIND);
    size_t size = 0;
    const char *data = NULL;
    if (LUA_TSTRING == lua_type(lua, 2)) {
        data = luaL_checklstring(lua, 2, &size);
        if (16 != size) {
            data = NULL;
        }
    }
    if (NULL == data) {
        pgsql_bind_null(bind);
    } else {
        pgsql_bind_uuid(bind, data);
    }
    return 0;
}
//pgsql.bind
LUAMOD_API int luaopen_pgsql_bind(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lpgsql_bind_new },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "clear",       _lpgsql_bind_clear },
        { "null",        _lpgsql_bind_null },
        { "bool",        _lpgsql_bind_bool },
        { "int16",       _lpgsql_bind_int16 },
        { "int32",       _lpgsql_bind_int32 },
        { "int64",       _lpgsql_bind_int64 },
        { "float",       _lpgsql_bind_float },
        { "double",      _lpgsql_bind_double },
        { "text",        _lpgsql_bind_text },
        { "bytea",       _lpgsql_bind_bytea },
        { "timestamp",   _lpgsql_bind_timestamp },
        { "timestamptz", _lpgsql_bind_timestamptz },
        { "date",        _lpgsql_bind_date },
        { "uuid",        _lpgsql_bind_uuid },
        { "__gc",        _lpgsql_bind_free },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_PGSQL_BIND, reg_new, reg_func);
    return 1;
}
/// <summary>
/// 从 pgpack_ctx 数据包中创建结果集读取器
/// </summary>
/// <param name="pgpack" type="lightuserdata">pgpack_ctx 指针</param>
/// <param name="format" type="integer">期望格式（0 = 文本，1 = 二进制）</param>
/// <returns type="_pgsql_reader_ctx?">reader 对象；失败返回 nil</returns>
static int32_t _lpgsql_reader_new(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    pgpack_ctx *pgpack = lua_touserdata(lua, 1);
    pgpack_format format = (pgpack_format)luaL_checkinteger(lua, 2);
    pgsql_reader_ctx *reader = pgsql_reader_init(pgpack, format);
    if (NULL == reader) {
        lua_pushnil(lua);
        return 1;
    }
    pgsql_reader_ctx **rd = lua_newuserdata(lua, sizeof(pgsql_reader_ctx *));
    *rd = reader;
    ASSOC_MTABLE(lua, MT_PGSQL_READER);
    return 1;
}
/// <summary>
/// 释放结果集读取器及其持有的所有行数据（绑定为 __gc）
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <returns>无</returns>
static int32_t _lpgsql_reader_free(lua_State *lua) {
    pgsql_reader_ctx **reader = luaL_checkudata(lua, 1, MT_PGSQL_READER);
    if (NULL != *reader) {
        pgsql_reader_free(*reader);
        *reader = NULL;
    }
    return 0;
}
/// <summary>
/// 返回结果集总行数
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <returns type="integer">行数</returns>
static int32_t _lpgsql_reader_size(lua_State *lua) {
    pgsql_reader_ctx **reader = luaL_checkudata(lua, 1, MT_PGSQL_READER);
    lua_pushinteger(lua, pgsql_reader_size(*reader));
    return 1;
}
/// <summary>
/// 将游标跳转到指定行位置（从 0 开始）
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <param name="pos" type="integer">目标行下标</param>
/// <returns>无</returns>
static int32_t _lpgsql_reader_seek(lua_State *lua) {
    pgsql_reader_ctx **reader = luaL_checkudata(lua, 1, MT_PGSQL_READER);
    pgsql_reader_seek(*reader, (size_t)luaL_checkinteger(lua, 2));
    return 0;
}
/// <summary>
/// 判断游标是否已到达结果集末尾
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <returns type="boolean">已到末尾 true，否则 false</returns>
static int32_t _lpgsql_reader_eof(lua_State *lua) {
    pgsql_reader_ctx **reader = luaL_checkudata(lua, 1, MT_PGSQL_READER);
    lua_pushboolean(lua, pgsql_reader_eof(*reader));
    return 1;
}
/// <summary>
/// 将游标移动到下一行
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <returns>无</returns>
static int32_t _lpgsql_reader_next(lua_State *lua) {
    pgsql_reader_ctx **reader = luaL_checkudata(lua, 1, MT_PGSQL_READER);
    pgsql_reader_next(*reader);
    return 0;
}
/// <summary>
/// 读取当前行指定字段的 bool 值
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <param name="name" type="string">字段名</param>
/// <returns type="boolean">true 表示读取成功（含字段为 NULL）；false 表示读取失败</returns>
/// <returns type="boolean?">字段布尔值；字段为 NULL 时不返回</returns>
static int32_t _lpgsql_reader_bool(lua_State *lua) {
    pgsql_reader_ctx **reader = luaL_checkudata(lua, 1, MT_PGSQL_READER);
    const char *name = luaL_checkstring(lua, 2);
    int32_t err;
    int32_t val = pgsql_reader_bool(*reader, name, &err);
    if (ERR_OK == err) {
        lua_pushboolean(lua, 1);
        lua_pushboolean(lua, val);
        return 2;
    }
    if (1 == err) {
        lua_pushboolean(lua, 1);
        return 1;
    }
    lua_pushboolean(lua, 0);
    return 1;
}
/// <summary>
/// 读取当前行指定字段的整数值（支持 int2/int4/int8）
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <param name="name" type="string">字段名</param>
/// <returns type="boolean">true 表示读取成功（含字段为 NULL）；false 表示读取失败</returns>
/// <returns type="integer?">字段整数值；字段为 NULL 时不返回</returns>
static int32_t _lpgsql_reader_integer(lua_State *lua) {
    pgsql_reader_ctx **reader = luaL_checkudata(lua, 1, MT_PGSQL_READER);
    const char *name = luaL_checkstring(lua, 2);
    int32_t err;
    int64_t val = pgsql_reader_integer(*reader, name, &err);
    if (ERR_OK == err) {
        lua_pushboolean(lua, 1);
        lua_pushinteger(lua, val);
        return 2;
    }
    if (1 == err) {
        lua_pushboolean(lua, 1);
        return 1;
    }
    lua_pushboolean(lua, 0);
    return 1;
}
/// <summary>
/// 读取当前行指定字段的浮点值（支持 float4/float8）
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <param name="name" type="string">字段名</param>
/// <returns type="boolean">true 表示读取成功（含字段为 NULL）；false 表示读取失败</returns>
/// <returns type="number?">字段浮点值；字段为 NULL 时不返回</returns>
static int32_t _lpgsql_reader_double(lua_State *lua) {
    pgsql_reader_ctx **reader = luaL_checkudata(lua, 1, MT_PGSQL_READER);
    const char *name = luaL_checkstring(lua, 2);
    int32_t err;
    double val = pgsql_reader_double(*reader, name, &err);
    if (ERR_OK == err) {
        lua_pushboolean(lua, 1);
        lua_pushnumber(lua, val);
        return 2;
    }
    if (1 == err) {
        lua_pushboolean(lua, 1);
        return 1;
    }
    lua_pushboolean(lua, 0);
    return 1;
}
/// <summary>
/// 读取当前行指定字段的文本值
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <param name="name" type="string">字段名</param>
/// <returns type="boolean">true 表示读取成功（含字段为 NULL）；false 表示读取失败</returns>
/// <returns type="lightuserdata?">数据指针；字段为 NULL 时不返回</returns>
/// <returns type="integer?">字节数；字段为 NULL 时不返回</returns>
static int32_t _lpgsql_reader_text(lua_State *lua) {
    pgsql_reader_ctx **reader = luaL_checkudata(lua, 1, MT_PGSQL_READER);
    const char *name = luaL_checkstring(lua, 2);
    int32_t err;
    int32_t lens = 0;
    const char *val = pgsql_reader_text(*reader, name, &lens, &err);
    if (ERR_OK == err) {
        lua_pushboolean(lua, 1);
        lua_pushlightuserdata(lua, (void *)val);
        lua_pushinteger(lua, lens);
        return 3;
    }
    if (1 == err) {
        lua_pushboolean(lua, 1);
        return 1;
    }
    lua_pushboolean(lua, 0);
    return 1;
}
/// <summary>
/// 读取当前行指定字段的 BYTEA 值
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <param name="name" type="string">字段名</param>
/// <returns type="boolean">true 表示读取成功（含字段为 NULL）；false 表示读取失败</returns>
/// <returns type="lightuserdata?">数据指针；字段为 NULL 时不返回</returns>
/// <returns type="integer?">字节数；字段为 NULL 时不返回</returns>
static int32_t _lpgsql_reader_bytea(lua_State *lua) {
    pgsql_reader_ctx **reader = luaL_checkudata(lua, 1, MT_PGSQL_READER);
    const char *name = luaL_checkstring(lua, 2);
    int32_t err;
    int32_t lens = 0;
    const char *val = pgsql_reader_bytea(*reader, name, &lens, &err);
    if (ERR_OK == err) {
        lua_pushboolean(lua, 1);
        lua_pushlightuserdata(lua, (void *)val);
        lua_pushinteger(lua, lens);
        return 3;
    }
    if (1 == err) {
        lua_pushboolean(lua, 1);
        return 1;
    }
    lua_pushboolean(lua, 0);
    return 1;
}
/// <summary>
/// 读取当前行指定字段的 TIMESTAMP / TIMESTAMPTZ 值（相对 PG 纪元的微秒数）
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <param name="name" type="string">字段名</param>
/// <returns type="boolean">true 表示读取成功（含字段为 NULL）；false 表示读取失败</returns>
/// <returns type="integer?">微秒数；字段为 NULL 时不返回</returns>
static int32_t _lpgsql_reader_timestamp(lua_State *lua) {
    pgsql_reader_ctx **reader = luaL_checkudata(lua, 1, MT_PGSQL_READER);
    const char *name = luaL_checkstring(lua, 2);
    int32_t err;
    int64_t val = pgsql_reader_timestamp(*reader, name, &err);
    if (ERR_OK == err) {
        lua_pushboolean(lua, 1);
        lua_pushinteger(lua, val);
        return 2;
    }
    if (1 == err) {
        lua_pushboolean(lua, 1);
        return 1;
    }
    lua_pushboolean(lua, 0);
    return 1;
}
/// <summary>
/// 读取当前行指定字段的 DATE 值（相对 PG 纪元的天数）
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <param name="name" type="string">字段名</param>
/// <returns type="boolean">true 表示读取成功（含字段为 NULL）；false 表示读取失败</returns>
/// <returns type="integer?">天数；字段为 NULL 时不返回</returns>
static int32_t _lpgsql_reader_date(lua_State *lua) {
    pgsql_reader_ctx **reader = luaL_checkudata(lua, 1, MT_PGSQL_READER);
    const char *name = luaL_checkstring(lua, 2);
    int32_t err;
    int32_t val = pgsql_reader_date(*reader, name, &err);
    if (ERR_OK == err) {
        lua_pushboolean(lua, 1);
        lua_pushinteger(lua, val);
        return 2;
    }
    if (1 == err) {
        lua_pushboolean(lua, 1);
        return 1;
    }
    lua_pushboolean(lua, 0);
    return 1;
}
/// <summary>
/// 读取当前行指定字段的 UUID 值（16 字节），以 Lua 字符串返回
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <param name="name" type="string">字段名</param>
/// <returns type="boolean">true 表示读取成功（含字段为 NULL）；false 表示读取失败</returns>
/// <returns type="string?">16 字节 UUID 串；字段为 NULL 时不返回</returns>
static int32_t _lpgsql_reader_uuid(lua_State *lua) {
    pgsql_reader_ctx **reader = luaL_checkudata(lua, 1, MT_PGSQL_READER);
    const char *name = luaL_checkstring(lua, 2);
    int32_t err;
    char uuid[16];
    int32_t rtn = pgsql_reader_uuid(*reader, name, uuid, &err);
    if (1 == err) {
        lua_pushboolean(lua, 1);
        return 1;
    }
    if (ERR_OK == rtn && ERR_OK == err) {
        lua_pushboolean(lua, 1);
        lua_pushlstring(lua, uuid, 16);
        return 2;
    }
    lua_pushboolean(lua, 0);
    return 1;
}
/// <summary>
/// 判断当前行指定字段是否为 NULL
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <param name="name" type="string">字段名</param>
/// <returns type="boolean">NULL 返回 true，否则 false</returns>
static int32_t _lpgsql_reader_isnull(lua_State *lua) {
    pgsql_reader_ctx **reader = luaL_checkudata(lua, 1, MT_PGSQL_READER);
    const char *name = luaL_checkstring(lua, 2);
    lua_pushboolean(lua, pgsql_reader_isnull(*reader, name));
    return 1;
}
//pgsql.reader
LUAMOD_API int luaopen_pgsql_reader(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lpgsql_reader_new },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "size",      _lpgsql_reader_size },
        { "seek",      _lpgsql_reader_seek },
        { "eof",       _lpgsql_reader_eof },
        { "next",      _lpgsql_reader_next },
        { "bool",      _lpgsql_reader_bool },
        { "integer",   _lpgsql_reader_integer },
        { "double",    _lpgsql_reader_double },
        { "text",      _lpgsql_reader_text },
        { "bytea",     _lpgsql_reader_bytea },
        { "timestamp", _lpgsql_reader_timestamp },
        { "date",      _lpgsql_reader_date },
        { "uuid",      _lpgsql_reader_uuid },
        { "isnull",    _lpgsql_reader_isnull },
        { "__gc",      _lpgsql_reader_free },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_PGSQL_READER, reg_new, reg_func);
    return 1;
}
/// <summary>
/// 返回 pgpack_ctx 数据包类型（PGPACK_OK / ERR / NOTIFICATION / COPY_IN / COPY_OUT 等）
/// </summary>
/// <param name="pgpack" type="lightuserdata">pgpack_ctx 指针</param>
/// <returns type="integer">数据包类型枚举值</returns>
static int32_t _lpgsql_pack_type(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    pgpack_ctx *pgpack = lua_touserdata(lua, 1);
    lua_pushinteger(lua, pgpack->type);
    return 1;
}
/// <summary>
/// 从 pgpack_ctx 中解析受影响的行数（依赖 CommandComplete 标签）
/// </summary>
/// <param name="pgpack" type="lightuserdata">pgpack_ctx 指针</param>
/// <returns type="integer">受影响行数</returns>
static int32_t _lpgsql_affected_rows(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    pgpack_ctx *pgpack = lua_touserdata(lua, 1);
    lua_pushinteger(lua, pgsql_affected_rows(pgpack));
    return 1;
}
/// <summary>
/// 返回 pgpack_ctx 中的 CommandComplete 命令完成标签字符串
/// </summary>
/// <param name="pgpack" type="lightuserdata">pgpack_ctx 指针</param>
/// <returns type="string">CommandComplete 标签（如 "INSERT 0 1"）</returns>
static int32_t _lpgsql_complete(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    pgpack_ctx *pgpack = lua_touserdata(lua, 1);
    lua_pushstring(lua, pgpack->complete);
    return 1;
}
/// <summary>
/// 从 PGPACK_ERR 类型的 pgpack_ctx 中提取错误描述字符串
/// </summary>
/// <param name="pgpack" type="lightuserdata">pgpack_ctx 指针</param>
/// <returns type="string?">错误描述；类型不符时返回 nil</returns>
static int32_t _lpgsql_erro(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    pgpack_ctx *pgpack = lua_touserdata(lua, 1);
    if (NULL == pgpack || PGPACK_ERR != pgpack->type) {
        lua_pushnil(lua);
        return 1;
    }
    lua_pushstring(lua, (const char *)pgpack->pack);
    return 1;
}
/// <summary>
/// 从 PGPACK_NOTIFICATION 类型的 pgpack_ctx 中提取通知信息
/// </summary>
/// <param name="pgpack" type="lightuserdata">pgpack_ctx 指针</param>
/// <returns type="integer?">发送方 pid；类型不符时返回 nil（仅 1 个返回值）</returns>
/// <returns type="string?">channel 名</returns>
/// <returns type="string?">通知内容</returns>
static int32_t _lpgsql_notification(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    pgpack_ctx *pgpack = lua_touserdata(lua, 1);
    if (NULL == pgpack || PGPACK_NOTIFICATION != pgpack->type) {
        lua_pushnil(lua);
        return 1;
    }
    pgpack_notification *notif = (pgpack_notification *)pgpack->pack;
    lua_pushinteger(lua, notif->pid);
    lua_pushstring(lua, notif->channel);
    lua_pushstring(lua, notif->notification);
    return 3;
}
/// <summary>
/// 从 PGPACK_COPY_IN 类型的 pgpack_ctx 中提取 format 和列数
/// </summary>
/// <param name="pgpack" type="lightuserdata">pgpack_ctx 指针</param>
/// <returns type="integer?">format（0 文本，1 二进制）；类型不符时返回 nil（仅 1 个返回值）</returns>
/// <returns type="integer?">列数</returns>
static int32_t _lpgsql_copy_in_info(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    pgpack_ctx *pgpack = lua_touserdata(lua, 1);
    if (NULL == pgpack || PGPACK_COPY_IN != pgpack->type) {
        lua_pushnil(lua);
        return 1;
    }
    pgpack_copy_in_ctx *ci = (pgpack_copy_in_ctx *)pgpack->pack;
    lua_pushinteger(lua, ci->format);
    lua_pushinteger(lua, ci->ncol);
    return 2;
}
/// <summary>
/// 从 PGPACK_COPY_OUT 类型的 pgpack_ctx 中提取累积数据指针和长度
/// </summary>
/// <param name="pgpack" type="lightuserdata">pgpack_ctx 指针</param>
/// <returns type="lightuserdata?">数据指针；类型不符时返回 nil（仅 1 个返回值）</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lpgsql_copy_out_data(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    pgpack_ctx *pgpack = lua_touserdata(lua, 1);
    if (NULL == pgpack || PGPACK_COPY_OUT != pgpack->type) {
        lua_pushnil(lua);
        return 1;
    }
    pgpack_copy_out_ctx *co = (pgpack_copy_out_ctx *)pgpack->pack;
    LPUB_RET_LUD(lua, co->data.data, co->data.offset);
}
/// <summary>
/// 打包简单查询消息（Query）
/// </summary>
/// <param name="sql" type="string">SQL 语句</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lpgsql_pack_query(lua_State *lua) {
    const char *sql = luaL_checkstring(lua, 1);
    size_t size;
    void *pack = pgsql_pack_query(sql, &size);
    LPUB_RET_LUD(lua, pack, size);
}
/// <summary>
/// 打包 Terminate 消息（通知服务端关闭连接）
/// </summary>
/// <param>无</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lpgsql_pack_terminate(lua_State *lua) {
    (void)lua;
    size_t size;
    void *pack = pgsql_pack_terminate(&size);
    LPUB_RET_LUD(lua, pack, size);
}
/// <summary>
/// 打包预处理语句 Parse + Sync 消息
/// </summary>
/// <param name="name" type="string">语句名（""= 未命名）</param>
/// <param name="sql" type="string">SQL 语句</param>
/// <param name="nparam" type="integer">参数数量</param>
/// <param name="oids" type="integer[]?">OID 整数数组（按参数顺序）；nil 表示由服务端推断</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lpgsql_pack_stmt_prepare(lua_State *lua) {
    const char *name = luaL_checkstring(lua, 1);
    const char *sql = luaL_checkstring(lua, 2);
    int16_t nparam = (int16_t)luaL_checkinteger(lua, 3);
    uint32_t *oids = NULL;
    int16_t i;
    if (LUA_TTABLE == lua_type(lua, 4) && nparam > 0) {
        MALLOC(oids, sizeof(uint32_t) * nparam);
        for (i = 0; i < nparam; i++) {
            lua_rawgeti(lua, 4, i + 1);
            oids[i] = (uint32_t)lua_tointeger(lua, -1);
            lua_pop(lua, 1);
        }
    }
    size_t size;
    void *pack = pgsql_pack_stmt_prepare(name, sql, nparam, oids, &size);
    FREE(oids);
    LPUB_RET_LUD(lua, pack, size);
}
/// <summary>
/// 打包预处理语句 Bind + Describe + Execute + Sync 消息
/// </summary>
/// <param name="name" type="string">语句名</param>
/// <param name="bind" type="userdata?">参数绑定上下文；nil 表示无参</param>
/// <param name="fmt" type="integer?">结果列格式（0 文本，1 二进制），默认 0</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lpgsql_pack_stmt_execute(lua_State *lua) {
    const char *name = luaL_checkstring(lua, 1);
    pgsql_bind_ctx *bind = NULL;
    if (LUA_TUSERDATA == lua_type(lua, 2)) {
        bind = luaL_checkudata(lua, 2, MT_PGSQL_BIND);
    }
    pgpack_format fmt = FORMAT_TEXT;
    if (LUA_TNUMBER == lua_type(lua, 3)) {
        fmt = (pgpack_format)luaL_checkinteger(lua, 3);
    }
    size_t size;
    void *pack = pgsql_pack_stmt_execute(name, bind, fmt, &size);
    LPUB_RET_LUD(lua, pack, size);
}
/// <summary>
/// 打包预处理语句 Close + Sync 消息
/// </summary>
/// <param name="name" type="string">语句名</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lpgsql_pack_stmt_close(lua_State *lua) {
    const char *name = luaL_checkstring(lua, 1);
    size_t size;
    void *pack = pgsql_pack_stmt_close(name, &size);
    LPUB_RET_LUD(lua, pack, size);
}
/// <summary>
/// 打包 CopyData 消息
/// </summary>
/// <param name="data" type="string|userdata|lightuserdata">数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 userdata/lightuserdata 时必填</param>
/// <returns type="lightuserdata?">命令数据指针；data 为 nil 时返回 nil</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lpgsql_pack_copy_data(lua_State *lua) {
    size_t lens = 0;
    const void *data = NULL;
    switch (lua_type(lua, 1)) {
    case LUA_TSTRING:
        data = luaL_checklstring(lua, 1, &lens);
        break;
    case LUA_TUSERDATA:
    case LUA_TLIGHTUSERDATA:
        data = lua_touserdata(lua, 1);
        lens = (size_t)luaL_checkinteger(lua, 2);
        break;
    default:
        break;
    }
    if (NULL == data) {
        lua_pushnil(lua);
        lua_pushinteger(lua, 0);
        return 2;
    }
    size_t size;
    void *pack = pgsql_pack_copy_data(data, lens, &size);
    LPUB_RET_LUD(lua, pack, size);
}
/// <summary>
/// 打包 CopyDone 消息（通知服务端 COPY FROM STDIN 发送完毕）
/// </summary>
/// <param>无</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lpgsql_pack_copy_done(lua_State *lua) {
    (void)lua;
    size_t size;
    void *pack = pgsql_pack_copy_done(&size);
    LPUB_RET_LUD(lua, pack, size);
}
/// <summary>
/// 打包 CopyFail 消息（中止 COPY FROM STDIN）
/// </summary>
/// <param name="msg" type="string">错误原因描述</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lpgsql_pack_copy_fail(lua_State *lua) {
    const char *msg = luaL_checkstring(lua, 1);
    size_t size;
    void *pack = pgsql_pack_copy_fail(msg, &size);
    LPUB_RET_LUD(lua, pack, size);
}
/// <summary>
/// 创建 pgsql 客户端连接上下文（不立即建立连接）
/// </summary>
/// <param name="ip" type="string">服务器 IP</param>
/// <param name="port" type="integer">服务器端口</param>
/// <param name="evssl" type="lightuserdata|nil">SSL 上下文；nil 表示明文</param>
/// <param name="user" type="string">用户名</param>
/// <param name="password" type="string">密码</param>
/// <param name="database" type="string">数据库名</param>
/// <returns type="_pgsql_ctx?">pgsql 对象；初始化失败返回 nil</returns>
static int32_t _lpgsql_new(lua_State *lua) {
    const char *ip = luaL_checkstring(lua, 1);
    uint16_t port = (uint16_t)luaL_checkinteger(lua, 2);
    struct evssl_ctx *evssl = NULL;
    if (LUA_TNIL != lua_type(lua, 3)) {
        LUACHECK_LUDATA(lua, 3);
        evssl = lua_touserdata(lua, 3);
    }
    const char *user = luaL_checkstring(lua, 4);
    const char *password = luaL_checkstring(lua, 5);
    const char *database = luaL_checkstring(lua, 6);
    pgsql_ctx *pg = lua_newuserdata(lua, sizeof(pgsql_ctx));
    if (ERR_OK != pgsql_init(pg, ip, port, evssl, user, password, database)) {
        lua_pop(lua, 1);
        lua_pushnil(lua);
    } else {
        ASSOC_MTABLE(lua, MT_PGSQL);
    }
    return 1;
}
/// <summary>
/// 发送 Terminate 消息并断开连接，清理套接字绑定（绑定为 __gc）
/// </summary>
/// <param name="self" type="userdata">pgsql 对象</param>
/// <returns>无</returns>
static int32_t _lpgsql_free(lua_State *lua) {
    pgsql_ctx *pg = luaL_checkudata(lua, 1, MT_PGSQL);
    if (NULL != pg->task && INVALID_SOCK != pg->fd) {
        size_t size;
        void *pack = pgsql_pack_terminate(&size);
        ev_ud_context(&pg->task->loader->netev, pg->fd, pg->skid, NULL);
        ev_send(&pg->task->loader->netev, pg->fd, pg->skid, pack, size, 0);
    }
    secure_zero(pg->password, sizeof(pg->password));
    return 0;
}
/// <summary>
/// 发起到 pgsql 服务端的异步连接请求
/// </summary>
/// <param name="self" type="userdata">pgsql 对象</param>
/// <returns type="boolean">发起成功 true，失败 false</returns>
static int32_t _lpgsql_try_connect(lua_State *lua) {
    pgsql_ctx *pg = luaL_checkudata(lua, 1, MT_PGSQL);
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (ERR_OK == pgsql_try_connect(task, pg)) {
        lua_pushboolean(lua, 1);
    } else {
        lua_pushboolean(lua, 0);
    }
    return 1;
}
/// <summary>
/// 更新连接的用户名和密码（下次重连时生效）
/// </summary>
/// <param name="self" type="userdata">pgsql 对象</param>
/// <param name="user" type="string">新用户名</param>
/// <param name="password" type="string">新密码</param>
/// <returns>无</returns>
static int32_t _lpgsql_set_userpwd(lua_State *lua) {
    pgsql_ctx *pg = luaL_checkudata(lua, 1, MT_PGSQL);
    const char *user = luaL_checkstring(lua, 2);
    const char *password = luaL_checkstring(lua, 3);
    pgsql_set_userpwd(pg, user, password);
    return 0;
}
/// <summary>
/// 更新目标数据库名（下次重连时生效）
/// </summary>
/// <param name="self" type="userdata">pgsql 对象</param>
/// <param name="database" type="string">新数据库名</param>
/// <returns>无</returns>
static int32_t _lpgsql_set_db(lua_State *lua) {
    pgsql_ctx *pg = luaL_checkudata(lua, 1, MT_PGSQL);
    const char *database = luaL_checkstring(lua, 2);
    pgsql_set_db(pg, database);
    return 0;
}
/// <summary>
/// 获取当前配置的数据库名
/// </summary>
/// <param name="self" type="userdata">pgsql 对象</param>
/// <returns type="string">数据库名</returns>
static int32_t _lpgsql_get_db(lua_State *lua) {
    pgsql_ctx *pg = luaL_checkudata(lua, 1, MT_PGSQL);
    lua_pushstring(lua, pgsql_get_db(pg));
    return 1;
}
/// <summary>
/// 返回当前 pgsql 连接的 fd 和 skid
/// </summary>
/// <param name="self" type="userdata">pgsql 对象</param>
/// <returns type="integer">socket fd</returns>
/// <returns type="integer">skid</returns>
static int32_t _lpgsql_sock_id(lua_State *lua) {
    pgsql_ctx *pg = luaL_checkudata(lua, 1, MT_PGSQL);
    lua_pushinteger(lua, pg->fd);
    lua_pushinteger(lua, pg->skid);
    return 2;
}
/// <summary>
/// 构造 CancelRequest 消息（使用连接的 pid 和 key）
/// </summary>
/// <param name="self" type="userdata">pgsql 对象</param>
/// <returns type="string">16 字节 CancelRequest 二进制串</returns>
static int32_t _lpgsql_pack_cancel(lua_State *lua) {
    pgsql_ctx *pg = luaL_checkudata(lua, 1, MT_PGSQL);
    char buf[16];
    pgsql_pack_cancel(buf, pg->pid, pg->key);
    lua_pushlstring(lua, buf, 16);
    return 1;
}
/// <summary>
/// 返回服务端就绪状态字符
/// </summary>
/// <param name="self" type="userdata">pgsql 对象</param>
/// <returns type="integer">字符码（'I'=空闲，'T'=事务中，'E'=事务失败）</returns>
static int32_t _lpgsql_readyforquery(lua_State *lua) {
    pgsql_ctx *pg = luaL_checkudata(lua, 1, MT_PGSQL);
    lua_pushinteger(lua, pg->readyforquery);
    return 1;
}
//pgsql
LUAMOD_API int luaopen_pgsql(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new",            _lpgsql_new },
        { "pack_type",      _lpgsql_pack_type },
        { "affected_rows",  _lpgsql_affected_rows },
        { "complete",       _lpgsql_complete },
        { "erro",           _lpgsql_erro },
        { "notification",   _lpgsql_notification },
        { "copy_in_info",   _lpgsql_copy_in_info },
        { "copy_out_data",  _lpgsql_copy_out_data },
        { "pack_query",     _lpgsql_pack_query },
        { "pack_terminate", _lpgsql_pack_terminate },
        { "pack_stmt_prepare", _lpgsql_pack_stmt_prepare },
        { "pack_stmt_execute", _lpgsql_pack_stmt_execute },
        { "pack_stmt_close",   _lpgsql_pack_stmt_close },
        { "pack_copy_data",    _lpgsql_pack_copy_data },
        { "pack_copy_done",    _lpgsql_pack_copy_done },
        { "pack_copy_fail",    _lpgsql_pack_copy_fail },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "try_connect",    _lpgsql_try_connect },
        { "set_userpwd",    _lpgsql_set_userpwd },
        { "set_db",         _lpgsql_set_db },
        { "get_db",         _lpgsql_get_db },
        { "sock_id",        _lpgsql_sock_id },
        { "pack_cancel",    _lpgsql_pack_cancel },
        { "readyforquery",  _lpgsql_readyforquery },
        { "__gc",           _lpgsql_free },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_PGSQL, reg_new, reg_func);
    return 1;
}
