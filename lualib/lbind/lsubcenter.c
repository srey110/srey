#include "lbind/lpub.h"

/// <summary>
/// 订阅 topic(可含通配);仅投递不挂起,挂起等响应由 Lua 协程层(sc_client.subscribe)负责。
/// </summary>
/// <param name="sc_name" type="string|integer">subcenter task 字符串名或数字句柄</param>
/// <param name="sess" type="integer">会话 id(非 0);响应回带,Lua 侧据此 _coro_wait 配对</param>
/// <param name="topic" type="string">订阅模式;空/nil 由 C 侧拒绝并返 false</param>
/// <returns type="boolean">成功投递 true;topic 非法/sess=0/subcenter 不可达 false</returns>
static int32_t _lsc_subscribe(lua_State *lua) {
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    name_t sc_name = (LUA_TSTRING == lua_type(lua, 1))
        ? task_find_name(g_loader, lua_tostring(lua, 1))
        : (name_t)luaL_checkinteger(lua, 1);
    uint64_t sess = (uint64_t)luaL_checkinteger(lua, 2);
    const char *topic = (LUA_TSTRING == lua_type(lua, 3)) ? lua_tostring(lua, 3) : NULL;
    lua_pushboolean(lua, ERR_OK == sc_subscribe(task, sc_name, sess, topic));
    return 1;
}
/// <summary>
/// 共享订阅;仅投递不挂起。同 group 内多订阅者轮询接收 publish,不收 retained。
/// </summary>
/// <param name="sc_name" type="string|integer">subcenter task 字符串名或数字句柄</param>
/// <param name="sess" type="integer">会话 id(非 0);响应回带</param>
/// <param name="topic" type="string">订阅模式</param>
/// <param name="group" type="string">共享组名;空/nil 由 C 侧拒绝</param>
/// <returns type="boolean">成功投递 true;参数非法/sess=0/subcenter 不可达 false</returns>
static int32_t _lsc_subscribe_shared(lua_State *lua) {
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    name_t sc_name = (LUA_TSTRING == lua_type(lua, 1))
        ? task_find_name(g_loader, lua_tostring(lua, 1))
        : (name_t)luaL_checkinteger(lua, 1);
    uint64_t sess = (uint64_t)luaL_checkinteger(lua, 2);
    const char *topic = (LUA_TSTRING == lua_type(lua, 3)) ? lua_tostring(lua, 3) : NULL;
    const char *group = (LUA_TSTRING == lua_type(lua, 4)) ? lua_tostring(lua, 4) : NULL;
    lua_pushboolean(lua, ERR_OK == sc_subscribe_shared(task, sc_name, sess, topic, group));
    return 1;
}
/// <summary>取消订阅;仅投递不挂起。</summary>
/// <param name="sc_name" type="string|integer">subcenter task 字符串名或数字句柄</param>
/// <param name="sess" type="integer">会话 id(非 0);响应回带</param>
/// <param name="topic" type="string">订阅模式;须与 subscribe 时一致</param>
/// <returns type="boolean">成功投递 true;topic 非法/sess=0/subcenter 不可达 false</returns>
static int32_t _lsc_unsubscribe(lua_State *lua) {
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    name_t sc_name = (LUA_TSTRING == lua_type(lua, 1))
        ? task_find_name(g_loader, lua_tostring(lua, 1))
        : (name_t)luaL_checkinteger(lua, 1);
    uint64_t sess = (uint64_t)luaL_checkinteger(lua, 2);
    const char *topic = (LUA_TSTRING == lua_type(lua, 3)) ? lua_tostring(lua, 3) : NULL;
    lua_pushboolean(lua, ERR_OK == sc_unsubscribe(task, sc_name, sess, topic));
    return 1;
}
/// <summary>取消共享订阅;仅投递不挂起。</summary>
/// <param name="sc_name" type="string|integer">subcenter task 字符串名或数字句柄</param>
/// <param name="sess" type="integer">会话 id(非 0);响应回带</param>
/// <param name="topic" type="string">订阅模式;须与 subscribe_shared 时一致</param>
/// <param name="group" type="string">共享组名;须与 subscribe_shared 时一致</param>
/// <returns type="boolean">成功投递 true;参数非法/sess=0/subcenter 不可达 false</returns>
static int32_t _lsc_unsubscribe_shared(lua_State *lua) {
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    name_t sc_name = (LUA_TSTRING == lua_type(lua, 1))
        ? task_find_name(g_loader, lua_tostring(lua, 1))
        : (name_t)luaL_checkinteger(lua, 1);
    uint64_t sess = (uint64_t)luaL_checkinteger(lua, 2);
    const char *topic = (LUA_TSTRING == lua_type(lua, 3)) ? lua_tostring(lua, 3) : NULL;
    const char *group = (LUA_TSTRING == lua_type(lua, 4)) ? lua_tostring(lua, 4) : NULL;
    lua_pushboolean(lua, ERR_OK == sc_unsubscribe_shared(task, sc_name, sess, topic, group));
    return 1;
}
/// <summary>发布消息到精确 topic;仅投递不挂起。</summary>
/// <param name="sc_name" type="string|integer">subcenter task 字符串名或数字句柄</param>
/// <param name="sess" type="integer">会话 id(非 0);响应回带</param>
/// <param name="topic" type="string">精确 topic;不允许含通配</param>
/// <param name="data" type="string?">payload;nil 等价空 payload</param>
/// <returns type="boolean">成功投递 true;topic 非法/sess=0/subcenter 不可达 false</returns>
static int32_t _lsc_publish(lua_State *lua) {
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    name_t sc_name = (LUA_TSTRING == lua_type(lua, 1))
        ? task_find_name(g_loader, lua_tostring(lua, 1))
        : (name_t)luaL_checkinteger(lua, 1);
    uint64_t sess = (uint64_t)luaL_checkinteger(lua, 2);
    const char *topic = (LUA_TSTRING == lua_type(lua, 3)) ? lua_tostring(lua, 3) : NULL;
    size_t plen = 0;
    void *data = (LUA_TSTRING == lua_type(lua, 4)) ? (void *)lua_tolstring(lua, 4, &plen) : NULL;
    lua_pushboolean(lua, ERR_OK == sc_publish(task, sc_name, sess, topic, data, plen));
    return 1;
}
/// <summary>发布保留消息;仅投递不挂起。data 为 nil/空时清空 retained 槽位。</summary>
/// <param name="sc_name" type="string|integer">subcenter task 字符串名或数字句柄</param>
/// <param name="sess" type="integer">会话 id(非 0);响应回带</param>
/// <param name="topic" type="string">精确 topic</param>
/// <param name="data" type="string?">retained payload;nil/空清空槽位;超 SC_RETAINED_MAX_SIZE 拒绝</param>
/// <returns type="boolean">成功投递 true;参数非法/超长/sess=0/subcenter 不可达 false</returns>
static int32_t _lsc_publish_retained(lua_State *lua) {
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    name_t sc_name = (LUA_TSTRING == lua_type(lua, 1))
        ? task_find_name(g_loader, lua_tostring(lua, 1))
        : (name_t)luaL_checkinteger(lua, 1);
    uint64_t sess = (uint64_t)luaL_checkinteger(lua, 2);
    const char *topic = (LUA_TSTRING == lua_type(lua, 3)) ? lua_tostring(lua, 3) : NULL;
    size_t plen = 0;
    void *data = (LUA_TSTRING == lua_type(lua, 4)) ? (void *)lua_tolstring(lua, 4, &plen) : NULL;
    lua_pushboolean(lua, ERR_OK == sc_publish_retained(task, sc_name, sess, topic, data, plen));
    return 1;
}
/// <summary>查询匹配 pattern 的当前 retained;仅投递不挂起,响应 buffer 由 Lua 协程层解码。</summary>
/// <param name="sc_name" type="string|integer">subcenter task 字符串名或数字句柄</param>
/// <param name="sess" type="integer">会话 id(非 0);响应回带</param>
/// <param name="pattern" type="string">查询模式;可含通配</param>
/// <returns type="boolean">成功投递 true;pattern 非法/sess=0/subcenter 不可达 false</returns>
static int32_t _lsc_query_retained(lua_State *lua) {
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    name_t sc_name = (LUA_TSTRING == lua_type(lua, 1))
        ? task_find_name(g_loader, lua_tostring(lua, 1))
        : (name_t)luaL_checkinteger(lua, 1);
    uint64_t sess = (uint64_t)luaL_checkinteger(lua, 2);
    const char *pattern = (LUA_TSTRING == lua_type(lua, 3)) ? lua_tostring(lua, 3) : NULL;
    lua_pushboolean(lua, ERR_OK == sc_query_retained(task, sc_name, sess, pattern));
    return 1;
}
/// <summary>列出所有订阅 topic;仅投递不挂起,响应 buffer 由 Lua 协程层解码。</summary>
/// <param name="sc_name" type="string|integer">subcenter task 字符串名或数字句柄</param>
/// <param name="sess" type="integer">会话 id(非 0);响应回带</param>
/// <returns type="boolean">成功投递 true;sess=0/subcenter 不可达 false</returns>
static int32_t _lsc_topics(lua_State *lua) {
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    name_t sc_name = (LUA_TSTRING == lua_type(lua, 1))
        ? task_find_name(g_loader, lua_tostring(lua, 1))
        : (name_t)luaL_checkinteger(lua, 1);
    uint64_t sess = (uint64_t)luaL_checkinteger(lua, 2);
    lua_pushboolean(lua, ERR_OK == sc_topics(task, sc_name, sess));
    return 1;
}
/// <summary>列出所有 retained topic 元信息;仅投递不挂起,响应 buffer 由 Lua 协程层解码。</summary>
/// <param name="sc_name" type="string|integer">subcenter task 字符串名或数字句柄</param>
/// <param name="sess" type="integer">会话 id(非 0);响应回带</param>
/// <returns type="boolean">成功投递 true;sess=0/subcenter 不可达 false</returns>
static int32_t _lsc_retained_topics(lua_State *lua) {
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    name_t sc_name = (LUA_TSTRING == lua_type(lua, 1))
        ? task_find_name(g_loader, lua_tostring(lua, 1))
        : (name_t)luaL_checkinteger(lua, 1);
    uint64_t sess = (uint64_t)luaL_checkinteger(lua, 2);
    lua_pushboolean(lua, ERR_OK == sc_retained_topics(task, sc_name, sess));
    return 1;
}
/// <summary>注册/更新当前 task 的 publisher 元数据;仅投递不挂起。</summary>
/// <param name="sc_name" type="string|integer">subcenter task 字符串名或数字句柄</param>
/// <param name="sess" type="integer">会话 id(非 0);响应回带</param>
/// <param name="meta" type="string?">元数据;nil/空等价清除;超 SC_META_MAX_SIZE 返 false</param>
/// <returns type="boolean">成功投递 true;超长/sess=0/subcenter 不可达 false</returns>
static int32_t _lsc_set_meta(lua_State *lua) {
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    name_t sc_name = (LUA_TSTRING == lua_type(lua, 1))
        ? task_find_name(g_loader, lua_tostring(lua, 1))
        : (name_t)luaL_checkinteger(lua, 1);
    uint64_t sess = (uint64_t)luaL_checkinteger(lua, 2);
    size_t mlen = 0;
    const void *meta = (LUA_TSTRING == lua_type(lua, 3)) ? lua_tolstring(lua, 3, &mlen) : NULL;
    lua_pushboolean(lua, ERR_OK == sc_set_meta(task, sc_name, sess, meta, mlen));
    return 1;
}
/// <summary>
/// 解析 REQ_SC_DELIVER 推送 wire(订阅者 _on_deliver 调用),取代 Lua string.unpack。
/// </summary>
/// <param name="data" type="lightuserdata">REQ_SC_DELIVER 消息 data 指针</param>
/// <param name="size" type="integer">data 字节数</param>
/// <returns type="table?">解析结果表 { kind, publisher, topic, payload, meta?, group }:
///     kind 0 普通/1 共享;publisher 0=已失效;topic/payload 空为 "";meta 无则字段缺省;
///     group 共享投递组名,普通投递为 "";wire 截断/损坏返 nil</returns>
static int32_t _lsc_parse_deliver(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    const void *data = lua_touserdata(lua, 1);
    size_t size = (size_t)luaL_checkinteger(lua, 2);
    sc_deliver d;
    if (ERR_OK != sc_parse_deliver(data, size, &d)) {
        lua_pushnil(lua);
        return 1;
    }
    lua_createtable(lua, 0, 6);
    lua_pushinteger(lua, d.kind);
    lua_setfield(lua, -2, "kind");
    lua_pushinteger(lua, (lua_Integer)d.publisher);
    lua_setfield(lua, -2, "publisher");
    lua_pushlstring(lua, d.tlen > 0 ? d.topic : "", d.tlen);
    lua_setfield(lua, -2, "topic");
    lua_pushlstring(lua, d.plen > 0 ? d.payload : "", d.plen);
    lua_setfield(lua, -2, "payload");
    if (d.mlen > 0) {
        lua_pushlstring(lua, d.meta, d.mlen);
        lua_setfield(lua, -2, "meta");
    }
    lua_pushlstring(lua, d.glen > 0 ? d.group : "", d.glen);
    lua_setfield(lua, -2, "group");
    return 1;
}
/// <summary>解析 query_retained 响应 buffer 为数组,内部逐条 sc_parse_retained;取代 Lua string.unpack。</summary>
/// <param name="data" type="lightuserdata">query_retained 响应 data 指针</param>
/// <param name="size" type="integer">data 字节数</param>
/// <returns type="table[]">{ publisher, topic, payload, meta? } 数组;空 buffer 返空表</returns>
static int32_t _lsc_parse_retained(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    const void *data = lua_touserdata(lua, 1);
    size_t size = (size_t)luaL_checkinteger(lua, 2);
    binary_ctx br;
    binary_init(&br, (char *)data, size, 0);
    lua_newtable(lua);
    int32_t idx = 0;
    sc_retained r;
    while (br.offset < br.size) {
        if (ERR_OK != sc_parse_retained(&br, &r)) {
            break;
        }
        lua_createtable(lua, 0, 4);
        lua_pushinteger(lua, (lua_Integer)r.publisher);
        lua_setfield(lua, -2, "publisher");
        lua_pushlstring(lua, r.tlen > 0 ? r.topic : "", r.tlen);
        lua_setfield(lua, -2, "topic");
        lua_pushlstring(lua, r.plen > 0 ? r.payload : "", r.plen);
        lua_setfield(lua, -2, "payload");
        if (r.mlen > 0) {
            lua_pushlstring(lua, r.meta, r.mlen);
            lua_setfield(lua, -2, "meta");
        }
        lua_rawseti(lua, -2, ++idx);
    }
    return 1;
}
/// <summary>解析 topics 响应 buffer 为数组,内部逐条 sc_parse_topics。</summary>
/// <param name="data" type="lightuserdata">topics 响应 data 指针</param>
/// <param name="size" type="integer">data 字节数</param>
/// <returns type="table[]">{ topic, normal, shared } 数组;空 buffer 返空表</returns>
static int32_t _lsc_parse_topics(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    const void *data = lua_touserdata(lua, 1);
    size_t size = (size_t)luaL_checkinteger(lua, 2);
    binary_ctx br;
    binary_init(&br, (char *)data, size, 0);
    lua_newtable(lua);
    int32_t idx = 0;
    sc_topic t;
    while (br.offset < br.size) {
        if (ERR_OK != sc_parse_topics(&br, &t)) {
            break;
        }
        lua_createtable(lua, 0, 3);
        lua_pushlstring(lua, t.tlen > 0 ? t.topic : "", t.tlen);
        lua_setfield(lua, -2, "topic");
        lua_pushinteger(lua, t.normal);
        lua_setfield(lua, -2, "normal");
        lua_pushinteger(lua, t.shared);
        lua_setfield(lua, -2, "shared");
        lua_rawseti(lua, -2, ++idx);
    }
    return 1;
}
/// <summary>解析 retained_topics 响应 buffer 为数组,内部逐条 sc_parse_retained_topics。</summary>
/// <param name="data" type="lightuserdata">retained_topics 响应 data 指针</param>
/// <param name="size" type="integer">data 字节数</param>
/// <returns type="table[]">{ topic, publisher, size, meta_size } 数组;空 buffer 返空表</returns>
static int32_t _lsc_parse_retained_topics(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    const void *data = lua_touserdata(lua, 1);
    size_t size = (size_t)luaL_checkinteger(lua, 2);
    binary_ctx br;
    binary_init(&br, (char *)data, size, 0);
    lua_newtable(lua);
    int32_t idx = 0;
    sc_retained_topic rt;
    while (br.offset < br.size) {
        if (ERR_OK != sc_parse_retained_topics(&br, &rt)) {
            break;
        }
        lua_createtable(lua, 0, 4);
        lua_pushlstring(lua, rt.tlen > 0 ? rt.topic : "", rt.tlen);
        lua_setfield(lua, -2, "topic");
        lua_pushinteger(lua, (lua_Integer)rt.publisher);
        lua_setfield(lua, -2, "publisher");
        lua_pushinteger(lua, rt.size);
        lua_setfield(lua, -2, "size");
        lua_pushinteger(lua, rt.meta_size);
        lua_setfield(lua, -2, "meta_size");
        lua_rawseti(lua, -2, ++idx);
    }
    return 1;
}
//srey.subcenter
LUAMOD_API int luaopen_subcenter(lua_State *lua) {
    luaL_Reg reg[] = {
        { "subscribe", _lsc_subscribe },
        { "subscribe_shared", _lsc_subscribe_shared },
        { "unsubscribe", _lsc_unsubscribe },
        { "unsubscribe_shared", _lsc_unsubscribe_shared },
        { "publish", _lsc_publish },
        { "publish_retained", _lsc_publish_retained },
        { "query_retained", _lsc_query_retained },
        { "topics", _lsc_topics },
        { "retained_topics", _lsc_retained_topics },
        { "set_meta", _lsc_set_meta },
        
        { "parse_deliver", _lsc_parse_deliver },
        { "parse_retained", _lsc_parse_retained },
        { "parse_topics", _lsc_parse_topics },
        { "parse_retained_topics", _lsc_parse_retained_topics },

        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
