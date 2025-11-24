#ifndef MQTT_PACK_H_
#define MQTT_PACK_H_

#include "utils/binary.h"
#include "protocol/mqtt/mqtt_struct.h"

//属性打包函数(MQTT5.0)
int32_t mqtt_props_fixnum(binary_ctx *props, mqtt_prop_flag flag, int32_t val);
int32_t mqtt_props_varnum(binary_ctx *props, mqtt_prop_flag flag, int32_t val);
int32_t mqtt_props_binary(binary_ctx *props, mqtt_prop_flag flag, void *data, int32_t lens);
int32_t mqtt_props_kv(binary_ctx *props, mqtt_prop_flag flag, void *key, size_t klens, void *val, size_t vlens);
/// <summary>
/// 订阅 主题打包
/// </summary>
/// <param name="topics">binary_ctx</param>
/// <param name="version">mqtt_protversion</param>
/// <param name="topic">主题名 UTF-8字符串</param>
/// <param name="qos">最大服务质量</param>
/// <param name="nl">非本地(No Local  MQTT5.0) 1:表示应用消息不能被转发给发布此消息的客户标识符。共享订阅时设为1将造成协议错误</param>
/// <param name="rap">发布保留(MQTT5.0) 
/// 1:表示向此订阅转发应用消息时保持消息被发布时设置的保留(RETAIN)标志。
/// 0:表示向此订阅转发应用消息时把保留标志设置为0。当订阅建立之后，发送保留消息时保留标志设置为1。
/// </param>
/// <param name="retain">保留操作(MQTT5.0) 
/// 0:订阅建立时发送保留消息 
/// 1:订阅建立时，若该订阅当前不存在则发送保留消息
/// 2:订阅建立时不要发送保留消息
/// </param>
void mqtt_topics_subscribe(binary_ctx *topics, mqtt_protversion version, const char *topic,
    int8_t qos, int8_t nl, int8_t rap, int8_t retain);
/// <summary>
/// 取消订阅 主题打包
/// </summary>
/// <param name="topics">binary_ctx</param>
/// <param name="topic">主题名 UTF-8字符串</param>
void mqtt_topics_unsubscribe(binary_ctx *topics, const char *topic);
/// <summary>
/// 连接请求
/// </summary>
/// <param name="version">mqtt_protversion</param>
/// <param name="cleanstart">新开始 表明此次连接是一个新的会话还是一个已存在的会话的延续</param>
/// <param name="keepalive">保持连接</param>
/// <param name="clientid">唯一的客户标识符 UTF-8字符串</param>
/// <param name="user">用户名  UTF-8字符串</param>
/// <param name="password">密码 二进制数据</param>
/// <param name="pwlens">密码长度</param>
/// <param name="willtopic">遗嘱主题 UTF-8字符串</param>
/// <param name="willpayload">遗嘱载荷 二进制数据</param>
/// <param name="wplens">遗嘱载荷长度</param>
/// <param name="willqos">遗嘱 QoS(0 1 2)</param>
/// <param name="willretain">遗嘱保留</param>
/// <param name="connprops">CONNECT属性(MQTT5.0):
/// SESSION_EXPIRY(0x11) RECEIVE_MAXIMUM(0x21) MAXIMUM_PACKETSIZE(0x27) 
/// TOPICALIAS_MAXIMUM(0x22) REQRESP_INFO(0x19) REQPROBLEM_INFO(0x17) USER_PROPERTY(0x26) AUTH_METHOD(0x15) AUTH_DATA(0x16))
/// </param>
/// <param name="willprops">遗嘱属性(MQTT5.0):
/// WILLDELAY_INTERVAL(0x18) PAYLOAD_FORMAT(0x01) MSG_EXPIRY(0x02) CONTENT_TYPE(0x03) RESP_TOPIC(0x08) CORRELATION_DATA(0x09) USER_PROPERTY(0x26)
/// </param>
/// <param name="lens">组包后的数据长度</param>
/// <returns>char * 数据包</returns>
char *mqtt_pack_connect(mqtt_protversion version, int8_t cleanstart, int16_t keepalive, const char *clientid,
    const char *user, char *password, size_t pwlens, 
    const char *willtopic, char *willpayload, size_t wplens, int8_t willqos, int8_t willretain,
    binary_ctx *connprops, binary_ctx *willprops, size_t *lens);
