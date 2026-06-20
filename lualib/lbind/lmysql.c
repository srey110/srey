#include "lbind/lpub.h"

#define MT_MYSQL_BIND   "_mysql_bind_ctx"
#define MT_MYSQL_READER "_mysql_reader_ctx"
#define MT_MYSQL_STMT   "_mysql_stmt_ctx"
#define MT_MYSQL        "_mysql_ctx"

/// <summary>
/// 创建 MySQL 参数绑定上下文（用于预处理语句或查询参数化）
/// </summary>
/// <param>无</param>
/// <returns type="_mysql_bind_ctx">bind 对象</returns>
static int32_t _lmysql_bind_new(lua_State *lua) {
    mysql_bind_ctx *mbind = lua_newuserdata(lua, sizeof(mysql_bind_ctx));
    mysql_bind_init(mbind);
    ASSOC_MTABLE(lua, MT_MYSQL_BIND);
    return 1;
}
/// <summary>
/// 释放绑定上下文内部资源（绑定为 __gc，由 Lua GC 自动调用）
/// </summary>
/// <param name="self" type="userdata">bind 对象</param>
/// <returns>无</returns>
static int32_t _lmysql_bind_free(lua_State *lua) {
    mysql_bind_ctx *mbind = luaL_checkudata(lua, 1, MT_MYSQL_BIND);
    mysql_bind_free(mbind);
    return 0;
}
/// <summary>
/// 清空所有已绑定参数，可复用上下文
/// </summary>
/// <param name="self" type="userdata">bind 对象</param>
/// <returns>无</returns>
static int32_t _lmysql_bind_clear(lua_State *lua) {
    mysql_bind_ctx *mbind = luaL_checkudata(lua, 1, MT_MYSQL_BIND);
    mysql_bind_clear(mbind);
    return 0;
}
/// <summary>
/// 绑定一个 NULL 参数（注册为 bind:null）
/// </summary>
/// <param name="self" type="userdata">bind 对象</param>
/// <param name="name" type="string?">具名参数名；nil 表示按位置绑定</param>
/// <returns>无</returns>
static int32_t _lmysql_bind_nil(lua_State *lua) {
    mysql_bind_ctx *mbind = luaL_checkudata(lua, 1, MT_MYSQL_BIND);
    char *name = NULL;
    if (LUA_TSTRING == lua_type(lua, 2)) {
        name = (char *)luaL_checkstring(lua, 2);
    }
    mysql_bind_nil(mbind, name);
    return 0;
}
/// <summary>
/// 绑定字符串参数（data 为 nil 时自动转为 NULL 绑定）
/// </summary>
/// <param name="self" type="userdata">bind 对象</param>
/// <param name="name" type="string?">具名参数名；nil 表示按位置绑定</param>
/// <param name="data" type="string|userdata|nil">字符串值；nil 视为 NULL</param>
/// <param name="size" type="integer?">data 为 userdata 时必填，表示数据字节数</param>
/// <returns>无</returns>
static int32_t _lmysql_bind_string(lua_State *lua) {
    mysql_bind_ctx *mbind = luaL_checkudata(lua, 1, MT_MYSQL_BIND);
    char *name = NULL;
    if (LUA_TSTRING == lua_type(lua, 2)) {
        name = (char *)luaL_checkstring(lua, 2);
    }
    size_t size;
    char *data = NULL;
    switch (lua_type(lua, 3)) {
    case LUA_TSTRING:
        data = (char *)luaL_checklstring(lua, 3, &size);
        break;
    case LUA_TUSERDATA:
        data = lua_touserdata(lua, 3);
        size = (size_t)luaL_checkinteger(lua, 4);
        break;
    default:
        break;
    }
    if (NULL == data) {
        mysql_bind_nil(mbind, name);
    } else {
        mysql_bind_string(mbind, name, data, size);
    }
    return 0;
}
/// <summary>
/// 绑定整数参数（非 number/boolean 类型时自动转为 NULL 绑定）
/// </summary>
/// <param name="self" type="userdata">bind 对象</param>
/// <param name="name" type="string?">具名参数名；nil 表示按位置绑定</param>
/// <param name="val" type="integer|boolean|nil">整数值；非 number/boolean 视为 NULL</param>
/// <returns>无</returns>
static int32_t _lmysql_bind_integer(lua_State *lua) {
    mysql_bind_ctx *mbind = luaL_checkudata(lua, 1, MT_MYSQL_BIND);
    char *name = NULL;
    if (LUA_TSTRING == lua_type(lua, 2)) {
        name = (char *)luaL_checkstring(lua, 2);
    }
    int32_t type = lua_type(lua, 3);
    if (LUA_TNUMBER != type
        && LUA_TBOOLEAN != type) {
        mysql_bind_nil(mbind, name);
        return 0;
    }
    int64_t val = (int64_t)luaL_checkinteger(lua, 3);
    mysql_bind_integer(mbind, name, val);
    return 0;
}
/// <summary>
/// 绑定浮点数参数（非 number 类型时自动转为 NULL 绑定）
/// </summary>
/// <param name="self" type="userdata">bind 对象</param>
/// <param name="name" type="string?">具名参数名；nil 表示按位置绑定</param>
/// <param name="val" type="number|nil">单精度浮点值；非 number 视为 NULL</param>
/// <returns>无</returns>
static int32_t _lmysql_bind_float(lua_State *lua) {
    mysql_bind_ctx *mbind = luaL_checkudata(lua, 1, MT_MYSQL_BIND);
    char *name = NULL;
    if (LUA_TSTRING == lua_type(lua, 2)) {
        name = (char *)luaL_checkstring(lua, 2);
    }
    int32_t type = lua_type(lua, 3);
    if (LUA_TNUMBER != type) {
        mysql_bind_nil(mbind, name);
        return 0;
    }
    float val = (float)luaL_checknumber(lua, 3);
    mysql_bind_float(mbind, name, val);
    return 0;
}
/// <summary>
/// 绑定双精度浮点参数（非 number 类型时自动转为 NULL 绑定）
/// </summary>
/// <param name="self" type="userdata">bind 对象</param>
/// <param name="name" type="string?">具名参数名；nil 表示按位置绑定</param>
/// <param name="val" type="number|nil">双精度浮点值；非 number 视为 NULL</param>
/// <returns>无</returns>
static int32_t _lmysql_bind_double(lua_State *lua) {
    mysql_bind_ctx *mbind = luaL_checkudata(lua, 1, MT_MYSQL_BIND);
    char *name = NULL;
    if (LUA_TSTRING == lua_type(lua, 2)) {
        name = (char *)luaL_checkstring(lua, 2);
    }
    int32_t type = lua_type(lua, 3);
    if (LUA_TNUMBER != type) {
        mysql_bind_nil(mbind, name);
        return 0;
    }
    double val = luaL_checknumber(lua, 3);
    mysql_bind_double(mbind, name, val);
    return 0;
}
/// <summary>
/// 绑定 DATETIME 参数（非 number 类型时自动转为 NULL 绑定）
/// </summary>
/// <param name="self" type="userdata">bind 对象</param>
/// <param name="name" type="string?">具名参数名；nil 表示按位置绑定</param>
/// <param name="ts" type="integer|nil">Unix 时间戳（秒）；非 number 视为 NULL</param>
/// <returns>无</returns>
static int32_t _lmysql_bind_datetime(lua_State *lua) {
    mysql_bind_ctx *mbind = luaL_checkudata(lua, 1, MT_MYSQL_BIND);
    char *name = NULL;
    if (LUA_TSTRING == lua_type(lua, 2)) {
        name = (char *)luaL_checkstring(lua, 2);
    }
    int32_t type = lua_type(lua, 3);
    if (LUA_TNUMBER != type) {
        mysql_bind_nil(mbind, name);
        return 0;
    }
    time_t ts = (time_t)luaL_checkinteger(lua, 3);
    mysql_bind_datetime(mbind, name, ts);
    return 0;
}
/// <summary>
/// 绑定 TIME 参数（支持负值时间段）
/// </summary>
/// <param name="self" type="userdata">bind 对象</param>
/// <param name="name" type="string?">具名参数名；nil 表示按位置绑定</param>
/// <param name="is_negative" type="integer">1 表示负值时间段，0 正值</param>
/// <param name="days" type="integer">天数</param>
/// <param name="hour" type="integer">小时</param>
/// <param name="minute" type="integer">分钟</param>
/// <param name="second" type="integer">秒</param>
/// <returns>无</returns>
static int32_t _lmysql_bind_time(lua_State *lua) {
    mysql_bind_ctx *mbind = luaL_checkudata(lua, 1, MT_MYSQL_BIND);
    char *name = NULL;
    if (LUA_TSTRING == lua_type(lua, 2)) {
        name = (char *)luaL_checkstring(lua, 2);
    }
    int8_t is_negative = (int8_t)luaL_checkinteger(lua, 3);
    int32_t days = (int32_t)luaL_checkinteger(lua, 4);
    int8_t hour = (int8_t)luaL_checkinteger(lua, 5);
    int8_t minute = (int8_t)luaL_checkinteger(lua, 6);
    int8_t second = (int8_t)luaL_checkinteger(lua, 7);
    mysql_bind_time(mbind, name, is_negative, days, hour, minute, second);
    return 0;
}
//mysql.bind
LUAMOD_API int luaopen_mysql_bind(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lmysql_bind_new },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "clear", _lmysql_bind_clear },
        { "null", _lmysql_bind_nil },
        { "string", _lmysql_bind_string },
        { "integer", _lmysql_bind_integer },
        { "float", _lmysql_bind_float },
        { "double", _lmysql_bind_double },
        { "datetime", _lmysql_bind_datetime },
        { "time", _lmysql_bind_time },
        { "__gc", _lmysql_bind_free },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_MYSQL_BIND, reg_new, reg_func);
    return 1;
}
/// <summary>
/// 从 mpack 数据包中创建结果集读取器
/// </summary>
/// <param name="mpack" type="lightuserdata">mpack_ctx 数据包指针</param>
/// <returns type="_mysql_reader_ctx?">reader 对象；失败返回 nil</returns>
static int32_t _lmysql_reader_new(lua_State *lua) {
    luaL_checktype(lua, 1, LUA_TLIGHTUSERDATA);
    mpack_ctx *mpack = lua_touserdata(lua, 1);
    mysql_reader_ctx *reader = mysql_reader_init(mpack);
    if (NULL == reader) {
        lua_pushnil(lua);
        return 1;
    }
    mysql_reader_ctx **rd = lua_newuserdata(lua, sizeof(mysql_reader_ctx *));
    *rd = reader;
    ASSOC_MTABLE(lua, MT_MYSQL_READER);
    return 1;
}
/// <summary>
/// 释放结果集读取器资源（绑定为 __gc，由 Lua GC 自动调用）
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <returns>无</returns>
static int32_t _lmysql_reader_free(lua_State *lua) {
    mysql_reader_ctx **reader = luaL_checkudata(lua, 1, MT_MYSQL_READER);
    if (NULL != *reader) {
        mysql_reader_free(*reader);
        *reader = NULL;
    }
    return 0;
}
/// <summary>
/// 返回结果集总行数
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <returns type="integer">行数</returns>
static int32_t _lmysql_reader_size(lua_State *lua) {
    mysql_reader_ctx **reader = luaL_checkudata(lua, 1, MT_MYSQL_READER);
    lua_pushinteger(lua, mysql_reader_size(*reader));
    return 1;
}
/// <summary>
/// 定位到指定行位置
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <param name="pos" type="integer">目标行下标</param>
/// <returns>无</returns>
static int32_t _lmysql_reader_seek(lua_State *lua) {
    mysql_reader_ctx **reader = luaL_checkudata(lua, 1, MT_MYSQL_READER);
    size_t pos = (size_t)luaL_checkinteger(lua, 2);
    mysql_reader_seek(*reader, pos);
    return 0;
}
/// <summary>
/// 判断是否已到达结果集末尾
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <returns type="boolean">已到末尾 true，否则 false</returns>
static int32_t _lmysql_reader_eof(lua_State *lua) {
    mysql_reader_ctx **reader = luaL_checkudata(lua, 1, MT_MYSQL_READER);
    lua_pushboolean(lua, mysql_reader_eof(*reader));
    return 1;
}
/// <summary>
/// 移动到下一行
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <returns>无</returns>
static int32_t _lmysql_reader_next(lua_State *lua) {
    mysql_reader_ctx **reader = luaL_checkudata(lua, 1, MT_MYSQL_READER);
    mysql_reader_next(*reader);
    return 0;
}
/// <summary>
/// 读取当前行指定字段的整数值
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <param name="name" type="string">字段名</param>
/// <returns type="boolean">true 表示读取成功（含字段为 NULL）；false 表示读取失败</returns>
/// <returns type="integer?">字段整数值；字段为 NULL 时不返回此值</returns>
static int32_t _lmysql_reader_integer(lua_State *lua) {
    mysql_reader_ctx **reader = luaL_checkudata(lua, 1, MT_MYSQL_READER);
    const char *name = luaL_checkstring(lua, 2);
    int32_t err;
    int64_t val = mysql_reader_integer(*reader, name, &err);
    if (ERR_OK == err) {
        lua_pushboolean(lua, 1);
        lua_pushinteger(lua, val);
        return 2;
    }
    if (1 == err) {
        // 字段值为 NULL
        lua_pushboolean(lua, 1);
        return 1;
    }
    lua_pushboolean(lua, 0);
    return 1;
}
/// <summary>
/// 读取当前行指定字段的单精度浮点值
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <param name="name" type="string">字段名</param>
/// <returns type="boolean">true 表示读取成功（含字段为 NULL）；false 表示读取失败</returns>
/// <returns type="number?">字段单精度浮点值；字段为 NULL 时不返回此值</returns>
static int32_t _lmysql_reader_float(lua_State *lua) {
    mysql_reader_ctx **reader = luaL_checkudata(lua, 1, MT_MYSQL_READER);
    const char *name = luaL_checkstring(lua, 2);
    int32_t err;
    float val = mysql_reader_float(*reader, name, &err);
    if (ERR_OK == err) {
        lua_pushboolean(lua, 1);
        lua_pushnumber(lua, (double)val);
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
/// 读取当前行指定字段的双精度浮点值
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <param name="name" type="string">字段名</param>
/// <returns type="boolean">true 表示读取成功（含字段为 NULL）；false 表示读取失败</returns>
/// <returns type="number?">字段双精度浮点值；字段为 NULL 时不返回此值</returns>
static int32_t _lmysql_reader_double(lua_State *lua) {
    mysql_reader_ctx **reader = luaL_checkudata(lua, 1, MT_MYSQL_READER);
    const char *name = luaL_checkstring(lua, 2);
    int32_t err;
    double val = mysql_reader_double(*reader, name, &err);
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
/// 读取当前行指定字段的字符串值（返回 lightuserdata + 长度）
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <param name="name" type="string">字段名</param>
/// <returns type="boolean">true 表示读取成功（含字段为 NULL）；false 表示读取失败</returns>
/// <returns type="lightuserdata?">字段数据指针；字段为 NULL 时不返回此值</returns>
/// <returns type="integer?">字段字节数；字段为 NULL 时不返回此值</returns>
static int32_t _lmysql_reader_string(lua_State *lua) {
    mysql_reader_ctx **reader = luaL_checkudata(lua, 1, MT_MYSQL_READER);
    const char *name = luaL_checkstring(lua, 2);
    int32_t err;
    size_t lens = 0;
    char *val = mysql_reader_string(*reader, name, &lens, &err);
    if (ERR_OK == err) {
        lua_pushboolean(lua, 1);
        lua_pushlightuserdata(lua, val);
        lua_pushinteger(lua, lens);
        return 3;
    }
    if (1 == err) {
        // 字段值为 NULL
        lua_pushboolean(lua, 1);
        return 1;
    }
    lua_pushboolean(lua, 0);
    return 1;
}
/// <summary>
/// 读取当前行指定字段的 DATETIME 值（转换为 Unix 时间戳）
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <param name="name" type="string">字段名</param>
/// <returns type="boolean">true 表示读取成功（含字段为 NULL）；false 表示读取失败</returns>
/// <returns type="integer?">Unix 时间戳；字段为 NULL 时不返回此值</returns>
static int32_t _lmysql_reader_datetime(lua_State *lua) {
    mysql_reader_ctx **reader = luaL_checkudata(lua, 1, MT_MYSQL_READER);
    const char *name = luaL_checkstring(lua, 2);
    int32_t err;
    int64_t val = mysql_reader_datetime(*reader, name, &err);
    if (ERR_OK == err) {
        lua_pushboolean(lua, 1);
        lua_pushinteger(lua, val);
        return 2;
    }
    if (1 == err) {
        // 字段值为 NULL
        lua_pushboolean(lua, 1);
        return 1;
    }
    lua_pushboolean(lua, 0);
    return 1;
}
/// <summary>
/// 读取当前行指定字段的 TIME 值
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <param name="name" type="string">字段名</param>
/// <returns type="boolean">true 表示读取成功（含字段为 NULL）；false 表示读取失败</returns>
/// <returns type="boolean?">is_negative 标志；字段为 NULL 时不返回</returns>
/// <returns type="integer?">days；字段为 NULL 时不返回</returns>
/// <returns type="integer?">hour；字段为 NULL 时不返回</returns>
/// <returns type="integer?">minute；字段为 NULL 时不返回</returns>
/// <returns type="integer?">second；字段为 NULL 时不返回</returns>
/// <returns type="integer?">usec（0~999999）；字段为 NULL 时不返回</returns>
static int32_t _lmysql_reader_time(lua_State *lua) {
    mysql_reader_ctx **reader = luaL_checkudata(lua, 1, MT_MYSQL_READER);
    const char *name = luaL_checkstring(lua, 2);
    int32_t err;
    struct tm dt = { 0 };
    uint32_t usec;
    int32_t is_negative = mysql_reader_time(*reader, name, &dt, &usec, &err);
    if (ERR_OK == err) {
        lua_pushboolean(lua, 1);
        lua_pushboolean(lua, is_negative);
        lua_pushinteger(lua, dt.tm_mday);  // 天数（TIME 类型用 tm_mday 表示）
        lua_pushinteger(lua, dt.tm_hour);
        lua_pushinteger(lua, dt.tm_min);
        lua_pushinteger(lua, dt.tm_sec);
        lua_pushinteger(lua, usec);
        return 7;
    }
    if (1 == err) {
        // 字段值为 NULL
        lua_pushboolean(lua, 1);
        return 1;
    }
    lua_pushboolean(lua, 0);
    return 1;
}
//mysql.reader
LUAMOD_API int luaopen_mysql_reader(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lmysql_reader_new },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "size",  _lmysql_reader_size },
        { "seek", _lmysql_reader_seek },
        { "eof", _lmysql_reader_eof },
        { "next", _lmysql_reader_next },
        { "integer", _lmysql_reader_integer },
        { "float", _lmysql_reader_float },
        { "double", _lmysql_reader_double },
        { "string", _lmysql_reader_string },
        { "datetime", _lmysql_reader_datetime },
        { "time", _lmysql_reader_time },
        { "__gc", _lmysql_reader_free },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_MYSQL_READER, reg_new, reg_func);
    return 1;
}
/// <summary>
/// 从 mpack 数据包中创建预处理语句上下文
/// </summary>
/// <param name="mpack" type="lightuserdata">mpack_ctx 数据包指针</param>
/// <returns type="_mysql_stmt_ctx?">stmt 对象；失败返回 nil</returns>
static int32_t _lmysql_stmt_new(lua_State *lua) {
    luaL_checktype(lua, 1, LUA_TLIGHTUSERDATA);
    mpack_ctx *mpack = lua_touserdata(lua, 1);
    mysql_stmt_ctx *stmt = mysql_stmt_init(mpack);
    if (NULL == stmt) {
        lua_pushnil(lua);
        return 1;
    }
    mysql_stmt_ctx **st = lua_newuserdata(lua, sizeof(mysql_stmt_ctx *));
    *st = stmt;
    ASSOC_MTABLE(lua, MT_MYSQL_STMT);
    return 1;
}
/// <summary>
/// 关闭预处理语句并释放资源（绑定为 __gc，由 Lua GC 自动调用）
/// </summary>
/// <param name="self" type="userdata">stmt 对象</param>
/// <returns>无</returns>
static int32_t _lmysql_stmt_free(lua_State *lua) {
    mysql_stmt_ctx **stmt = luaL_checkudata(lua, 1, MT_MYSQL_STMT);
    if (NULL != *stmt) {
        mysql_stmt_close(*stmt);
        *stmt = NULL;
    }
    return 0;
}
/// <summary>
/// 打包预处理语句执行请求
/// </summary>
/// <param name="self" type="userdata">stmt 对象</param>
/// <param name="bind" type="userdata?">参数绑定上下文；nil 表示无参数</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmysql_pack_stmt_execute(lua_State *lua) {
    mysql_stmt_ctx **stmt = luaL_checkudata(lua, 1, MT_MYSQL_STMT);
    mysql_bind_ctx *mbind = NULL;
    if (LUA_TUSERDATA == lua_type(lua, 2)) {
        mbind = luaL_checkudata(lua, 2, MT_MYSQL_BIND);
    }
    size_t size;
    void *pack = mysql_pack_stmt_execute(*stmt, mbind, &size);
    if (NULL == pack) {
        return luaL_error(lua, "stmt_execute: bind count does not match params count.");
    }
    LPUB_RET_LUD(lua, pack, size);
}
/// <summary>
/// 打包预处理语句重置请求
/// </summary>
/// <param name="self" type="userdata">stmt 对象</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmysql_pack_stmt_reset(lua_State *lua) {
    mysql_stmt_ctx **stmt = luaL_checkudata(lua, 1, MT_MYSQL_STMT);
    size_t size;
    void *pack = mysql_pack_stmt_reset(*stmt, &size);
    LPUB_RET_LUD(lua, pack, size);
}
/// <summary>
/// 获取预处理语句所属连接的 fd 和 skid
/// </summary>
/// <param name="self" type="userdata">stmt 对象</param>
/// <returns type="integer">socket fd</returns>
/// <returns type="integer">skid</returns>
static int32_t _lmysql_stmt_sock_id(lua_State *lua) {
    mysql_stmt_ctx **stmt = luaL_checkudata(lua, 1, MT_MYSQL_STMT);
    lua_pushinteger(lua, (*stmt)->mysql->client.fd);
    lua_pushinteger(lua, (*stmt)->mysql->client.skid);
    return 2;
}
//mysql.stmt
LUAMOD_API int luaopen_mysql_stmt(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lmysql_stmt_new },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "pack_stmt_execute", _lmysql_pack_stmt_execute },
        { "pack_stmt_reset", _lmysql_pack_stmt_reset },
        { "sock_id", _lmysql_stmt_sock_id },
        { "__gc", _lmysql_stmt_free },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_MYSQL_STMT, reg_new, reg_func);
    return 1;
}
/// <summary>
/// 打包 COM_INIT_DB（切换数据库）命令
/// </summary>
/// <param name="self" type="userdata">mysql 对象</param>
/// <param name="db" type="string">数据库名</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmysql_pack_selectdb(lua_State *lua) {
    mysql_ctx *mysql = luaL_checkudata(lua, 1, MT_MYSQL);
    const char *db = luaL_checkstring(lua, 2);
    size_t size;
    void *pack = mysql_pack_selectdb(mysql, db, &size);
    LPUB_RET_LUD(lua, pack, size);
}
/// <summary>
/// 打包 COM_PING 心跳命令
/// </summary>
/// <param name="self" type="userdata">mysql 对象</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmysql_pack_ping(lua_State *lua) {
    mysql_ctx *mysql = luaL_checkudata(lua, 1, MT_MYSQL);
    size_t size;
    void *pack = mysql_pack_ping(mysql, &size);
    LPUB_RET_LUD(lua, pack, size);
}
/// <summary>
/// 打包查询命令（支持参数化绑定）
/// </summary>
/// <param name="self" type="userdata">mysql 对象</param>
/// <param name="sql" type="string">SQL 语句</param>
/// <param name="bind" type="userdata?">参数绑定上下文；nil 表示无参数</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmysql_pack_query(lua_State *lua) {
    mysql_ctx *mysql = luaL_checkudata(lua, 1, MT_MYSQL);
    const char *sql = luaL_checkstring(lua, 2);
    mysql_bind_ctx *mbind = NULL;
    if (LUA_TUSERDATA == lua_type(lua, 3)){
        mbind = luaL_checkudata(lua, 3, MT_MYSQL_BIND);
    }
    size_t size;
    void *pack = mysql_pack_query(mysql, sql, mbind, &size);
    if (NULL == pack) {
        return luaL_error(lua, "pack_query: payload exceeds 16MB.");
    }
    LPUB_RET_LUD(lua, pack, size);
}
/// <summary>
/// 打包 COM_QUIT 断连命令
/// </summary>
/// <param name="self" type="userdata">mysql 对象</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmysql_pack_quit(lua_State *lua) {
    mysql_ctx *mysql = luaL_checkudata(lua, 1, MT_MYSQL);
    size_t size;
    void *pack = mysql_pack_quit(mysql, &size);
    LPUB_RET_LUD(lua, pack, size);
}
/// <summary>
/// 打包预处理语句准备命令（COM_STMT_PREPARE）
/// </summary>
/// <param name="self" type="userdata">mysql 对象</param>
/// <param name="sql" type="string">SQL 语句模板（含 ? 占位符）</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmysql_pack_stmt_prepare(lua_State *lua) {
    mysql_ctx *mysql = luaL_checkudata(lua, 1, MT_MYSQL);
    const char *sql = luaL_checkstring(lua, 2);
    size_t size;
    void *pack = mysql_pack_stmt_prepare(mysql, sql, &size);
    if (NULL == pack) {
        return luaL_error(lua, "pack_stmt_prepare: payload exceeds 16MB.");
    }
    LPUB_RET_LUD(lua, pack, size);
}
/// <summary>
/// 创建 MySQL 客户端上下文（不立即建立连接）
/// </summary>
/// <param name="ip" type="string">服务器 IP</param>
/// <param name="port" type="integer">服务器端口</param>
/// <param name="evssl" type="lightuserdata|nil">SSL 上下文；nil 表示明文</param>
/// <param name="user" type="string">用户名</param>
/// <param name="password" type="string">密码</param>
/// <param name="database" type="string">初始数据库</param>
/// <param name="charset" type="string">字符集（如 "utf8mb4"）</param>
/// <param name="maxpk" type="integer?">最大包大小（字节）；省略或非数字时使用默认</param>
/// <returns type="_mysql_ctx?">mysql 对象；初始化失败返回 nil</returns>
static int32_t _lmysql_new(lua_State *lua) {
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
    const char *charset = luaL_checkstring(lua, 7);
    uint32_t maxpk = 0;
    if (LUA_TNUMBER == lua_type(lua, 8)) {
        maxpk = (uint32_t)luaL_checkinteger(lua, 8);
    }
    mysql_ctx *mysql = lua_newuserdata(lua, sizeof(mysql_ctx));
    if (ERR_OK != mysql_init(mysql, ip, port, evssl, user, password, database, charset, maxpk)) {
        lua_pop(lua, 1);   // 弹出 userdata，避免在 nil 下方遗留无用中间值
        lua_pushnil(lua);
    } else {
        ASSOC_MTABLE(lua, MT_MYSQL);
    }
    return 1;
}
/// <summary>
/// 获取 mpack 数据包的封包类型
/// </summary>
/// <param name="mpack" type="lightuserdata">mpack_ctx 数据包指针</param>
/// <returns type="integer">封包类型枚举值</returns>
static int32_t _lmysql_pack_type(lua_State *lua) {
    luaL_checktype(lua, 1, LUA_TLIGHTUSERDATA);
    mpack_ctx *mpack = lua_touserdata(lua, 1);
    lua_pushinteger(lua, mpack->pack_type);
    return 1;
}
/// <summary>
/// 发送 QUIT 命令并清理连接上下文绑定（绑定为 __gc，由 Lua GC 自动调用）
/// </summary>
/// <param name="self" type="userdata">mysql 对象</param>
/// <returns>无</returns>
static int32_t _lmysql_free(lua_State *lua) {
    mysql_ctx *mysql = luaL_checkudata(lua, 1, MT_MYSQL);
    if (NULL != mysql->task
        && INVALID_SOCK != mysql->client.fd) {
        size_t size;
        void *pack = mysql_pack_quit(mysql, &size);
        (void)ev_ud_context(&mysql->task->loader->netev, mysql->client.fd, mysql->client.skid, NULL);
        ev_send(&mysql->task->loader->netev, mysql->client.fd, mysql->client.skid, pack, size, 0);
    }
    secure_zero(mysql->server.salt, sizeof(mysql->server.salt));
    secure_zero(mysql->client.password, sizeof(mysql->client.password));
    return 0;
}
/// <summary>
/// 尝试建立 MySQL 连接（异步）
/// </summary>
/// <param name="self" type="userdata">mysql 对象</param>
/// <returns type="boolean">发起成功 true，失败 false</returns>
static int32_t _lmysql_try_connect(lua_State *lua) {
    mysql_ctx *mysql = luaL_checkudata(lua, 1, MT_MYSQL);
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    int32_t rtn = mysql_try_connect(task, mysql);
    if (ERR_OK == rtn) {
        lua_pushboolean(lua, 1);
    } else {
        lua_pushboolean(lua, 0);
    }
    return 1;
}
/// <summary>
/// 返回 MySQL 服务端版本字符串
/// </summary>
/// <param name="self" type="userdata">mysql 对象</param>
/// <returns type="string">服务端版本</returns>
static int32_t _lmysql_version(lua_State *lua) {
    mysql_ctx *mysql = luaL_checkudata(lua, 1, MT_MYSQL);
    lua_pushstring(lua, mysql_version(mysql));
    return 1;
}
/// <summary>
/// 返回最近一次错误信息并清除错误状态
/// </summary>
/// <param name="self" type="userdata">mysql 对象</param>
/// <returns type="string">错误信息</returns>
static int32_t _lmysql_erro(lua_State *lua) {
    mysql_ctx *mysql = luaL_checkudata(lua, 1, MT_MYSQL);
    lua_pushstring(lua, mysql_erro(mysql, NULL));
    mysql_erro_clear(mysql);
    return 1;
}
/// <summary>
/// 返回当前 MySQL 连接的 fd 和 skid
/// </summary>
/// <param name="self" type="userdata">mysql 对象</param>
/// <returns type="integer">socket fd</returns>
/// <returns type="integer">skid</returns>
static int32_t _lmysql_sock_id(lua_State *lua) {
    mysql_ctx *mysql = luaL_checkudata(lua, 1, MT_MYSQL);
    lua_pushinteger(lua, mysql->client.fd);
    lua_pushinteger(lua, mysql->client.skid);
    return 2;
}
/// <summary>
/// 返回最后一次 INSERT 操作生成的自增 id
/// </summary>
/// <param name="self" type="userdata">mysql 对象</param>
/// <returns type="integer">last insert id</returns>
static int32_t _lmysql_last_id(lua_State *lua) {
    mysql_ctx *mysql = luaL_checkudata(lua, 1, MT_MYSQL);
    lua_pushinteger(lua, mysql_last_id(mysql));
    return 1;
}
/// <summary>
/// 返回最后一次 UPDATE / DELETE / INSERT 操作影响的行数
/// </summary>
/// <param name="self" type="userdata">mysql 对象</param>
/// <returns type="integer">affected rows</returns>
static int32_t _lmysql_affectd_rows(lua_State *lua) {
    mysql_ctx *mysql = luaL_checkudata(lua, 1, MT_MYSQL);
    lua_pushinteger(lua, mysql_affected_rows(mysql));
    return 1;
}
//mysql
LUAMOD_API int luaopen_mysql(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lmysql_new },
        { "pack_type", _lmysql_pack_type },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "pack_selectdb", _lmysql_pack_selectdb },
        { "pack_ping", _lmysql_pack_ping },
        { "pack_query", _lmysql_pack_query },
        { "pack_quit", _lmysql_pack_quit },
        { "pack_stmt_prepare", _lmysql_pack_stmt_prepare },
        { "try_connect", _lmysql_try_connect },
        { "version", _lmysql_version },
        { "erro", _lmysql_erro },
        { "sock_id", _lmysql_sock_id },
        { "last_id", _lmysql_last_id },
        { "affectd_rows", _lmysql_affectd_rows },
        { "__gc", _lmysql_free },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_MYSQL, reg_new, reg_func);
    return 1;
}
