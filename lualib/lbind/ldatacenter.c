#include "lbind/lpub.h"

/// <summary>
/// 写入或覆盖 KV;仅投递不挂起,挂起等响应由 Lua 协程层(dc_client.set)负责。
/// </summary>
/// <param name="dc_name" type="string|integer">DataCenter task 字符串名或数字句柄</param>
/// <param name="sess" type="integer">会话 id(非 0);响应回带,Lua 侧据此 _coro_wait 配对</param>
/// <param name="key" type="string">key 字符串;空/nil/超长由 C 侧拒绝并返 false</param>
/// <param name="val" type="string?">value;nil 或空串等价软清空</param>
/// <returns type="boolean">成功投递 true;key 非法/datacenter 不可达 false</returns>
static int32_t _ldc_set(lua_State *lua) {
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    name_t dc_name = (LUA_TSTRING == lua_type(lua, 1))
        ? task_find_name(g_loader, lua_tostring(lua, 1))
        : (name_t)luaL_checkinteger(lua, 1);
    uint64_t sess = (uint64_t)luaL_checkinteger(lua, 2);
    const char *key = (LUA_TSTRING == lua_type(lua, 3)) ? lua_tostring(lua, 3) : NULL;
    size_t vlen = 0;
    void *val = (LUA_TSTRING == lua_type(lua, 4)) ? (void *)lua_tolstring(lua, 4, &vlen) : NULL;
    lua_pushboolean(lua, ERR_OK == dc_set(task, dc_name, sess, key, val, vlen));
    return 1;
}
/// <summary>
/// 读 KV;仅投递不挂起,响应(命中值/不存在的 ERR_FAILED)由 Lua 协程层接收。
/// </summary>
/// <param name="dc_name" type="string|integer">DataCenter task 字符串名或数字句柄</param>
/// <param name="sess" type="integer">会话 id(非 0);响应回带</param>
/// <param name="key" type="string">key 字符串</param>
/// <returns type="boolean">成功投递 true;key 非法/sess=0/datacenter 不可达 false</returns>
static int32_t _ldc_get(lua_State *lua) {
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    name_t dc_name = (LUA_TSTRING == lua_type(lua, 1))
        ? task_find_name(g_loader, lua_tostring(lua, 1))
        : (name_t)luaL_checkinteger(lua, 1);
    uint64_t sess = (uint64_t)luaL_checkinteger(lua, 2);
    const char *key = (LUA_TSTRING == lua_type(lua, 3)) ? lua_tostring(lua, 3) : NULL;
    lua_pushboolean(lua, ERR_OK == dc_get(task, dc_name, sess, key));
    return 1;
}
/// <summary>
/// 读 KV;未命中由 DataCenter 挂 pending,响应到达由 set 触发(超时靠 Lua 侧 request_timeout 兜底)。仅投递不挂起。
/// </summary>
/// <param name="dc_name" type="string|integer">DataCenter task 字符串名或数字句柄</param>
/// <param name="sess" type="integer">会话 id(非 0);响应回带</param>
/// <param name="key" type="string">key 字符串</param>
/// <returns type="boolean">成功投递 true;key 非法/sess=0/datacenter 不可达 false</returns>
static int32_t _ldc_wait(lua_State *lua) {
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    name_t dc_name = (LUA_TSTRING == lua_type(lua, 1))
        ? task_find_name(g_loader, lua_tostring(lua, 1))
        : (name_t)luaL_checkinteger(lua, 1);
    uint64_t sess = (uint64_t)luaL_checkinteger(lua, 2);
    const char *key = (LUA_TSTRING == lua_type(lua, 3)) ? lua_tostring(lua, 3) : NULL;
    lua_pushboolean(lua, ERR_OK == dc_wait(task, dc_name, sess, key));
    return 1;
}
/// <summary>
/// 删除指定 key 的 KV 条目;仅投递不挂起,只清 KV 不影响已挂起 waiter。
/// </summary>
/// <param name="dc_name" type="string|integer">DataCenter task 字符串名或数字句柄</param>
/// <param name="sess" type="integer">会话 id(非 0);响应回带</param>
/// <param name="key" type="string">key 字符串</param>
/// <returns type="boolean">成功投递 true;key 非法/datacenter 不可达 false</returns>
static int32_t _ldc_del(lua_State *lua) {
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    name_t dc_name = (LUA_TSTRING == lua_type(lua, 1))
        ? task_find_name(g_loader, lua_tostring(lua, 1))
        : (name_t)luaL_checkinteger(lua, 1);
    uint64_t sess = (uint64_t)luaL_checkinteger(lua, 2);
    const char *key = (LUA_TSTRING == lua_type(lua, 3)) ? lua_tostring(lua, 3) : NULL;
    lua_pushboolean(lua, ERR_OK == dc_del(task, dc_name, sess, key));
    return 1;
}
/// <summary>
/// 列出全部 key;仅投递不挂起,响应 buffer(每条 | u16 klen | key |)由 Lua 协程层解码。
/// </summary>
/// <param name="dc_name" type="string|integer">DataCenter task 字符串名或数字句柄</param>
/// <param name="sess" type="integer">会话 id(非 0);响应回带</param>
/// <returns type="boolean">成功投递 true;sess=0/datacenter 不可达 false</returns>
static int32_t _ldc_keys(lua_State *lua) {
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    name_t dc_name = (LUA_TSTRING == lua_type(lua, 1))
        ? task_find_name(g_loader, lua_tostring(lua, 1))
        : (name_t)luaL_checkinteger(lua, 1);
    uint64_t sess = (uint64_t)luaL_checkinteger(lua, 2);
    lua_pushboolean(lua, ERR_OK == dc_keys(task, dc_name, sess));
    return 1;
}
/// <summary>解析 dc_keys 响应 buffer 为 key 字符串数组,内部逐条 dc_parse_keys;取代 Lua string.unpack。</summary>
/// <param name="data" type="lightuserdata">dc_keys 响应 data 指针</param>
/// <param name="size" type="integer">data 字节数</param>
/// <returns type="string[]">key 字符串数组;空 buffer 返空表</returns>
static int32_t _ldc_parse_keys(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    const void *data = lua_touserdata(lua, 1);
    size_t size = (size_t)luaL_checkinteger(lua, 2);
    binary_ctx br;
    binary_init(&br, (char *)data, size, 0);
    lua_newtable(lua);
    int32_t idx = 0;
    dc_key k;
    while (br.offset < br.size) {
        if (ERR_OK != dc_parse_keys(&br, &k)) {
            break;
        }
        lua_pushlstring(lua, k.klen > 0 ? k.key : "", k.klen);
        lua_rawseti(lua, -2, ++idx);
    }
    return 1;
}
//srey.datacenter
LUAMOD_API int luaopen_datacenter(lua_State *lua) {
    luaL_Reg reg[] = {
        { "set", _ldc_set },
        { "get", _ldc_get },
        { "wait", _ldc_wait },
        { "del", _ldc_del },
        { "keys", _ldc_keys },
        
        { "parse_keys", _ldc_parse_keys },

        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
