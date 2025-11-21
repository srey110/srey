#ifndef MQTT_STRUCT_H_
#define MQTT_STRUCT_H_

#include "event/event.h"
#include "protocol/mqtt/mqtt_macro.h"

typedef struct mqtt_fixhead {//固定头
    uint8_t flags;//标志
    mqtt_proto proto;//控制报文的类型
    size_t remaining_lens;
}mqtt_fixhead;
typedef struct mqtt_propertie {//属性
    mqtt_propt_flag flag;//标识符
    int32_t nval;//数字值
    size_t slens;//sval长度
    size_t flens;//fval长度
    char *sval;//第二值(用户属性时为value)
    char fval[0];//第一值(用户属性时为key值)
}mqtt_propertie;
ARRAY_DECL(mqtt_propertie *, arr_propertie);
typedef struct mqtt_connect_varhead {
    int8_t version;//协议版本
    int8_t cleanstart;//新开始  连接是一个新的会话还是一个已存在的会话的延续 1:丢弃任何已存在的会话，并开始一个新的会话 0:恢复会话
    int8_t willflag;//遗嘱标志 
    int8_t willqos;//遗嘱服务质量
    int8_t willretain;//遗嘱保留标志
    int8_t passwordflag;//密码标志
    int8_t userflag;//用户名标志
    uint16_t keepalive;//保持连接 秒
    arr_propertie_ctx *properties;//属性  5.0
}mqtt_connect_varhead;
typedef struct mqtt_connect_payload {
    char *clientid;//客户标识符
    arr_propertie_ctx *properties;//遗嘱属性 5.0
    char *willtopic;//遗嘱主题
    char *willpayload;//遗嘱载荷
    size_t wplens;//遗嘱载荷长度
    char *user;//用户名
    char *password;//密码
    size_t pslens;//密码长度
}mqtt_connect_payload;
typedef struct mqtt_connack_varhead {
    int8_t sesspresent;//会话存在
    int8_t reason;//连接原因码
    arr_propertie_ctx *properties;//属性  5.0
}mqtt_connack_varhead;
//PUBLISH报文的预期响应 QoS 0	无响应; QoS 1	PUBACK报文; QoS 2	PUBREC报文
typedef struct mqtt_publish_varhead {
    int8_t dup;//0:表示这是客户端或服务端第一次请求发送这个PUBLISH报文。1:表示这可能是一个早前报文请求的重发。QoS为0的消息，DUP标志必须设置为0
    int8_t qos;//服务质量等级 0 最多分发一次 1 至少分发一次 2 只分发一次
    int8_t retain;//保留标志 1:服务端必须存储此消息，并用其替换此话题下任何已存在的消息
    int16_t packid;//报文标识符  只有当QoS等级是1或2时才有
    char *topic;//主题名
    arr_propertie_ctx *properties;//属性 5.0
}mqtt_publish_varhead;
typedef struct mqtt_publish_payload {
    int32_t lens;//长度
    char content[0];//内容
}mqtt_publish_payload;
typedef struct mqtt_puback_varhead {
    int8_t reason;//原因码  5.0
    int16_t packid;//报文标识符
    arr_propertie_ctx *properties;//属性 5.0
}mqtt_puback_varhead;
typedef struct mqtt_pubrec_varhead {
    int8_t reason;//原因码  5.0
    int16_t packid;//报文标识符
    arr_propertie_ctx *properties;//属性 5.0
}mqtt_pubrec_varhead;
typedef struct mqtt_pubrel_varhead {
    int8_t reason;//原因码 5.0
    int16_t packid;//报文标识符
    arr_propertie_ctx *properties;//属性 5.0
}mqtt_pubrel_varhead;
typedef struct mqtt_pubcomp_varhead {
    int8_t reason;//原因码 5.0
    int16_t packid;//报文标识符
    arr_propertie_ctx *properties;//属性 5.0
}mqtt_pubcomp_varhead;
typedef struct mqtt_subscribe_varhead {
    int16_t packid;//报文标识符
    arr_propertie_ctx *properties;//属性 5.0
}mqtt_subscribe_varhead;
typedef struct subscribe_option {
    int8_t subop;
    char *topic;
}subscribe_option;
ARRAY_DECL(subscribe_option *, arr_subscribe_option);
typedef struct mqtt_subscribe_payload {
    arr_subscribe_option_ctx subop;
}mqtt_subscribe_payload;
typedef struct mqtt_suback_varhead {
    int16_t packid;//报文标识符
    arr_propertie_ctx *properties;//属性 5.0
}mqtt_suback_varhead;
typedef struct mqtt_suback_payload {
    int32_t rlens;//原因码长度
    char reasons[0];//原因码列表
}mqtt_suback_payload;
typedef struct mqtt_unsubscribe_varhead {
    int16_t packid;//报文标识符
    arr_propertie_ctx *properties;//属性 5.0
}mqtt_unsubscribe_varhead;
typedef struct mqtt_unsubscribe_payload {
    arr_ptr_ctx topics;//主题过滤器
}mqtt_unsubscribe_payload;
typedef struct mqtt_unsuback_varhead {
    int16_t packid;//报文标识符
    arr_propertie_ctx *properties;//属性 5.0
}mqtt_unsuback_varhead;
typedef struct mqtt_unsuback_payload { //5.0
    int32_t rlens;//原因码长度
    char reasons[0];//原因码列表
}mqtt_unsuback_payload;
typedef struct mqtt_disconnect_varhead {//5.0
    int8_t reason;//断开原因码 5.0
    arr_propertie_ctx *properties;//属性 5.0
}mqtt_disconnect_varhead;
typedef struct mqtt_auth_varhead {//5.0
    int8_t reason;//认证原因码
    arr_propertie_ctx *properties;//属性 5.0
}mqtt_auth_varhead;
typedef struct mqtt_pack_ctx {
    int8_t version;//协议版本
    //固定报头
    mqtt_fixhead fixhead;
    //可变报头
    void *varhead;
    //载荷
    void *payload;
}mqtt_pack_ctx;
typedef struct mqtt_ctx {
    int8_t version;//协议版本 mqtt_protoversion
}mqtt_ctx;

void _mqtt_propertie_free(arr_propertie_ctx *properties);
void _mqtt_connect_varhead_free(void *data);
void _mqtt_connect_payload_free(void *data);
void _mqtt_connack_varhead_free(void *data);
void _mqtt_publish_varhead_free(void *data);
void _mqtt_publish_payload_free(void *data);
void _mqtt_puback_varhead_free(void *data);
void _mqtt_pubrec_varhead_free(void *data);
void _mqtt_pubrel_varhead_free(void *data);
void _mqtt_pubcomp_varhead_free(void *data);
void _mqtt_subscribe_varhead_free(void *data);
void _mqtt_subscribe_payload_free(void *data);
void _mqtt_suback_varhead_free(void *data);
void _mqtt_suback_payload_free(void *data);
void _mqtt_unsubscribe_varhead_free(void *data);
void _mqtt_unsubscribe_payload_free(void *data);
void _mqtt_unsuback_varhead_free(void *data);
void _mqtt_unsuback_payload_free(void *data);
void _mqtt_disconnect_varhead_free(void *data);
void _mqtt_auth_varhead_free(void *data);

#endif//MQTT_STRUCT_H_
