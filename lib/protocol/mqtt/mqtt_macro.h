#ifndef MQTT_MACRO_H_
#define MQTT_MACRO_H_

#include "base/macro.h"

typedef enum mqtt_protversion {//版本
    MQTT_311 = 0x04,
    MQTT_50 = 0x05
}mqtt_protversion;
typedef enum mqtt_prop_flag {
    PAYLOAD_FORMAT           = 0x01,//载荷格式说明	字节	PUBLISH, Will Properties
    MSG_EXPIRY               = 0x02,//消息过期时间	四字节整数	PUBLISH, Will Properties
    CONTENT_TYPE             = 0x03,//内容类型	UTF-8编码字符串	PUBLISH, Will Properties
    RESP_TOPIC               = 0x08,//响应主题	UTF-8编码字符串	PUBLISH, Will Properties
    CORRELATION_DATA         = 0x09,//相关数据	二进制数据	PUBLISH, Will Properties
    SUBSCRIPTION_ID          = 0x0B,//定义标识符	变长字节整数	PUBLISH, SUBSCRIBE
    SESSION_EXPIRY           = 0x11,//会话过期间隔	四字节整数	CONNECT, CONNACK, DISCONNECT
    CLIENT_ID                = 0x12,//分配客户标识符	UTF-8编码字符串	CONNACK
    SERVER_KEEPALIVE         = 0x13,//服务端保活时间	双字节整数	CONNACK
    AUTH_METHOD              = 0x15,//认证方法	UTF-8编码字符串	CONNECT, CONNACK, AUTH
    AUTH_DATA                = 0x16,//认证数据	二进制数据	CONNECT, CONNACK, AUTH
    REQPROBLEM_INFO          = 0x17,//请求问题信息	字节	CONNECT
    WILLDELAY_INTERVAL       = 0x18,//遗嘱延时间隔	四字节整数	Will Properties
    REQRESP_INFO             = 0x19,//请求响应信息	字节	CONNECT
    RESP_INFO                = 0x1A,//请求信息	UTF-8编码字符串	CONNACK
    SERVER_REFERENCE         = 0x1C,//服务端参考	UTF-8编码字符串	CONNACK, DISCONNECT
    REASON_STR               = 0x1F,//原因字符串	UTF-8编码字符串	CONNACK, PUBACK, PUBREC, PUBREL, PUBCOMP, SUBACK, UNSUBACK, DISCONNECT, AUTH
    RECEIVE_MAXIMUM          = 0x21,//接收最大数量	双字节整数	CONNECT, CONNACK
    TOPICALIAS_MAXIMUM       = 0x22,//主题别名最大长度	双字节整数	CONNECT, CONNACK
    TOPIC_ALIAS              = 0x23,//主题别名	双字节整数	PUBLISH
    MAXIMUM_QOS              = 0x24,//最大QoS	字节	CONNACK
    RETAIN_AVAILABLE         = 0x25,//保留属性可用性	字节	CONNACK
    USER_PROPERTY            = 0x26,//用户属性	UTF-8字符串对	CONNECT, CONNACK, PUBLISH, Will Properties, PUBACK, PUBREC, PUBREL, PUBCOMP, SUBSCRIBE, SUBACK, UNSUBSCRIBE, UNSUBACK, DISCONNECT, AUTH
    MAXIMUM_PACKETSIZE       = 0x27,//最大报文长度	四字节整数	CONNECT, CONNACK
    WILDCARD_SUBSCRIPTION    = 0x28,//通配符订阅可用性	字节	CONNACK
    SUBSCRIPTIONID_AVAILABLE = 0x29,//订阅标识符可用性	字节	CONNACK
    SHARED_SUBSCRIPTION      = 0x2A //共享订阅可用性	字节	CONNACK
}mqtt_prop_flag;
typedef enum mqtt_prot {
    MQTT_RESERVED = 0x00, //禁止	保留
    MQTT_CONNECT,         //客户端到服务端  客户端请求连接服务端
    MQTT_CONNACK,         //服务端到客户端  连接报文确认
    MQTT_PUBLISH,         //两个方向都允许  发布消息
    MQTT_PUBACK,          //两个方向都允许  QoS 1消息发布收到确认
    MQTT_PUBREC,          //两个方向都允许  发布收到（保证交付第一步）
    MQTT_PUBREL,          //两个方向都允许  发布释放（保证交付第二步）
    MQTT_PUBCOMP,         //两个方向都允许  QoS 2消息发布完成（保证交互第三步）
    MQTT_SUBSCRIBE,       //客户端到服务端  客户端订阅请求
    MQTT_SUBACK,          //服务端到客户端  订阅请求报文确认
    MQTT_UNSUBSCRIBE,     //客户端到服务端  客户端取消订阅请求
    MQTT_UNSUBACK,        //服务端到客户端  取消订阅报文确认
    MQTT_PINGREQ,         //客户端到服务端  心跳请求
    MQTT_PINGRESP,        //服务端到客户端  心跳响应
    MQTT_DISCONNECT,      //两个方向都允许  断开连接通知
    MQTT_AUTH,            //两个方向都允许  认证信息交换
}mqtt_prot;

#endif//MQTT_MACRO_H_