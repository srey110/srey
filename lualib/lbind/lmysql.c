#include "lbind/lpub.h"

#define MT_MYSQL_BIND   "_mysql_bind_ctx"
#define MT_MYSQL_READER "_mysql_reader_ctx"
#define MT_MYSQL_STMT   "_mysql_stmt_ctx"
#define MT_MYSQL        "_mysql_ctx"
// stmt 的四个入口共用: 自身非空 + 宿主没被 __gc 关掉(宿主关了 stmt 就拒绝，理由见 lpub_owner_ptr)
#define LMYSQL_STMT_ARG(lua, var) \
    LPUB_UD_ARG((lua), mysql_stmt_ctx, MT_MYSQL_STMT, var, "stmt freed") \
    if (NULL == lpub_owner_ptr((lua), MT_MYSQL)) { \
        return luaL_error((lua), "mysql stmt: owner mysql already freed"); \
    }
// 六个按列名取值的 reader 入口共用的开场白：取 reader + 取列名并查一次列下标 + 备好 err。
// err 是三态(ERR_OK 有值 / 1 字段是 SQL NULL / 其余读取失败)，由 mysql_reader、pgsql_reader 两侧
// 共同产出，单值入口用 lpub.h 的 LPUB_READER_RTN 收尾
#define LMYSQL_READER_GET(lua, rvar, nvar, cvar, evar) \
    LPUB_UD_ARG((lua), mysql_reader_ctx, MT_MYSQL_READER, rvar, "reader freed") \
    const char *nvar; \
    int32_t cvar = _lmysql_reader_col((lua), *rvar, 2, &nvar); \
    int32_t evar
