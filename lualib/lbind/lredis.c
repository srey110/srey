#include "lbind/lpub.h"

// Redis 聚合表一次预分配的槽位上限。不是协议上限,只是"预分配到此为止":元素个数由对端声明,
// 超出的部分照常按需增长
#define REDIS_PREALLOC_MAX 4096

// redis.unpack 的一层未闭合聚合；这层的表与暂存键在 Lua 栈上（布局见 _lredis_unpack_multi）
typedef struct redis_unpack_frame {
    int32_t status; // map/attr：0=期望 key，1=期望 val；数组类：已追加的元素个数（下一个下标减 1）
    int32_t ismap;  // map / attr：键值交替
    int32_t isattr; // attr 闭合后不再消费父层
    int64_t nelem;  // 剩余待处理节点数（map/attr 已 ×2）
}redis_unpack_frame;

// 内部辅助：构造 Redis 聚合类型（array/set/map/push/attr）的空容器表。
// 元素随后由上层追加进这张表：array/set/push 落数组部分，map/attr 按 key 落哈希部分，
// 故按 ismap 分开预分配，省掉从 0 起逐次翻倍的 rehash。extra 是额外留给哨兵字段的哈希槽。
// 预分配量卡 REDIS_PREALLOC_MAX：nelem 是对端声明的数字，超出的部分照常增长
static void _lredis_agg(lua_State *lua, int64_t nelem, int32_t ismap, int32_t extra) {
    int32_t pre = 0;
    if (nelem > 0) {
        pre = (int32_t)(nelem > REDIS_PREALLOC_MAX ? REDIS_PREALLOC_MAX : nelem);
    }
    lua_createtable(lua, 0 != ismap ? 0 : pre, (0 != ismap ? pre : 0) + extra);
}
// 压一个节点的值：标量原样压，聚合压空容器（见 _lredis_agg）。返回聚合类型名，标量返回 NULL
static const char *_lredis_push(lua_State *lua, redis_pack_ctx *pk, int32_t extra) {
    switch (pk->prot) {
    case RESP_STRING:// 简单字符串
    case RESP_ERROR:// 错误字符串
    case RESP_BSTRING:// 批量字符串
    case RESP_BERROR:// 批量错误
    case RESP_VERB:// 带类型的字符串
        if (pk->len < 0) {
            lua_pushnil(lua);
        } else if (0 == pk->len) {
            lua_pushstring(lua, "");
        } else {
            lua_pushlstring(lua, pk->data, (size_t)pk->len);
        }
        return NULL;
    case RESP_INTEGER:// 整数
        lua_pushinteger(lua, pk->ival);
        return NULL;
    case RESP_BIGNUM:// 大整数(任意精度,以字符串返回)
        lua_pushlstring(lua, pk->data, (size_t)pk->len);
        return NULL;
    case RESP_BOOL:// 布尔值
        lua_pushboolean(lua, (int32_t)pk->ival);
        return NULL;
    case RESP_DOUBLE:// 浮点数
        lua_pushnumber(lua, pk->dval);
        return NULL;
    case RESP_ARRAY:// 数组
        _lredis_agg(lua, pk->nelem, 0, extra);
        return "array";
    case RESP_SET:// 集合
        _lredis_agg(lua, pk->nelem, 0, extra);
        return "set";
    case RESP_PUSHE:// 推送消息
        _lredis_agg(lua, pk->nelem, 0, extra);
        return "push";
    case RESP_MAP:// 映射
        _lredis_agg(lua, pk->nelem, 1, extra);
        return "map";
    case RESP_ATTR:// 属性
        _lredis_agg(lua, pk->nelem, 1, extra);
        return "attr";
    case RESP_NIL:// Null 值
    default:
        lua_pushnil(lua);
        return NULL;
    }
}
// 是否错误应答（简单错误 / 批量错误）
static inline int32_t _lredis_iserr(int32_t prot) {
    return RESP_ERROR == prot || RESP_BERROR == prot;
}
// value / unpack 共用：顶层错误应答压 false 与错误文本，返回 2
static int32_t _lredis_rtnerr(lua_State *lua, redis_pack_ctx *pk) {
    lua_pushboolean(lua, 0);
    _lredis_push(lua, pk, 0);
    return 2;
}
// value / node / next / unpack 共用：nil / none 与 NULL 指针都返回 NULL(调用方按各自个数压 nil)，其余非 light userdata 报错
static redis_pack_ctx *_lredis_opt(lua_State *lua) {
    if (lua_isnoneornil(lua, 1)) {
        return NULL;
    }
    LUACHECK_LUDATA_OPT(lua, 1);
    return lua_touserdata(lua, 1);
}
/// <summary>
/// 解析一个 Redis RESP 节点的值
/// </summary>
/// <param name="pk" type="lightuserdata?">redis_pack_ctx 节点指针；nil 时返回 nil</param>
/// <returns type="string|integer|number|boolean|nil|RedisAggValue">标量直接返回；聚合类型（array/set/map/push/attr）返回 RedisAggValue；错误应答返回 false</returns>
/// <returns type="string?">错误应答的文本；其余情况没有这个返回值</returns>
static int32_t _lredis_value(lua_State *lua) {
    redis_pack_ctx *pk = _lredis_opt(lua);
    if (NULL == pk) {
        return lpub_rtn_nil(lua, 1);
    }
    if (_lredis_iserr(pk->prot)) {
        return _lredis_rtnerr(lua, pk);
    }
    const char *kind = _lredis_push(lua, pk, 2);
    if (NULL != kind) {
        lua_pushstring(lua, kind);
        lua_setfield(lua, -2, "resp_type");
        lua_pushinteger(lua, pk->nelem);
        lua_setfield(lua, -2, "resp_nelem");
    }
    return 1;
}
/// <summary>
/// 一次取齐一个 Redis RESP 节点：值、聚合类型、元素计数、下一节点。聚合返回的是不带
/// resp_type / resp_nelem 哨兵的空容器，类型与计数从第 2、3 个返回值拿
/// </summary>
/// <param name="pk" type="lightuserdata?">redis_pack_ctx 节点指针；nil 时 4 个返回值全是 nil</param>
/// <returns type="string|integer|number|boolean|RedisAggPayload|nil">节点值；聚合为按 nelem 预分配的空表</returns>
/// <returns type="string?">聚合类型名 array/set/push/map/attr；错误应答为 "error"（节点值是错误文本）；其余标量为 nil</returns>
/// <returns type="integer?">聚合元素计数（-1 为 RESP3 的 nil 聚合）；标量为 nil</returns>
/// <returns type="lightuserdata?">下一个节点指针；没有后续节点为 nil</returns>
static int32_t _lredis_node(lua_State *lua) {
    redis_pack_ctx *pk = _lredis_opt(lua);
    if (NULL == pk) {
        return lpub_rtn_nil(lua, 4);
    }
    const char *kind = _lredis_push(lua, pk, 0);
    if (NULL == kind) {
        if (_lredis_iserr(pk->prot)) {
            lua_pushliteral(lua, "error");
        } else {
            lua_pushnil(lua);
        }
        lua_pushnil(lua);
    } else {
        lua_pushstring(lua, kind);
        lua_pushinteger(lua, pk->nelem);
    }
    if (NULL == pk->next) {
        lua_pushnil(lua);
    } else {
        lua_pushlightuserdata(lua, pk->next);
    }
    return 4;
}
/// <summary>
/// 获取 Redis RESP 链表中下一个节点指针
/// </summary>
/// <param name="pk" type="lightuserdata?">redis_pack_ctx 节点指针；nil 时返回 nil</param>
/// <returns type="lightuserdata?">下一个节点指针；无后续节点返回 nil</returns>
static int32_t _lredis_next(lua_State *lua) {
    redis_pack_ctx *pk = _lredis_opt(lua);
    if (NULL == pk
        || NULL == pk->next) {
        return lpub_rtn_nil(lua, 1);
    }
    lua_pushlightuserdata(lua, pk->next);
    return 1;
}
// 是否聚合节点（array / set / push / map / attr）
static inline int32_t _lredis_isagg(int32_t prot) {
    return RESP_ARRAY == prot || RESP_SET == prot || RESP_PUSHE == prot
        || RESP_MAP == prot || RESP_ATTR == prot;
}
// 聚合节点压值：nelem >= 0 压按 nelem 预分配的空容器（同 redis.node，0 即空表），
// <0（RESP3 的 nil 聚合）negfalse 非 0 压 false，否则压 nil
static void _lredis_aggval(lua_State *lua, redis_pack_ctx *pk, int32_t negfalse) {
    if (pk->nelem >= 0) {
        _lredis_push(lua, pk, 0);
    } else if (0 != negfalse) {
        lua_pushboolean(lua, 0);
    } else {
        lua_pushnil(lua);
    }
}
// 压要写进表的标量值：nil 换成 false（表里的值不能是 nil），错误应答写成 { err = 文本 }
static void _lredis_scalar(lua_State *lua, redis_pack_ctx *pk) {
    _lredis_push(lua, pk, 0);
    if (lua_isnil(lua, -1)) {
        lua_pop(lua, 1);
        lua_pushboolean(lua, 0);
    } else if (_lredis_iserr(pk->prot)) {
        lua_createtable(lua, 0, 1);
        lua_insert(lua, -2);
        lua_setfield(lua, -2, "err");
    }
}
// 弹出栈顶值追加到 idx 处的表尾，同 t[#t + 1] = v。只给 map/attr 父层与顶层多值用：
// 数组类父层的表只由顺序追加建成，下标直接取帧里的计数，省掉每次求表长
static inline void _lredis_append(lua_State *lua, int32_t idx) {
    lua_rawseti(lua, idx, (lua_Integer)lua_rawlen(lua, idx) + 1);
}
// 弹出栈顶值，以 kidx 处暂存的键写进 tidx 处的表；键为 nil 或 NaN（RESP3 的 ,nan）时整对丢弃
static void _lredis_mapset(lua_State *lua, int32_t tidx, int32_t kidx) {
    int32_t type = lua_type(lua, kidx);
    if (LUA_TNIL == type
        || (LUA_TNUMBER == type && isnan(lua_tonumber(lua, kidx)))) {
        lua_pop(lua, 1);
        return;
    }
    lua_pushvalue(lua, kidx);
    lua_insert(lua, -2);
    lua_rawset(lua, tidx);
}
// 栈顶的聚合表开成新的一层：补一格暂存键，记下计数与类型。返回新深度
static int32_t _lredis_open(lua_State *lua, redis_unpack_frame *stack, int32_t depth,
    redis_pack_ctx *pk) {
    ASSERTAB(depth < REDIS_MAX_DEPTH, "redis unpack stack overflow.");
    redis_unpack_frame *fr = &stack[depth];
    fr->status = 0;
    fr->isattr = (RESP_ATTR == pk->prot);
    fr->ismap = (fr->isattr || RESP_MAP == pk->prot);
    fr->nelem = fr->ismap ? pk->nelem * 2 : pk->nelem;
    lua_pushnil(lua);
    return depth + 1;
}
// 栈顶层消费一个节点：计数归零即出栈并级联消费父层，attr 出栈后停（它不占父层的元素数）；
// 出栈层在 Lua 栈上的表与暂存键一并丢掉。返回新深度
static int32_t _lredis_consume(lua_State *lua, redis_unpack_frame *stack, int32_t depth) {
    while (depth > 0) {
        stack[depth - 1].nelem--;
        if (stack[depth - 1].nelem > 0) {
            break;
        }
        depth--;
        if (0 != stack[depth].isattr) {
            break;
        }
    }
    lua_settop(lua, 2 * depth + 2);
    return depth;
}
// 多节点回复，首节点必须是聚合。Lua 栈布局：1 = pk，2 = 结果，第 i 层（0 起）的表在 3+2i、
// 暂存键在 4+2i；每处理完一个节点栈顶都回到 2+2*depth
static int32_t _lredis_unpack_multi(lua_State *lua, redis_pack_ctx *pk) {
    if (!_lredis_isagg(pk->prot)) {
        LOG_WARN("resp message error.");
        return lpub_rtn_nil(lua, 1);
    }
    lua_settop(lua, 1);
    luaL_checkstack(lua, 2 * REDIS_MAX_DEPTH + 8, NULL);// 每层占表与暂存键两格，另留几格临时值
    redis_unpack_frame stack[REDIS_MAX_DEPTH];
    int32_t depth = 0;
    // 首节点为 attr 时结果多包一层数组：{val} / {{}} / {}，后面的值依次追加
    int32_t isattr = (RESP_ATTR == pk->prot);
    if (pk->nelem > 0) {
        if (isattr) {
            lua_createtable(lua, 1, 0);
        }
        _lredis_aggval(lua, pk, 0);
        lua_pushvalue(lua, -1);
        if (isattr) {
            lua_rawseti(lua, 2, 1);
        }
        depth = _lredis_open(lua, stack, depth, pk);
    } else if (0 == pk->nelem) {
        lua_createtable(lua, isattr, 0);
        if (isattr) {
            lua_createtable(lua, 0, 0);
            lua_rawseti(lua, 2, 1);
        }
    } else if (isattr) {
        lua_createtable(lua, 0, 0);
    } else {
        return lpub_rtn_nil(lua, 1);
    }
    redis_unpack_frame *parent;
    int32_t tidx;
    int32_t askey;
    for (pk = pk->next; NULL != pk; pk = pk->next) {
        if (0 == depth) {
            // 顶层多值（attr 之后的数据）：顺序追加进结果
            if (!_lredis_isagg(pk->prot)) {
                _lredis_scalar(lua, pk);
                _lredis_append(lua, 2);
                continue;
            }
            _lredis_aggval(lua, pk, 1);
            if (pk->nelem > 0) {
                lua_pushvalue(lua, -1);
                _lredis_append(lua, 2);
                depth = _lredis_open(lua, stack, depth, pk);
            } else {
                _lredis_append(lua, 2);
            }
            continue;
        }
        parent = &stack[depth - 1];
        tidx = 2 * depth + 1;
        if (!_lredis_isagg(pk->prot)) {
            if (0 == parent->ismap) {
                _lredis_scalar(lua, pk);
                lua_rawseti(lua, tidx, ++parent->status);
            } else if (0 == parent->status) {
                parent->status = 1;
                _lredis_push(lua, pk, 0);
                lua_replace(lua, tidx + 1);
            } else {
                parent->status = 0;
                _lredis_scalar(lua, pk);
                _lredis_mapset(lua, tidx, tidx + 1);
            }
            depth = _lredis_consume(lua, stack, depth);
            continue;
        }
        // 父为 map/attr 且自身不是 attr 时键值交替，否则顺序追加。
        // 当键暂存时 nil 聚合取 nil（随后整对丢弃），其余位置取 false
        isattr = (RESP_ATTR == pk->prot);
        askey = (0 != parent->ismap && !isattr && 0 == parent->status);
        _lredis_aggval(lua, pk, !askey);
        lua_pushvalue(lua, -1);
        if (askey) {
            parent->status = 1;
            lua_replace(lua, tidx + 1);
        } else if (0 != parent->ismap && !isattr) {
            parent->status = 0;
            _lredis_mapset(lua, tidx, tidx + 1);
        } else if (0 == parent->ismap) {
            lua_rawseti(lua, tidx, ++parent->status);
        } else {
            _lredis_append(lua, tidx);// map 里的 attr 追加到 #t + 1
        }
        if (pk->nelem > 0) {
            depth = _lredis_open(lua, stack, depth, pk);
        } else if (isattr) {
            lua_pop(lua, 1);// 空 attr 与 nil attr 不占父层的元素数
        } else {
            depth = _lredis_consume(lua, stack, depth);
        }
    }
    lua_settop(lua, 2);
    return 1;
}
/// <summary>
/// 把一条 RESP 回复（redis_pack_ctx 链表）整段转成 Lua 值。聚合转成表：array/set/push 为数组，
/// map/attr 为键值表，键为 nil 或 NaN 的整对丢弃；写进表的 nil 一律写成 false（当值的 nil 聚合同样）。
/// 嵌在聚合里的 attr 按顺序追加进所在的表（父为 map 也是追加到 #t + 1）。回复以 attr 开头时结果多包
/// 一层数组：首个 attr 的表在前（空 attr 为空表，nil attr 不占位），其后的 attr 与数据依次追加。
/// 多节点却不以聚合开头、聚合声明了元素却没有后继节点时打 WARN。
/// 聚合里的错误元素（如 EXEC 结果）写成 { err = 错误文本 }，当 map 键时就是文本本身
/// </summary>
/// <param name="pk" type="lightuserdata?">回复首节点指针；nil 时返回 nil</param>
/// <returns type="any">解包后的值：标量原样；空聚合为空表；nil 聚合、坏包与 pk 为 nil 时为 nil；错误应答为 false</returns>
/// <returns type="string?">错误应答的文本；其余情况没有这个返回值</returns>
static int32_t _lredis_unpack(lua_State *lua) {
    redis_pack_ctx *pk = _lredis_opt(lua);
    if (NULL == pk) {
        return lpub_rtn_nil(lua, 1);
    }
    if (NULL != pk->next) {
        return _lredis_unpack_multi(lua, pk);
    }
    // 单节点：标量原值，空聚合为空表，nil 聚合为 nil
    if (_lredis_iserr(pk->prot)) {
        return _lredis_rtnerr(lua, pk);
    }
    if (!_lredis_isagg(pk->prot)) {
        _lredis_push(lua, pk, 0);
        return 1;
    }
    if (0 == pk->nelem) {
        lua_createtable(lua, 0, 0);
        return 1;
    }
    if (-1 != pk->nelem) {
        LOG_WARN("resp message error.");// 声明了元素却没有后继节点
    }
    return lpub_rtn_nil(lua, 1);
}
// pack / pack_ud 共用：数字、字符串、nil 之外的参数就地换成 tostring 的结果，返回参数个数。
// luaL_tolstring 要压栈、还可能抛错，必须在开缓冲或分配之前做完
static int32_t _lredis_args(lua_State *lua) {
    int32_t n = lua_gettop(lua);
    int32_t type;
    for (int32_t i = 1; i <= n; i++) {
        type = lua_type(lua, i);
        if (LUA_TNIL != type && LUA_TNUMBER != type && LUA_TSTRING != type) {
            luaL_tolstring(lua, i, NULL);
            lua_replace(lua, i);
        }
    }
    return n;
}
// 非 nil 参数要写进 bulk 的字节：数字照 num_str 的规则写进 nbuf（整数值的浮点按整数写），其余已是字符串。不压栈
static const char *_lredis_str(lua_State *lua, int32_t i, char *nbuf, size_t *lens) {
    int32_t isint;
    lua_Integer iv;
    if (LUA_TNUMBER != lua_type(lua, i)) {
        return lua_tolstring(lua, i, lens);
    }
    iv = lua_tointegerx(lua, i, &isint);
    if (isint) {
        *lens = i64tostr(nbuf, (int64_t)iv, 10);
    } else {
        *lens = (size_t)lua_numbertocstring(lua, i, nbuf) - 1;
    }
    return nbuf;
}
/// <summary>
/// 将命令及参数编成 RESP 请求（array of bulk string）。参数按 tostring 转串，数字照 num_str 的规则：
/// 整数值的浮点按整数写，其余浮点同 tostring；nil 编成空 bulk 并打一条告警
/// </summary>
/// <param name="..." type="any">命令名及参数，如 redis.pack("SET", "key", "value")</param>
/// <returns type="string">RESP 编码后的请求字符串</returns>
static int32_t _lredis_pack(lua_State *lua) {
    int32_t n = _lredis_args(lua);
    size_t lens;
    size_t k;
    const char *s;
    char *p;
    char nbuf[LUA_N2SBUFFSZ];
    luaL_Buffer lbuf;
    luaL_buffinit(lua, &lbuf);
    p = luaL_prepbuffsize(&lbuf, 24);
    p[0] = '*';
    k = 1 + u64tostr(p + 1, (uint64_t)n, 10);
    p[k++] = '\r';
    p[k++] = '\n';
    luaL_addsize(&lbuf, k);
    for (int32_t i = 1; i <= n; i++) {
        if (lua_isnil(lua, i)) {
            LOG_WARN("redis.pack: nil argument #%d, encoded as empty bulk string", i);
            luaL_addlstring(&lbuf, "$0\r\n\r\n", 6);
            continue;
        }
        s = _lredis_str(lua, i, nbuf, &lens);
        // "$" + 最多 20 位长度 + 两个 CRLF
        p = luaL_prepbuffsize(&lbuf, lens + 25);
        p[0] = '$';
        k = 1 + u64tostr(p + 1, (uint64_t)lens, 10);
        p[k++] = '\r';
        p[k++] = '\n';
        memcpy(p + k, s, lens);
        k += lens;
        p[k++] = '\r';
        p[k++] = '\n';
        luaL_addsize(&lbuf, k);
    }
    luaL_pushresult(&lbuf);
    return 1;
}
/// <summary>
/// 同 pack，但整条编进一块 C 内存：交给 srey.syn_send(sk, data, size, 0) 零拷贝发出，不再先建 Lua 字符串。
/// 参数转换与 nil 告警同 pack；__tostring 抛错、总长超 INT32_MAX 报错都发生在分配之前
/// </summary>
/// <param name="..." type="any">命令名及参数，同 pack</param>
/// <returns type="lightuserdata">RESP 编码后的请求；所有权归调用方，交给 copy=0 的发送后由框架释放，不发要自己 utils.ud_free</returns>
/// <returns type="integer">字节数</returns>
static int32_t _lredis_pack_ud(lua_State *lua) {
    int32_t n = _lredis_args(lua);
    int32_t type;
    size_t lens;
    size_t total = 24;// "*" + 个数 + CRLF
    const char *s;
    char nbuf[LUA_N2SBUFFSZ];
    binary_ctx bw;
    // 按类型估上界一次分配到位：每个参数 "$" + 长度 + 两个 CRLF 至多 25 字节（nil 的 6 字节含在内），
    // 整数至多 20 位、浮点按 nbuf 长度估。每加一个就判上限，免得多个大参数在 32 位上累加回绕
    int32_t i;
    for (i = 1; i <= n; i++) {
        type = lua_type(lua, i);
        if (LUA_TSTRING == type) {
            total += 25 + (size_t)lua_rawlen(lua, i);
        } else if (LUA_TNUMBER == type) {
            total += 25 + (lua_isinteger(lua, i) ? 20 : sizeof(nbuf));
        } else {
            total += 25;
        }
        if (total > INT32_MAX) {
            return luaL_error(lua, "redis.pack_ud: %s", LENS_RANGE);
        }
    }
    binary_init_write(&bw, total, 0);
    binary_set_binary(&bw, "*", 1);
    binary_set_uint(&bw, (uint64_t)n, 10);
    binary_set_binary(&bw, FLAG_CRLF, CRLF_SIZE);
    for (i = 1; i <= n; i++) {
        if (lua_isnil(lua, i)) {
            LOG_WARN("redis.pack_ud: nil argument #%d, encoded as empty bulk string", i);
            binary_set_binary(&bw, "$0\r\n\r\n", 6);
            continue;
        }
        s = _lredis_str(lua, i, nbuf, &lens);
        binary_set_binary(&bw, "$", 1);
        binary_set_uint(&bw, (uint64_t)lens, 10);
        binary_set_binary(&bw, FLAG_CRLF, CRLF_SIZE);
        binary_set_binary(&bw, s, lens);
        binary_set_binary(&bw, FLAG_CRLF, CRLF_SIZE);
    }
    return lpub_rtn_lud(lua, bw.data, bw.offset);
}
//srey.redis
LUAMOD_API int luaopen_redis(lua_State *lua) {
    luaL_Reg reg[] = {
        { "value", _lredis_value },
        { "next", _lredis_next },
        { "node", _lredis_node },
        { "unpack", _lredis_unpack },
        { "pack", _lredis_pack },
        { "pack_ud", _lredis_pack_ud },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
