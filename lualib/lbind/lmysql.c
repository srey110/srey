#include "lbind/lpub.h"

#define MT_MYSQL_BIND   "_mysql_bind_ctx"
#define MT_MYSQL_READER "_mysql_reader_ctx"
#define MT_MYSQL_STMT   "_mysql_stmt_ctx"
#define MT_MYSQL        "_mysql_ctx"
// stmt 的四个入口共用: 自身非空 + 宿主还活着。后一半的理由见 lpub_owner_ptr
#define LMYSQL_STMT_ARG(lua, var) \
    LPUB_UD_ARG((lua), mysql_stmt_ctx, MT_MYSQL_STMT, var, "stmt freed") \
    if (NULL == lpub_owner_ptr((lua), MT_MYSQL)) { \
        return luaL_error((lua), "mysql stmt: owner mysql already freed"); \
    }
// 六个按列名取值的 reader 入口共用的开场白：取 reader + 取列名 + 备好 err。
// err 是三态(ERR_OK 有值 / 1 字段是 SQL NULL / 其余读取失败)，由 mysql_reader、pgsql_reader 两侧
// 共同产出：有值那支自己压 true 加值，另两态一律 lpub_rtn_bool(lua, 1 == err) 收尾（NULL 也算成功）
#define LMYSQL_READER_GET(lua, rvar, nvar, evar) \
    LPUB_UD_ARG((lua), mysql_reader_ctx, MT_MYSQL_READER, rvar, "reader freed") \
    const char *nvar = luaL_checkstring((lua), 2); \
    int32_t evar
