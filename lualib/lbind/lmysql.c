#include "lbind/lpub.h"

// Lua 绑定：创建 MySQL 参数绑定上下文（用于预处理语句或查询参数化）
static int32_t _lmysql_bind_new(lua_State *lua) {
    mysql_bind_ctx *mbind = lua_newuserdata(lua, sizeof(mysql_bind_ctx));
    mysql_bind_init(mbind);
    ASSOC_MTABLE(lua, "_mysql_bind_ctx");
    return 1;
}
// Lua 绑定（__gc）：释放绑定上下文内部资源
static int32_t _lmysql_bind_free(lua_State *lua) {
    mysql_bind_ctx *mbind = lua_touserdata(lua, 1);
    mysql_bind_free(mbind);
    return 0;
}
// Lua 绑定：清空所有已绑定参数，可复用上下文
static int32_t _lmysql_bind_clear(lua_State *lua) {
    mysql_bind_ctx *mbind = lua_touserdata(lua, 1);
    mysql_bind_clear(mbind);
    return 0;
}
// Lua 绑定：绑定一个 NULL 参数；name 为具名参数名（可选）
static int32_t _lmysql_bind_nil(lua_State *lua) {
    mysql_bind_ctx *mbind = lua_touserdata(lua, 1);
    char *name = NULL;
    if (LUA_TSTRING == lua_type(lua, 2)) {
        name = (char *)luaL_checkstring(lua, 2);
    }
    mysql_bind_nil(mbind, name);
    return 0;
}
// Lua 绑定：绑定字符串参数；data 为 nil 时自动转为 NULL 绑定
static int32_t _lmysql_bind_string(lua_State *lua) {
    mysql_bind_ctx *mbind = lua_touserdata(lua, 1);
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
// Lua 绑定：绑定整数参数；非数字/布尔类型时自动转为 NULL 绑定
static int32_t _lmysql_bind_integer(lua_State *lua) {
    mysql_bind_ctx *mbind = lua_touserdata(lua, 1);
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
// Lua 绑定：绑定浮点数参数；非数字类型时自动转为 NULL 绑定
static int32_t _lmysql_bind_double(lua_State *lua) {
    mysql_bind_ctx *mbind = lua_touserdata(lua, 1);
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
// Lua 绑定：绑定 DATETIME 参数，ts 为 Unix 时间戳（秒）；非数字时转为 NULL
static int32_t _lmysql_bind_datetime(lua_State *lua) {
    mysql_bind_ctx *mbind = lua_touserdata(lua, 1);
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
// Lua 绑定：绑定 TIME 参数，支持负值时间段；参数为 is_negative/days/hour/minute/second
static int32_t _lmysql_bind_time(lua_State *lua) {
    mysql_bind_ctx *mbind = lua_touserdata(lua, 1);
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
        { "double", _lmysql_bind_double },
        { "datetime", _lmysql_bind_datetime },
        { "time", _lmysql_bind_time },
        { "__gc", _lmysql_bind_free },
        { NULL, NULL }
    };
    REG_MTABLE(lua, "_mysql_bind_ctx", reg_new, reg_func);
    return 1;
}
// Lua 绑定：从 mpack 数据包中创建结果集读取器，失败返回 nil
static int32_t _lmysql_reader_new(lua_State *lua) {
    mpack_ctx *mpack = lua_touserdata(lua, 1);
    mysql_reader_ctx *reader = mysql_reader_init(mpack);
    if (NULL == reader) {
        lua_pushnil(lua);
        return 1;
    }
    mysql_reader_ctx **rd = lua_newuserdata(lua, sizeof(mysql_reader_ctx *));
    *rd = reader;
    ASSOC_MTABLE(lua, "_mysql_reader_ctx");
    return 1;
}
// Lua 绑定（__gc）：释放结果集读取器资源
static int32_t _lmysql_reader_free(lua_State *lua) {
    mysql_reader_ctx **reader = lua_touserdata(lua, 1);
    mysql_reader_free(*reader);
    return 0;
}
// Lua 绑定：返回结果集总行数
static int32_t _lmysql_reader_size(lua_State *lua) {
    mysql_reader_ctx **reader = lua_touserdata(lua, 1);
    lua_pushinteger(lua, mysql_reader_size(*reader));
    return 1;
}
// Lua 绑定：定位到指定行位置
static int32_t _lmysql_reader_seek(lua_State *lua) {
    mysql_reader_ctx **reader = lua_touserdata(lua, 1);
    size_t pos = (size_t)luaL_checkinteger(lua, 2);
    mysql_reader_seek(*reader, pos);
    return 0;
}
// Lua 绑定：判断是否已到达结果集末尾，返回布尔值
static int32_t _lmysql_reader_eof(lua_State *lua) {
    mysql_reader_ctx **reader = lua_touserdata(lua, 1);
    lua_pushboolean(lua, mysql_reader_eof(*reader));
    return 1;
}
// Lua 绑定：移动到下一行
static int32_t _lmysql_reader_next(lua_State *lua) {
    mysql_reader_ctx **reader = lua_touserdata(lua, 1);
    mysql_reader_next(*reader);
    return 0;
}
// Lua 绑定：读取当前行指定字段的整数值；成功返回 true + 整数，字段为 NULL 返回 true，失败返回 false
static int32_t _lmysql_reader_integer(lua_State *lua) {
    mysql_reader_ctx **reader = lua_touserdata(lua, 1);
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
// Lua 绑定：读取当前行指定字段的浮点数值；成功返回 true + 浮点数，字段为 NULL 返回 true，失败返回 false
static int32_t _lmysql_reader_double(lua_State *lua) {
    mysql_reader_ctx **reader = lua_touserdata(lua, 1);
    const char *name = luaL_checkstring(lua, 2);
    int32_t err;
    double val = mysql_reader_double(*reader, name, &err);
    if (ERR_OK == err) {
        lua_pushboolean(lua, 1);
        lua_pushnumber(lua, val);
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
// Lua 绑定：读取当前行指定字段的字符串值；成功返回 true + userdata + 长度，字段为 NULL 返回 true，失败返回 false
static int32_t _lmysql_reader_string(lua_State *lua) {
    mysql_reader_ctx **reader = lua_touserdata(lua, 1);
    const char *name = luaL_checkstring(lua, 2);
    int32_t err;
    size_t lens;
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
// Lua 绑定：读取当前行指定字段的 DATETIME 值（转换为 Unix 时间戳）；
// 成功返回 true + 整数，字段为 NULL 返回 true，失败返回 false
static int32_t _lmysql_reader_datetime(lua_State *lua) {
    mysql_reader_ctx **reader = lua_touserdata(lua, 1);
    const char *name = luaL_checkstring(lua, 2);
    int32_t err;
    uint64_t val = mysql_reader_datetime(*reader, name, &err);
    if (ERR_OK == err) {
        lua_pushboolean(lua, 1);
        lua_pushinteger(lua, (int64_t)val);
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
// Lua 绑定：读取当前行指定字段的 TIME 值；
// 成功返回 true + is_negative + days + hour + min + sec，字段为 NULL 返回 true，失败返回 false
static int32_t _lmysql_reader_time(lua_State *lua) {
    mysql_reader_ctx **reader = lua_touserdata(lua, 1);
    const char *name = luaL_checkstring(lua, 2);
    int32_t err;
    struct tm dt;
    int32_t is_negative = mysql_reader_time(*reader, name, &dt, &err);
    if (ERR_OK == err) {
        lua_pushboolean(lua, 1);
        lua_pushboolean(lua, is_negative);
        lua_pushinteger(lua, dt.tm_mday);  // 天数（TIME 类型用 tm_mday 表示）
        lua_pushinteger(lua, dt.tm_hour);
        lua_pushinteger(lua, dt.tm_min);
        lua_pushinteger(lua, dt.tm_sec);
        return 6;
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
        { "double", _lmysql_reader_double },
        { "string", _lmysql_reader_string },
        { "datetime", _lmysql_reader_datetime },
        { "time", _lmysql_reader_time },
        { "__gc", _lmysql_reader_free },
        { NULL, NULL }
    };
    REG_MTABLE(lua, "_mysql_reader_ctx", reg_new, reg_func);
    return 1;
}
// Lua 绑定：从 mpack 数据包中创建预处理语句上下文，失败返回 nil
static int32_t _lmysql_stmt_new(lua_State *lua) {
    mpack_ctx *mpack = lua_touserdata(lua, 1);
    mysql_stmt_ctx *stmt = mysql_stmt_init(mpack);
    if (NULL == stmt) {
        lua_pushnil(lua);
        return 1;
    }
    mysql_stmt_ctx **st = lua_newuserdata(lua, sizeof(mysql_stmt_ctx *));
    *st = stmt;
    ASSOC_MTABLE(lua, "_mysql_stmt_ctx");
    return 1;
}
// Lua 绑定（__gc）：关闭预处理语句并释放资源
static int32_t _lmysql_stmt_free(lua_State *lua) {
    mysql_stmt_ctx **stmt = lua_touserdata(lua, 1);
    mysql_stmt_close(*stmt);
    return 0;
}
// Lua 绑定：打包预处理语句执行请求，可附带参数绑定上下文；返回数据指针和长度
static int32_t _lmysql_pack_stmt_execute(lua_State *lua) {
    mysql_stmt_ctx **stmt = lua_touserdata(lua, 1);
    mysql_bind_ctx *mbind = NULL;
    if (LUA_TUSERDATA == lua_type(lua, 2)) {
        mbind = lua_touserdata(lua, 2);
    }
    size_t size;
    void *pack = mysql_pack_stmt_execute(*stmt, mbind, &size);
    lua_pushlightuserdata(lua, pack);
    lua_pushinteger(lua, size);
    return 2;
}
// Lua 绑定：打包预处理语句重置请求；返回数据指针和长度
static int32_t _lmysql_pack_stmt_reset(lua_State *lua) {
    mysql_stmt_ctx **stmt = lua_touserdata(lua, 1);
    size_t size;
    void *pack = mysql_pack_stmt_reset(*stmt, &size);
    lua_pushlightuserdata(lua, pack);
    lua_pushinteger(lua, size);
    return 2;
}
// Lua 绑定：获取预处理语句所属连接的 fd 和 skid
static int32_t _lmysql_stmt_sock_id(lua_State *lua) {
    mysql_stmt_ctx **stmt = lua_touserdata(lua, 1);
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
    REG_MTABLE(lua, "_mysql_stmt_ctx", reg_new, reg_func);
    return 1;
}
// Lua 绑定：打包 COM_INIT_DB（切换数据库）命令；返回数据指针和长度
static int32_t _lmysql_pack_selectdb(lua_State *lua) {
    mysql_ctx *mysql = lua_touserdata(lua, 1);
    const char *db = luaL_checkstring(lua, 2);
    size_t size;
    void *pack = mysql_pack_selectdb(mysql, db, &size);
    lua_pushlightuserdata(lua, pack);
    lua_pushinteger(lua, size);
    return 2;
}
// Lua 绑定：打包 COM_PING 心跳命令；返回数据指针和长度
static int32_t _lmysql_pack_ping(lua_State *lua) {
    mysql_ctx *mysql = lua_touserdata(lua, 1);
    size_t size;
    void *pack = mysql_pack_ping(mysql, &size);
    lua_pushlightuserdata(lua, pack);
    lua_pushinteger(lua, size);
    return 2;
}
// Lua 绑定：打包查询命令（支持参数化绑定）；返回数据指针和长度
static int32_t _lmysql_pack_query(lua_State *lua) {
    mysql_ctx *mysql = lua_touserdata(lua, 1);
    const char *sql = luaL_checkstring(lua, 2);
    mysql_bind_ctx *mbind = NULL;
    if (LUA_TUSERDATA == lua_type(lua, 3)){
        mbind = lua_touserdata(lua, 3);
    }
    size_t size;
    void *pack = mysql_pack_query(mysql, sql, mbind, &size);
    lua_pushlightuserdata(lua, pack);
    lua_pushinteger(lua, size);
    return 2;
}
// Lua 绑定：打包 COM_QUIT 断连命令；返回数据指针和长度
static int32_t _lmysql_pack_quit(lua_State *lua) {
    mysql_ctx *mysql = lua_touserdata(lua, 1);
    size_t size;
    void *pack = mysql_pack_quit(mysql, &size);
    lua_pushlightuserdata(lua, pack);
    lua_pushinteger(lua, size);
    return 2;
}
// Lua 绑定：打包预处理语句准备命令；返回数据指针和长度
static int32_t _lmysql_pack_stmt_prepare(lua_State *lua) {
    mysql_ctx *mysql = lua_touserdata(lua, 1);
    const char *sql = luaL_checkstring(lua, 2);
    size_t size;
    void *pack = mysql_pack_stmt_prepare(mysql, sql, &size);
    lua_pushlightuserdata(lua, pack);
    lua_pushinteger(lua, size);
    return 2;
}
// Lua 绑定：创建 MySQL 客户端上下文，初始化连接参数（不立即建立连接）
static int32_t _lmysql_new(lua_State *lua) {
    const char *ip = luaL_checkstring(lua, 1);
    uint16_t port = (uint16_t)luaL_checkinteger(lua, 2);
    struct evssl_ctx *evssl = lua_touserdata(lua, 3);
    const char *user = luaL_checkstring(lua, 4);
    const char *password = luaL_checkstring(lua, 5);
    const char *database = luaL_checkstring(lua, 6);
    const char *charset = luaL_checkstring(lua, 7);
    uint32_t maxpk = 0;
    if (LUA_TNUMBER == lua_type(lua, 8)) {
        maxpk = (uint32_t)luaL_checkinteger(lua, 8);
    }
    mysql_ctx *mysql = lua_newuserdata(lua, sizeof(mysql_ctx));
    mysql_init(mysql, ip, port, evssl, user, password, database, charset, maxpk);
    ASSOC_MTABLE(lua, "_mysql_ctx");
    return 1;
}
// Lua 绑定：获取 mpack 数据包的封包类型
static int32_t _lmysql_pack_type(lua_State *lua) {
    mpack_ctx *mpack = lua_touserdata(lua, 1);
    lua_pushinteger(lua, mpack->pack_type);
    return 1;
}
// Lua 绑定（__gc）：发送 QUIT 命令并清理连接上下文绑定
static int32_t _lmysql_free(lua_State *lua) {
    mysql_ctx *mysql = lua_touserdata(lua, 1);
    if (NULL != mysql->task) {
        size_t size;
        void *pack = mysql_pack_quit(mysql, &size);
        ev_ud_context(&mysql->task->loader->netev, mysql->client.fd, mysql->client.skid, NULL);
        ev_send(&mysql->task->loader->netev, mysql->client.fd, mysql->client.skid, pack, size, 0);
    }
    return 0;
}
// Lua 绑定：尝试建立 MySQL 连接（异步），成功返回 true，失败返回 false
static int32_t _lmysql_try_connect(lua_State *lua) {
    mysql_ctx *mysql = lua_touserdata(lua, 1);
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    int32_t rtn = mysql_try_connect(task, mysql);
    if (ERR_OK == rtn) {
        lua_pushboolean(lua, 1);
    } else {
        lua_pushboolean(lua, 0);
    }
    return 1;
}
// Lua 绑定：返回 MySQL 服务端版本字符串
static int32_t _lmysql_version(lua_State *lua) {
    mysql_ctx *mysql = lua_touserdata(lua, 1);
    lua_pushstring(lua, mysql_version(mysql));
    return 1;
}
// Lua 绑定：返回最近一次错误信息字符串，并清除错误状态
static int32_t _lmysql_erro(lua_State *lua) {
    mysql_ctx *mysql = lua_touserdata(lua, 1);
    lua_pushstring(lua, mysql_erro(mysql, NULL));
    mysql_erro_clear(mysql);
    return 1;
}
// Lua 绑定：返回当前连接的 fd 和 skid
static int32_t _lmysql_sock_id(lua_State *lua) {
    mysql_ctx *mysql = lua_touserdata(lua, 1);
    lua_pushinteger(lua, mysql->client.fd);
    lua_pushinteger(lua, mysql->client.skid);
    return 2;
}
// Lua 绑定：返回最后一次 INSERT 操作生成的自增 id
static int32_t _lmysql_last_id(lua_State *lua) {
    mysql_ctx *mysql = lua_touserdata(lua, 1);
    lua_pushinteger(lua, mysql_last_id(mysql));
    return 1;
}
// Lua 绑定：返回最后一次 UPDATE/DELETE/INSERT 操作影响的行数
static int32_t _lmysql_affectd_rows(lua_State *lua) {
    mysql_ctx *mysql = lua_touserdata(lua, 1);
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
    REG_MTABLE(lua, "_mysql_ctx", reg_new, reg_func);
    return 1;
}