/// <summary>
/// 确认连接请求
/// </summary>
/// <param name="version">mqtt_protversion</param>
/// <param name="sesspresent">会话存在标志通知客户端，服务端是否正在使用此客户标识符之前连接的会话状态</param>
/// <param name="reason">连接原因码</param>
/// <param name="props">属性(MQTT5.0):
/// SESSION_EXPIRY(0x11) RECEIVE_MAXIMUM(0x21) MAXIMUM_QOS(0x24) RETAIN_AVAILABLE(0x25) MAXIMUM_PACKETSIZE(0x27) 
/// CLIENT_ID(0x12) TOPICALIAS_MAXIMUM(0x22) REASON_STR(0x1F) USER_PROPERTY(0x26) WILDCARD_SUBSCRIPTION(0x28) SUBSCRIPTIONID_AVAILABLE(0x29)
/// SHARED_SUBSCRIPTION(0x2A) SERVER_KEEPALIVE(0x13) RESP_INFO(0x1A) SERVER_REFERENCE(0x1C) AUTH_METHOD(0x15) AUTH_DATA(0x16)
/// </param>
/// <param name="lens">组包后的数据长度</param>
/// <returns>char * 数据包</returns>
char *mqtt_pack_connack(mqtt_protversion version, int8_t sesspresent, uint8_t reason, binary_ctx *props, size_t *lens);
/// <summary>
/// 发布消息
/// </summary>
/// <param name="version">mqtt_protversion</param>
/// <param name="retain">保留标志 如果客户端发给服务端的PUBLISH报文的保留（Retain）标志被设置为1，服务端必须存储此应用消息，并用其替换此话题下任何已存在的消息</param>
/// <param name="qos">服务质量等级(0 1 2)
/// 0 :无返回
/// 1: PUBLISH -> PUBACK
/// 2: 
/// Client  --PUBLISH    ->  Server
/// Client  <- PUBREC    --  Server
/// Client  -- PUBREL    ->  Server
/// Client  <- PUBCOMP   --  Server 
/// </param>
/// <param name="dup">重发标志 0:表示这是客户端或服务端第一次请求发送这个PUBLISH报文 1:表示这可能是一个早前报文请求的重发</param>
/// <param name="topic">主题名 UTF-8字符串</param>
/// <param name="packid">报文标识符(只有当QoS等级是1或2时)</param>
/// <param name="payload">载荷</param>
/// <param name="pllens">载荷长度</param>
/// <param name="props">属性(MQTT5.0):
/// PAYLOAD_FORMAT(0x01) MSG_EXPIRY(0x02) TOPIC_ALIAS(0x23) RESP_TOPIC(0x08) CORRELATION_DATA(0x09) USER_PROPERTY(0x26)
/// SUBSCRIPTION_ID(0x0B) CONTENT_TYPE(0x03)
/// </param>
/// <param name="lens">组包后的数据长度</param>
/// <returns>char * 数据包</returns>
char *mqtt_pack_publish(mqtt_protversion version, int8_t retain, int8_t qos, int8_t dup,
    const char *topic, int16_t packid, char *payload, size_t pllens, binary_ctx *props, size_t *lens);