// 七个 bind 入口共用的开场白: 取 bind 对象 + 取可选具名参数(nil 即按位置绑定)，取法只此一处。
// 名字只收字符串或不传，传错类型当场报错，免得静默退化成按位置绑定
#define LMYSQL_BIND_ARG(lua, bindvar, namevar) \
    mysql_bind_ctx *bindvar = lpub_check_udata((lua), 1, MT_MYSQL_BIND); \
    const char *namevar = NULL; \
    luaL_argcheck((lua), lua_isnoneornil((lua), 2) || LUA_TSTRING == lua_type((lua), 2), 2, \
                  "name must be a string"); \
    if (LUA_TSTRING == lua_type((lua), 2)) { \
        namevar = lpub_check_cstr((lua), 2); \
    }

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
/// 释放绑定上下文内部资源（绑定为 __gc，由 Lua GC 自动调用）。
/// 可重复调用；释放后再绑定参数等同于刚 new 出来的空上下文
/// </summary>
/// <param name="self" type="userdata">bind 对象</param>
/// <returns>无</returns>
static int32_t _lmysql_bind_free(lua_State *lua) {
    mysql_bind_ctx *mbind = lpub_check_udata(lua, 1, MT_MYSQL_BIND);
    mysql_bind_free(mbind);
    return 0;
}
/// <summary>
/// 清空所有已绑定参数，可复用上下文
/// </summary>
/// <param name="self" type="userdata">bind 对象</param>
/// <returns>无</returns>
static int32_t _lmysql_bind_clear(lua_State *lua) {
    mysql_bind_ctx *mbind = lpub_check_udata(lua, 1, MT_MYSQL_BIND);
    mysql_bind_clear(mbind);
    return 0;
}
/// <summary>
/// 绑定一个 NULL 参数（注册为 bind:null）
/// </summary>
/// <param name="self" type="userdata">bind 对象</param>
/// <param name="name" type="string?">具名参数名；nil 表示按位置绑定，含 NUL 报错</param>
/// <returns>无</returns>
static int32_t _lmysql_bind_nil(lua_State *lua) {
    LMYSQL_BIND_ARG(lua, mbind, name);
    mysql_bind_nil(mbind, name);
    return 0;
}
/// <summary>
/// 绑定字符串参数（data 为 nil 时自动转为 NULL 绑定）
/// </summary>
/// <param name="self" type="userdata">bind 对象</param>
/// <param name="name" type="string?">具名参数名；nil 表示按位置绑定，含 NUL 报错</param>
/// <param name="data" type="string|lightuserdata|nil">字符串值；nil 绑定 NULL，其余类型报错，取值规则见 lpub_opt_buf</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数，取值 [0, INT32_MAX]</param>
/// <returns>无</returns>
static int32_t _lmysql_bind_string(lua_State *lua) {
    LMYSQL_BIND_ARG(lua, mbind, name);
    size_t size;
    char *data = lpub_opt_buf(lua, 3, &size);
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
/// <param name="name" type="string?">具名参数名；nil 表示按位置绑定，含 NUL 报错</param>
/// <param name="val" type="integer|boolean|nil">整数值；非 number/boolean 视为 NULL</param>
/// <returns>无</returns>
static int32_t _lmysql_bind_integer(lua_State *lua) {
    LMYSQL_BIND_ARG(lua, mbind, name);
    int32_t type = lua_type(lua, 3);
    int64_t val;
    if (LUA_TBOOLEAN == type) {
        val = lua_toboolean(lua, 3);
    } else if (LUA_TNUMBER == type) {
        val = (int64_t)luaL_checkinteger(lua, 3);
    } else {
        mysql_bind_nil(mbind, name);
        return 0;
    }
    mysql_bind_integer(mbind, name, val);
    return 0;
}
/// <summary>
/// 绑定浮点数参数（非 number 类型时自动转为 NULL 绑定）
/// </summary>
/// <param name="self" type="userdata">bind 对象</param>
/// <param name="name" type="string?">具名参数名；nil 表示按位置绑定，含 NUL 报错</param>
/// <param name="val" type="number|nil">单精度浮点值；非 number 视为 NULL</param>
/// <returns>无</returns>
static int32_t _lmysql_bind_float(lua_State *lua) {
    LMYSQL_BIND_ARG(lua, mbind, name);
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
/// <param name="name" type="string?">具名参数名；nil 表示按位置绑定，含 NUL 报错</param>
/// <param name="val" type="number|nil">双精度浮点值；非 number 视为 NULL</param>
/// <returns>无</returns>
static int32_t _lmysql_bind_double(lua_State *lua) {
    LMYSQL_BIND_ARG(lua, mbind, name);
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
/// <param name="name" type="string?">具名参数名；nil 表示按位置绑定，含 NUL 报错</param>
/// <param name="ts" type="integer|nil">Unix 时间戳（秒，按本地时区换成日期时间；reader:datetime 给的是微秒，回绑先除以 1000000）。
/// 取值为 MySQL DATETIME 范围 1000-01-01 00:00:00 ~ 9999-12-31 23:59:59（UTC）对应的秒数，越界或超出本平台 time_t 报错；
/// Windows 上 1970 年前或 3000 年后本地时间换算失败，按零日期绑定。非 number 视为 NULL</param>
/// <returns>无</returns>
static int32_t _lmysql_bind_datetime(lua_State *lua) {
    LMYSQL_BIND_ARG(lua, mbind, name);
    int32_t type = lua_type(lua, 3);
    if (LUA_TNUMBER != type) {
        mysql_bind_nil(mbind, name);
        return 0;
    }
    // 卡在 MySQL DATETIME 的范围内；time_t 为 32 位时再判一次，免得截成另一个时间
    int64_t sec = lpub_check_range(lua, 3, -30610224000, 253402300799, "datetime out of range");
    luaL_argcheck(lua, (int64_t)(time_t)sec == sec, 3, "datetime exceeds time_t");
    mysql_bind_datetime(mbind, name, (time_t)sec);
    return 0;
}
/// <summary>
/// 绑定 TIME 参数（支持负值时间段）
/// </summary>
/// <param name="self" type="userdata">bind 对象</param>
/// <param name="name" type="string?">具名参数名；nil 表示按位置绑定，含 NUL 报错</param>
/// <param name="is_negative" type="integer">1 表示负值时间段，0 正值</param>
/// <param name="days" type="integer">天数，须 大于等于 0（符号只由 is_negative 表示）；线格式里是 4 字节无符号字段，负数写出去就是四十亿天</param>
/// <param name="hour" type="integer">小时，须 大于等于 0（符号只由 is_negative 表示）</param>
/// <param name="minute" type="integer">分钟，须 大于等于 0</param>
/// <param name="second" type="integer">秒，须 大于等于 0</param>
/// <returns>无</returns>
static int32_t _lmysql_bind_time(lua_State *lua) {
    LMYSQL_BIND_ARG(lua, mbind, name);
    // 五个字段原样进 MYSQL_TYPE_TIME 报文, 截断或传负数出来都是另一个合法时间且无从报错:
    // 符号由 is_negative 单独带, 时分秒各占一个字节, 传 -1 到服务端就成了 255
    int8_t is_negative = (int8_t)lpub_check_flag(lua, 3);
    int32_t days = (int32_t)lpub_check_range(lua, 4, 0, INT32_MAX, "days out of range");
    int8_t hour = (int8_t)lpub_check_range(lua, 5, 0, INT8_MAX, "hour out of range");
    int8_t minute = (int8_t)lpub_check_range(lua, 6, 0, INT8_MAX, "minute out of range");
    int8_t second = (int8_t)lpub_check_range(lua, 7, 0, INT8_MAX, "second out of range");
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
    LPUB_LUD_ARG(lua, mpack_ctx, 1, mpack);
    mysql_reader_ctx *reader = mysql_reader_init(mpack);
    if (NULL == reader) {
        return lpub_rtn_nil(lua, 1);
    }
    lpub_push_ud(lua, reader, MT_MYSQL_READER);
    return 1;
}
/// <summary>
/// 释放结果集读取器资源（绑定为 __gc，由 Lua GC 自动调用）
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <returns>无</returns>
static int32_t _lmysql_reader_free(lua_State *lua) {
    mysql_reader_ctx **reader = lpub_check_udata(lua, 1, MT_MYSQL_READER);
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
    LPUB_UD_ARG(lua, mysql_reader_ctx, MT_MYSQL_READER, reader, "reader freed");
    lua_pushinteger(lua, mysql_reader_size(*reader));
    return 1;
}
/// <summary>
/// 定位到指定行位置
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <param name="pos" type="integer">目标行下标（0 基），取值 [0, INT32_MAX]，超出报错；不小于行数时游标不动</param>
/// <returns>无</returns>
static int32_t _lmysql_reader_seek(lua_State *lua) {
    LPUB_UD_ARG(lua, mysql_reader_ctx, MT_MYSQL_READER, reader, "reader freed");
    size_t pos = lpub_check_lens(lua, 2, INT32_MAX);
    mysql_reader_seek(*reader, pos);
    return 0;
}
/// <summary>
/// 判断是否已到达结果集末尾
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <returns type="boolean">已到末尾 true，否则 false</returns>
static int32_t _lmysql_reader_eof(lua_State *lua) {
    LPUB_UD_ARG(lua, mysql_reader_ctx, MT_MYSQL_READER, reader, "reader freed");
    return lpub_rtn_bool(lua, mysql_reader_eof(*reader));
}
/// <summary>
/// 移动到下一行
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <returns>无</returns>
static int32_t _lmysql_reader_next(lua_State *lua) {
    LPUB_UD_ARG(lua, mysql_reader_ctx, MT_MYSQL_READER, reader, "reader freed");
    mysql_reader_next(*reader);
    return 0;
}
// 取栈位 idx 的列名，按 Lua 串自带长度查一次列下标；name 回出列名给报错用
static inline int32_t _lmysql_reader_col(lua_State *lua, mysql_reader_ctx *reader, int32_t idx, const char **name) {
    size_t nlens;
    *name = luaL_checklstring(lua, idx, &nlens);
    return mysql_reader_col(reader, *name, nlens);
}
// 整数列按 UNSIGNED 标志选有无符号读，err 同 mysql_reader_integer，col 是 name 那一列的下标。无符号值超出 int64 时：
// raise 非 0 抛错（reader:get，它的其余失败也抛），否则打 WARN 并按读取失败返回（reader:integer，其余失败返回 false）
static int64_t _lmysql_reader_int(lua_State *lua, mysql_reader_ctx *reader, int32_t col, const char *name,
                                  int32_t raise, int32_t *err) {
    if (0 == mysql_reader_unsigned_at(reader, col)) {
        return mysql_reader_integer_at(reader, col, err);
    }
    uint64_t val = mysql_reader_uinteger_at(reader, col, err);
    if (ERR_OK == *err && val > (uint64_t)INT64_MAX) {
        if (0 != raise) {
            luaL_error(lua, "reader:get column '%s' unsigned value exceeds int64", name);
        }
        LOG_WARN("reader:integer column '%s' unsigned value exceeds int64.", name);
        *err = ERR_FAILED;
        return 0;
    }
    return (int64_t)val;
}
/// <summary>
/// 读取当前行指定字段的整数值；带 UNSIGNED 标志的列按无符号读，值超出 int64 时打 WARN（日志带列名）
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <param name="name" type="string">字段名</param>
/// <returns type="boolean">true 表示读取成功（含字段为 NULL）；false 表示读取失败（含无符号值超出 int64）</returns>
/// <returns type="integer?">字段整数值；字段为 NULL 时不返回此值</returns>
static int32_t _lmysql_reader_integer(lua_State *lua) {
    LMYSQL_READER_GET(lua, reader, name, col, err);
    int64_t val = _lmysql_reader_int(lua, *reader, col, name, 0, &err);
    LPUB_READER_RTN(lua, err, lua_pushinteger, val);
}
/// <summary>
/// 读取当前行指定字段的单精度浮点值
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <param name="name" type="string">字段名</param>
/// <returns type="boolean">true 表示读取成功（含字段为 NULL）；false 表示读取失败</returns>
/// <returns type="number?">字段单精度浮点值；字段为 NULL 时不返回此值</returns>
static int32_t _lmysql_reader_float(lua_State *lua) {
    LMYSQL_READER_GET(lua, reader, name, col, err);
    float val = mysql_reader_float_at(*reader, col, &err);
    LPUB_READER_RTN(lua, err, lua_pushnumber, (double)val);
}
/// <summary>
/// 读取当前行指定字段的双精度浮点值
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <param name="name" type="string">字段名</param>
/// <returns type="boolean">true 表示读取成功（含字段为 NULL）；false 表示读取失败</returns>
/// <returns type="number?">字段双精度浮点值；字段为 NULL 时不返回此值</returns>
static int32_t _lmysql_reader_double(lua_State *lua) {
    LMYSQL_READER_GET(lua, reader, name, col, err);
    double val = mysql_reader_double_at(*reader, col, &err);
    LPUB_READER_RTN(lua, err, lua_pushnumber, val);
}
/// <summary>
/// 读取当前行指定字段的字符串值
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <param name="name" type="string">字段名</param>
/// <param name="asstr" type="boolean?">真值时第 2 个返回值改为拷贝出来的 Lua 字符串，不再有第 3 个</param>
/// <returns type="boolean">true 表示读取成功（含字段为 NULL）；false 表示读取失败</returns>
/// <returns type="lightuserdata|string?">字段数据指针（reader 内部行缓冲的**借用**指针，随 reader 释放而失效，
/// 调用方既不拥有它、也不能对它调 utils.ud_free 或以 copy=0 交给 srey.send；空串也给非 NULL 指针，
/// 回绑时不会被当成 SQL NULL）；asstr 时为 Lua 字符串；字段为 NULL 时不返回此值</returns>
/// <returns type="integer?">字段字节数；asstr 或字段为 NULL 时不返回此值</returns>
static int32_t _lmysql_reader_string(lua_State *lua) {
    LMYSQL_READER_GET(lua, reader, name, col, err);
    size_t lens = 0;
    char *val = mysql_reader_string_at(*reader, col, &lens, &err);
    if (ERR_OK == err) {
        int32_t asstr = lua_toboolean(lua, 3);// 必须在压栈前读：压了 true 之后栈位 3 就是它
        lua_pushboolean(lua, 1);
        if (0 != asstr) {
            lua_pushlstring(lua, val, lens);
            return 2;
        }
        // 空串的行缓冲指针是 NULL，给个非 NULL 地址：NULL 回灌 lpub_opt_buf 会被当成 nil 绑成 SQL NULL
        lua_pushlightuserdata(lua, NULL != val ? val : (void *)"");
        lua_pushinteger(lua, lens);
        return 3;
    }
    return lpub_rtn_bool(lua, 1 == err);
}
/// <summary>
/// 读取当前行指定字段的 DATETIME 值（微秒精度），按本地时区换算；Windows 上 1970 年前与 3000 年后、
/// macOS 上 1900 年前的值读取失败
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <param name="name" type="string">字段名</param>
/// <returns type="boolean">true 表示读取成功（含字段为 NULL）；false 表示读取失败（含字段量程越界）</returns>
/// <returns type="integer?">微秒精度 Unix 时间戳，零日期为 0；字段为 NULL 时不返回此值</returns>
static int32_t _lmysql_reader_datetime(lua_State *lua) {
    LMYSQL_READER_GET(lua, reader, name, col, err);
    int64_t val = mysql_reader_datetime_at(*reader, col, &err);
    LPUB_READER_RTN(lua, err, lua_pushinteger, val);
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
    LMYSQL_READER_GET(lua, reader, name, col, err);
    struct tm dt = { 0 };
    uint32_t usec;
    int32_t is_negative = mysql_reader_time_at(*reader, col, &dt, &usec, &err);
    if (ERR_OK == err) {
        lua_pushboolean(lua, 1);
        lua_pushboolean(lua, is_negative);
        lua_pushinteger(lua, dt.tm_mday);// 天数（TIME 类型用 tm_mday 表示）
        lua_pushinteger(lua, dt.tm_hour);
        lua_pushinteger(lua, dt.tm_min);
        lua_pushinteger(lua, dt.tm_sec);
        lua_pushinteger(lua, usec);
        return 7;
    }
    return lpub_rtn_bool(lua, 1 == err);
}
/// <summary>
/// 按列名一次取多个字段，按列类型自动选取值方式：整数列给 integer（UNSIGNED 列同 reader:integer），
/// FLOAT / DOUBLE 给 number，字符串类列给 Lua 字符串（拷贝），DATE / DATETIME / TIMESTAMP 给微秒时间戳；
/// TIME 列返回的是多个值，不在此列，用 time
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <param name="..." type="string">字段名，至少一个</param>
/// <returns type="any">每个字段名一个值，SQL NULL 为 nil（先判 NULL 再看类型，TIME、字面量 NULL 等不在上述分组的列
/// 值为 NULL 时也是 nil）；任一字段读取失败（无此列、类型不支持、解析失败、无符号值超出 int64）直接抛错，
/// 错误信息带列名</returns>
static int32_t _lmysql_reader_get(lua_State *lua) {
    LPUB_UD_ARG(lua, mysql_reader_ctx, MT_MYSQL_READER, reader, "reader freed");
    int32_t n = lua_gettop(lua);
    luaL_argcheck(lua, n >= 2, 2, "column name expected");
    luaL_checkstack(lua, n, "too many columns");
    const char *name;
    int32_t col;
    int32_t err;
    int64_t ival;
    double dval;
    size_t lens = 0;
    char *sval;
    for (int32_t i = 2; i <= n; i++) {
        col = _lmysql_reader_col(lua, *reader, i, &name);
        err = ERR_FAILED;
        switch (mysql_reader_cls_at(*reader, col)) {
        case MYSQL_CLS_INT:
            ival = _lmysql_reader_int(lua, *reader, col, name, 1, &err);
            if (ERR_OK == err) {
                lua_pushinteger(lua, ival);
            }
            break;
        case MYSQL_CLS_FLOAT:
            dval = (double)mysql_reader_float_at(*reader, col, &err);
            if (ERR_OK == err) {
                lua_pushnumber(lua, dval);
            }
            break;
        case MYSQL_CLS_DOUBLE:
            dval = mysql_reader_double_at(*reader, col, &err);
            if (ERR_OK == err) {
                lua_pushnumber(lua, dval);
            }
            break;
        case MYSQL_CLS_STRING:
            sval = mysql_reader_string_at(*reader, col, &lens, &err);
            if (ERR_OK == err) {
                lua_pushlstring(lua, sval, lens);
            }
            break;
        case MYSQL_CLS_DATETIME:
            ival = mysql_reader_datetime_at(*reader, col, &err);
            if (ERR_OK == err) {
                lua_pushinteger(lua, ival);
            }
            break;
        default:
            if (0 != mysql_reader_isnull_at(*reader, col)) {
                err = 1;
            }
            break;
        }
        if (1 == err) {
            lua_pushnil(lua);// SQL NULL
        } else if (ERR_OK != err) {
            return luaL_error(lua, "reader:get column '%s' read failed", name);
        }
    }
    return n - 1;
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
        { "get", _lmysql_reader_get },
        { "__gc", _lmysql_reader_free },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_MYSQL_READER, reg_new, reg_func);
    return 1;
}
/// <summary>
/// 从 mpack 数据包中创建预处理语句上下文，并把 mysql 对象锚定为 uservalue 防其先于 stmt 被 GC。
/// stmt 另持宿主 C 块的一份引用，__gc 时交还
/// </summary>
/// <param name="mysql" type="userdata">所属 mysql 对象（锚定为 uservalue 保活），须是收到 mpack 的那条连接，否则报错</param>
/// <param name="mpack" type="lightuserdata">mpack_ctx 数据包指针</param>
/// <returns type="_mysql_stmt_ctx?">stmt 对象；失败返回 nil</returns>
static int32_t _lmysql_stmt_new(lua_State *lua) {
    LPUB_UD_ARG(lua, mysql_ctx, MT_MYSQL, ud, "mysql freed");
    LPUB_LUD_ARG(lua, mpack_ctx, 2, mpack);
    mysql_stmt_ctx *stmt = mysql_stmt_init(mpack);
    if (NULL == stmt) {
        return lpub_rtn_nil(lua, 1);
    }
    if (stmt->mysql != *ud) {
        mysql_stmt_free(stmt);
        return luaL_argerror(lua, 1, "stmt belongs to another mysql");
    }
    lpub_push_ud(lua, stmt, MT_MYSQL_STMT);
    PROT_REF_ACQUIRE(stmt->mysql);// 排在所有会报错的步骤之后，__gc 的 release 才与它成对
    lua_pushvalue(lua, 1);
    lua_setiuservalue(lua, -2, 1);
    return 1;
}
/// <summary>
/// 释放预处理语句的本地资源（绑定为 __gc，由 Lua GC 自动调用）。
/// 只做本地释放，不发 COM_STMT_CLOSE——finalizer 不能 yield，取不到连接那把命令串行化的锁，
/// 绕过它发包会插进别的协程正在进行的半双工交换里。服务端那份句柄要么由业务显式
/// 调 stmt:close() 释放，要么留到连接关闭
/// </summary>
/// <param name="self" type="userdata">stmt 对象</param>
/// <returns>无</returns>
static int32_t _lmysql_stmt_free(lua_State *lua) {
    mysql_stmt_ctx **stmt = lpub_check_udata(lua, 1, MT_MYSQL_STMT);
    if (NULL != *stmt) {
        mysql_ctx *mysql = (*stmt)->mysql;
        mysql_stmt_free(*stmt);
        *stmt = NULL;
        PROT_REF_RELEASE(mysql);
    }
    return 0;
}
/// <summary>
/// 打包预处理语句执行请求
/// </summary>
/// <param name="self" type="userdata">stmt 对象</param>
/// <param name="bind" type="userdata?">参数绑定上下文；nil 表示无参数，其余类型报错。
/// 绑定个数须等于 prepare 时声明的参数个数（含声明了参数而 bind 为 nil），不符报错</param>
/// <returns type="lightuserdata?">命令数据指针；载荷超 16MB 时返回 nil</returns>
/// <returns type="integer?">数据长度；命令数据指针为 nil 时一并为 nil</returns>
static int32_t _lmysql_pack_stmt_execute(lua_State *lua) {
    LMYSQL_STMT_ARG(lua, stmt);
    mysql_bind_ctx *mbind = lua_isnoneornil(lua, 2) ? NULL : lpub_check_udata(lua, 2, MT_MYSQL_BIND);
    int32_t count = (NULL == mbind) ? 0 : mbind->count;
    if (count != (int32_t)(*stmt)->params_count) {
        return luaL_error(lua, "stmt_execute bind count mismatch: expect %d, got %d.",
                          (int32_t)(*stmt)->params_count, count);
    }
    size_t size;
    void *pack = mysql_pack_stmt_execute(*stmt, mbind, &size);
    return lpub_rtn_lud(lua, pack, size);
}
/// <summary>
/// 打包预处理语句重置请求
/// </summary>
/// <param name="self" type="userdata">stmt 对象</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmysql_pack_stmt_reset(lua_State *lua) {
    LMYSQL_STMT_ARG(lua, stmt);
    size_t size;
    void *pack = mysql_pack_stmt_reset(*stmt, &size);
    return lpub_rtn_lud(lua, pack, size);
}
/// <summary>
/// 打包预处理语句关闭请求（COM_STMT_CLOSE，服务端不回响应）。
/// 不发则服务端句柄留到连接关闭才回收，长连接上会累积到 max_prepared_stmt_count 上限
/// </summary>
/// <param name="self" type="userdata">stmt 对象</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmysql_pack_stmt_close(lua_State *lua) {
    LMYSQL_STMT_ARG(lua, stmt);
    size_t size;
    void *pack = mysql_pack_stmt_close(*stmt, &size);
    return lpub_rtn_lud(lua, pack, size);
}
/// <summary>
/// 获取预处理语句所属连接的 fd 和 skid
/// </summary>
/// <param name="self" type="userdata">stmt 对象</param>
/// <returns type="userdata">连接标识；失败时其 valid 字段为 false</returns>
static int32_t _lmysql_stmt_sock_id(lua_State *lua) {
    LMYSQL_STMT_ARG(lua, stmt);
    lua_getiuservalue(lua, 1, 1);// 借宿主 mysql 句柄的缓存槽，槽 1 是 stmt 自己锚宿主用的
    lpub_push_sock_slot(lua, -1, &(*stmt)->mysql->client.sk);
    return 1;
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
        { "pack_stmt_close", _lmysql_pack_stmt_close },
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
/// <returns type="lightuserdata?">命令数据指针；库名超 256 字节时返回 nil</returns>
/// <returns type="integer?">数据长度；命令数据指针为 nil 时一并为 nil</returns>
static int32_t _lmysql_pack_selectdb(lua_State *lua) {
    LPUB_UD_ARG(lua, mysql_ctx, MT_MYSQL, ud, "mysql freed");
    const char *db = lpub_check_cstr(lua, 2);
    size_t size;
    void *pack = mysql_pack_selectdb(*ud, db, &size);
    return lpub_rtn_lud(lua, pack, size);
}
/// <summary>
/// 打包 COM_PING 心跳命令
/// </summary>
/// <param name="self" type="userdata">mysql 对象</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmysql_pack_ping(lua_State *lua) {
    LPUB_UD_ARG(lua, mysql_ctx, MT_MYSQL, ud, "mysql freed");
    size_t size;
    void *pack = mysql_pack_ping(*ud, &size);
    return lpub_rtn_lud(lua, pack, size);
}
/// <summary>
/// 打包查询命令（支持参数化绑定）
/// </summary>
/// <param name="self" type="userdata">mysql 对象</param>
/// <param name="sql" type="string">SQL 语句</param>
/// <param name="bind" type="userdata?">参数绑定上下文；nil 表示无参数，其余类型报错</param>
/// <returns type="lightuserdata?">命令数据指针；载荷超 16MB 时返回 nil</returns>
/// <returns type="integer?">数据长度；命令数据指针为 nil 时一并为 nil</returns>
static int32_t _lmysql_pack_query(lua_State *lua) {
    LPUB_UD_ARG(lua, mysql_ctx, MT_MYSQL, ud, "mysql freed");
    size_t sqllen;
    const char *sql = luaL_checklstring(lua, 2, &sqllen);
    mysql_bind_ctx *mbind = lua_isnoneornil(lua, 3) ? NULL : lpub_check_udata(lua, 3, MT_MYSQL_BIND);
    size_t size;
    void *pack = mysql_pack_query2(*ud, sql, sqllen, mbind, &size);
    return lpub_rtn_lud(lua, pack, size);
}
/// <summary>
/// 打包 COM_QUIT 断连命令
/// </summary>
/// <param name="self" type="userdata">mysql 对象</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmysql_pack_quit(lua_State *lua) {
    LPUB_UD_ARG(lua, mysql_ctx, MT_MYSQL, ud, "mysql freed");
    size_t size;
    void *pack = mysql_pack_quit(&size);
    return lpub_rtn_lud(lua, pack, size);
}
/// <summary>
/// 打包预处理语句准备命令（COM_STMT_PREPARE）
/// </summary>
/// <param name="self" type="userdata">mysql 对象</param>
/// <param name="sql" type="string">SQL 语句模板（含 ? 占位符）</param>
/// <returns type="lightuserdata?">命令数据指针；载荷超 16MB 时返回 nil</returns>
/// <returns type="integer?">数据长度；命令数据指针为 nil 时一并为 nil</returns>
static int32_t _lmysql_pack_stmt_prepare(lua_State *lua) {
    LPUB_UD_ARG(lua, mysql_ctx, MT_MYSQL, ud, "mysql freed");
    size_t sqllen;
    const char *sql = luaL_checklstring(lua, 2, &sqllen);
    size_t size;
    void *pack = mysql_pack_stmt_prepare2(*ud, sql, sqllen, &size);
    return lpub_rtn_lud(lua, pack, size);
}
/// <summary>
/// 创建 MySQL 客户端上下文（不立即建立连接）
/// </summary>
/// <param name="ip" type="string">服务器 IP</param>
/// <param name="port" type="integer">服务器端口</param>
/// <param name="evssl" type="lightuserdata|nil">SSL 上下文；nil 表示明文</param>
/// <param name="user" type="string">用户名，最长 128 字节</param>
/// <param name="password" type="string">密码，最长 256 字节。caching_sha2_password 账号在明文连接上遇到服务端缓存未命中(如服务端重启后首连)
/// 认证失败：本实现明文下不做 RSA 公钥交换。出路：配 SSL、先经 SSL 登录一次让服务端缓存口令，或换 mysql_native_password 账号(MySQL 8.4 默认关、9.0 已移除该插件)</param>
/// <param name="database" type="string">初始数据库，最长 256 字节</param>
/// <param name="charset" type="string">字符集（如 "utf8mb4"）</param>
/// <param name="maxpk" type="integer?">最大包大小（字节）；省略或非数字时使用默认</param>
/// <returns type="_mysql_ctx?">mysql 对象；ip / user / password / database 超长时返回 nil</returns>
static int32_t _lmysql_new(lua_State *lua) {
    const char *ip = lpub_check_cstr(lua, 1);
    uint16_t port = lpub_check_u16(lua, 2, PORT_OUT_OF_RANGE);
    struct evssl_ctx *evssl = lpub_check_evssl(lua, 3);
    const char *user = lpub_check_cstr(lua, 4);
    const char *password = lpub_check_cstr(lua, 5);
    const char *database = lpub_check_cstr(lua, 6);
    const char *charset = lpub_check_cstr(lua, 7);
    uint32_t maxpk = 0;
    if (LUA_TNUMBER == lua_type(lua, 8)) {
        maxpk = lpub_check_u32(lua, 8, "maxpack out of range");
    }
    mysql_ctx **ud = (mysql_ctx **)lpub_push_ud(lua, NULL, MT_MYSQL);
    mysql_ctx *mysql;
    MALLOC(mysql, sizeof(mysql_ctx));
    if (ERR_OK != mysql_init(mysql, ip, port, evssl, user, password, database, charset, maxpk)) {
        SECURE_FREE(mysql, sizeof(mysql_ctx));
        return lpub_rtn_nil(lua, 1);
    }
    ATOMIC_SET_RELAXED(&mysql->ref, 1);// Lua 持有者份额
    *ud = mysql;
    return 1;
}
/// <summary>
/// 获取 mpack 数据包的封包类型，收多结果集时省一次 has_more
/// </summary>
/// <param name="mpack" type="lightuserdata">mpack_ctx 数据包指针</param>
/// <returns type="integer">封包类型枚举值</returns>
/// <returns type="boolean">同 has_more：true=其后还有结果集需继续接收；false=已是最后一个</returns>
static int32_t _lmysql_pack_type(lua_State *lua) {
    LPUB_LUD_ARG(lua, mpack_ctx, 1, mpack);
    lua_pushinteger(lua, mpack->pack_type);
    lua_pushboolean(lua, mysql_more(mpack));
    return 2;
}
/// <summary>
/// 查询该响应包之后是否还有更多结果集（多语句 / 存储过程 CALL 多结果集）
/// </summary>
/// <param name="mpack" type="lightuserdata">mpack_ctx 数据包指针</param>
/// <returns type="boolean">true=其后还有结果集需继续接收；false=已是最后一个</returns>
static int32_t _lmysql_has_more(lua_State *lua) {
    LPUB_LUD_ARG(lua, mpack_ctx, 1, mpack);
    return lpub_rtn_bool(lua, mysql_more(mpack));
}
/// <summary>
/// 发送 QUIT 命令并清理连接上下文绑定（绑定为 __gc，由 Lua GC 自动调用）
/// </summary>
/// <param name="self" type="userdata">mysql 对象</param>
/// <returns>无</returns>
static int32_t _lmysql_free(lua_State *lua) {
    mysql_ctx **ud = lpub_check_udata(lua, 1, MT_MYSQL);
    mysql_ctx *mysql = *ud;
    if (NULL == mysql) {
        return 0;
    }
    if (NULL != mysql->task
        && !sock_is_invalid(&mysql->client.sk)) {
        size_t size;
        void *pack = mysql_pack_quit(&size);
        ev_send(&mysql->task->loader->netev, &mysql->client.sk, pack, size, 0);
        // 主动关连接：触发该 socket 的 udfree 释放事件侧份额，否则弃用的活连接块滞留至对端关
        ev_close(&mysql->task->loader->netev, &mysql->client.sk);
    }
    *ud = NULL;
    // mpack 由网络线程 udfree 释放，__gc 不碰(防跨线程 UAF)；
    // 密码与 salt 同理不在这里擦，擦除已挪进 PROT_REF_RELEASE
    PROT_REF_RELEASE(mysql);
    return 0;
}
/// <summary>
/// 尝试建立 MySQL 连接（异步）。用 srey.wait_connect(sk) 等连接建立、不传 ssl，再用 srey.wait_handshaked 等认证结果：
/// 配了 TLS 时由协议在握手中途自行升级，只收到 CONNECT 与 HANDSHAKED，不会收到 SSLEXCHANGED
/// </summary>
/// <param name="self" type="userdata">mysql 对象</param>
/// <returns type="boolean">发起成功 true，失败 false</returns>
static int32_t _lmysql_try_connect(lua_State *lua) {
    LPUB_UD_ARG(lua, mysql_ctx, MT_MYSQL, ud, "mysql freed");
    LPUB_CUR_TASK(lua, task);
    int32_t rtn = mysql_try_connect(task, *ud, 1);
    return lpub_rtn_bool(lua, ERR_OK == rtn);
}
/// <summary>
/// 返回 MySQL 服务端版本字符串
/// </summary>
/// <param name="self" type="userdata">mysql 对象</param>
/// <returns type="string">服务端版本</returns>
static int32_t _lmysql_version(lua_State *lua) {
    LPUB_UD_ARG(lua, mysql_ctx, MT_MYSQL, ud, "mysql freed");
    lua_pushstring(lua, mysql_version(*ud));
    return 1;
}
/// <summary>
/// 返回最近一个经 record / record_handshake 记下的 ERR 包错误并清除错误状态；成功的命令不清它。
/// 多协程共用连接时须在命令返回后、本协程下次挂起之前读
/// </summary>
/// <param name="self" type="userdata">mysql 对象</param>
/// <returns type="string">错误信息（服务端文案，随版本与 locale 变）</returns>
/// <returns type="integer">MySQL 错误号(0~65535)；无错误时为 0。要区分可重试（1213 死锁 / 1205 锁等待
/// 超时）与不可重试（1062 重复键）只能靠它，文案不可依赖</returns>
static int32_t _lmysql_erro(lua_State *lua) {
    LPUB_UD_ARG(lua, mysql_ctx, MT_MYSQL, ud, "mysql freed");
    mysql_ctx *mysql = *ud;
    int32_t code = 0;
    lua_pushstring(lua, mysql_erro(mysql, &code));
    lua_pushinteger(lua, code);
    mysql_erro_clear(mysql);
    return 2;
}
/// <summary>
/// 返回当前 MySQL 连接的 fd 和 skid
/// </summary>
/// <param name="self" type="userdata">mysql 对象</param>
/// <returns type="userdata">连接标识；失败时其 valid 字段为 false</returns>
static int32_t _lmysql_sock_id(lua_State *lua) {
    LPUB_UD_ARG(lua, mysql_ctx, MT_MYSQL, ud, "mysql freed");
    mysql_ctx *mysql = *ud;
    lpub_push_sock_slot(lua, 1, &mysql->client.sk);
    return 1;
}
// last_id / affectd_rows 的收尾：值超出 int64 时打 WARN（带 what）并返回 false，同 reader:integer
static int32_t _lmysql_rtn_count(lua_State *lua, uint64_t val, const char *what) {
    if (val > (uint64_t)INT64_MAX) {
        LOG_WARN("mysql %s exceeds int64.", what);
        return lpub_rtn_bool(lua, 0);
    }
    lua_pushinteger(lua, (lua_Integer)val);
    return 1;
}
/// <summary>
/// 返回最近一个经 record 记下的 OK 包自增 id（非 INSERT 语句为 0，ERR 包与结果集不改它）；值超出 int64 时打 WARN。
/// 读取时机同 erro
/// </summary>
/// <param name="self" type="userdata">mysql 对象</param>
/// <returns type="integer|false">last insert id；超出 int64 时为 false</returns>
static int32_t _lmysql_last_id(lua_State *lua) {
    LPUB_UD_ARG(lua, mysql_ctx, MT_MYSQL, ud, "mysql freed");
    return _lmysql_rtn_count(lua, mysql_last_id(*ud), "last_id");
}
/// <summary>
/// 返回最近一个经 record 记下的 OK 包影响行数（ERR 包与结果集不改它）；值超出 int64 时打 WARN。读取时机同 erro
/// </summary>
/// <param name="self" type="userdata">mysql 对象</param>
/// <returns type="integer|false">affected rows；超出 int64 时为 false</returns>
static int32_t _lmysql_affectd_rows(lua_State *lua) {
    LPUB_UD_ARG(lua, mysql_ctx, MT_MYSQL, ud, "mysql freed");
    return _lmysql_rtn_count(lua, mysql_affected_rows(*ud), "affected_rows");
}
/// <summary>
/// 把 OK 包的影响行数与自增 id、或 ERR 包的错误记到本连接上，供 last_id / affectd_rows / erro 读；其余类型的包不改它们。
/// 只在持本连接串行化执行权、刚收到该包的协程里调
/// </summary>
/// <param name="self" type="userdata">mysql 对象</param>
/// <param name="mpack" type="lightuserdata">本连接收到的 mpack_ctx 数据包指针</param>
/// <returns>无</returns>
static int32_t _lmysql_record(lua_State *lua) {
    LPUB_UD_ARG(lua, mysql_ctx, MT_MYSQL, ud, "mysql freed");
    LPUB_LUD_ARG(lua, mpack_ctx, 2, mpack);
    mysql_record(*ud, mpack);
    return 0;
}
/// <summary>
/// 把握手失败时 wait_handshaked 交出的 ERR 包体解析后记到本连接上，之后 erro 能取到错误号与文案。
/// 只在持本连接串行化执行权的建连协程里调
/// </summary>
/// <param name="self" type="userdata">mysql 对象</param>
/// <param name="body" type="lightuserdata?">握手失败载荷；nil 时什么都不做</param>
/// <param name="size" type="integer?">载荷字节数；body 为 lightuserdata 时必填</param>
/// <returns type="string">记下的错误串；没有载荷时为空串</returns>
static int32_t _lmysql_record_handshake(lua_State *lua) {
    LPUB_UD_ARG(lua, mysql_ctx, MT_MYSQL, ud, "mysql freed");
    const char *body = NULL;
    size_t lens = 0;
    if (!lua_isnoneornil(lua, 2)) {
        LUACHECK_LUDATA(lua, 2);
        body = lua_touserdata(lua, 2);
        lens = lpub_check_lens(lua, 3, 0);
    }
    lua_pushstring(lua, mysql_record_handshake(*ud, body, lens));
    return 1;
}
/// <summary>
/// 取 OK 包带的影响行数，多结果集时每条语句各自一份；值超出 int64 时打 WARN
/// </summary>
/// <param name="mpack" type="lightuserdata">mpack_ctx 数据包指针</param>
/// <returns type="integer|false">affected rows，不是 OK 包时为 0；超出 int64 时为 false</returns>
static int32_t _lmysql_pack_affected_rows(lua_State *lua) {
    LPUB_LUD_ARG(lua, mpack_ctx, 1, mpack);
    return _lmysql_rtn_count(lua, mysql_pack_affected_rows(mpack), "affected_rows");
}
/// <summary>
/// 取 OK 包带的自增 id，多结果集时每条语句各自一份；值超出 int64 时打 WARN
/// </summary>
/// <param name="mpack" type="lightuserdata">mpack_ctx 数据包指针</param>
/// <returns type="integer|false">last insert id，非 INSERT 或不是 OK 包时为 0；超出 int64 时为 false</returns>
static int32_t _lmysql_pack_last_id(lua_State *lua) {
    LPUB_LUD_ARG(lua, mpack_ctx, 1, mpack);
    return _lmysql_rtn_count(lua, mysql_pack_last_id(mpack), "last_id");
}
/// <summary>
/// 取 ERR 包带的错误
/// </summary>
/// <param name="mpack" type="lightuserdata">mpack_ctx 数据包指针</param>
/// <returns type="string">错误信息，没有时(含不是 ERR 包)为空串</returns>
/// <returns type="integer">MySQL 错误号(0~65535)；不是 ERR 包时为 0</returns>
static int32_t _lmysql_pack_erro(lua_State *lua) {
    LPUB_LUD_ARG(lua, mpack_ctx, 1, mpack);
    int32_t code = 0;
    lua_pushstring(lua, mysql_pack_erro(mpack, &code));
    lua_pushinteger(lua, code);
    return 2;
}
//mysql
LUAMOD_API int luaopen_mysql(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lmysql_new },
        { "pack_type", _lmysql_pack_type },
        { "has_more", _lmysql_has_more },
        { "pack_affected_rows", _lmysql_pack_affected_rows },
        { "pack_last_id", _lmysql_pack_last_id },
        { "pack_erro", _lmysql_pack_erro },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "record", _lmysql_record },
        { "record_handshake", _lmysql_record_handshake },
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
