#ifndef MQTT_STRUCT_H_
#define MQTT_STRUCT_H_

#include "event/event.h"
#include "protocol/mqtt/mqtt_macro.h"

typedef struct mqtt_fixhead {//固定头
    uint8_t flags;           //固定头标志位
    mqtt_prot prot;          //控制报文的类型
    size_t remaining_lens;   //剩余长度（不含固定头）
}mqtt_fixhead;
typedef struct mqtt_propertie {//属性
    mqtt_prop_flag flag;//标识符
    int64_t nval;//数字值
    size_t slens;//sval长度
    size_t flens;//fval长度
    char *sval;//第二值(用户属性时为value)
    char fval[];//第一值(用户属性时为key值)
}mqtt_propertie;
typedef struct mqtt_connect_varhead {
    int8_t version;//协议版本
    int8_t cleanstart;//新开始  连接是一个新的会话还是一个已存在的会话的延续 1:丢弃任何已存在的会话，并开始一个新的会话 0:恢复会话
    int8_t willflag;//遗嘱标志 
    int8_t willqos;//遗嘱服务质量
    int8_t willretain;//遗嘱保留标志
    int8_t passwordflag;//密码标志
    int8_t userflag;//用户名标志
    uint16_t keepalive;//保持连接 秒
    array_ctx *properties;//属性  5.0 元素 mqtt_propertie *
}mqtt_connect_varhead;
typedef struct mqtt_connect_payload {
    char *clientid;//客户标识符
    array_ctx *properties;//遗嘱属性 5.0 元素 mqtt_propertie *
    char *willtopic;//遗嘱主题
    char *willpayload;//遗嘱载荷
    size_t wplens;//遗嘱载荷长度
    char *user;//用户名
    char *password;//密码
    size_t pslens;//密码长度
}mqtt_connect_payload;
typedef struct mqtt_connack_varhead {
    int8_t sesspresent;//会话存在
    uint8_t reason;//连接原因码
    array_ctx *properties;//属性  5.0 元素 mqtt_propertie *
}mqtt_connack_varhead;
//PUBLISH报文的预期响应 QoS 0	无响应; QoS 1	PUBACK报文; QoS 2	PUBREC报文
typedef struct mqtt_publish_varhead {
    int8_t dup;//0:表示这是客户端或服务端第一次请求发送这个PUBLISH报文。1:表示这可能是一个早前报文请求的重发。QoS为0的消息，DUP标志必须设置为0
    int8_t qos;//服务质量等级 0 最多分发一次 1 至少分发一次 2 只分发一次
    int8_t retain;//保留标志 1:服务端必须存储此消息，并用其替换此话题下任何已存在的消息
    int16_t packid;//报文标识符  只有当QoS等级是1或2时才有
    char *topic;//主题名
    array_ctx *properties;//属性 5.0 元素 mqtt_propertie *
}mqtt_publish_varhead;
typedef struct mqtt_publish_payload {
    int32_t lens;//长度
    char content[];//内容
}mqtt_publish_payload;
// PUBACK / PUBREC / PUBREL / PUBCOMP 共用可变报头
typedef struct mqtt_pubackrel_varhead {
    uint8_t reason;//原因码 5.0
    int16_t packid;//报文标识符
    array_ctx *properties;//属性 5.0 元素 mqtt_propertie *
}mqtt_pubackrel_varhead;
// SUBSCRIBE / SUBACK / UNSUBSCRIBE / UNSUBACK 共用可变报头
typedef struct mqtt_subreqresp_varhead {
    int16_t packid;//报文标识符
    array_ctx *properties;//属性 5.0 元素 mqtt_propertie *
}mqtt_subreqresp_varhead;
typedef struct subscribe_option {
    int8_t qos;//最大服务质量
    int8_t nl;//非本地(MQTT5.0)
    int8_t rap;//发布保留(MQTT5.0)
    int8_t retain;//保留操作(MQTT5.0)
    char *topic;
}subscribe_option;
typedef struct mqtt_subscribe_payload {
    array_ctx subop;            // 元素 subscribe_option *
}mqtt_subscribe_payload;
typedef struct mqtt_unsubscribe_payload {
    array_ctx topics;//主题过滤器（元素 char *）
}mqtt_unsubscribe_payload;
// SUBACK / UNSUBACK 共用载荷（rlens + reasons[]，柔性数组，5.0）
typedef struct mqtt_reasonlist_payload {
    int32_t rlens;//原因码长度
    uint8_t reasons[];//原因码列表
}mqtt_reasonlist_payload;
// DISCONNECT / AUTH 共用可变报头（{reason, properties}，5.0）
typedef struct mqtt_reason_varhead {//5.0
    uint8_t reason;//原因码 5.0
    array_ctx *properties;//属性 5.0 元素 mqtt_propertie *
}mqtt_reason_varhead;
typedef struct mqtt_pack_ctx {
    int8_t version;      //协议版本
    //固定报头
    mqtt_fixhead fixhead;
    //可变报头
    void *varhead;
    //载荷
    void *payload;
}mqtt_pack_ctx;
typedef struct mqtt_ctx {
    int8_t version;//协议版本 mqtt_protversion
}mqtt_ctx;

// 释放属性数组及其元素
void _mqtt_propertie_free(array_ctx *properties);
// 释放 CONNECT 可变报头
void _mqtt_connect_varhead_free(void *data);
// 释放 CONNECT 载荷
void _mqtt_connect_payload_free(void *data);
// 释放 CONNACK 可变报头
void _mqtt_connack_varhead_free(void *data);
// 释放 PUBLISH 可变报头
void _mqtt_publish_varhead_free(void *data);
// 释放 PUBLISH 载荷
void _mqtt_publish_payload_free(void *data);
// 释放 PUBACK / PUBREC / PUBREL / PUBCOMP 共用可变报头
void _mqtt_pubackrel_varhead_free(void *data);
// 释放 SUBSCRIBE / SUBACK / UNSUBSCRIBE / UNSUBACK 共用可变报头
void _mqtt_subreqresp_varhead_free(void *data);
// 释放 SUBSCRIBE 载荷（含主题列表）
void _mqtt_subscribe_payload_free(void *data);
// 释放 UNSUBSCRIBE 载荷（含主题列表）
void _mqtt_unsubscribe_payload_free(void *data);
// 释放 SUBACK / UNSUBACK 共用载荷
void _mqtt_reasonlist_payload_free(void *data);
// 释放 DISCONNECT / AUTH 共用可变报头
void _mqtt_reason_varhead_free(void *data);

#endif//MQTT_STRUCT_H_
