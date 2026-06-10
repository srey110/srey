#include "lbind/lpub.h"

#define MT_MONGO         "_mongo_ctx"
#define MT_MONGO_SESSION "_mongo_session_ctx"

// 从 Lua 栈 idx 位置提取可选 BSON 选项指针；缺失或非 lightuserdata 返 NULL
static char *_lmongo_get_opts(lua_State *lua, int idx) {
    return lua_islightuserdata(lua, idx) ? lua_touserdata(lua, idx) : NULL;
}
// ---- mongo ----
/// <summary>
/// 创建 MongoDB 连接上下文（不立即建立连接）
/// </summary>
/// <param name="ip" type="string">服务器 IP</param>
/// <param name="port" type="integer">服务器端口</param>
/// <param name="evssl" type="lightuserdata|nil">SSL 上下文；nil 表示明文</param>
/// <param name="db" type="string">初始数据库名</param>
/// <returns type="_mongo_ctx">mongo 对象</returns>
static int32_t _lmongo_new(lua_State *lua) {
    const char *ip = luaL_checkstring(lua, 1);
    uint16_t port = (uint16_t)luaL_checkinteger(lua, 2);
    struct evssl_ctx *evssl = NULL;
    if (LUA_TNIL != lua_type(lua, 3)) {
        LUACHECK_LUDATA(lua, 3);
        evssl = lua_touserdata(lua, 3);
    }
    const char *db = luaL_checkstring(lua, 4);
    mongo_ctx *mongo = lua_newuserdata(lua, sizeof(mongo_ctx));
    mongo_init(mongo, ip, port, evssl, db);
    ASSOC_MTABLE(lua, MT_MONGO);
    return 1;
}
/// <summary>
/// 清理连接上下文；若连接仍打开则关闭 socket（绑定为 __gc）
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <returns>无</returns>
static int32_t _lmongo_free(lua_State *lua) {
    mongo_ctx *mongo = luaL_checkudata(lua, 1, MT_MONGO);
    if (NULL != mongo->task && INVALID_SOCK != mongo->fd) {
        ev_ud_context(&mongo->task->loader->netev, mongo->fd, mongo->skid, NULL);
        ev_close(&mongo->task->loader->netev, mongo->fd, mongo->skid, 0);
        mongo->fd = INVALID_SOCK;
    }
    secure_zero(mongo->user, sizeof(mongo->user));
    secure_zero(mongo->password, sizeof(mongo->password));
    return 0;
}
/// <summary>
/// 发起异步 TCP 连接。调用方需用 srey.wait_connect(fd, skid, ssl) 同步等待连接建立
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <returns type="integer">socket fd；失败返回 INVALID_SOCK</returns>
/// <returns type="integer?">skid；仅在 fd 有效时返回</returns>
static int32_t _lmongo_try_connect(lua_State *lua) {
    mongo_ctx *mongo = luaL_checkudata(lua, 1, MT_MONGO);
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (ERR_OK != mongo_try_connect(task, mongo)) {
        lua_pushinteger(lua, INVALID_SOCK);
        return 1;
    }
    lua_pushinteger(lua, mongo->fd);
    lua_pushinteger(lua, (lua_Integer)mongo->skid);
    return 2;
}
/// <summary>
/// 将指定连接切换到 AUTH 状态，供 SCRAM 认证流程使用
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="fd" type="integer">socket fd</param>
/// <param name="skid" type="integer">连接 skid</param>
/// <returns>无</returns>
static int32_t _lmongo_set_auth_status(lua_State *lua) {
    luaL_checkudata(lua, 1, MT_MONGO);
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 2);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 3);
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    ev_ud_status(&task->loader->netev, fd, skid, (uint8_t)mongo_status_auth());
    return 0;
}
/// <summary>
/// 设置当前数据库名
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="db" type="string">数据库名</param>
/// <returns>无</returns>
static int32_t _lmongo_db(lua_State *lua) {
    mongo_ctx *mongo = luaL_checkudata(lua, 1, MT_MONGO);
    const char *db = luaL_checkstring(lua, 2);
    mongo_db(mongo, db);
    return 0;
}
/// <summary>
/// 设置认证数据库名
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="db" type="string">认证数据库名</param>
/// <returns>无</returns>
static int32_t _lmongo_authdb(lua_State *lua) {
    mongo_ctx *mongo = luaL_checkudata(lua, 1, MT_MONGO);
    const char *db = luaL_checkstring(lua, 2);
    mongo_authdb(mongo, db);
    return 0;
}
/// <summary>
/// 设置当前集合名
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="col" type="string">集合名</param>
/// <returns>无</returns>
static int32_t _lmongo_collection(lua_State *lua) {
    mongo_ctx *mongo = luaL_checkudata(lua, 1, MT_MONGO);
    const char *col = luaL_checkstring(lua, 2);
    mongo_collection(mongo, col);
    return 0;
}
/// <summary>
/// 设置认证用户名和密码
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="user" type="string">用户名</param>
/// <param name="pwd" type="string">密码</param>
/// <returns>无</returns>
static int32_t _lmongo_user_pwd(lua_State *lua) {
    mongo_ctx *mongo = luaL_checkudata(lua, 1, MT_MONGO);
    const char *user = luaL_checkstring(lua, 2);
    const char *pwd  = luaL_checkstring(lua, 3);
    mongo_user_pwd(mongo, user, pwd);
    return 0;
}
/// <summary>
/// 检查响应包错误并提取影响文档数
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="mgopack" type="lightuserdata">mgopack_ctx 指针</param>
/// <returns type="integer">影响文档数 n；服务端报错返回 -1</returns>
static int32_t _lmongo_check_error(lua_State *lua) {
    (void)luaL_checkudata(lua, 1, MT_MONGO);
    LUACHECK_LUDATA(lua, 2);
    mgopack_ctx *mgopack = lua_touserdata(lua, 2);
    lua_pushinteger(lua, mongo_parse_check_error(mgopack));
    return 1;
}
/// <summary>
/// 解析 startSession 响应
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="mgopack" type="lightuserdata">mgopack_ctx 响应指针</param>
/// <returns type="string?">16 字节会话 UUID；失败返回 nil（仅 1 个返回值）</returns>
/// <returns type="integer?">超时分钟数</returns>
static int32_t _lmongo_parse_startsession(lua_State *lua) {
    (void)luaL_checkudata(lua, 1, MT_MONGO);
    LUACHECK_LUDATA(lua, 2);
    mgopack_ctx *mgopack = lua_touserdata(lua, 2);
    char uuid[UUID_LENS];
    int32_t timeout;
    if (!mongo_parse_startsession(mgopack, uuid, &timeout)) {
        lua_pushnil(lua);
        return 1;
    }
    lua_pushlstring(lua, uuid, UUID_LENS);
    lua_pushinteger(lua, timeout);
    return 2;
}
/// <summary>
/// 设置下一条命令的消息标志位
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="flag" type="integer">mongo_flags 枚举值</param>
/// <returns>无</returns>
static int32_t _lmongo_set_flag(lua_State *lua) {
    mongo_ctx *mongo = luaL_checkudata(lua, 1, MT_MONGO);
    mongo_flags flag = (mongo_flags)luaL_checkinteger(lua, 2);
    mongo_set_flag(mongo, flag);
    return 0;
}
/// <summary>
/// 检查消息标志位是否已设置
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="flag" type="integer">mongo_flags 枚举值</param>
/// <returns type="boolean">已设置 true，否则 false</returns>
static int32_t _lmongo_check_flag(lua_State *lua) {
    mongo_ctx *mongo = luaL_checkudata(lua, 1, MT_MONGO);
    mongo_flags flag = (mongo_flags)luaL_checkinteger(lua, 2);
    lua_pushboolean(lua, mongo_check_flag(mongo, flag));
    return 1;
}
/// <summary>
/// 清除所有消息标志位
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <returns type="integer">清除前的旧标志位</returns>
static int32_t _lmongo_clear_flag(lua_State *lua) {
    mongo_ctx *mongo = luaL_checkudata(lua, 1, MT_MONGO);
    lua_pushinteger(lua, mongo_clear_flag(mongo));
    return 1;
}
/// <summary>
/// 返回当前请求 ID
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <returns type="integer">请求 ID</returns>
static int32_t _lmongo_requestid(lua_State *lua) {
    mongo_ctx *mongo = luaL_checkudata(lua, 1, MT_MONGO);
    lua_pushinteger(lua, mongo_requestid(mongo));
    return 1;
}
/// <summary>
/// 构造 hello 握手命令包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="opts" type="lightuserdata?">附加 BSON 选项</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmongo_pack_hello(lua_State *lua) {
    mongo_ctx *mongo = luaL_checkudata(lua, 1, MT_MONGO);
    char *opts = _lmongo_get_opts(lua, 2);
    size_t size;
    void *pack = mongo_pack_hello(mongo, opts, &size);
    LPUB_RET_LUD(lua, pack, (lua_Integer)size);
}
/// <summary>
/// 构造 ping 心跳命令包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmongo_pack_ping(lua_State *lua) {
    mongo_ctx *mongo = luaL_checkudata(lua, 1, MT_MONGO);
    size_t size;
    void *pack = mongo_pack_ping(mongo, &size);
    LPUB_RET_LUD(lua, pack, (lua_Integer)size);
}
/// <summary>
/// 构造 drop 删集合命令包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="opts" type="lightuserdata?">附加 BSON 选项</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmongo_pack_drop(lua_State *lua) {
    mongo_ctx *mongo = luaL_checkudata(lua, 1, MT_MONGO);
    char *opts = _lmongo_get_opts(lua, 2);
    size_t size;
    void *pack = mongo_pack_drop(mongo, opts, &size);
    LPUB_RET_LUD(lua, pack, (lua_Integer)size);
}
/// <summary>
/// 构造 insert 插入命令包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="docs" type="lightuserdata">BSON 数组格式文档列表指针</param>
/// <param name="dlens" type="integer">docs 字节数</param>
/// <param name="opts" type="lightuserdata?">附加 BSON 选项</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmongo_pack_insert(lua_State *lua) {
    mongo_ctx *mongo = luaL_checkudata(lua, 1, MT_MONGO);
    LUACHECK_LUDATA(lua, 2);
    char *docs = lua_touserdata(lua, 2);
    size_t dlens = (size_t)luaL_checkinteger(lua, 3);
    char *opts = _lmongo_get_opts(lua, 4);
    size_t size;
    void *pack = mongo_pack_insert(mongo, docs, dlens, opts, &size);
    LPUB_RET_LUD(lua, pack, (lua_Integer)size);
}
/// <summary>
/// 构造 update 更新命令包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="updates" type="lightuserdata">BSON 数组格式更新列表指针</param>
/// <param name="ulens" type="integer">updates 字节数</param>
/// <param name="opts" type="lightuserdata?">附加 BSON 选项</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmongo_pack_update(lua_State *lua) {
    mongo_ctx *mongo = luaL_checkudata(lua, 1, MT_MONGO);
    LUACHECK_LUDATA(lua, 2);
    char *updates = lua_touserdata(lua, 2);
    size_t ulens = (size_t)luaL_checkinteger(lua, 3);
    char *opts = _lmongo_get_opts(lua, 4);
    size_t size;
    void *pack = mongo_pack_update(mongo, updates, ulens, opts, &size);
    LPUB_RET_LUD(lua, pack, (lua_Integer)size);
}
/// <summary>
/// 构造 delete 删除命令包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="deletes" type="lightuserdata">BSON 数组格式删除列表指针</param>
/// <param name="dlens" type="integer">deletes 字节数</param>
/// <param name="opts" type="lightuserdata?">附加 BSON 选项</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmongo_pack_delete(lua_State *lua) {
    mongo_ctx *mongo = luaL_checkudata(lua, 1, MT_MONGO);
    LUACHECK_LUDATA(lua, 2);
    char *deletes = lua_touserdata(lua, 2);
    size_t dlens = (size_t)luaL_checkinteger(lua, 3);
    char *opts = _lmongo_get_opts(lua, 4);
    size_t size;
    void *pack = mongo_pack_delete(mongo, deletes, dlens, opts, &size);
    LPUB_RET_LUD(lua, pack, (lua_Integer)size);
}
/// <summary>
/// 构造 bulkWrite 批量写操作命令包（MongoDB 8.0+）
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="ops" type="lightuserdata">BSON 数组格式操作列表指针</param>
/// <param name="olens" type="integer">ops 字节数</param>
/// <param name="nsinfo" type="lightuserdata">BSON 数组格式命名空间信息指针</param>
/// <param name="nlens" type="integer">nsinfo 字节数</param>
/// <param name="opts" type="lightuserdata?">附加 BSON 选项</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmongo_pack_bulkwrite(lua_State *lua) {
    mongo_ctx *mongo = luaL_checkudata(lua, 1, MT_MONGO);
    LUACHECK_LUDATA(lua, 2);
    char *ops = lua_touserdata(lua, 2);
    size_t olens = (size_t)luaL_checkinteger(lua, 3);
    LUACHECK_LUDATA(lua, 4);
    char *nsinfo = lua_touserdata(lua, 4);
    size_t nlens = (size_t)luaL_checkinteger(lua, 5);
    char *opts = _lmongo_get_opts(lua, 6);
    size_t size;
    void *pack = mongo_pack_bulkwrite(mongo, ops, olens, nsinfo, nlens, opts, &size);
    LPUB_RET_LUD(lua, pack, (lua_Integer)size);
}
/// <summary>
/// 构造 find 查询命令包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="filter" type="lightuserdata?">BSON 过滤条件；nil 表示全部</param>
/// <param name="flens" type="integer?">filter 字节数</param>
/// <param name="opts" type="lightuserdata?">附加 BSON 选项</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmongo_pack_find(lua_State *lua) {
    mongo_ctx *mongo = luaL_checkudata(lua, 1, MT_MONGO);
    char *filter = NULL;
    size_t flens = 0;
    if (lua_islightuserdata(lua, 2)) {
        filter = lua_touserdata(lua, 2);
        flens  = (size_t)luaL_checkinteger(lua, 3);
    }
    char *opts = _lmongo_get_opts(lua, 4);
    size_t size;
    void *pack = mongo_pack_find(mongo, filter, flens, opts, &size);
    LPUB_RET_LUD(lua, pack, (lua_Integer)size);
}
/// <summary>
/// 构造 aggregate 聚合命令包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="pipeline" type="lightuserdata">BSON 数组格式聚合管道指针</param>
/// <param name="pllens" type="integer">pipeline 字节数</param>
/// <param name="opts" type="lightuserdata?">附加 BSON 选项</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmongo_pack_aggregate(lua_State *lua) {
    mongo_ctx *mongo = luaL_checkudata(lua, 1, MT_MONGO);
    LUACHECK_LUDATA(lua, 2);
    char *pipeline = lua_touserdata(lua, 2);
    size_t pllens = (size_t)luaL_checkinteger(lua, 3);
    char *opts = _lmongo_get_opts(lua, 4);
    size_t size;
    void *pack = mongo_pack_aggregate(mongo, pipeline, pllens, opts, &size);
    LPUB_RET_LUD(lua, pack, (lua_Integer)size);
}
/// <summary>
/// 构造 getMore 获取游标后续批次命令包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="cursorid" type="integer">游标 ID</param>
/// <param name="opts" type="lightuserdata?">附加 BSON 选项</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmongo_pack_getmore(lua_State *lua) {
    mongo_ctx *mongo = luaL_checkudata(lua, 1, MT_MONGO);
    int64_t cursorid = (int64_t)luaL_checkinteger(lua, 2);
    char *opts = _lmongo_get_opts(lua, 3);
    size_t size;
    void *pack = mongo_pack_getmore(mongo, cursorid, opts, &size);
    LPUB_RET_LUD(lua, pack, (lua_Integer)size);
}
/// <summary>
/// 构造 killCursors 关闭游标命令包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="cursorids" type="lightuserdata">BSON 数组格式游标 ID 列表指针</param>
/// <param name="cslens" type="integer">cursorids 字节数</param>
/// <param name="opts" type="lightuserdata?">附加 BSON 选项</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmongo_pack_killcursors(lua_State *lua) {
    mongo_ctx *mongo = luaL_checkudata(lua, 1, MT_MONGO);
    LUACHECK_LUDATA(lua, 2);
    char *cursorids = lua_touserdata(lua, 2);
    size_t cslens = (size_t)luaL_checkinteger(lua, 3);
    char *opts = _lmongo_get_opts(lua, 4);
    size_t size;
    void *pack = mongo_pack_killcursors(mongo, cursorids, cslens, opts, &size);
    LPUB_RET_LUD(lua, pack, (lua_Integer)size);
}
/// <summary>
/// 构造 distinct 去重查询命令包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="key" type="string">去重字段名</param>
/// <param name="query" type="lightuserdata?">BSON 过滤条件；nil 表示全部</param>
/// <param name="qlens" type="integer?">query 字节数</param>
/// <param name="opts" type="lightuserdata?">附加 BSON 选项</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmongo_pack_distinct(lua_State *lua) {
    mongo_ctx *mongo = luaL_checkudata(lua, 1, MT_MONGO);
    const char *key = luaL_checkstring(lua, 2);
    char *query = NULL;
    size_t qlens = 0;
    if (lua_islightuserdata(lua, 3)) {
        query = lua_touserdata(lua, 3);
        qlens = (size_t)luaL_checkinteger(lua, 4);
    }
    char *opts = _lmongo_get_opts(lua, 5);
    size_t size;
    void *pack = mongo_pack_distinct(mongo, key, query, qlens, opts, &size);
    LPUB_RET_LUD(lua, pack, (lua_Integer)size);
}
/// <summary>
/// 构造 findAndModify 原子查找并修改/删除命令包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="query" type="lightuserdata?">BSON 过滤条件；nil 表示全部</param>
/// <param name="qlens" type="integer?">query 字节数</param>
/// <param name="remove" type="integer">非零表示删除匹配文档</param>
/// <param name="pipeline" type="integer">非零时 update 为聚合数组</param>
/// <param name="update" type="lightuserdata?">BSON 更新文档或聚合数组；删除时可 nil</param>
/// <param name="ulens" type="integer?">update 字节数</param>
/// <param name="opts" type="lightuserdata?">附加 BSON 选项</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmongo_pack_findandmodify(lua_State *lua) {
    mongo_ctx *mongo = luaL_checkudata(lua, 1, MT_MONGO);
    char *query = NULL;
    size_t qlens = 0;
    if (lua_islightuserdata(lua, 2)) {
        query = lua_touserdata(lua, 2);
        qlens = (size_t)luaL_checkinteger(lua, 3);
    }
    int32_t remove   = (int32_t)luaL_checkinteger(lua, 4);
    int32_t pipeline = (int32_t)luaL_checkinteger(lua, 5);
    char *update = NULL;
    size_t ulens = 0;
    if (lua_islightuserdata(lua, 6)) {
        update = lua_touserdata(lua, 6);
        ulens  = (size_t)luaL_checkinteger(lua, 7);
    }
    char *opts = _lmongo_get_opts(lua, 8);
    size_t size;
    void *pack = mongo_pack_findandmodify(mongo, query, qlens, remove, pipeline, update, ulens, opts, &size);
    LPUB_RET_LUD(lua, pack, (lua_Integer)size);
}
/// <summary>
/// 构造 count 文档计数命令包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="query" type="lightuserdata?">BSON 过滤条件；nil 表示全部</param>
/// <param name="qlens" type="integer?">query 字节数</param>
/// <param name="opts" type="lightuserdata?">附加 BSON 选项</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmongo_pack_count(lua_State *lua) {
    mongo_ctx *mongo = luaL_checkudata(lua, 1, MT_MONGO);
    char *query = NULL;
    size_t qlens = 0;
    if (lua_islightuserdata(lua, 2)) {
        query = lua_touserdata(lua, 2);
        qlens = (size_t)luaL_checkinteger(lua, 3);
    }
    char *opts = _lmongo_get_opts(lua, 4);
    size_t size;
    void *pack = mongo_pack_count(mongo, query, qlens, opts, &size);
    LPUB_RET_LUD(lua, pack, (lua_Integer)size);
}
/// <summary>
/// 构造 createIndexes 创建索引命令包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="indexes" type="lightuserdata">BSON 数组格式索引定义指针</param>
/// <param name="ilens" type="integer">indexes 字节数</param>
/// <param name="opts" type="lightuserdata?">附加 BSON 选项</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmongo_pack_createindexes(lua_State *lua) {
    mongo_ctx *mongo = luaL_checkudata(lua, 1, MT_MONGO);
    LUACHECK_LUDATA(lua, 2);
    char *indexes = lua_touserdata(lua, 2);
    size_t ilens = (size_t)luaL_checkinteger(lua, 3);
    char *opts = _lmongo_get_opts(lua, 4);
    size_t size;
    void *pack = mongo_pack_createindexes(mongo, indexes, ilens, opts, &size);
    LPUB_RET_LUD(lua, pack, (lua_Integer)size);
}
/// <summary>
/// 构造 dropIndexes 删除索引命令包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="indexes" type="lightuserdata">BSON 数组格式索引名列表指针</param>
/// <param name="ilens" type="integer">indexes 字节数</param>
/// <param name="opts" type="lightuserdata?">附加 BSON 选项</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmongo_pack_dropindexes(lua_State *lua) {
    mongo_ctx *mongo = luaL_checkudata(lua, 1, MT_MONGO);
    LUACHECK_LUDATA(lua, 2);
    char *indexes = lua_touserdata(lua, 2);
    size_t ilens = (size_t)luaL_checkinteger(lua, 3);
    char *opts = _lmongo_get_opts(lua, 4);
    size_t size;
    void *pack = mongo_pack_dropindexes(mongo, indexes, ilens, opts, &size);
    LPUB_RET_LUD(lua, pack, (lua_Integer)size);
}
/// <summary>
/// 构造 startSession 命令包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmongo_pack_startsession(lua_State *lua) {
    mongo_ctx *mongo = luaL_checkudata(lua, 1, MT_MONGO);
    size_t size;
    void *pack = mongo_pack_startsession(mongo, &size);
    LPUB_RET_LUD(lua, pack, (lua_Integer)size);
}
/// <summary>
/// 构造 SCRAM 认证第一步（saslStart）请求包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="authmod" type="string">认证机制名（如 "SCRAM-SHA-256"）</param>
/// <returns type="lightuserdata?">命令数据指针；失败返回 nil</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lmongo_pack_auth_first(lua_State *lua) {
    mongo_ctx *mongo = luaL_checkudata(lua, 1, MT_MONGO);
    const char *authmod = luaL_checkstring(lua, 2);
    size_t size;
    void *pack = mongo_pack_scram_client_first(mongo, authmod, &size);
    if (NULL == pack) {
        lua_pushnil(lua);
        return 1;
    }
    LPUB_RET_LUD(lua, pack, (lua_Integer)size);
}
/// <summary>
/// 构造 SCRAM 认证第二步（saslContinue）请求包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="convid" type="integer">对话 id（来自第一步响应）</param>
/// <param name="payload" type="lightuserdata">客户端 final payload 指针</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmongo_pack_auth_final(lua_State *lua) {
    mongo_ctx *mongo = luaL_checkudata(lua, 1, MT_MONGO);
    int32_t convid = (int32_t)luaL_checkinteger(lua, 2);
    LUACHECK_LUDATA(lua, 3);
    char *payload = lua_touserdata(lua, 3);
    size_t size;
    void *pack = mongo_pack_scram_client_final(mongo, convid, payload, &size);
    LPUB_RET_LUD(lua, pack, (lua_Integer)size);
}
/// <summary>
/// 解析消息包 Section 类型
/// </summary>
/// <param name="mgopack" type="lightuserdata">mgopack_ctx 指针</param>
/// <returns type="integer">Section 类型（0 = 正文，1 = 文档序列）</returns>
static int32_t _lmongo_pack_type(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    mgopack_ctx *mgopack = lua_touserdata(lua, 1);
    lua_pushinteger(lua, mgopack->kind);
    return 1;
}
/// <summary>
/// 返回消息包的 BSON 文档数据
/// </summary>
/// <param name="mgopack" type="lightuserdata">mgopack_ctx 指针</param>
/// <returns type="lightuserdata">BSON 文档指针</returns>
/// <returns type="integer">文档字节数</returns>
static int32_t _lmongo_doc(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    mgopack_ctx *mgopack = lua_touserdata(lua, 1);
    LPUB_RET_LUD(lua, mgopack->doc, (lua_Integer)mgopack->dlens);
}
/// <summary>
/// 返回消息包请求 ID
/// </summary>
/// <param name="mgopack" type="lightuserdata">mgopack_ctx 指针</param>
/// <returns type="integer">请求 ID</returns>
static int32_t _lmongo_reqid(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    mgopack_ctx *mgopack = lua_touserdata(lua, 1);
    lua_pushinteger(lua, mgopack->reqid);
    return 1;
}
/// <summary>
/// 返回消息包标志位
/// </summary>
/// <param name="mgopack" type="lightuserdata">mgopack_ctx 指针</param>
/// <returns type="integer">flags 字段</returns>
static int32_t _lmongo_flags(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    mgopack_ctx *mgopack = lua_touserdata(lua, 1);
    lua_pushinteger(lua, (lua_Integer)mgopack->flags);
    return 1;
}
/// <summary>
/// 从响应包中提取游标 ID
/// </summary>
/// <param name="mgopack" type="lightuserdata">mgopack_ctx 指针</param>
/// <returns type="integer">游标 ID；0 表示无游标</returns>
static int32_t _lmongo_cursorid(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    mgopack_ctx *mgopack = lua_touserdata(lua, 1);
    lua_pushinteger(lua, mongo_cursorid(mgopack));
    return 1;
}
/// <summary>
/// 解析 SCRAM 认证响应
/// </summary>
/// <param name="mgopack" type="lightuserdata">mgopack_ctx 响应指针</param>
/// <returns type="boolean">解析成功 true，失败 false</returns>
/// <returns type="integer">对话 id（convid）</returns>
/// <returns type="boolean">是否已完成最终认证</returns>
/// <returns type="lightuserdata">payload 指针</returns>
/// <returns type="integer">payload 字节数</returns>
static int32_t _lmongo_parse_auth_response(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    mgopack_ctx *mgopack = lua_touserdata(lua, 1);
    int32_t convid = 0;
    int32_t done = 0;
    char *payload = NULL;
    size_t plens = 0;
    int32_t ok = mongo_parse_auth_response(mgopack, &convid, &done, &payload, &plens);
    lua_pushboolean(lua, ok);
    lua_pushinteger(lua, convid);
    lua_pushboolean(lua, done);
    lua_pushlightuserdata(lua, payload);
    lua_pushinteger(lua, (lua_Integer)plens);
    return 5;
}
//mongo
LUAMOD_API int luaopen_mongo(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new",                  _lmongo_new },
        { "pack_type",            _lmongo_pack_type },
        { "doc",                  _lmongo_doc },
        { "reqid",                _lmongo_reqid },
        { "flags",                _lmongo_flags },
        { "cursorid",             _lmongo_cursorid },
        { "parse_auth_response",  _lmongo_parse_auth_response },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "try_connect",          _lmongo_try_connect },
        { "set_auth_status",      _lmongo_set_auth_status },
        { "db",                   _lmongo_db },
        { "authdb",               _lmongo_authdb },
        { "collection",           _lmongo_collection },
        { "user_pwd",             _lmongo_user_pwd },
        { "check_error",          _lmongo_check_error },
        { "parse_startsession",   _lmongo_parse_startsession },
        { "set_flag",             _lmongo_set_flag },
        { "check_flag",           _lmongo_check_flag },
        { "clear_flag",           _lmongo_clear_flag },
        { "requestid",            _lmongo_requestid },
        { "pack_hello",           _lmongo_pack_hello },
        { "pack_ping",            _lmongo_pack_ping },
        { "pack_drop",            _lmongo_pack_drop },
        { "pack_insert",          _lmongo_pack_insert },
        { "pack_update",          _lmongo_pack_update },
        { "pack_delete",          _lmongo_pack_delete },
        { "pack_bulkwrite",       _lmongo_pack_bulkwrite },
        { "pack_find",            _lmongo_pack_find },
        { "pack_aggregate",       _lmongo_pack_aggregate },
        { "pack_getmore",         _lmongo_pack_getmore },
        { "pack_killcursors",     _lmongo_pack_killcursors },
        { "pack_distinct",        _lmongo_pack_distinct },
        { "pack_findandmodify",   _lmongo_pack_findandmodify },
        { "pack_count",           _lmongo_pack_count },
        { "pack_createindexes",   _lmongo_pack_createindexes },
        { "pack_dropindexes",     _lmongo_pack_dropindexes },
        { "pack_startsession",    _lmongo_pack_startsession },
        { "pack_auth_first",      _lmongo_pack_auth_first },
        { "pack_auth_final",      _lmongo_pack_auth_final },
        { "__gc",                 _lmongo_free },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_MONGO, reg_new, reg_func);
    return 1;
}
// ---- mongo.session ----
/// <summary>
/// 从已解析的 startSession 响应数据创建会话上下文
/// </summary>
/// <param name="mongo" type="_mongo_ctx">所属 mongo 连接</param>
/// <param name="uuid" type="string">16 字节会话 UUID</param>
/// <param name="timeout" type="integer">超时分钟数</param>
/// <returns type="_mongo_session_ctx?">session 对象；uuid 长度非 16 时返回 nil</returns>
static int32_t _lmongo_session_new(lua_State *lua) {
    mongo_ctx *mongo = luaL_checkudata(lua, 1, MT_MONGO);
    size_t uuid_lens;
    const char *uuid_str = luaL_checklstring(lua, 2, &uuid_lens);
    int32_t timeout = (int32_t)luaL_checkinteger(lua, 3);
    if (UUID_LENS != uuid_lens) {
        lua_pushnil(lua);
        return 1;
    }
    mongo_session **psession = lua_newuserdata(lua, sizeof(mongo_session *));
    *psession = NULL;
    ASSOC_MTABLE(lua, MT_MONGO_SESSION);
    mongo_session *session;
    MALLOC(session, sizeof(mongo_session));
    memcpy(session->uuid, uuid_str, UUID_LENS);
    session->mongo       = mongo;
    session->timeoutmin  = timeout;
    session->txnnumber   = 0;
    session->options     = NULL;
    session->timeout     = nowsec() + (uint64_t)timeout * 60;
    *psession = session;
    lua_pushvalue(lua, 1);
    lua_setiuservalue(lua, -2, 1);
    return 1;
}
/// <summary>
/// 释放会话内存（不发送 endSessions，由 Lua 层负责；同时作为 __gc / free 调用）
/// </summary>
/// <param name="self" type="userdata">session 对象</param>
/// <returns>无</returns>
static int32_t _lmongo_session_free(lua_State *lua) {
    mongo_session **psession = luaL_checkudata(lua, 1, MT_MONGO_SESSION);
    if (NULL != *psession) {
        mongo_session *session = *psession;
        if (NULL != session->mongo && session->mongo->session == session) {
            session->mongo->session = NULL;
        }
        FREE(session->options);
        FREE(session);
        *psession = NULL;
    }
    return 0;
}
/// <summary>
/// 开始事务（递增 txnNumber，构建事务选项 BSON，设置 mongo->session）
/// </summary>
/// <param name="self" type="userdata">session 对象</param>
/// <returns>无</returns>
static int32_t _lmongo_session_begin(lua_State *lua) {
    mongo_session **psession = luaL_checkudata(lua, 1, MT_MONGO_SESSION);
    mongo_begin(*psession);
    return 0;
}
/// <summary>
/// 事务操作完成后清理（释放 options 并解除 mongo->session 绑定）
/// </summary>
/// <param name="self" type="userdata">session 对象</param>
/// <returns>无</returns>
static int32_t _lmongo_session_done(lua_State *lua) {
    mongo_session **psession = luaL_checkudata(lua, 1, MT_MONGO_SESSION);
    mongo_session *session = *psession;
    if (NULL == session) {
        return 0;
    }
    FREE(session->options);
    session->options = NULL;
    if (NULL != session->mongo && session->mongo->session == session) {
        session->mongo->session = NULL;
    }
    return 0;
}
/// <summary>
/// 构造 refreshSessions 刷新会话命令包
/// </summary>
/// <param name="self" type="userdata">session 对象</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmongo_session_pack_refresh(lua_State *lua) {
    mongo_session **psession = luaL_checkudata(lua, 1, MT_MONGO_SESSION);
    size_t size;
    void *pack = mongo_pack_refreshsession(*psession, &size);
    LPUB_RET_LUD(lua, pack, (lua_Integer)size);
}
/// <summary>
/// 构造 endSessions 结束会话命令包
/// </summary>
/// <param name="self" type="userdata">session 对象</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmongo_session_pack_endsession(lua_State *lua) {
    mongo_session **psession = luaL_checkudata(lua, 1, MT_MONGO_SESSION);
    size_t size;
    void *pack = mongo_pack_endsession(*psession, &size);
    LPUB_RET_LUD(lua, pack, (lua_Integer)size);
}
/// <summary>
/// 构造 commitTransaction 提交事务命令包
/// </summary>
/// <param name="self" type="userdata">session 对象</param>
/// <param name="opts" type="lightuserdata?">附加 BSON 选项</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmongo_session_pack_commit(lua_State *lua) {
    mongo_session **psession = luaL_checkudata(lua, 1, MT_MONGO_SESSION);
    char *opts = _lmongo_get_opts(lua, 2);
    size_t size;
    void *pack = mongo_pack_committransaction(*psession, opts, &size);
    LPUB_RET_LUD(lua, pack, (lua_Integer)size);
}
/// <summary>
/// 构造 abortTransaction 回滚事务命令包
/// </summary>
/// <param name="self" type="userdata">session 对象</param>
/// <param name="opts" type="lightuserdata?">附加 BSON 选项</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmongo_session_pack_abort(lua_State *lua) {
    mongo_session **psession = luaL_checkudata(lua, 1, MT_MONGO_SESSION);
    char *opts = _lmongo_get_opts(lua, 2);
    size_t size;
    void *pack = mongo_pack_aborttransaction(*psession, opts, &size);
    LPUB_RET_LUD(lua, pack, (lua_Integer)size);
}
//mongo.session
LUAMOD_API int luaopen_mongo_session(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lmongo_session_new },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "begin",            _lmongo_session_begin },
        { "done",             _lmongo_session_done },
        { "pack_refresh",     _lmongo_session_pack_refresh },
        { "pack_endsession",  _lmongo_session_pack_endsession },
        { "pack_commit",      _lmongo_session_pack_commit },
        { "pack_abort",       _lmongo_session_pack_abort },
        { "free",             _lmongo_session_free },
        { "__gc",             _lmongo_session_free },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_MONGO_SESSION, reg_new, reg_func);
    return 1;
}
