#ifndef MQTT_H_
#define MQTT_H_

#include "event/event.h"

typedef enum mqtt_proto {
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
}mqtt_proto;

void _mqtt_pkfree(void *data);
void _mqtt_udfree(ud_cxt *ud);
struct mqtt_pack_ctx *mqtt_unpack(int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status);

#endif//MQTT_H_
