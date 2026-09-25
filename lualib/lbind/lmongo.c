#include "lbind/lpub.h"

#define MT_MONGO         "_mongo_ctx"
#define MT_MONGO_SESSION "_mongo_session_ctx"
#define MONGOFLAG_OUT_OF_RANGE "mongo flag out of range"
// session 的五个入口共用: 自身非空 + 宿主还活着。后一半的理由见 lpub_owner_ptr
#define LMONGO_SESSION_ARG(lua, var) \
    LPUB_UD_ARG((lua), mongo_session, MT_MONGO_SESSION, var, "session freed") \
    if (NULL == lpub_owner_ptr((lua), MT_MONGO)) { \
        return luaL_error((lua), "mongo session: owner mongo already freed"); \
    }

// 从 Lua 栈 idx 位置提取一个可选 BSON 文档及其字节数;nil / 没传 / 空缓冲一律返 NULL 且 *lens 置 0。
// 空缓冲也归零:BSON 最短的一篇也有 5 字节,零字节不是文档,放过去 bson_append_document 会写下
// 键却不写文档体,整条命令从那里开始错位。opts 与 filter / query / update 全走这里,免得相邻两个
// BSON 参数各写一套取法——一个认 string 另一个不认,传错了还静默当没传。
// bson_cat 要求随指针给出缓冲长度,故 lightuserdata 必须在 idx+1 附上字节数;这里只验长度本身
// 合法,"是不是一篇落在缓冲内的完整文档"交给 bson_cat 判——畸形文档不抛 Lua 错,整条命令组包失败返 nil
static char *_lmongo_get_opts(lua_State *lua, int32_t idx, size_t *lens) {
    *lens = 0;
    if (lua_isnoneornil(lua, idx)) {
        return NULL;
    }
    char *doc = lpub_check_buf(lua, idx, lens, NULL);
    return EMPTYPTR(doc, *lens) ? NULL : doc;
}
// ---- mongo ----
/// <summary>
/// 创建 MongoDB 连接上下文（不立即建立连接）
/// </summary>
/// <param name="ip" type="string">服务器 IP</param>
/// <param name="port" type="integer">服务器端口</param>
/// <param name="evssl" type="lightuserdata|nil">SSL 上下文；nil 表示明文</param>
/// <param name="db" type="string">初始数据库名</param>
/// <returns type="_mongo_ctx?">mongo 对象；ip 或 db 超 63 字节导致初始化失败时返回 nil</returns>
static int32_t _lmongo_new(lua_State *lua) {
    const char *ip = luaL_checkstring(lua, 1);
    uint16_t port = lpub_check_u16(lua, 2, PORT_OUT_OF_RANGE);
    struct evssl_ctx *evssl = lpub_check_evssl(lua, 3);
    const char *db = luaL_checkstring(lua, 4);
    mongo_ctx **ud = (mongo_ctx **)lpub_push_ud(lua, NULL, MT_MONGO);
    mongo_ctx *mongo;
    MALLOC(mongo, sizeof(mongo_ctx));
    if (ERR_OK != mongo_init(mongo, ip, port, evssl, db)) {
        FREE(mongo);
        return lpub_rtn_nil(lua, 1);
    }
    ATOMIC_SET_RELAXED(&mongo->ref, 1);// Lua 持有者份额
    *ud = mongo;
    return 1;
}
/// <summary>
/// 清理连接上下文；若连接仍打开则关闭 socket（绑定为 __gc）
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <returns>无</returns>
static int32_t _lmongo_free(lua_State *lua) {
    mongo_ctx **ud = luaL_checkudata(lua, 1, MT_MONGO);
    mongo_ctx *mongo = *ud;
    if (NULL == mongo) {
        return 0;
    }
    if (NULL != mongo->task && !sock_is_invalid(&mongo->sk)) {
        // 主动关连接：触发该 socket 的 udfree 释放事件侧份额，否则弃用的活连接块滞留至对端关
        ev_close(&mongo->task->loader->netev, &mongo->sk);
    }
    *ud = NULL;
    // scram 由网络线程 udfree 释放，__gc 不碰(防跨线程 UAF)；
    // 用户名与密码同理不在这里擦，擦除已挪进 PROT_REF_RELEASE
    PROT_REF_RELEASE(mongo);
    return 0;
}
/// <summary>
/// 发起异步 TCP 连接。调用方需用 srey.wait_connect(&sk, ssl) 同步等待连接建立
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <returns type="userdata">连接标识；失败时其 valid 字段为 false，返回值个数恒为 1</returns>
static int32_t _lmongo_try_connect(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    mongo_ctx *mongo = *ud;
    LPUB_CUR_TASK(lua, task);
    if (ERR_OK != mongo_try_connect(task, mongo, 1)) {
        // 失败也推一个连接标识,个数与成功路径一致;调用方只判 sk.valid
        return lpub_push_sock_invalid(lua);
    }
    lpub_push_sock(lua, &mongo->sk);
    return 1;
}
/// <summary>
/// 返回当前 MongoDB 连接的 fd 和 skid
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <returns type="userdata">连接标识；失败时其 valid 字段为 false</returns>
static int32_t _lmongo_sock_id(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    mongo_ctx *mongo = *ud;
    lpub_push_sock(lua, &mongo->sk);
    return 1;
}
/// <summary>
/// 将指定连接切换到 AUTH 状态，供 SCRAM 认证流程使用
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="sk" type="userdata">连接标识，由 core.connect / core.udp / 各 accept 回调给出</param>
/// <returns type="boolean">成功 true，stop 非0失败</returns>
static int32_t _lmongo_set_auth_status(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    sock_ctx *sk = lpub_check_sock(lua, 2);
    LPUB_CUR_TASK(lua, task);
    return lpub_rtn_bool(lua, ERR_OK == ev_ud_status(&task->loader->netev, sk, (uint8_t)mongo_status_auth()));
}
/// <summary>
/// 设置当前数据库名
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="db" type="string">数据库名</param>
/// <returns type="boolean">设置成功 true；超 63 字节返 false 且不改动任何字段</returns>
static int32_t _lmongo_db(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    const char *db = luaL_checkstring(lua, 2);
    return lpub_rtn_bool(lua, ERR_OK == mongo_db(*ud, db));
}
/// <summary>
/// 设置认证数据库名
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="db" type="string">认证数据库名</param>
/// <returns type="boolean">设置成功 true；超 63 字节返 false 且不改动任何字段</returns>
static int32_t _lmongo_authdb(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    const char *db = luaL_checkstring(lua, 2);
    return lpub_rtn_bool(lua, ERR_OK == mongo_authdb(*ud, db));
}
/// <summary>
/// 设置当前集合名
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="col" type="string">集合名；非字符串报错</param>
/// <returns type="boolean">设置成功 true；超 63 字节返 false 且不改动任何字段</returns>
static int32_t _lmongo_collection(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    // 只认真字符串:luaL_checkstring 连数字也收,ctx:getmore 漏传 col 时 cursorid 会静默当集合名
    luaL_argcheck(lua, LUA_TSTRING == lua_type(lua, 2), 2, "collection must be a string");
    const char *col = luaL_checkstring(lua, 2);
    return lpub_rtn_bool(lua, ERR_OK == mongo_collection(*ud, col));
}
/// <summary>
/// 设置认证用户名和密码
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="user" type="string">用户名</param>
/// <param name="pwd" type="string">密码</param>
/// <returns type="boolean">设置成功 true；超 63 字节返 false 且不改动任何字段</returns>
static int32_t _lmongo_user_pwd(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    const char *user = luaL_checkstring(lua, 2);
    const char *pwd = luaL_checkstring(lua, 3);
    return lpub_rtn_bool(lua, ERR_OK == mongo_user_pwd(*ud, user, pwd));
}
/// <summary>
/// 检查响应包错误并提取影响文档数
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="mgopack" type="lightuserdata">mgopack_ctx 指针</param>
/// <returns type="integer">影响文档数 n；服务端报错返回 -1</returns>
static int32_t _lmongo_check_error(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    LPUB_LUD_ARG(lua, mgopack_ctx, 2, mgopack);
    lua_pushinteger(lua, mongo_parse_check_error(mgopack));
    return 1;
}
/// <summary>
/// 解析 startSession 响应
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="mgopack" type="lightuserdata">mgopack_ctx 响应指针</param>
/// <returns type="string?">16 字节会话 UUID；失败返回 nil（连同后续返回值一并为 nil，共 2 个）</returns>
/// <returns type="integer?">超时分钟数</returns>
static int32_t _lmongo_parse_startsession(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    LPUB_LUD_ARG(lua, mgopack_ctx, 2, mgopack);
    char uuid[UUID_LENS];
    int32_t timeout;
    if (!mongo_parse_startsession(mgopack, uuid, &timeout)) {
        return lpub_rtn_nil(lua, 2);
    }
    lua_pushlstring(lua, uuid, UUID_LENS);
    lua_pushinteger(lua, timeout);
    return 2;
}
// 取标志位参数：先按 lua_Integer 判范围再窄化，再拒掉掩码里没有的位
static int32_t _lmongo_arg_flag(lua_State *lua, int32_t idx) {
    int32_t flag = (int32_t)lpub_check_range(lua, idx, 0, MONGO_FLAGS_ALL, MONGOFLAG_OUT_OF_RANGE);
    luaL_argcheck(lua, 0 == (flag & ~MONGO_FLAGS_ALL), idx, MONGOFLAG_OUT_OF_RANGE);
    return flag;
}
/// <summary>
/// 置上消息标志位；置上就一直有效直到 clear_flag，语义与后果见 C 层 mongo_set_flag
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="flag" type="integer">mongo_flags 的按位或；含枚举外的位报错。
/// 0 合法：读命令先 clear_flag 存下旧值，完事把它原样传回来还原，那个值可能就是 0。
/// C 层只实现了 MORETOCOME，其余位收下即丢弃</param>
/// <returns>无</returns>
static int32_t _lmongo_set_flag(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    int32_t flag = _lmongo_arg_flag(lua, 2);
    mongo_set_flag(*ud, flag);
    return 0;
}
/// <summary>
/// 检查消息标志位是否已设置
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="flag" type="integer">单个 mongo_flags 枚举值；含枚举外的位报错</param>
/// <returns type="boolean">已设置 true，否则 false</returns>
static int32_t _lmongo_check_flag(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    int32_t flag = _lmongo_arg_flag(lua, 2);
    return lpub_rtn_bool(lua, mongo_check_flag(*ud, (mongo_flags)flag));
}
/// <summary>
/// 读回已组好的数据包里写着的消息标志位。
/// "要不要等回包"必须问这个而不是 check_flag：后者读的是连接级可变字段，而组包与真正发送
/// 之间隔着一次会挂起的加锁，那期间公开的 set_flag / clear_flag 一改，包里写的和判定读的
/// 就成了两回事。详细后果见 C 层 mongo_pack_check_flag
/// </summary>
/// <param name="pack" type="lightuserdata">pack_* 组出的数据包</param>
/// <param name="flag" type="integer">单个 mongo_flags 枚举值；含枚举外的位报错</param>
/// <returns type="boolean">该包写着此标志位 true，否则 false</returns>
static int32_t _lmongo_pack_check_flag(lua_State *lua) {
    LPUB_LUD_ARG(lua, void, 1, pack);
    int32_t flag = _lmongo_arg_flag(lua, 2);
    return lpub_rtn_bool(lua, mongo_pack_check_flag(pack, (mongo_flags)flag));
}
/// <summary>
/// 清除所有消息标志位
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <returns type="integer">清除前的旧标志位</returns>
static int32_t _lmongo_clear_flag(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    lua_pushinteger(lua, mongo_clear_flag(*ud));
    return 1;
}
/// <summary>
/// 强制清空当前挂载的事务会话指针（不释放 session 对象本身），重连后调用避免跨代残留 lsid/txnNumber
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <returns>无</returns>
static int32_t _lmongo_clear_session(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    mongo_clear_session(*ud);
    return 0;
}
/// <summary>
/// 续期连接当前绑定会话的超时时刻；未绑定会话时无操作。
/// 由发送路径在收到应答后调用——服务端处理过带 lsid 的命令就已延长会话寿命。
/// 会话不在事务里（连接没绑它）时续期只能靠 session:renew()
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <returns>无</returns>
static int32_t _lmongo_session_touch(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    mongo_session_touch(*ud);
    return 0;
}
/// <summary>
/// 返回当前请求 ID
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <returns type="integer">请求 ID</returns>
static int32_t _lmongo_requestid(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    lua_pushinteger(lua, mongo_requestid(*ud));
    return 1;
}
/// <summary>
/// 构造 hello 握手命令包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="opts" type="string|lightuserdata|nil">附加 BSON 选项；须是一篇完整 BSON 文档</param>
/// <param name="optslens" type="integer?">opts 为 lightuserdata 时必填，缓冲字节数，取值 [0, INT32_MAX]</param>
/// <returns type="lightuserdata?">命令数据指针；opts 不是落在缓冲内的完整文档、或超单包上限(MONGO_MAX_PACK_LENS)被丢弃时返回 nil</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lmongo_pack_hello(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    size_t optlens;
    char *opts = _lmongo_get_opts(lua, 2, &optlens);
    size_t size;
    void *pack = mongo_pack_hello(*ud, opts, optlens, &size);
    return lpub_rtn_lud(lua, pack, size);
}
/// <summary>
/// 构造 ping 心跳命令包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmongo_pack_ping(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    size_t size;
    void *pack = mongo_pack_ping(*ud, &size);
    return lpub_rtn_lud(lua, pack, size);
}
/// <summary>
/// 构造 drop 删集合命令包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="opts" type="string|lightuserdata|nil">附加 BSON 选项；须是一篇完整 BSON 文档</param>
/// <param name="optslens" type="integer?">opts 为 lightuserdata 时必填，缓冲字节数，取值 [0, INT32_MAX]</param>
/// <returns type="lightuserdata?">命令数据指针；opts 不是落在缓冲内的完整文档、或超单包上限(MONGO_MAX_PACK_LENS)被丢弃时返回 nil</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lmongo_pack_drop(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    size_t optlens;
    char *opts = _lmongo_get_opts(lua, 2, &optlens);
    size_t size;
    void *pack = mongo_pack_drop(*ud, opts, optlens, &size);
    return lpub_rtn_lud(lua, pack, size);
}
/// <summary>
/// 构造 insert 插入命令包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="docs" type="lightuserdata">BSON 数组格式文档列表指针</param>
/// <param name="dlens" type="integer">docs 字节数</param>
/// <param name="opts" type="string|lightuserdata|nil">附加 BSON 选项；须是一篇完整 BSON 文档</param>
/// <param name="optslens" type="integer?">opts 为 lightuserdata 时必填，缓冲字节数，取值 [0, INT32_MAX]</param>
/// <returns type="lightuserdata?">命令数据指针；必填的数组参数为空、opts 不是落在缓冲内的完整文档、或超单包上限(MONGO_MAX_PACK_LENS)被丢弃时返回 nil</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lmongo_pack_insert(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    LPUB_LUD_ARG(lua, char, 2, docs);
    size_t dlens = lpub_check_lens(lua, 3, INT32_MAX);
    size_t optlens;
    char *opts = _lmongo_get_opts(lua, 4, &optlens);
    size_t size;
    void *pack = mongo_pack_insert(*ud, docs, dlens, opts, optlens, &size);
    return lpub_rtn_lud(lua, pack, size);
}
/// <summary>
/// 构造 update 更新命令包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="updates" type="lightuserdata">BSON 数组格式更新列表指针</param>
/// <param name="ulens" type="integer">updates 字节数</param>
/// <param name="opts" type="string|lightuserdata|nil">附加 BSON 选项；须是一篇完整 BSON 文档</param>
/// <param name="optslens" type="integer?">opts 为 lightuserdata 时必填，缓冲字节数，取值 [0, INT32_MAX]</param>
/// <returns type="lightuserdata?">命令数据指针；必填的数组参数为空、opts 不是落在缓冲内的完整文档、或超单包上限(MONGO_MAX_PACK_LENS)被丢弃时返回 nil</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lmongo_pack_update(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    LPUB_LUD_ARG(lua, char, 2, updates);
    size_t ulens = lpub_check_lens(lua, 3, INT32_MAX);
    size_t optlens;
    char *opts = _lmongo_get_opts(lua, 4, &optlens);
    size_t size;
    void *pack = mongo_pack_update(*ud, updates, ulens, opts, optlens, &size);
    return lpub_rtn_lud(lua, pack, size);
}
/// <summary>
/// 构造 delete 删除命令包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="deletes" type="lightuserdata">BSON 数组格式删除列表指针</param>
/// <param name="dlens" type="integer">deletes 字节数</param>
/// <param name="opts" type="string|lightuserdata|nil">附加 BSON 选项；须是一篇完整 BSON 文档</param>
/// <param name="optslens" type="integer?">opts 为 lightuserdata 时必填，缓冲字节数，取值 [0, INT32_MAX]</param>
/// <returns type="lightuserdata?">命令数据指针；必填的数组参数为空、opts 不是落在缓冲内的完整文档、或超单包上限(MONGO_MAX_PACK_LENS)被丢弃时返回 nil</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lmongo_pack_delete(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    LPUB_LUD_ARG(lua, char, 2, deletes);
    size_t dlens = lpub_check_lens(lua, 3, INT32_MAX);
    size_t optlens;
    char *opts = _lmongo_get_opts(lua, 4, &optlens);
    size_t size;
    void *pack = mongo_pack_delete(*ud, deletes, dlens, opts, optlens, &size);
    return lpub_rtn_lud(lua, pack, size);
}
/// <summary>
/// 构造 bulkWrite 批量写操作命令包（MongoDB 8.0+）
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="ops" type="lightuserdata">BSON 数组格式操作列表指针</param>
/// <param name="olens" type="integer">ops 字节数</param>
/// <param name="nsinfo" type="lightuserdata">BSON 数组格式命名空间信息指针</param>
/// <param name="nlens" type="integer">nsinfo 字节数</param>
/// <param name="opts" type="string|lightuserdata|nil">附加 BSON 选项；须是一篇完整 BSON 文档</param>
/// <param name="optslens" type="integer?">opts 为 lightuserdata 时必填，缓冲字节数，取值 [0, INT32_MAX]</param>
/// <returns type="lightuserdata?">命令数据指针；必填的数组参数为空、opts 不是落在缓冲内的完整文档、或超单包上限(MONGO_MAX_PACK_LENS)被丢弃时返回 nil</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lmongo_pack_bulkwrite(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    LPUB_LUD_ARG(lua, char, 2, ops);
    size_t olens = lpub_check_lens(lua, 3, INT32_MAX);
    LPUB_LUD_ARG(lua, char, 4, nsinfo);
    size_t nlens = lpub_check_lens(lua, 5, INT32_MAX);
    size_t optlens;
    char *opts = _lmongo_get_opts(lua, 6, &optlens);
    size_t size;
    void *pack = mongo_pack_bulkwrite(*ud, ops, olens, nsinfo, nlens, opts, optlens, &size);
    return lpub_rtn_lud(lua, pack, size);
}
/// <summary>
/// 构造 find 查询命令包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="filter" type="string|lightuserdata|nil">BSON 过滤条件；nil 或空缓冲都表示全部，取值规则见 _lmongo_get_opts</param>
/// <param name="flens" type="integer?">filter 字节数</param>
/// <param name="opts" type="string|lightuserdata|nil">附加 BSON 选项；须是一篇完整 BSON 文档</param>
/// <param name="optslens" type="integer?">opts 为 lightuserdata 时必填，缓冲字节数，取值 [0, INT32_MAX]</param>
/// <returns type="lightuserdata?">命令数据指针；opts 不是落在缓冲内的完整文档、或超单包上限(MONGO_MAX_PACK_LENS)被丢弃时返回 nil</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lmongo_pack_find(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    size_t flens;
    char *filter = _lmongo_get_opts(lua, 2, &flens);
    size_t optlens;
    char *opts = _lmongo_get_opts(lua, 4, &optlens);
    size_t size;
    void *pack = mongo_pack_find(*ud, filter, flens, opts, optlens, &size);
    return lpub_rtn_lud(lua, pack, size);
}
/// <summary>
/// 构造 aggregate 聚合命令包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="pipeline" type="lightuserdata">BSON 数组格式聚合管道指针</param>
/// <param name="pllens" type="integer">pipeline 字节数</param>
/// <param name="opts" type="string|lightuserdata|nil">附加 BSON 选项；须是一篇完整 BSON 文档</param>
/// <param name="optslens" type="integer?">opts 为 lightuserdata 时必填，缓冲字节数，取值 [0, INT32_MAX]</param>
/// <returns type="lightuserdata?">命令数据指针；必填的数组参数为空、opts 不是落在缓冲内的完整文档、或超单包上限(MONGO_MAX_PACK_LENS)被丢弃时返回 nil</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lmongo_pack_aggregate(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    LPUB_LUD_ARG(lua, char, 2, pipeline);
    size_t pllens = lpub_check_lens(lua, 3, INT32_MAX);
    size_t optlens;
    char *opts = _lmongo_get_opts(lua, 4, &optlens);
    size_t size;
    void *pack = mongo_pack_aggregate(*ud, pipeline, pllens, opts, optlens, &size);
    return lpub_rtn_lud(lua, pack, size);
}
/// <summary>
/// 构造 getMore 获取游标后续批次命令包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="cursorid" type="integer">游标 ID</param>
/// <param name="opts" type="string|lightuserdata|nil">附加 BSON 选项；须是一篇完整 BSON 文档</param>
/// <param name="optslens" type="integer?">opts 为 lightuserdata 时必填，缓冲字节数，取值 [0, INT32_MAX]</param>
/// <returns type="lightuserdata?">命令数据指针；opts 不是落在缓冲内的完整文档、或超单包上限(MONGO_MAX_PACK_LENS)被丢弃时返回 nil</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lmongo_pack_getmore(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    int64_t cursorid = (int64_t)luaL_checkinteger(lua, 2);
    size_t optlens;
    char *opts = _lmongo_get_opts(lua, 3, &optlens);
    size_t size;
    void *pack = mongo_pack_getmore(*ud, cursorid, opts, optlens, &size);
    return lpub_rtn_lud(lua, pack, size);
}
/// <summary>
/// 构造 killCursors 关闭游标命令包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="cursorids" type="lightuserdata">BSON 数组格式游标 ID 列表指针</param>
/// <param name="cslens" type="integer">cursorids 字节数</param>
/// <param name="opts" type="string|lightuserdata|nil">附加 BSON 选项；须是一篇完整 BSON 文档</param>
/// <param name="optslens" type="integer?">opts 为 lightuserdata 时必填，缓冲字节数，取值 [0, INT32_MAX]</param>
/// <returns type="lightuserdata?">命令数据指针；必填的数组参数为空、opts 不是落在缓冲内的完整文档、或超单包上限(MONGO_MAX_PACK_LENS)被丢弃时返回 nil</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lmongo_pack_killcursors(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    LPUB_LUD_ARG(lua, char, 2, cursorids);
    size_t cslens = lpub_check_lens(lua, 3, INT32_MAX);
    size_t optlens;
    char *opts = _lmongo_get_opts(lua, 4, &optlens);
    size_t size;
    void *pack = mongo_pack_killcursors(*ud, cursorids, cslens, opts, optlens, &size);
    return lpub_rtn_lud(lua, pack, size);
}
/// <summary>
/// 构造 distinct 去重查询命令包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="key" type="string">去重字段名</param>
/// <param name="query" type="string|lightuserdata|nil">BSON 过滤条件；nil 或空缓冲都表示全部，取值规则见 _lmongo_get_opts</param>
/// <param name="qlens" type="integer?">query 字节数</param>
/// <param name="opts" type="string|lightuserdata|nil">附加 BSON 选项；须是一篇完整 BSON 文档</param>
/// <param name="optslens" type="integer?">opts 为 lightuserdata 时必填，缓冲字节数，取值 [0, INT32_MAX]</param>
/// <returns type="lightuserdata?">命令数据指针；opts 不是落在缓冲内的完整文档、或超单包上限(MONGO_MAX_PACK_LENS)被丢弃时返回 nil</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lmongo_pack_distinct(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    const char *key = luaL_checkstring(lua, 2);
    size_t qlens;
    char *query = _lmongo_get_opts(lua, 3, &qlens);
    size_t optlens;
    char *opts = _lmongo_get_opts(lua, 5, &optlens);
    size_t size;
    void *pack = mongo_pack_distinct(*ud, key, query, qlens, opts, optlens, &size);
    return lpub_rtn_lud(lua, pack, size);
}
/// <summary>
/// 构造 findAndModify 原子查找并修改/删除命令包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="query" type="string|lightuserdata|nil">BSON 过滤条件；nil 或空缓冲都表示全部，取值规则见 _lmongo_get_opts</param>
/// <param name="qlens" type="integer?">query 字节数</param>
/// <param name="remove" type="integer">非零表示删除匹配文档</param>
/// <param name="pipeline" type="integer">非零时 update 为聚合数组</param>
/// <param name="update" type="string|lightuserdata|nil">BSON 更新文档或聚合数组；remove 非零时可省，为零时必填——nil 与空缓冲一律报错</param>
/// <param name="ulens" type="integer?">update 字节数</param>
/// <param name="opts" type="string|lightuserdata|nil">附加 BSON 选项；须是一篇完整 BSON 文档</param>
/// <param name="optslens" type="integer?">opts 为 lightuserdata 时必填，缓冲字节数，取值 [0, INT32_MAX]</param>
/// <returns type="lightuserdata?">命令数据指针；opts 不是落在缓冲内的完整文档、或超单包上限(MONGO_MAX_PACK_LENS)被丢弃时返回 nil</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lmongo_pack_findandmodify(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    size_t qlens;
    char *query = _lmongo_get_opts(lua, 2, &qlens);
    int32_t remove = (0 != luaL_checkinteger(lua, 4));
    int32_t pipeline = (0 != luaL_checkinteger(lua, 5));
    size_t ulens;
    char *update = _lmongo_get_opts(lua, 6, &ulens);
    luaL_argcheck(lua, remove || NULL != update, 6, "update required when remove is 0");
    size_t optlens;
    char *opts = _lmongo_get_opts(lua, 8, &optlens);
    size_t size;
    void *pack = mongo_pack_findandmodify(*ud, query, qlens, remove, pipeline, update, ulens, opts, optlens, &size);
    return lpub_rtn_lud(lua, pack, size);
}
/// <summary>
/// 构造 count 文档计数命令包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="query" type="string|lightuserdata|nil">BSON 过滤条件；nil 或空缓冲都表示全部，取值规则见 _lmongo_get_opts</param>
/// <param name="qlens" type="integer?">query 字节数</param>
/// <param name="opts" type="string|lightuserdata|nil">附加 BSON 选项；须是一篇完整 BSON 文档</param>
/// <param name="optslens" type="integer?">opts 为 lightuserdata 时必填，缓冲字节数，取值 [0, INT32_MAX]</param>
/// <returns type="lightuserdata?">命令数据指针；opts 不是落在缓冲内的完整文档、或超单包上限(MONGO_MAX_PACK_LENS)被丢弃时返回 nil</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lmongo_pack_count(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    size_t qlens;
    char *query = _lmongo_get_opts(lua, 2, &qlens);
    size_t optlens;
    char *opts = _lmongo_get_opts(lua, 4, &optlens);
    size_t size;
    void *pack = mongo_pack_count(*ud, query, qlens, opts, optlens, &size);
    return lpub_rtn_lud(lua, pack, size);
}
/// <summary>
/// 构造 createIndexes 创建索引命令包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="indexes" type="lightuserdata">BSON 数组格式索引定义指针</param>
/// <param name="ilens" type="integer">indexes 字节数</param>
/// <param name="opts" type="string|lightuserdata|nil">附加 BSON 选项；须是一篇完整 BSON 文档</param>
/// <param name="optslens" type="integer?">opts 为 lightuserdata 时必填，缓冲字节数，取值 [0, INT32_MAX]</param>
/// <returns type="lightuserdata?">命令数据指针；必填的数组参数为空、opts 不是落在缓冲内的完整文档、或超单包上限(MONGO_MAX_PACK_LENS)被丢弃时返回 nil</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lmongo_pack_createindexes(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    LPUB_LUD_ARG(lua, char, 2, indexes);
    size_t ilens = lpub_check_lens(lua, 3, INT32_MAX);
    size_t optlens;
    char *opts = _lmongo_get_opts(lua, 4, &optlens);
    size_t size;
    void *pack = mongo_pack_createindexes(*ud, indexes, ilens, opts, optlens, &size);
    return lpub_rtn_lud(lua, pack, size);
}
/// <summary>
/// 构造 dropIndexes 删除索引命令包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="indexes" type="lightuserdata">BSON 数组格式索引名列表指针</param>
/// <param name="ilens" type="integer">indexes 字节数</param>
/// <param name="opts" type="string|lightuserdata|nil">附加 BSON 选项；须是一篇完整 BSON 文档</param>
/// <param name="optslens" type="integer?">opts 为 lightuserdata 时必填，缓冲字节数，取值 [0, INT32_MAX]</param>
/// <returns type="lightuserdata?">命令数据指针；必填的数组参数为空、opts 不是落在缓冲内的完整文档、或超单包上限(MONGO_MAX_PACK_LENS)被丢弃时返回 nil</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lmongo_pack_dropindexes(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    LPUB_LUD_ARG(lua, char, 2, indexes);
    size_t ilens = lpub_check_lens(lua, 3, INT32_MAX);
    size_t optlens;
    char *opts = _lmongo_get_opts(lua, 4, &optlens);
    size_t size;
    void *pack = mongo_pack_dropindexes(*ud, indexes, ilens, opts, optlens, &size);
    return lpub_rtn_lud(lua, pack, size);
}
/// <summary>
/// 构造 startSession 命令包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmongo_pack_startsession(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    size_t size;
    void *pack = mongo_pack_startsession(*ud, &size);
    return lpub_rtn_lud(lua, pack, size);
}
/// <summary>
/// 构造 SCRAM 认证第一步（saslStart）请求包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="authmod" type="string">认证机制名（如 "SCRAM-SHA-256"）</param>
/// <returns type="lightuserdata?">命令数据指针；失败返回 nil</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lmongo_pack_auth_first(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    const char *authmod = luaL_checkstring(lua, 2);
    size_t size;
    void *pack = mongo_pack_scram_client_first(*ud, authmod, &size);
    return lpub_rtn_lud(lua, pack, size);
}
/// <summary>
/// 构造 SCRAM 认证第二步（saslContinue）请求包
/// </summary>
/// <param name="self" type="userdata">mongo 对象</param>
/// <param name="convid" type="integer">对话 id（来自第一步响应）</param>
/// <param name="payload" type="lightuserdata">客户端 final payload 指针，由 crypt.scram 算出
///     （c=,r=,p=）；不是 parse_auth_response 给的那份服务端 payload，本函数不要求 NUL 结尾</param>
/// <param name="plens" type="integer">payload 字节数</param>
/// <returns type="lightuserdata?">命令数据指针；payload 撑得总长超单包上限(MONGO_MAX_PACK_LENS)时返回 nil，与数据长度一并为 nil</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lmongo_pack_auth_final(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    int32_t convid = (int32_t)luaL_checkinteger(lua, 2);
    LPUB_LUD_ARG(lua, char, 3, payload);
    // 走 lpub_check_lens 拿上界：越界直接 bson_append_binary 的 ASSERTAB 会打死整个进程
    size_t plens = lpub_check_lens(lua, 4, INT32_MAX);
    size_t size;
    void *pack = mongo_pack_scram_client_final(*ud, convid, payload, plens, &size);
    return lpub_rtn_lud(lua, pack, size);
}
/// <summary>
/// 解析消息包 Section 类型
/// </summary>
/// <param name="mgopack" type="lightuserdata">mgopack_ctx 指针</param>
/// <returns type="integer">Section 类型（0 = 正文，1 = 文档序列）</returns>
static int32_t _lmongo_pack_type(lua_State *lua) {
    LPUB_LUD_ARG(lua, mgopack_ctx, 1, mgopack);
    lua_pushinteger(lua, mgopack->kind);
    return 1;
}
/// <summary>
/// 返回消息包的 BSON 文档数据
/// </summary>
/// <param name="mgopack" type="lightuserdata">mgopack_ctx 指针</param>
/// <returns type="lightuserdata">BSON 文档指针（指向消息缓冲内部，随该消息释放而失效）</returns>
/// <returns type="integer">文档字节数；本段无正文时为 0</returns>
static int32_t _lmongo_doc(lua_State *lua) {
    LPUB_LUD_ARG(lua, mgopack_ctx, 1, mgopack);
    return lpub_rtn_lud(lua, mgopack->doc, mgopack->dlens);
}
/// <summary>
/// 返回消息包请求 ID
/// </summary>
/// <param name="mgopack" type="lightuserdata">mgopack_ctx 指针</param>
/// <returns type="integer">请求 ID</returns>
static int32_t _lmongo_reqid(lua_State *lua) {
    LPUB_LUD_ARG(lua, mgopack_ctx, 1, mgopack);
    lua_pushinteger(lua, mgopack->reqid);
    return 1;
}
/// <summary>
/// 返回消息包标志位
/// </summary>
/// <param name="mgopack" type="lightuserdata">mgopack_ctx 指针</param>
/// <returns type="integer">flags 字段</returns>
static int32_t _lmongo_flags(lua_State *lua) {
    LPUB_LUD_ARG(lua, mgopack_ctx, 1, mgopack);
    lua_pushinteger(lua, (lua_Integer)mgopack->flags);
    return 1;
}
/// <summary>
/// 从响应包中提取游标 ID
/// </summary>
/// <param name="mgopack" type="lightuserdata">mgopack_ctx 指针</param>
/// <returns type="integer">游标 ID；0 表示无游标</returns>
static int32_t _lmongo_cursorid(lua_State *lua) {
    LPUB_LUD_ARG(lua, mgopack_ctx, 1, mgopack);
    lua_pushinteger(lua, mongo_cursorid(mgopack));
    return 1;
}
/// <summary>
/// 解析 SCRAM 认证响应
/// </summary>
/// <param name="mgopack" type="lightuserdata">mgopack_ctx 响应指针</param>
/// <returns type="boolean">成功 true（其余 4 个值有效），失败 false（其余 4 个为 nil）</returns>
/// <returns type="integer">对话 id（convid）；成功时有效</returns>
/// <returns type="boolean">是否已完成最终认证；成功时有效</returns>
/// <returns type="lightuserdata">payload 指针；成功时有效；指针指向 mgopack 内部缓冲区，需在当前协程周期内消费</returns>
/// <returns type="integer">payload 字节数；成功时有效</returns>
static int32_t _lmongo_parse_auth_response(lua_State *lua) {
    LPUB_LUD_ARG(lua, mgopack_ctx, 1, mgopack);
    int32_t convid = 0;
    int32_t done = 0;
    char *payload = NULL;
    size_t plens = 0;
    int32_t ok = mongo_parse_auth_response(mgopack, &convid, &done, &payload, &plens);
    if (!ok) {
        lua_pushboolean(lua, 0);
        return 1 + lpub_rtn_nil(lua, 4);
    }
    lua_pushboolean(lua, 1);
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
        { "pack_check_flag",      _lmongo_pack_check_flag },
        { "cursorid",             _lmongo_cursorid },
        { "parse_auth_response",  _lmongo_parse_auth_response },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "try_connect",          _lmongo_try_connect },
        { "sock_id",              _lmongo_sock_id },
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
        { "clear_session",        _lmongo_clear_session },
        { "session_touch",        _lmongo_session_touch },
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
    LPUB_UD_ARG(lua, mongo_ctx, MT_MONGO, ud, "mongo freed");
    mongo_ctx *mongo = *ud;
    size_t uuid_lens;
    const char *uuid_str = luaL_checklstring(lua, 2, &uuid_lens);
    int32_t timeout = (int32_t)lpub_check_range(lua, 3, 0, INT32_MAX, "session timeout minutes out of range");
    if (UUID_LENS != uuid_lens) {
        return lpub_rtn_nil(lua, 1);
    }
    mongo_session **psession = (mongo_session **)lpub_push_ud(lua, NULL, MT_MONGO_SESSION);
    mongo_session *session;
    CALLOC(session, 1, sizeof(mongo_session));
    memcpy(session->uuid, uuid_str, UUID_LENS);
    session->mongo = mongo;
    session->timeoutmin = timeout;
    mongo_session_renew(session);
    *psession = session;
    lua_pushvalue(lua, 1);
    lua_setiuservalue(lua, -2, 1);
    return 1;
}
/// <summary>
/// 把本会话的超时时刻续到"此刻 + logicalSessionTimeoutMinutes"。
/// 与 mongo 上的 session_touch 分工：那个续的是连接当前绑定的会话，事务外恒为空；
/// refreshSessions 允许在事务外发，续期只能落到会话自己身上
/// </summary>
/// <param name="self" type="userdata">session 对象</param>
/// <returns>无</returns>
static int32_t _lmongo_session_renew(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_session, MT_MONGO_SESSION, ud, "session freed");
    mongo_session_renew(*ud);
    return 0;
}
/// <summary>
/// 会话距超时还剩多少秒；调用方据此决定何时发 refreshSessions（session:refresh()）
/// </summary>
/// <param name="self" type="userdata">session 对象</param>
/// <returns type="integer">剩余秒数；已过期为 0 或负数；服务端未给出超时分钟数时恒为 0</returns>
static int32_t _lmongo_session_expires(lua_State *lua) {
    LPUB_UD_ARG(lua, mongo_session, MT_MONGO_SESSION, ud, "session freed");
    lua_pushinteger(lua, (lua_Integer)mongo_session_expires(*ud));
    return 1;
}
// 解除宿主对本 session 的绑定。宿主可能已被 m:__gc() 先释放，那时 session->mongo 是悬垂指针，
// 连读都不能读，所以先确认宿主还在
static void _lmongo_session_unbind(lua_State *lua, mongo_session *session) {
    if (NULL != lpub_owner_ptr(lua, MT_MONGO)
        && NULL != session->mongo && session->mongo->session == session) {
        session->mongo->session = NULL;
    }
}
/// <summary>
/// 释放会话：先解绑宿主 mongo 上的引用，再释放 options 与自身；同时作为 __gc 调用。
/// 不发送 endSessions，那一步由 Lua 层负责。可重复调用——内部指针置空后二次调用即早退
/// </summary>
/// <param name="self" type="userdata">mongo 会话对象</param>
/// <returns>无</returns>
static int32_t _lmongo_session_free(lua_State *lua) {
    mongo_session **psession = luaL_checkudata(lua, 1, MT_MONGO_SESSION);
    if (NULL != *psession) {
        mongo_session *session = *psession;
        _lmongo_session_unbind(lua, session);
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
/// <returns type="boolean">成功 true；该连接上已有别的 session 处于事务中、或本会话已随旧连接失效时 false</returns>
static int32_t _lmongo_session_begin(lua_State *lua) {
    LMONGO_SESSION_ARG(lua, psession);
    return lpub_rtn_bool(lua, ERR_OK == mongo_begin(*psession));
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
    _lmongo_session_unbind(lua, session);
    return 0;
}
/// <summary>
/// 构造 refreshSessions 刷新会话命令包
/// </summary>
/// <param name="self" type="userdata">session 对象</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmongo_session_pack_refresh(lua_State *lua) {
    LMONGO_SESSION_ARG(lua, psession);
    size_t size;
    void *pack = mongo_pack_refreshsession(*psession, &size);
    return lpub_rtn_lud(lua, pack, size);
}
/// <summary>
/// 构造 endSessions 结束会话命令包
/// </summary>
/// <param name="self" type="userdata">session 对象</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmongo_session_pack_endsession(lua_State *lua) {
    LMONGO_SESSION_ARG(lua, psession);
    size_t size;
    void *pack = mongo_pack_endsession(*psession, &size);
    return lpub_rtn_lud(lua, pack, size);
}
/// <summary>
/// 构造 commitTransaction 提交事务命令包
/// </summary>
/// <param name="self" type="userdata">session 对象</param>
/// <param name="opts" type="string|lightuserdata|nil">附加 BSON 选项；须是一篇完整 BSON 文档</param>
/// <param name="optslens" type="integer?">opts 为 lightuserdata 时必填，缓冲字节数，取值 [0, INT32_MAX]</param>
/// <returns type="lightuserdata?">命令数据指针；opts 不是落在缓冲内的完整文档、或超单包上限(MONGO_MAX_PACK_LENS)被丢弃、
/// 或连接已不再绑定该 session 时返回 nil</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lmongo_session_pack_commit(lua_State *lua) {
    LMONGO_SESSION_ARG(lua, psession);
    size_t optlens;
    char *opts = _lmongo_get_opts(lua, 2, &optlens);
    size_t size;
    void *pack = mongo_pack_committransaction(*psession, opts, optlens, &size);
    return lpub_rtn_lud(lua, pack, size);
}
/// <summary>
/// 构造 abortTransaction 回滚事务命令包
/// </summary>
/// <param name="self" type="userdata">session 对象</param>
/// <param name="opts" type="string|lightuserdata|nil">附加 BSON 选项；须是一篇完整 BSON 文档</param>
/// <param name="optslens" type="integer?">opts 为 lightuserdata 时必填，缓冲字节数，取值 [0, INT32_MAX]</param>
/// <returns type="lightuserdata?">命令数据指针；opts 不是落在缓冲内的完整文档、或超单包上限(MONGO_MAX_PACK_LENS)被丢弃、
/// 或连接已不再绑定该 session 时返回 nil</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lmongo_session_pack_abort(lua_State *lua) {
    LMONGO_SESSION_ARG(lua, psession);
    size_t optlens;
    char *opts = _lmongo_get_opts(lua, 2, &optlens);
    size_t size;
    void *pack = mongo_pack_aborttransaction(*psession, opts, optlens, &size);
    return lpub_rtn_lud(lua, pack, size);
}
//mongo.session
LUAMOD_API int luaopen_mongo_session(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lmongo_session_new },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "begin",            _lmongo_session_begin },
        { "renew",            _lmongo_session_renew },
        { "expires_in",       _lmongo_session_expires },
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