// 七个 bind 入口共用的开场白: 取 bind 对象 + 取可选具名参数(栈位 2 非字符串即按位置绑定)。
// 具名参数的取法散在七处的话, 将来要换取法(如改用 luaL_optlstring 拿长度)得挨个找齐,
// 改漏一个不会有编译期信号 —— 那个 bind 会静默退化成按位置绑定, 参数错位写进 MySQL
#define LMYSQL_BIND_ARG(lua, bindvar, namevar) \
    mysql_bind_ctx *bindvar = luaL_checkudata((lua), 1, MT_MYSQL_BIND); \
    char *namevar = NULL; \
    if (LUA_TSTRING == lua_type((lua), 2)) { \
        namevar = (char *)luaL_checkstring((lua), 2); \
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
    LMYSQL_BIND_ARG(lua, mbind, name);
    mysql_bind_nil(mbind, name);
    return 0;
}
/// <summary>
/// 绑定字符串参数（data 为 nil 时自动转为 NULL 绑定）
/// </summary>
/// <param name="self" type="userdata">bind 对象</param>
/// <param name="name" type="string?">具名参数名；nil 表示按位置绑定</param>
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
/// <param name="name" type="string?">具名参数名；nil 表示按位置绑定</param>
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
/// <param name="name" type="string?">具名参数名；nil 表示按位置绑定</param>
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
/// <param name="name" type="string?">具名参数名；nil 表示按位置绑定</param>
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
/// <param name="name" type="string?">具名参数名；nil 表示按位置绑定</param>
/// <param name="ts" type="integer|nil">Unix 时间戳（秒）；非 number 视为 NULL</param>
/// <returns>无</returns>
static int32_t _lmysql_bind_datetime(lua_State *lua) {
    LMYSQL_BIND_ARG(lua, mbind, name);
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
    LPUB_UD_ARG(lua, mysql_reader_ctx, MT_MYSQL_READER, reader, "reader freed");
    lua_pushinteger(lua, mysql_reader_size(*reader));
    return 1;
}
/// <summary>
/// 定位到指定行位置
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <param name="pos" type="integer">目标行下标，取值 [0, INT32_MAX]，越界报错</param>
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
/// <summary>
/// 读取当前行指定字段的整数值
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <param name="name" type="string">字段名</param>
/// <returns type="boolean">true 表示读取成功（含字段为 NULL）；false 表示读取失败</returns>
/// <returns type="integer?">字段整数值；字段为 NULL 时不返回此值</returns>
static int32_t _lmysql_reader_integer(lua_State *lua) {
    LMYSQL_READER_GET(lua, reader, name, err);
    int64_t val = mysql_reader_integer(*reader, name, &err);
    if (ERR_OK == err) {
        lua_pushboolean(lua, 1);
        lua_pushinteger(lua, val);
        return 2;
    }
    return lpub_rtn_bool(lua, 1 == err);
}
/// <summary>
/// 读取当前行指定字段的单精度浮点值
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <param name="name" type="string">字段名</param>
/// <returns type="boolean">true 表示读取成功（含字段为 NULL）；false 表示读取失败</returns>
/// <returns type="number?">字段单精度浮点值；字段为 NULL 时不返回此值</returns>
static int32_t _lmysql_reader_float(lua_State *lua) {
    LMYSQL_READER_GET(lua, reader, name, err);
    float val = mysql_reader_float(*reader, name, &err);
    if (ERR_OK == err) {
        lua_pushboolean(lua, 1);
        lua_pushnumber(lua, (double)val);
        return 2;
    }
    return lpub_rtn_bool(lua, 1 == err);
}
/// <summary>
/// 读取当前行指定字段的双精度浮点值
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <param name="name" type="string">字段名</param>
/// <returns type="boolean">true 表示读取成功（含字段为 NULL）；false 表示读取失败</returns>
/// <returns type="number?">字段双精度浮点值；字段为 NULL 时不返回此值</returns>
static int32_t _lmysql_reader_double(lua_State *lua) {
    LMYSQL_READER_GET(lua, reader, name, err);
    double val = mysql_reader_double(*reader, name, &err);
    if (ERR_OK == err) {
        lua_pushboolean(lua, 1);
        lua_pushnumber(lua, val);
        return 2;
    }
    return lpub_rtn_bool(lua, 1 == err);
}
/// <summary>
/// 读取当前行指定字段的字符串值（返回 lightuserdata + 长度）
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <param name="name" type="string">字段名</param>
/// <returns type="boolean">true 表示读取成功（含字段为 NULL）；false 表示读取失败</returns>
/// <returns type="lightuserdata?">字段数据指针（reader 内部行缓冲的**借用**指针，随 reader 释放而失效，
/// 调用方既不拥有它、也不能对它调 utils.ud_free 或以 copy=0 交给 srey.send）；字段为 NULL 时不返回此值</returns>
/// <returns type="integer?">字段字节数；字段为 NULL 时不返回此值</returns>
static int32_t _lmysql_reader_string(lua_State *lua) {
    LMYSQL_READER_GET(lua, reader, name, err);
    size_t lens = 0;
    char *val = mysql_reader_string(*reader, name, &lens, &err);
    if (ERR_OK == err) {
        lua_pushboolean(lua, 1);
        lua_pushlightuserdata(lua, val);
        lua_pushinteger(lua, lens);
        return 3;
    }
    return lpub_rtn_bool(lua, 1 == err);
}
/// <summary>
/// 读取当前行指定字段的 DATETIME 值（微秒精度）
/// </summary>
/// <param name="self" type="userdata">reader 对象</param>
/// <param name="name" type="string">字段名</param>
/// <returns type="boolean">true 表示读取成功（含字段为 NULL）；false 表示读取失败</returns>
/// <returns type="integer?">微秒精度 Unix 时间戳；字段为 NULL 时不返回此值</returns>
static int32_t _lmysql_reader_datetime(lua_State *lua) {
    LMYSQL_READER_GET(lua, reader, name, err);
    int64_t val = mysql_reader_datetime(*reader, name, &err);
    if (ERR_OK == err) {
        lua_pushboolean(lua, 1);
        lua_pushinteger(lua, val);
        return 2;
    }
    return lpub_rtn_bool(lua, 1 == err);
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
    LMYSQL_READER_GET(lua, reader, name, err);
    struct tm dt = { 0 };
    uint32_t usec;
    int32_t is_negative = mysql_reader_time(*reader, name, &dt, &usec, &err);
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
/// 从 mpack 数据包中创建预处理语句上下文，并把 mysql 对象锚定为 uservalue 防其先于 stmt 被 GC
/// </summary>
/// <param name="mysql" type="userdata">所属 mysql 对象（锚定为 uservalue 保活）</param>
/// <param name="mpack" type="lightuserdata">mpack_ctx 数据包指针</param>
/// <returns type="_mysql_stmt_ctx?">stmt 对象；失败返回 nil</returns>
static int32_t _lmysql_stmt_new(lua_State *lua) {
    LPUB_UD_ARG(lua, mysql_ctx, MT_MYSQL, ud, "mysql freed");
    LPUB_LUD_ARG(lua, mpack_ctx, 2, mpack);
    mysql_stmt_ctx *stmt = mysql_stmt_init(mpack);
    if (NULL == stmt) {
        return lpub_rtn_nil(lua, 1);
    }
    lpub_push_ud(lua, stmt, MT_MYSQL_STMT);
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
    mysql_stmt_ctx **stmt = luaL_checkudata(lua, 1, MT_MYSQL_STMT);
    if (NULL != *stmt) {
        // mysql_stmt_free 只碰 stmt 自己的 params / fields，不读 stmt->mysql——
        // 宿主先被 m:__gc() 释放过也无妨。走 mysql_stmt_close 则会读 mysql->serial
        mysql_stmt_free(*stmt);
        *stmt = NULL;
    }
    return 0;
}
/// <summary>
/// 打包预处理语句执行请求
/// </summary>
/// <param name="self" type="userdata">stmt 对象</param>
/// <param name="bind" type="userdata?">参数绑定上下文；nil 表示无参数</param>
/// <returns type="lightuserdata">命令数据指针；组包失败直接抛出，不返回 nil（调用方契约违反，运行期无从降级，
/// 判定依据见 lpub_rtn_lud 的说明）。两种失败 C 层都只给 NULL+size 0，无从区分，故文案并列：
/// 参数个数对不上（含声明了参数而 bind 为 nil），或组完的载荷超 16MB（这条 C 层另有一条 LOG_WARN）</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmysql_pack_stmt_execute(lua_State *lua) {
    LMYSQL_STMT_ARG(lua, stmt);
    mysql_bind_ctx *mbind = NULL;
    if (LUA_TUSERDATA == lua_type(lua, 2)) {
        mbind = luaL_checkudata(lua, 2, MT_MYSQL_BIND);
    }
    size_t size;
    void *pack = mysql_pack_stmt_execute(*stmt, mbind, &size);
    if (NULL == pack) {
        return luaL_error(lua, "stmt_execute pack failed: bind count mismatch, or payload exceeds 16MB (see log).");
    }
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
/// 不发这条则服务端那份语句句柄要留到连接关闭才回收，长连接上逐次累积会撞
/// max_prepared_stmt_count（默认 16382），此后每次 prepare 都失败
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
/// <returns type="integer">socket fd</returns>
/// <returns type="integer">skid</returns>
static int32_t _lmysql_stmt_sock_id(lua_State *lua) {
    LMYSQL_STMT_ARG(lua, stmt);
    lua_pushinteger(lua, (*stmt)->mysql->client.sk.fd);
    lua_pushinteger(lua, (*stmt)->mysql->client.sk.skid);
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
/// <returns type="lightuserdata?">命令数据指针；库名超 63 字节时返回 nil</returns>
/// <returns type="integer?">数据长度；命令数据指针为 nil 时一并为 nil</returns>
static int32_t _lmysql_pack_selectdb(lua_State *lua) {
    LPUB_UD_ARG(lua, mysql_ctx, MT_MYSQL, ud, "mysql freed");
    const char *db = luaL_checkstring(lua, 2);
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
/// <param name="bind" type="userdata?">参数绑定上下文；nil 表示无参数</param>
/// <returns type="lightuserdata?">命令数据指针；载荷超 16MB 时返回 nil</returns>
/// <returns type="integer?">数据长度；命令数据指针为 nil 时一并为 nil</returns>
static int32_t _lmysql_pack_query(lua_State *lua) {
    LPUB_UD_ARG(lua, mysql_ctx, MT_MYSQL, ud, "mysql freed");
    const char *sql = luaL_checkstring(lua, 2);
    mysql_bind_ctx *mbind = NULL;
    if (LUA_TUSERDATA == lua_type(lua, 3)){
        mbind = luaL_checkudata(lua, 3, MT_MYSQL_BIND);
    }
    size_t size;
    void *pack = mysql_pack_query(*ud, sql, mbind, &size);
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
    const char *sql = luaL_checkstring(lua, 2);
    size_t size;
    void *pack = mysql_pack_stmt_prepare(*ud, sql, &size);
    return lpub_rtn_lud(lua, pack, size);
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
    uint16_t port = lpub_check_u16(lua, 2, PORT_OUT_OF_RANGE);
    struct evssl_ctx *evssl = lpub_check_evssl(lua, 3);
    const char *user = luaL_checkstring(lua, 4);
    const char *password = luaL_checkstring(lua, 5);
    const char *database = luaL_checkstring(lua, 6);
    const char *charset = luaL_checkstring(lua, 7);
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
    ATOMIC_SET(&mysql->ref, 1);// Lua 持有者份额
    *ud = mysql;
    return 1;
}
/// <summary>
/// 获取 mpack 数据包的封包类型
/// </summary>
/// <param name="mpack" type="lightuserdata">mpack_ctx 数据包指针</param>
/// <returns type="integer">封包类型枚举值</returns>
static int32_t _lmysql_pack_type(lua_State *lua) {
    LPUB_LUD_ARG(lua, mpack_ctx, 1, mpack);
    lua_pushinteger(lua, mpack->pack_type);
    return 1;
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
    mysql_ctx **ud = luaL_checkudata(lua, 1, MT_MYSQL);
    mysql_ctx *mysql = *ud;
    if (NULL == mysql) {
        return 0;
    }
    if (NULL != mysql->task
        && INVALID_SOCK != mysql->client.sk.fd) {
        size_t size;
        void *pack = mysql_pack_quit(&size);
        ev_send(&mysql->task->loader->netev, mysql->client.sk.fd, mysql->client.sk.skid, pack, size, 0);
        // 主动关连接：触发该 socket 的 udfree 释放事件侧份额，否则弃用的活连接块滞留至对端关
        ev_close(&mysql->task->loader->netev, mysql->client.sk.fd, mysql->client.sk.skid);
    }
    *ud = NULL;
    // mpack 由网络线程 udfree 释放，__gc 不碰(防跨线程 UAF)；
    // 密码与 salt 同理不在这里擦，擦除已挪进 PROT_REF_RELEASE
    PROT_REF_RELEASE(mysql);
    return 0;
}
/// <summary>
/// 尝试建立 MySQL 连接（异步）
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
/// 返回最近一次错误信息并清除错误状态
/// </summary>
/// <param name="self" type="userdata">mysql 对象</param>
/// <returns type="string">错误信息（服务端文案，随版本与 locale 变）</returns>
/// <returns type="integer">MySQL 错误号；无错误时为 0。要区分可重试（1213 死锁 / 1205 锁等待
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
/// <returns type="integer">socket fd</returns>
/// <returns type="integer">skid</returns>
static int32_t _lmysql_sock_id(lua_State *lua) {
    LPUB_UD_ARG(lua, mysql_ctx, MT_MYSQL, ud, "mysql freed");
    mysql_ctx *mysql = *ud;
    lua_pushinteger(lua, mysql->client.sk.fd);
    lua_pushinteger(lua, mysql->client.sk.skid);
    return 2;
}
/// <summary>
/// 返回最后一次 INSERT 操作生成的自增 id
/// </summary>
/// <param name="self" type="userdata">mysql 对象</param>
/// <returns type="integer">last insert id</returns>
static int32_t _lmysql_last_id(lua_State *lua) {
    LPUB_UD_ARG(lua, mysql_ctx, MT_MYSQL, ud, "mysql freed");
    lua_pushinteger(lua, mysql_last_id(*ud));
    return 1;
}
/// <summary>
/// 返回最后一次 UPDATE / DELETE / INSERT 操作影响的行数
/// </summary>
/// <param name="self" type="userdata">mysql 对象</param>
/// <returns type="integer">affected rows</returns>
static int32_t _lmysql_affectd_rows(lua_State *lua) {
    LPUB_UD_ARG(lua, mysql_ctx, MT_MYSQL, ud, "mysql freed");
    lua_pushinteger(lua, mysql_affected_rows(*ud));
    return 1;
}
//mysql
LUAMOD_API int luaopen_mysql(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lmysql_new },
        { "pack_type", _lmysql_pack_type },
        { "has_more", _lmysql_has_more },
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
