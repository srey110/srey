#include "lbind/lpub.h"

#define MT_MQTT_PROPS "_mqtt_props_ctx"

// 从 Lua 栈 idx 位置获取可选 binary_ctx（nil/none 返回 NULL）
static binary_ctx *_lmqtt_get_props(lua_State *lua, int idx) {
    if (lua_isnoneornil(lua, idx)) {
        return NULL;
    }
    return luaL_checkudata(lua, idx, MT_MQTT_PROPS);
}
// 从 Lua 栈 idx 读取 payload：string 自动取长度；lightuserdata 则取 idx+1 为长度
// 返回实际使用的下一个参数 index（用于后续参数定位）
static int32_t _lmqtt_get_payload(lua_State *lua, int idx, char **data, size_t *lens) {
    *data = NULL;
    *lens = 0;
    int t = lua_type(lua, idx);
    if (LUA_TSTRING == t) {
        *data = (char *)luaL_checklstring(lua, idx, lens);
        return idx + 1;
    }
    if (LUA_TLIGHTUSERDATA == t) {
        *data = lua_touserdata(lua, idx);
        *lens = (size_t)luaL_checkinteger(lua, idx + 1);
        return idx + 2;
    }
    return idx + 1;
}
// ---- mqtt.props (binary_ctx 构建器：属性 / 主题列表) ----
/// <summary>
/// 创建 MQTT 属性/主题缓冲区（初始为空）
/// </summary>
/// <param>无</param>
/// <returns type="_mqtt_props_ctx">props 对象</returns>
static int32_t _lmqtt_props_new(lua_State *lua) {
    binary_ctx *props = lua_newuserdata(lua, sizeof(binary_ctx));
    binary_init(props, NULL, 0, 0);
    ASSOC_MTABLE(lua, MT_MQTT_PROPS);
    return 1;
}
/// <summary>
/// 释放 props 内部缓冲区（同时作为 __gc / free 调用）
/// </summary>
/// <param name="self" type="userdata">props 对象</param>
/// <returns>无</returns>
static int32_t _lmqtt_props_free(lua_State *lua) {
    binary_ctx *props = luaL_checkudata(lua, 1, MT_MQTT_PROPS);
    binary_free(props);
    return 0;
}
/// <summary>
/// 追加固定长度数字属性（按 flag 对应的 1/2/4 字节宽度）
/// </summary>
/// <param name="self" type="userdata">props 对象</param>
/// <param name="flag" type="integer">属性标识 mqtt_prop_flag</param>
/// <param name="val" type="integer">属性数值</param>
/// <returns>无</returns>
static int32_t _lmqtt_props_fixnum(lua_State *lua) {
    binary_ctx *props = luaL_checkudata(lua, 1, MT_MQTT_PROPS);
    mqtt_prop_flag flag = (mqtt_prop_flag)luaL_checkinteger(lua, 2);
    uint32_t val = (uint32_t)luaL_checkinteger(lua, 3);
    mqtt_props_fixnum(props, flag, val);
    return 0;
}
/// <summary>
/// 追加可变长度数字属性（Variable Byte Integer）
/// </summary>
/// <param name="self" type="userdata">props 对象</param>
/// <param name="flag" type="integer">属性标识 mqtt_prop_flag</param>
/// <param name="val" type="integer">属性数值</param>
/// <returns>无</returns>
static int32_t _lmqtt_props_varnum(lua_State *lua) {
    binary_ctx *props = luaL_checkudata(lua, 1, MT_MQTT_PROPS);
    mqtt_prop_flag flag = (mqtt_prop_flag)luaL_checkinteger(lua, 2);
    uint32_t val = (uint32_t)luaL_checkinteger(lua, 3);
    mqtt_props_varnum(props, flag, val);
    return 0;
}
/// <summary>
/// 追加二进制数据属性
/// </summary>
/// <param name="self" type="userdata">props 对象</param>
/// <param name="flag" type="integer">属性标识 mqtt_prop_flag</param>
/// <param name="data" type="string|lightuserdata">数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <returns>无</returns>
static int32_t _lmqtt_props_binary(lua_State *lua) {
    binary_ctx *props = luaL_checkudata(lua, 1, MT_MQTT_PROPS);
    mqtt_prop_flag flag = (mqtt_prop_flag)luaL_checkinteger(lua, 2);
    char *data;
    size_t lens;
    _lmqtt_get_payload(lua, 3, &data, &lens);
    mqtt_props_binary(props, flag, data, lens);
    return 0;
}
/// <summary>
/// 追加 UTF-8 字符串对属性（用户属性 USER_PROPERTY）
/// </summary>
/// <param name="self" type="userdata">props 对象</param>
/// <param name="flag" type="integer">属性标识 mqtt_prop_flag</param>
/// <param name="key" type="string">键</param>
/// <param name="val" type="string">值</param>
/// <returns>无</returns>
static int32_t _lmqtt_props_kv(lua_State *lua) {
    binary_ctx *props = luaL_checkudata(lua, 1, MT_MQTT_PROPS);
    mqtt_prop_flag flag = (mqtt_prop_flag)luaL_checkinteger(lua, 2);
    size_t klens, vlens;
    const char *key = luaL_checklstring(lua, 3, &klens);
    const char *val = luaL_checklstring(lua, 4, &vlens);
    mqtt_props_kv(props, flag, (void *)key, klens, (void *)val, vlens);
    return 0;
}
/// <summary>
/// 向 topics 缓冲区追加一条订阅主题
/// </summary>
/// <param name="self" type="userdata">topics 缓冲区</param>
/// <param name="version" type="integer">协议版本 mqtt_protversion</param>
/// <param name="topic" type="string">主题</param>
/// <param name="qos" type="integer">QoS 等级 0/1/2</param>
/// <param name="nl" type="integer?">No Local 标志（MQTT 5.0），3.1.1 传 0；默认 0</param>
/// <param name="rap" type="integer?">Retain As Published 标志（MQTT 5.0），3.1.1 传 0；默认 0</param>
/// <param name="retain" type="integer?">Retain Handling（MQTT 5.0），3.1.1 传 0；默认 0</param>
/// <returns type="boolean">true 成功；false topic 长度超过 65535 字节</returns>
static int32_t _lmqtt_props_subscribe(lua_State *lua) {
    binary_ctx *topics = luaL_checkudata(lua, 1, MT_MQTT_PROPS);
    mqtt_protversion version = (mqtt_protversion)luaL_checkinteger(lua, 2);
    const char *topic = luaL_checkstring(lua, 3);
    int8_t qos = (int8_t)luaL_checkinteger(lua, 4);
    int8_t nl = (int8_t)luaL_optinteger(lua, 5, 0);
    int8_t rap = (int8_t)luaL_optinteger(lua, 6, 0);
    int8_t retain = (int8_t)luaL_optinteger(lua, 7, 0);
    lua_pushboolean(lua, ERR_OK == mqtt_topics_subscribe(topics, version, topic, qos, nl, rap, retain));
    return 1;
}
/// <summary>
/// 向 topics 缓冲区追加一条取消订阅主题
/// </summary>
/// <param name="self" type="userdata">topics 缓冲区</param>
/// <param name="topic" type="string">主题</param>
/// <returns type="boolean">true 成功；false topic 长度超过 65535 字节</returns>
static int32_t _lmqtt_props_unsubscribe(lua_State *lua) {
    binary_ctx *topics = luaL_checkudata(lua, 1, MT_MQTT_PROPS);
    const char *topic = luaL_checkstring(lua, 2);
    lua_pushboolean(lua, ERR_OK == mqtt_topics_unsubscribe(topics, topic));
    return 1;
}
/// <summary>
/// 返回缓冲区当前已写入数据。调用方拿到指针后禁止再写入 props(后续 realloc 会让返回指针悬挂);
/// 复用 props 须先 reset() 重置
/// </summary>
/// <param name="self" type="userdata">props 对象</param>
/// <returns type="lightuserdata?">数据指针；空时返回 nil</returns>
/// <returns type="integer">字节数；空时为 0</returns>
static int32_t _lmqtt_props_data(lua_State *lua) {
    binary_ctx *props = luaL_checkudata(lua, 1, MT_MQTT_PROPS);
    if (EMPTYPTR(props->data, props->offset)) {
        lua_pushnil(lua);
        lua_pushinteger(lua, 0);
        return 2;
    }
    LPUB_RET_LUD(lua, props->data, (lua_Integer)props->offset);
}
/// <summary>
/// 重置写入偏移（不释放内存，供后续复用）
/// </summary>
/// <param name="self" type="userdata">props 对象</param>
/// <returns>无</returns>
static int32_t _lmqtt_props_reset(lua_State *lua) {
    binary_ctx *props = luaL_checkudata(lua, 1, MT_MQTT_PROPS);
    binary_offset(props, 0);
    return 0;
}
/// <summary>
/// 发起异步 MQTT 连接。协议层内部 malloc 一份 mqtt_ctx 作为 ud->context，
/// 由框架 _mqtt_udfree 自动回收。调用方需用 srey.wait_connect(fd, skid, ssl)
/// 同步等待 TCP（含 SSL 握手）就绪
/// </summary>
/// <param name="version" type="integer">协议版本 mqtt_protversion</param>
/// <param name="sslname" type="string?">已注册的 SSL 上下文 name；省略或 "" 表示明文</param>
/// <param name="ip" type="string">对端 IP</param>
/// <param name="port" type="integer">对端端口</param>
/// <param name="netev" type="integer?">事件订阅掩码，默认 0</param>
/// <returns type="integer">socket fd；失败返回 INVALID_SOCK</returns>
/// <returns type="integer?">skid；仅在 fd 有效时返回</returns>
static int32_t _lmqtt_try_connect(lua_State *lua) {
    mqtt_protversion version = (mqtt_protversion)luaL_checkinteger(lua, 1);
    const char *sslname = luaL_optstring(lua, 2, NULL);
    const char *ip = luaL_checkstring(lua, 3);
    uint16_t port  = (uint16_t)luaL_checkinteger(lua, 4);
    int32_t netev  = (int32_t)luaL_optinteger(lua, 5, 0);
    struct evssl_ctx *evssl = NULL;
    if (!EMPTYSTR(sslname)) {
#if WITH_SSL
        evssl = evssl_qury(sslname);
#endif
        if (NULL == evssl) {
            lua_pushinteger(lua, INVALID_SOCK);
            return 1;
        }
    }
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    SOCKET fd;
    uint64_t skid;
    if (ERR_OK != mqtt_try_connect(task, evssl, ip, port, netev, version, &fd, &skid)) {
        lua_pushinteger(lua, INVALID_SOCK);
        return 1;
    }
    lua_pushinteger(lua, fd);
    lua_pushinteger(lua, (lua_Integer)skid);
    return 2;
}
// ---- 组包函数（模块级，第 1 参均为 version: mqtt_protversion） ----
/// <summary>
/// 构造 MQTT CONNECT 包
/// </summary>
/// <param name="version" type="integer">协议版本</param>
/// <param name="cleanstart" type="integer">CleanStart/CleanSession 标志</param>
/// <param name="keepalive" type="integer">心跳间隔秒数</param>
/// <param name="clientid" type="string">客户端 id</param>
/// <param name="user" type="string?">用户名；nil 不携带</param>
/// <param name="password" type="string?">密码；nil 不携带</param>
/// <param name="willtopic" type="string?">遗嘱主题；nil 不携带遗嘱</param>
/// <param name="willpayload" type="string?">遗嘱内容</param>
/// <param name="willqos" type="integer?">遗嘱 QoS，默认 0</param>
/// <param name="willretain" type="integer?">遗嘱 retain 标志，默认 0</param>
/// <param name="connprops" type="userdata?">CONNECT 属性 props 对象（MQTT 5.0）</param>
/// <param name="willprops" type="userdata?">遗嘱属性 props 对象（MQTT 5.0）</param>
/// <returns type="lightuserdata?">数据指针；失败返回 nil</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lmqtt_pack_connect(lua_State *lua) {
    mqtt_protversion version = (mqtt_protversion)luaL_checkinteger(lua, 1);
    int8_t   cleanstart = (int8_t)luaL_checkinteger(lua, 2);
    int16_t  keepalive  = (int16_t)luaL_checkinteger(lua, 3);
    const char *clientid = luaL_checkstring(lua, 4);
    const char *user = lua_isnoneornil(lua, 5) ? NULL : luaL_checkstring(lua, 5);
    char   *password = NULL;
    size_t  pwlens   = 0;
    if (!lua_isnoneornil(lua, 6)) {
        password = (char *)luaL_checklstring(lua, 6, &pwlens);
    }
    const char *willtopic = lua_isnoneornil(lua, 7) ? NULL : luaL_checkstring(lua, 7);
    char   *willpayload = NULL;
    size_t  wplens      = 0;
    if (!lua_isnoneornil(lua, 8)) {
        willpayload = (char *)luaL_checklstring(lua, 8, &wplens);
    }
    int8_t willqos    = (int8_t)luaL_optinteger(lua, 9, 0);
    int8_t willretain = (int8_t)luaL_optinteger(lua, 10, 0);
    binary_ctx *connprops = _lmqtt_get_props(lua, 11);
    binary_ctx *willprops = _lmqtt_get_props(lua, 12);
    size_t lens;
    char *pack = mqtt_pack_connect(version, cleanstart, keepalive,
                                   clientid, user, password, pwlens,
                                   willtopic, willpayload, wplens, willqos, willretain,
                                   connprops, willprops, &lens);
    if (NULL == pack) {
        lua_pushnil(lua);
        return 1;
    }
    LPUB_RET_LUD(lua, pack, (lua_Integer)lens);
}
/// <summary>
/// 构造 CONNACK 包（服务端使用）
/// </summary>
/// <param name="version" type="integer">协议版本</param>
/// <param name="sesspresent" type="integer">Session Present 标志</param>
/// <param name="reason" type="integer">Reason Code（MQTT 5.0），3.1.1 时为 return code</param>
/// <param name="props" type="userdata?">属性 props 对象</param>
/// <returns type="lightuserdata">数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmqtt_pack_connack(lua_State *lua) {
    mqtt_protversion version = (mqtt_protversion)luaL_checkinteger(lua, 1);
    int8_t  sesspresent = (int8_t)luaL_checkinteger(lua, 2);
    uint8_t reason      = (uint8_t)luaL_checkinteger(lua, 3);
    binary_ctx *props   = _lmqtt_get_props(lua, 4);
    size_t lens;
    char *pack = mqtt_pack_connack(version, sesspresent, reason, props, &lens);
    if (NULL == pack) {
        lua_pushnil(lua);
        return 1;
    }
    LPUB_RET_LUD(lua, pack, (lua_Integer)lens);
}
/// <summary>
/// 构造 MQTT PUBLISH 包
/// </summary>
/// <param name="version" type="integer">协议版本</param>
/// <param name="retain" type="integer">Retain 标志</param>
/// <param name="qos" type="integer">QoS 等级 0/1/2</param>
/// <param name="dup" type="integer">DUP 标志（重发）</param>
/// <param name="topic" type="string">主题</param>
/// <param name="packid" type="integer">报文 id（QoS 大于 0 时使用）</param>
/// <param name="payload" type="string|lightuserdata">载荷数据；字符串时长度自动取得</param>
/// <param name="paysize" type="integer?">payload 为 lightuserdata 时必填</param>
/// <param name="props" type="userdata?">属性 props 对象（MQTT 5.0）</param>
/// <returns type="lightuserdata?">数据指针；失败返回 nil</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lmqtt_pack_publish(lua_State *lua) {
    mqtt_protversion version = (mqtt_protversion)luaL_checkinteger(lua, 1);
    int8_t  retain  = (int8_t)luaL_checkinteger(lua, 2);
    int8_t  qos     = (int8_t)luaL_checkinteger(lua, 3);
    int8_t  dup     = (int8_t)luaL_checkinteger(lua, 4);
    const char *topic = luaL_checkstring(lua, 5);
    int16_t packid  = (int16_t)luaL_checkinteger(lua, 6);
    char   *payload = NULL;
    size_t  pllens  = 0;
    int32_t next    = _lmqtt_get_payload(lua, 7, &payload, &pllens);
    binary_ctx *props = _lmqtt_get_props(lua, next);
    size_t lens;
    char *pack = mqtt_pack_publish(version, retain, qos, dup,
                                   topic, packid, payload, pllens, props, &lens);
    if (NULL == pack) {
        lua_pushnil(lua);
        return 1;
    }
    LPUB_RET_LUD(lua, pack, (lua_Integer)lens);
}
/// <summary>
/// 构造 PUBACK 包（QoS 1 确认）
/// </summary>
/// <param name="version" type="integer">协议版本</param>
/// <param name="packid" type="integer">报文 id</param>
/// <param name="reason" type="integer?">Reason Code（MQTT 5.0），默认 0</param>
/// <param name="props" type="userdata?">属性 props 对象</param>
/// <returns type="lightuserdata">数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmqtt_pack_puback(lua_State *lua) {
    mqtt_protversion version = (mqtt_protversion)luaL_checkinteger(lua, 1);
    int16_t packid  = (int16_t)luaL_checkinteger(lua, 2);
    uint8_t reason  = (uint8_t)luaL_optinteger(lua, 3, 0);
    binary_ctx *props = _lmqtt_get_props(lua, 4);
    size_t lens;
    char *pack = mqtt_pack_puback(version, packid, reason, props, &lens);
    if (NULL == pack) {
        lua_pushnil(lua);
        return 1;
    }
    LPUB_RET_LUD(lua, pack, (lua_Integer)lens);
}
/// <summary>
/// 构造 PUBREC 包（QoS 2 第一步）
/// </summary>
/// <param name="version" type="integer">协议版本</param>
/// <param name="packid" type="integer">报文 id</param>
/// <param name="reason" type="integer?">Reason Code，默认 0</param>
/// <param name="props" type="userdata?">属性 props 对象</param>
/// <returns type="lightuserdata">数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmqtt_pack_pubrec(lua_State *lua) {
    mqtt_protversion version = (mqtt_protversion)luaL_checkinteger(lua, 1);
    int16_t packid  = (int16_t)luaL_checkinteger(lua, 2);
    uint8_t reason  = (uint8_t)luaL_optinteger(lua, 3, 0);
    binary_ctx *props = _lmqtt_get_props(lua, 4);
    size_t lens;
    char *pack = mqtt_pack_pubrec(version, packid, reason, props, &lens);
    if (NULL == pack) {
        lua_pushnil(lua);
        return 1;
    }
    LPUB_RET_LUD(lua, pack, (lua_Integer)lens);
}
/// <summary>
/// 构造 PUBREL 包（QoS 2 第二步）
/// </summary>
/// <param name="version" type="integer">协议版本</param>
/// <param name="packid" type="integer">报文 id</param>
/// <param name="reason" type="integer?">Reason Code，默认 0</param>
/// <param name="props" type="userdata?">属性 props 对象</param>
/// <returns type="lightuserdata">数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmqtt_pack_pubrel(lua_State *lua) {
    mqtt_protversion version = (mqtt_protversion)luaL_checkinteger(lua, 1);
    int16_t packid  = (int16_t)luaL_checkinteger(lua, 2);
    uint8_t reason  = (uint8_t)luaL_optinteger(lua, 3, 0);
    binary_ctx *props = _lmqtt_get_props(lua, 4);
    size_t lens;
    char *pack = mqtt_pack_pubrel(version, packid, reason, props, &lens);
    if (NULL == pack) {
        lua_pushnil(lua);
        return 1;
    }
    LPUB_RET_LUD(lua, pack, (lua_Integer)lens);
}
/// <summary>
/// 构造 PUBCOMP 包（QoS 2 第三步）
/// </summary>
/// <param name="version" type="integer">协议版本</param>
/// <param name="packid" type="integer">报文 id</param>
/// <param name="reason" type="integer?">Reason Code，默认 0</param>
/// <param name="props" type="userdata?">属性 props 对象</param>
/// <returns type="lightuserdata">数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmqtt_pack_pubcomp(lua_State *lua) {
    mqtt_protversion version = (mqtt_protversion)luaL_checkinteger(lua, 1);
    int16_t packid  = (int16_t)luaL_checkinteger(lua, 2);
    uint8_t reason  = (uint8_t)luaL_optinteger(lua, 3, 0);
    binary_ctx *props = _lmqtt_get_props(lua, 4);
    size_t lens;
    char *pack = mqtt_pack_pubcomp(version, packid, reason, props, &lens);
    if (NULL == pack) {
        lua_pushnil(lua);
        return 1;
    }
    LPUB_RET_LUD(lua, pack, (lua_Integer)lens);
}
/// <summary>
/// 构造 SUBSCRIBE 包
/// </summary>
/// <param name="version" type="integer">协议版本</param>
/// <param name="packid" type="integer">报文 id</param>
/// <param name="topics" type="userdata">订阅主题缓冲区（由 props:subscribe 填充）</param>
/// <param name="props" type="userdata?">属性 props 对象</param>
/// <returns type="lightuserdata">数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmqtt_pack_subscribe(lua_State *lua) {
    mqtt_protversion version = (mqtt_protversion)luaL_checkinteger(lua, 1);
    int16_t packid  = (int16_t)luaL_checkinteger(lua, 2);
    binary_ctx *topics = luaL_checkudata(lua, 3, MT_MQTT_PROPS);
    binary_ctx *props  = _lmqtt_get_props(lua, 4);
    size_t lens;
    char *pack = mqtt_pack_subscribe(version, packid, topics, props, &lens);
    if (NULL == pack) {
        lua_pushnil(lua);
        return 1;
    }
    LPUB_RET_LUD(lua, pack, (lua_Integer)lens);
}
/// <summary>
/// 构造 SUBACK 包（服务端使用）
/// </summary>
/// <param name="version" type="integer">协议版本</param>
/// <param name="packid" type="integer">报文 id</param>
/// <param name="reasons" type="string">原因码字节序列（每字节对应一个订阅主题的 QoS 或失败码）</param>
/// <param name="props" type="userdata?">属性 props 对象</param>
/// <returns type="lightuserdata">数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmqtt_pack_suback(lua_State *lua) {
    mqtt_protversion version = (mqtt_protversion)luaL_checkinteger(lua, 1);
    int16_t packid  = (int16_t)luaL_checkinteger(lua, 2);
    size_t rslens;
    const char *reasons = luaL_checklstring(lua, 3, &rslens);
    binary_ctx *props   = _lmqtt_get_props(lua, 4);
    size_t lens;
    char *pack = mqtt_pack_suback(version, packid,
                                  (uint8_t *)reasons, rslens, props, &lens);
    if (NULL == pack) {
        lua_pushnil(lua);
        return 1;
    }
    LPUB_RET_LUD(lua, pack, (lua_Integer)lens);
}
/// <summary>
/// 构造 UNSUBSCRIBE 包
/// </summary>
/// <param name="version" type="integer">协议版本</param>
/// <param name="packid" type="integer">报文 id</param>
/// <param name="topics" type="userdata">取消订阅主题缓冲区（由 props:unsubscribe 填充）</param>
/// <param name="props" type="userdata?">属性 props 对象</param>
/// <returns type="lightuserdata">数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmqtt_pack_unsubscribe(lua_State *lua) {
    mqtt_protversion version = (mqtt_protversion)luaL_checkinteger(lua, 1);
    int16_t packid  = (int16_t)luaL_checkinteger(lua, 2);
    binary_ctx *topics = luaL_checkudata(lua, 3, MT_MQTT_PROPS);
    binary_ctx *props  = _lmqtt_get_props(lua, 4);
    size_t lens;
    char *pack = mqtt_pack_unsubscribe(version, packid, topics, props, &lens);
    if (NULL == pack) {
        lua_pushnil(lua);
        return 1;
    }
    LPUB_RET_LUD(lua, pack, (lua_Integer)lens);
}
/// <summary>
/// 构造 UNSUBACK 包（服务端使用）
/// </summary>
/// <param name="version" type="integer">协议版本</param>
/// <param name="packid" type="integer">报文 id</param>
/// <param name="reasons" type="string">原因码字节序列（MQTT 5.0 有效，3.1.1 传 ""）</param>
/// <param name="props" type="userdata?">属性 props 对象</param>
/// <returns type="lightuserdata">数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmqtt_pack_unsuback(lua_State *lua) {
    mqtt_protversion version = (mqtt_protversion)luaL_checkinteger(lua, 1);
    int16_t packid  = (int16_t)luaL_checkinteger(lua, 2);
    size_t rslens;
    const char *reasons = luaL_checklstring(lua, 3, &rslens);
    binary_ctx *props   = _lmqtt_get_props(lua, 4);
    size_t lens;
    char *pack = mqtt_pack_unsuback(version, packid,
                                    (uint8_t *)reasons, rslens, props, &lens);
    if (NULL == pack) {
        lua_pushnil(lua);
        return 1;
    }
    LPUB_RET_LUD(lua, pack, (lua_Integer)lens);
}
/// <summary>
/// 构造 PINGREQ 包（客户端心跳）
/// </summary>
/// <param>无</param>
/// <returns type="lightuserdata">数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmqtt_pack_ping(lua_State *lua) {
    (void)lua;
    size_t lens;
    char *pack = mqtt_pack_ping(&lens);
    if (NULL == pack) {
        lua_pushnil(lua);
        return 1;
    }
    LPUB_RET_LUD(lua, pack, (lua_Integer)lens);
}
/// <summary>
/// 构造 PINGRESP 包（服务端心跳响应）
/// </summary>
/// <param>无</param>
/// <returns type="lightuserdata">数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmqtt_pack_pong(lua_State *lua) {
    (void)lua;
    size_t lens;
    char *pack = mqtt_pack_pong(&lens);
    if (NULL == pack) {
        lua_pushnil(lua);
        return 1;
    }
    LPUB_RET_LUD(lua, pack, (lua_Integer)lens);
}
/// <summary>
/// 构造 DISCONNECT 包
/// </summary>
/// <param name="version" type="integer">协议版本</param>
/// <param name="reason" type="integer?">Reason Code（MQTT 5.0），3.1.1 时忽略，默认 0</param>
/// <param name="props" type="userdata?">属性 props 对象</param>
/// <returns type="lightuserdata">数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmqtt_pack_disconnect(lua_State *lua) {
    mqtt_protversion version = (mqtt_protversion)luaL_checkinteger(lua, 1);
    uint8_t reason = (uint8_t)luaL_optinteger(lua, 2, 0);
    binary_ctx *props = _lmqtt_get_props(lua, 3);
    size_t lens;
    char *pack = mqtt_pack_disconnect(version, reason, props, &lens);
    if (NULL == pack) {
        lua_pushnil(lua);
        return 1;
    }
    LPUB_RET_LUD(lua, pack, (lua_Integer)lens);
}
/// <summary>
/// 构造 AUTH 包（MQTT 5.0 增强认证）
/// </summary>
/// <param name="version" type="integer">协议版本</param>
/// <param name="reason" type="integer?">Reason Code，默认 0</param>
/// <param name="props" type="userdata?">属性 props 对象</param>
/// <returns type="lightuserdata">数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lmqtt_pack_auth(lua_State *lua) {
    mqtt_protversion version = (mqtt_protversion)luaL_checkinteger(lua, 1);
    uint8_t reason = (uint8_t)luaL_optinteger(lua, 2, 0);
    binary_ctx *props = _lmqtt_get_props(lua, 3);
    size_t lens;
    char *pack = mqtt_pack_auth(version, reason, props, &lens);
    if (NULL == pack) {
        lua_pushnil(lua);
        return 1;
    }
    LPUB_RET_LUD(lua, pack, (lua_Integer)lens);
}
// ---- 模块级属性读取 ----
// 从 pack->varhead 中取 properties 指针（按 prot 分派）；无属性时返回 NULL
static array_ctx *_lmqtt_varhead_props(mqtt_pack_ctx *pack) {
    switch (pack->fixhead.prot) {
    case MQTT_CONNECT:     return ((mqtt_connect_varhead    *)pack->varhead)->properties;
    case MQTT_CONNACK:     return ((mqtt_connack_varhead    *)pack->varhead)->properties;
    case MQTT_PUBLISH:     return ((mqtt_publish_varhead    *)pack->varhead)->properties;
    case MQTT_PUBACK:
    case MQTT_PUBREC:
    case MQTT_PUBREL:
    case MQTT_PUBCOMP:     return ((mqtt_pubackrel_varhead  *)pack->varhead)->properties;
    case MQTT_SUBSCRIBE:
    case MQTT_SUBACK:
    case MQTT_UNSUBSCRIBE:
    case MQTT_UNSUBACK:    return ((mqtt_subreqresp_varhead *)pack->varhead)->properties;
    case MQTT_DISCONNECT:
    case MQTT_AUTH:        return ((mqtt_reason_varhead     *)pack->varhead)->properties;
    default:               return NULL;
    }
}
/// <summary>
/// 返回报文可变头的属性数组
/// </summary>
/// <param name="pack" type="lightuserdata">mqtt_pack_ctx 指针</param>
/// <returns type="lightuserdata?">属性数组指针；无属性或不支持的报文类型返回 nil</returns>
/// <returns type="integer">属性条数；空时为 0</returns>
static int32_t _lmqtt_props_of(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    mqtt_pack_ctx *pack = lua_touserdata(lua, 1);
    array_ctx *arr = _lmqtt_varhead_props(pack);
    if (NULL == arr || 0 == array_size(arr)) {
        lua_pushnil(lua);
        lua_pushinteger(lua, 0);
        return 2;
    }
    LPUB_RET_LUD(lua, arr, (lua_Integer)array_size(arr));
}
/// <summary>
/// 返回 CONNECT 报文载荷中的遗嘱属性数组
/// </summary>
/// <param name="pack" type="lightuserdata">mqtt_pack_ctx 指针</param>
/// <returns type="lightuserdata?">属性数组指针；非 CONNECT 报文或无遗嘱属性返回 nil</returns>
/// <returns type="integer">属性条数；空时为 0</returns>
static int32_t _lmqtt_connect_will_props(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    mqtt_pack_ctx *pack = lua_touserdata(lua, 1);
    if (MQTT_CONNECT != pack->fixhead.prot) {
        lua_pushnil(lua);
        lua_pushinteger(lua, 0);
        return 2;
    }
    mqtt_connect_payload *pl = (mqtt_connect_payload *)pack->payload;
    if (NULL == pl || EMPTYPTR(pl->properties, array_size(pl->properties))) {
        lua_pushnil(lua);
        lua_pushinteger(lua, 0);
        return 2;
    }
    LPUB_RET_LUD(lua, pl->properties, (lua_Integer)array_size(pl->properties));
}
/// <summary>
/// 按 1 起始下标读取属性数组中的一条属性
/// </summary>
/// <param name="arr" type="lightuserdata">属性数组指针</param>
/// <param name="i" type="integer">1 起始下标</param>
/// <returns type="integer?">属性 flag；越界时返回 nil（仅 1 个返回值）</returns>
/// <returns type="integer">数字属性的整数值（其他类型为 0）</returns>
/// <returns type="string?">字符串/二进制属性的值或 USER_PROPERTY 的 key；数字属性为 nil</returns>
/// <returns type="string?">USER_PROPERTY 的 value；其他属性为 nil</returns>
static int32_t _lmqtt_prop_at(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    array_ctx *arr = lua_touserdata(lua, 1);
    int32_t i = (int32_t)luaL_checkinteger(lua, 2) - 1;
    if (i < 0 || (uint32_t)i >= array_size(arr)) {
        lua_pushnil(lua);
        return 1;
    }
    mqtt_propertie *p = *(mqtt_propertie **)array_at(arr, i);
    lua_pushinteger(lua, p->flag);
    lua_pushinteger(lua, p->nval);
    if (p->flens > 0) {
        lua_pushlstring(lua, p->fval, p->flens);
    } else {
        lua_pushnil(lua);
    }
    if (NULL != p->sval && p->slens > 0) {
        lua_pushlstring(lua, p->sval, p->slens);
    } else {
        lua_pushnil(lua);
    }
    return 4;
}
// ---- 模块级解包访问器 ----
/// <summary>
/// 返回报文控制类型
/// </summary>
/// <param name="pack" type="lightuserdata">mqtt_pack_ctx 指针</param>
/// <returns type="integer">mqtt_prot 枚举值</returns>
static int32_t _lmqtt_prot(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    mqtt_pack_ctx *pack = lua_touserdata(lua, 1);
    lua_pushinteger(lua, pack->fixhead.prot);
    return 1;
}
/// <summary>
/// 返回报文协议版本
/// </summary>
/// <param name="pack" type="lightuserdata">mqtt_pack_ctx 指针</param>
/// <returns type="integer">mqtt_protversion 枚举值</returns>
static int32_t _lmqtt_pack_version(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    mqtt_pack_ctx *pack = lua_touserdata(lua, 1);
    lua_pushinteger(lua, pack->version);
    return 1;
}
/// <summary>
/// 解析 CONNECT 报文的可变报头与载荷，返回连接信息表（服务端用以识别客户端）。
/// CONNECT 报头的 MQTT5 属性经 pack_props 读取，遗嘱属性经 connect_will_props 读取，均不在此表内。
/// </summary>
/// <param name="pack" type="lightuserdata">mqtt_pack_ctx 指针</param>
/// <returns type="table?">{version, cleanstart, keepalive, willflag, willqos, willretain, clientid,
/// user?, password?, willtopic?, willpayload?}；非 CONNECT 报文返回 nil</returns>
static int32_t _lmqtt_connect_info(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    mqtt_pack_ctx *pack = lua_touserdata(lua, 1);
    if (MQTT_CONNECT != pack->fixhead.prot) {
        lua_pushnil(lua);
        return 1;
    }
    mqtt_connect_varhead *vh = (mqtt_connect_varhead *)pack->varhead;
    mqtt_connect_payload *pl = (mqtt_connect_payload *)pack->payload;
    if (NULL == vh || NULL == pl) {
        lua_pushnil(lua);
        return 1;
    }
    lua_createtable(lua, 0, 11);
    lua_pushinteger(lua, vh->version);    lua_setfield(lua, -2, "version");
    lua_pushinteger(lua, vh->cleanstart); lua_setfield(lua, -2, "cleanstart");
    lua_pushinteger(lua, vh->keepalive);  lua_setfield(lua, -2, "keepalive");
    lua_pushinteger(lua, vh->willflag);   lua_setfield(lua, -2, "willflag");
    lua_pushinteger(lua, vh->willqos);    lua_setfield(lua, -2, "willqos");
    lua_pushinteger(lua, vh->willretain); lua_setfield(lua, -2, "willretain");
    if (NULL != pl->clientid) {
        lua_pushstring(lua, pl->clientid);
        lua_setfield(lua, -2, "clientid");
    }
    if (vh->userflag && NULL != pl->user) {
        lua_pushstring(lua, pl->user);
        lua_setfield(lua, -2, "user");
    }
    if (vh->passwordflag && NULL != pl->password) {
        lua_pushlstring(lua, pl->password, pl->pslens);
        lua_setfield(lua, -2, "password");
    }
    if (vh->willflag) {
        if (NULL != pl->willtopic) {
            lua_pushstring(lua, pl->willtopic);
            lua_setfield(lua, -2, "willtopic");
        }
        if (NULL != pl->willpayload) {
            lua_pushlstring(lua, pl->willpayload, pl->wplens);
            lua_setfield(lua, -2, "willpayload");
        }
    }
    return 1;
}
/// <summary>
/// 解析 CONNACK 可变报头
/// </summary>
/// <param name="pack" type="lightuserdata">mqtt_pack_ctx 指针</param>
/// <returns type="integer">Session Present 标志</returns>
/// <returns type="integer">Reason Code / Return Code</returns>
static int32_t _lmqtt_connack(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    mqtt_pack_ctx *pack = lua_touserdata(lua, 1);
    mqtt_connack_varhead *vh = (mqtt_connack_varhead *)pack->varhead;
    lua_pushinteger(lua, vh->sesspresent);
    lua_pushinteger(lua, vh->reason);
    return 2;
}
/// <summary>
/// 解析 PUBLISH 可变报头和载荷
/// </summary>
/// <param name="pack" type="lightuserdata">mqtt_pack_ctx 指针</param>
/// <returns type="integer">dup 标志</returns>
/// <returns type="integer">qos 等级</returns>
/// <returns type="integer">retain 标志</returns>
/// <returns type="integer">报文 id</returns>
/// <returns type="string">主题</returns>
/// <returns type="string?">载荷内容；空时返回 nil</returns>
/// <returns type="integer">载荷字节数；空时为 0</returns>
static int32_t _lmqtt_publish(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    mqtt_pack_ctx *pack = lua_touserdata(lua, 1);
    mqtt_publish_varhead  *vh = (mqtt_publish_varhead *)pack->varhead;
    mqtt_publish_payload  *pl = (mqtt_publish_payload *)pack->payload;
    lua_pushinteger(lua, vh->dup);
    lua_pushinteger(lua, vh->qos);
    lua_pushinteger(lua, vh->retain);
    lua_pushinteger(lua, vh->packid);
    lua_pushstring(lua, vh->topic);
    if (NULL != pl && pl->lens > 0) {
        lua_pushlstring(lua, pl->content, (size_t)pl->lens);
        lua_pushinteger(lua, pl->lens);
    } else {
        lua_pushnil(lua);
        lua_pushinteger(lua, 0);
    }
    return 7;
}
/// <summary>
/// 解析 PUBACK 可变报头
/// </summary>
/// <param name="pack" type="lightuserdata">mqtt_pack_ctx 指针</param>
/// <returns type="integer">报文 id</returns>
/// <returns type="integer">Reason Code</returns>
static int32_t _lmqtt_puback(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    mqtt_pack_ctx *pack = lua_touserdata(lua, 1);
    mqtt_pubackrel_varhead *vh = (mqtt_pubackrel_varhead *)pack->varhead;
    lua_pushinteger(lua, vh->packid);
    lua_pushinteger(lua, vh->reason);
    return 2;
}
/// <summary>
/// 解析 PUBREC 可变报头
/// </summary>
/// <param name="pack" type="lightuserdata">mqtt_pack_ctx 指针</param>
/// <returns type="integer">报文 id</returns>
/// <returns type="integer">Reason Code</returns>
static int32_t _lmqtt_pubrec(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    mqtt_pack_ctx *pack = lua_touserdata(lua, 1);
    mqtt_pubackrel_varhead *vh = (mqtt_pubackrel_varhead *)pack->varhead;
    lua_pushinteger(lua, vh->packid);
    lua_pushinteger(lua, vh->reason);
    return 2;
}
/// <summary>
/// 解析 PUBREL 可变报头
/// </summary>
/// <param name="pack" type="lightuserdata">mqtt_pack_ctx 指针</param>
/// <returns type="integer">报文 id</returns>
/// <returns type="integer">Reason Code</returns>
static int32_t _lmqtt_pubrel(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    mqtt_pack_ctx *pack = lua_touserdata(lua, 1);
    mqtt_pubackrel_varhead *vh = (mqtt_pubackrel_varhead *)pack->varhead;
    lua_pushinteger(lua, vh->packid);
    lua_pushinteger(lua, vh->reason);
    return 2;
}
/// <summary>
/// 解析 PUBCOMP 可变报头
/// </summary>
/// <param name="pack" type="lightuserdata">mqtt_pack_ctx 指针</param>
/// <returns type="integer">报文 id</returns>
/// <returns type="integer">Reason Code</returns>
static int32_t _lmqtt_pubcomp(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    mqtt_pack_ctx *pack = lua_touserdata(lua, 1);
    mqtt_pubackrel_varhead *vh = (mqtt_pubackrel_varhead *)pack->varhead;
    lua_pushinteger(lua, vh->packid);
    lua_pushinteger(lua, vh->reason);
    return 2;
}
/// <summary>
/// 解析 SUBSCRIBE 可变报头与载荷（服务端用以回 SUBACK 并建立订阅）
/// </summary>
/// <param name="pack" type="lightuserdata">mqtt_pack_ctx 指针</param>
/// <returns type="integer">报文 id</returns>
/// <returns type="table">订阅项数组 [{topic, qos, nl, rap, retain}, ...]；无订阅时为空表</returns>
static int32_t _lmqtt_subscribe(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    mqtt_pack_ctx *pack = lua_touserdata(lua, 1);
    mqtt_subreqresp_varhead *vh = (mqtt_subreqresp_varhead *)pack->varhead;
    mqtt_subscribe_payload *pl = (mqtt_subscribe_payload *)pack->payload;
    lua_pushinteger(lua, vh->packid);
    uint32_t cnt = (NULL == pl) ? 0 : array_size(&pl->subop);
    lua_createtable(lua, (int32_t)cnt, 0);
    int32_t i;
    subscribe_option *op;
    for (i = 0; (uint32_t)i < cnt; i++) {
        op = *(subscribe_option **)array_at(&pl->subop, i);
        lua_createtable(lua, 0, 5);
        if (NULL != op->topic) {
            lua_pushstring(lua, op->topic);
            lua_setfield(lua, -2, "topic");
        }
        lua_pushinteger(lua, op->qos);    lua_setfield(lua, -2, "qos");
        lua_pushinteger(lua, op->nl);     lua_setfield(lua, -2, "nl");
        lua_pushinteger(lua, op->rap);    lua_setfield(lua, -2, "rap");
        lua_pushinteger(lua, op->retain); lua_setfield(lua, -2, "retain");
        lua_rawseti(lua, -2, i + 1);
    }
    return 2;
}
/// <summary>
/// 解析 UNSUBSCRIBE 可变报头与载荷（服务端用以回 UNSUBACK 并取消订阅）
/// </summary>
/// <param name="pack" type="lightuserdata">mqtt_pack_ctx 指针</param>
/// <returns type="integer">报文 id</returns>
/// <returns type="table">主题过滤器数组 [topic, ...]；无主题时为空表</returns>
static int32_t _lmqtt_unsubscribe(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    mqtt_pack_ctx *pack = lua_touserdata(lua, 1);
    mqtt_subreqresp_varhead *vh = (mqtt_subreqresp_varhead *)pack->varhead;
    mqtt_unsubscribe_payload *pl = (mqtt_unsubscribe_payload *)pack->payload;
    lua_pushinteger(lua, vh->packid);
    uint32_t cnt = (NULL == pl) ? 0 : array_size(&pl->topics);
    lua_createtable(lua, (int32_t)cnt, 0);
    int32_t i;
    char *topic;
    for (i = 0; (uint32_t)i < cnt; i++) {
        topic = *(char **)array_at(&pl->topics, i);
        lua_pushstring(lua, (NULL != topic) ? topic : "");
        lua_rawseti(lua, -2, i + 1);
    }
    return 2;
}
/// <summary>
/// 解析 SUBACK 可变报头和载荷
/// </summary>
/// <param name="pack" type="lightuserdata">mqtt_pack_ctx 指针</param>
/// <returns type="integer">报文 id</returns>
/// <returns type="string?">原因码字节序列；空时返回 nil</returns>
static int32_t _lmqtt_suback(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    mqtt_pack_ctx *pack = lua_touserdata(lua, 1);
    mqtt_subreqresp_varhead *vh = (mqtt_subreqresp_varhead *)pack->varhead;
    mqtt_reasonlist_payload *pl = (mqtt_reasonlist_payload *)pack->payload;
    lua_pushinteger(lua, vh->packid);
    if (NULL != pl && pl->rlens > 0) {
        lua_pushlstring(lua, (const char *)pl->reasons, (size_t)pl->rlens);
    } else {
        lua_pushnil(lua);
    }
    return 2;
}
/// <summary>
/// 解析 UNSUBACK 可变报头和载荷（MQTT 5.0 才有 reasons）
/// </summary>
/// <param name="pack" type="lightuserdata">mqtt_pack_ctx 指针</param>
/// <returns type="integer">报文 id</returns>
/// <returns type="string?">原因码字节序列；MQTT 3.1.1 或空时返回 nil</returns>
static int32_t _lmqtt_unsuback(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    mqtt_pack_ctx *pack = lua_touserdata(lua, 1);
    mqtt_subreqresp_varhead *vh = (mqtt_subreqresp_varhead *)pack->varhead;
    mqtt_reasonlist_payload *pl = (mqtt_reasonlist_payload *)pack->payload;
    lua_pushinteger(lua, vh->packid);
    if (NULL != pl && pl->rlens > 0) {
        lua_pushlstring(lua, (const char *)pl->reasons, (size_t)pl->rlens);
    } else {
        lua_pushnil(lua);
    }
    return 2;
}
/// <summary>
/// 解析 DISCONNECT 可变报头（MQTT 5.0）
/// </summary>
/// <param name="pack" type="lightuserdata">mqtt_pack_ctx 指针</param>
/// <returns type="integer">Reason Code；变报头为空时返回 0</returns>
static int32_t _lmqtt_disconnect(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    mqtt_pack_ctx *pack = lua_touserdata(lua, 1);
    mqtt_reason_varhead *vh = (mqtt_reason_varhead *)pack->varhead;
    if (NULL == vh) {
        lua_pushinteger(lua, 0);
    } else {
        lua_pushinteger(lua, vh->reason);
    }
    return 1;
}
/// <summary>
/// 返回指定报文类型和原因码对应的可读字符串
/// </summary>
/// <param name="prot" type="integer">mqtt_prot 枚举值</param>
/// <param name="code" type="integer">原因码</param>
/// <returns type="string">可读描述</returns>
static int32_t _lmqtt_reason(lua_State *lua) {
    mqtt_prot prot = (mqtt_prot)luaL_checkinteger(lua, 1);
    int32_t code   = (int32_t)luaL_checkinteger(lua, 2);
    lua_pushstring(lua, mqtt_reason(prot, code));
    return 1;
}
//mqtt
LUAMOD_API int luaopen_mqtt(lua_State *lua) {
    // MT_MQTT_PROPS 元表
    luaL_Reg props_reg_func[] = {
        { "fixnum",      _lmqtt_props_fixnum },
        { "varnum",      _lmqtt_props_varnum },
        { "binary",      _lmqtt_props_binary },
        { "kv",          _lmqtt_props_kv },
        { "subscribe",   _lmqtt_props_subscribe },
        { "unsubscribe", _lmqtt_props_unsubscribe },
        { "data",        _lmqtt_props_data },
        { "reset",       _lmqtt_props_reset },
        { "free",        _lmqtt_props_free },
        { "__gc",        _lmqtt_props_free },
        { NULL, NULL }
    };
    luaL_newmetatable(lua, MT_MQTT_PROPS);
    lua_pushvalue(lua, -1);
    lua_setfield(lua, -2, "__index");
    luaL_setfuncs(lua, props_reg_func, 0);
    lua_pop(lua, 1);
    // 模块表：所有函数均为模块级
    luaL_Reg reg_mod[] = {
        { "try_connect",       _lmqtt_try_connect },
        { "props",             _lmqtt_props_new },
        { "pack_props",        _lmqtt_props_of },
        { "connect_will_props",_lmqtt_connect_will_props },
        { "prop_at",           _lmqtt_prop_at },
        { "prot",              _lmqtt_prot },
        { "pack_version",      _lmqtt_pack_version },
        
        { "connect_info",      _lmqtt_connect_info },
        { "connack",           _lmqtt_connack },
        { "publish",           _lmqtt_publish },
        { "puback",            _lmqtt_puback },
        { "pubrec",            _lmqtt_pubrec },
        { "pubrel",            _lmqtt_pubrel },
        { "pubcomp",           _lmqtt_pubcomp },
        { "subscribe",         _lmqtt_subscribe },
        { "unsubscribe",       _lmqtt_unsubscribe },
        { "suback",            _lmqtt_suback },
        { "unsuback",          _lmqtt_unsuback },
        { "disconnect",        _lmqtt_disconnect },
        { "reason",            _lmqtt_reason },
        { "pack_connect",      _lmqtt_pack_connect },
        { "pack_connack",      _lmqtt_pack_connack },
        { "pack_publish",      _lmqtt_pack_publish },
        { "pack_puback",       _lmqtt_pack_puback },
        { "pack_pubrec",       _lmqtt_pack_pubrec },
        { "pack_pubrel",       _lmqtt_pack_pubrel },
        { "pack_pubcomp",      _lmqtt_pack_pubcomp },
        { "pack_subscribe",    _lmqtt_pack_subscribe },
        { "pack_suback",       _lmqtt_pack_suback },
        { "pack_unsubscribe",  _lmqtt_pack_unsubscribe },
        { "pack_unsuback",     _lmqtt_pack_unsuback },
        { "pack_ping",         _lmqtt_pack_ping },
        { "pack_pong",         _lmqtt_pack_pong },
        { "pack_disconnect",   _lmqtt_pack_disconnect },
        { "pack_auth",         _lmqtt_pack_auth },
        { NULL, NULL }
    };
    luaL_newlib(lua, reg_mod);
    return 1;
}