/// <summary>
/// 发布确认
/// </summary>
/// <param name="version">mqtt_protversion</param>
/// <param name="packid">报文标识符</param>
/// <param name="reason">原因码(MQTT5.0) 0x00:消息被接收。QoS为2的消息已发布 0x10:消息被接收，但没有订阅者(服务端才会发送)</param>
/// <param name="props">属性(MQTT5.0):
/// REASON_STR(0x1F) USER_PROPERTY(0x26)
/// </param>
/// <param name="lens">组包后的数据长度</param>
/// <returns>char * 数据包</returns>
char *mqtt_pack_puback(mqtt_protversion version, int16_t packid, uint8_t reason, binary_ctx *props, size_t *lens);
/// <summary>
/// 发布已接收（QoS 2，第一步）
/// </summary>
/// <param name="version">mqtt_protversion</param>
/// <param name="packid">报文标识符</param>
/// <param name="reason">原因码(MQTT5.0) 0x00:消息被接收。QoS为2的消息已发布 0x10:消息被接收，但没有订阅者(服务端才会发送)</param>
/// <param name="props">属性(MQTT5.0):
/// REASON_STR(0x1F) USER_PROPERTY(0x26)
/// </param>
/// <param name="lens">组包后的数据长度</param>
/// <returns>char * 数据包</returns>
char *mqtt_pack_pubrec(mqtt_protversion version, int16_t packid, uint8_t reason, binary_ctx *props, size_t *lens);
/// <summary>
/// 发布释放（QoS 2，第二步）
/// </summary>
/// <param name="version">mqtt_protversion</param>
/// <param name="packid">报文标识符</param>
/// <param name="reason">原因码(MQTT5.0)</param>
/// <param name="props">属性(MQTT5.0):
/// REASON_STR(0x1F) USER_PROPERTY(0x26)
/// </param>
/// <param name="lens">组包后的数据长度</param>
/// <returns>char * 数据包</returns>
char *mqtt_pack_pubrel(mqtt_protversion version, int16_t packid, uint8_t reason, binary_ctx *props, size_t *lens);
/// <summary>
/// 发布完成（QoS 2，第三步）
/// </summary>
/// <param name="version">mqtt_protversion</param>
/// <param name="packid">报文标识符</param>
/// <param name="reason">原因码(MQTT5.0)</param>
/// <param name="props">属性(MQTT5.0):
/// REASON_STR(0x1F) USER_PROPERTY(0x26)
/// </param>
/// <param name="lens">组包后的数据长度</param>
/// <returns>char * 数据包</returns>
char *mqtt_pack_pubcomp(mqtt_protversion version, int16_t packid, uint8_t reason, binary_ctx *props, size_t *lens);
/// <summary>
/// 订阅请求
/// </summary>
/// <param name="version">mqtt_protversion</param>
/// <param name="packid">报文标识符</param>
/// <param name="topics">mqtt_topics_subscribe函数打包的数据</param>
/// <param name="props">属性(MQTT5.0):
/// SUBSCRIPTION_ID(0x0B) USER_PROPERTY(0x26)
/// </param>
/// <param name="lens">组包后的数据长度</param>
/// <returns>char * 数据包</returns>
char *mqtt_pack_subscribe(mqtt_protversion version, int16_t packid, binary_ctx *topics, binary_ctx *props, size_t *lens);
/// <summary>
/// 订阅确认
/// </summary>
/// <param name="version">mqtt_protversion</param>
/// <param name="packid">报文标识符</param>
/// <param name="reasons">原因码列表 0x00:授予QoS等级0  0x01:授予QoS等级1 0x02:授予QoS等级2</param>
/// <param name="rslens">原因码列表长度</param>
/// <param name="props">属性(MQTT5.0):
/// REASON_STR(0x1F) USER_PROPERTY(0x26)
/// </param>
/// <param name="lens">组包后的数据长度</param>
/// <returns>char * 数据包</returns>
char *mqtt_pack_suback(mqtt_protversion version, int16_t packid, uint8_t *reasons, size_t rslens, binary_ctx *props, size_t *lens);
/// <summary>
/// 取消订阅请求
/// </summary>
/// <param name="version">mqtt_protversion</param>
/// <param name="packid">报文标识符</param>
/// <param name="topics">mqtt_topics_unsubscribe函数打包的数据</param>
/// <param name="props">属性(MQTT5.0):
/// SUBSCRIPTION_ID(0x0B) USER_PROPERTY(0x26)
/// </param>
/// <param name="lens">组包后的数据长度</param>
/// <returns>char * 数据包</returns>
char *mqtt_pack_unsubscribe(mqtt_protversion version, int16_t packid, binary_ctx *topics, binary_ctx *props, size_t *lens);
/// <summary>
/// 取消订阅确认
/// </summary>
/// <param name="version">mqtt_protversion</param>
/// <param name="packid">报文标识符</param>
/// <param name="reasons">原因码列表(MQTT5.0)</param>
/// <param name="rslens">原因码列表长度</param>
/// <param name="props">属性(MQTT5.0):
/// REASON_STR(0x1F) USER_PROPERTY(0x26)
/// </param>
/// <param name="lens">组包后的数据长度</param>
/// <returns>char * 数据包</returns>
char *mqtt_pack_unsuback(mqtt_protversion version, int16_t packid, uint8_t *reasons, size_t rslens, binary_ctx *props, size_t *lens);
/// <summary>
/// PING
/// </summary>
/// <param name="version">mqtt_protversion</param>
/// <param name="lens">组包后的数据长度</param>
/// <returns>char * 数据包</returns>
char *mqtt_pack_ping(mqtt_protversion version, size_t *lens);
/// <summary>
/// PONG
/// </summary>
/// <param name="version">mqtt_protversion</param>
/// <param name="lens">组包后的数据长度</param>
/// <returns>char * 数据包</returns>
char *mqtt_pack_pong(mqtt_protversion version, size_t *lens);
/// <summary>
/// 断开通知
/// </summary>
/// <param name="version">mqtt_protversion</param>
/// <param name="reason">原因码(MQTT5.0) 0x00:正常断开 0x04:包含遗嘱消息的断开</param>
/// <param name="props">属性(MQTT5.0):
/// SESSION_EXPIRY(0x11) REASON_STR(0x1F) USER_PROPERTY(0x26) SERVER_REFERENCE(0x1C)
/// </param>
/// <param name="lens">组包后的数据长度</param>
/// <returns>char * 数据包</returns>
char *mqtt_pack_disconnect(mqtt_protversion version, uint8_t reason, binary_ctx *props, size_t *lens);
/// <summary>
/// 认证交换(MQTT5.0)
/// </summary>
/// <param name="version">mqtt_protversion</param>
/// <param name="reason">原因码:
/// 编码  名称     发送端         说明
/// 0x00  成功     服务端         认证成功。
/// 0x18  继续认证 客户端或服务端 继续下一步认证。
/// 0x19  重新认证 客户端         开始重新认证。
///</param>
/// <param name="props">属性:
/// AUTH_METHOD(0x15) AUTH_DATA(0x16) REASON_STR(0x1F) USER_PROPERTY(0x26)
/// </param>
/// <param name="lens">组包后的数据长度</param>
/// <returns>char * 数据包</returns>
char *mqtt_pack_auth(mqtt_protversion version, uint8_t reason, binary_ctx *props, size_t *lens);

#endif//MQTT_PACK_H_
