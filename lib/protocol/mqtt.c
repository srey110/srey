#include "protocol/mqtt.h"
#include "protocol/protos.h"

typedef enum parse_status {
    INIT = 0,
    AUTH,
    COMMAND
}parse_status;
typedef struct mqtt_pack_fixhead {
    uint8_t flags;//标志
    int32_t roccupy;//剩余长度占用字节
    mqtt_proto proto;//控制报文的类型
    size_t remaining_lens;//剩余长度
}mqtt_pack_fixhead;
typedef struct mqtt_pack_ctx {
    //固定头
    mqtt_pack_fixhead fixhead;
}mqtt_pack_ctx;

void _mqtt_pkfree(void *data) {
    if (NULL == data) {
        return;
    }
    mqtt_pack_ctx *pack = (mqtt_pack_ctx *)data;
    FREE(pack);
}
void _mqtt_udfree(ud_cxt *ud) {
    
}
static int32_t _remaining_lens(buffer_ctx *buf, size_t blens, size_t *rlens) {
    *rlens = 0;
    char c;
    int32_t mcl = 1;
    for (size_t i = 1; i < blens && i <= 4; i++) {
        c = buffer_at(buf, i);
        *rlens += (c & 127) * mcl;
        if (!BIT_CHECK(c, 128)) {
            return (int32_t)i;
        }
        mcl *= 128;
    }
    return ERR_FAILED;
}
//客户端到服务端  客户端请求连接服务端
static int32_t _mqtt_connect(mqtt_pack_ctx *pack, int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    if (client) {
        BIT_SET(*status, PROTO_ERROR);
        return ERR_FAILED;
    }
    return ERR_OK;
}
//服务端到客户端  连接报文确认
static int32_t _mqtt_connack(mqtt_pack_ctx *pack, int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    if (!client) {
        BIT_SET(*status, PROTO_ERROR);
        return ERR_FAILED;
    }
    return ERR_OK;
}
static int32_t _mqtt_status_init(mqtt_pack_ctx *pack, int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    int32_t rtn = ERR_FAILED;
    switch (pack->fixhead.proto) {
    case MQTT_CONNECT:
        rtn = _mqtt_connect(pack, client, buf, ud, status);
        break;
    case MQTT_CONNACK:
        rtn = _mqtt_connack(pack, client, buf, ud, status);
        break;
    default:
        BIT_SET(*status, PROTO_ERROR);
        break;
    }
    return rtn;
}
//两个方向都允许  断开连接通知
static int32_t _mqtt_disconnect(mqtt_pack_ctx *pack, int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    return ERR_OK;
}
//两个方向都允许  认证信息交换
static int32_t _mqtt_auth(mqtt_pack_ctx *pack, int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    return ERR_OK;
}
static int32_t _mqtt_status_auth(mqtt_pack_ctx *pack, int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    int32_t rtn = ERR_FAILED;
    switch (pack->fixhead.proto) {
    case MQTT_DISCONNECT:
        rtn = _mqtt_disconnect(pack, client, buf, ud, status);
        break;
    case MQTT_AUTH:
        rtn = _mqtt_auth(pack, client, buf, ud, status);
        break;
    default:
        BIT_SET(*status, PROTO_ERROR);
        break;
    }
    return rtn;
}
//两个方向都允许  发布消息
static int32_t _mqtt_publish(mqtt_pack_ctx *pack, int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    return ERR_OK;
}
//两个方向都允许  QoS 1消息发布收到确认
static int32_t _mqtt_puback(mqtt_pack_ctx *pack, int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    return ERR_OK;
}
//两个方向都允许  发布收到（保证交付第一步）
static int32_t _mqtt_pubrec(mqtt_pack_ctx *pack, int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    return ERR_OK;
}
//两个方向都允许  发布释放（保证交付第二步）
static int32_t _mqtt_pubrel(mqtt_pack_ctx *pack, int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    return ERR_OK;
}
//两个方向都允许  QoS 2消息发布完成（保证交互第三步）
static int32_t _mqtt_pubcomp(mqtt_pack_ctx *pack, int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    return ERR_OK;
}
//客户端到服务端  客户端订阅请求
static int32_t _mqtt_subscribe(mqtt_pack_ctx *pack, int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    if (client) {
        BIT_SET(*status, PROTO_ERROR);
        return ERR_FAILED;
    }
    return ERR_OK;
}
//服务端到客户端  订阅请求报文确认
static int32_t _mqtt_suback(mqtt_pack_ctx *pack, int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    if (!client) {
        BIT_SET(*status, PROTO_ERROR);
        return ERR_FAILED;
    }
    return ERR_OK;
}
//客户端到服务端  客户端取消订阅请求
static int32_t _mqtt_unsubscribe(mqtt_pack_ctx *pack, int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    if (client) {
        BIT_SET(*status, PROTO_ERROR);
        return ERR_FAILED;
    }
    return ERR_OK;
}
//服务端到客户端  取消订阅报文确认
static int32_t _mqtt_unsuback(mqtt_pack_ctx *pack, int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    if (!client) {
        BIT_SET(*status, PROTO_ERROR);
        return ERR_FAILED;
    }
    return ERR_OK;
}
//客户端到服务端  心跳请求
static int32_t _mqtt_ping(mqtt_pack_ctx *pack, int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    if (client) {
        BIT_SET(*status, PROTO_ERROR);
        return ERR_FAILED;
    }
    return ERR_OK;
}
//服务端到客户端  心跳响应
static int32_t _mqtt_pong(mqtt_pack_ctx *pack, int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    if (!client) {
        BIT_SET(*status, PROTO_ERROR);
        return ERR_FAILED;
    }
    return ERR_OK;
}
static int32_t _mqtt_status_command(mqtt_pack_ctx *pack, int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    int32_t rtn = ERR_FAILED;
    switch (pack->fixhead.proto) {
    case MQTT_PUBLISH:
        rtn = _mqtt_publish(pack, client, buf, ud, status);
        break;
    case MQTT_PUBACK:
        rtn = _mqtt_puback(pack, client, buf, ud, status);
        break;
    case MQTT_PUBREC:
        rtn = _mqtt_pubrec(pack, client, buf, ud, status);
        break;
    case MQTT_PUBREL:
        rtn = _mqtt_pubrel(pack, client, buf, ud, status);
        break;
    case MQTT_PUBCOMP:
        rtn = _mqtt_pubcomp(pack, client, buf, ud, status);
        break;
    case MQTT_SUBSCRIBE:
        rtn = _mqtt_subscribe(pack, client, buf, ud, status);
        break;
    case MQTT_SUBACK:
        rtn = _mqtt_suback(pack, client, buf, ud, status);
        break;
    case MQTT_UNSUBSCRIBE:
        rtn = _mqtt_unsubscribe(pack, client, buf, ud, status);
        break;
    case MQTT_UNSUBACK:
        rtn = _mqtt_unsuback(pack, client, buf, ud, status);
        break;
    case MQTT_PINGREQ:
        rtn = _mqtt_ping(pack, client, buf, ud, status);
        break;
    case MQTT_PINGRESP:
        rtn = _mqtt_pong(pack, client, buf, ud, status);
        break;
    case MQTT_DISCONNECT:
        rtn = _mqtt_disconnect(pack, client, buf, ud, status);
        break;
    case MQTT_AUTH:
        rtn = _mqtt_auth(pack, client, buf, ud, status);
        break;
    default:
        BIT_SET(*status, PROTO_ERROR);
        break;
    }
    return rtn;
}
mqtt_pack_ctx *mqtt_unpack(int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    size_t blens = buffer_size(buf);
    if (blens < 2) {//固定头至少2字节
        BIT_SET(*status, PROTO_MOREDATA);
        return NULL;
    }
    size_t remaining_lens;
    int32_t roccupy = _remaining_lens(buf, blens, &remaining_lens);//返回剩余长度占用字节数
    if (ERR_FAILED == roccupy) {
        if (blens >= 5) {//剩余长度最大4个字节
            BIT_SET(*status, PROTO_ERROR);
            return NULL;
        }
        BIT_SET(*status, PROTO_MOREDATA);
        return NULL;
    }
    if (blens < 1 + roccupy + remaining_lens) {
        BIT_SET(*status, PROTO_MOREDATA);
        return NULL;
    }
    mqtt_pack_ctx *pack;
    CALLOC(pack, 1, sizeof(mqtt_pack_ctx));
    pack->fixhead.roccupy = roccupy;
    pack->fixhead.remaining_lens = remaining_lens;
    char val = buffer_at(buf, 0);
    pack->fixhead.proto = (mqtt_proto)(val >> 4);
    pack->fixhead.flags = (uint8_t)(val & 0x0F);
    int32_t rtn = ERR_FAILED;
    switch (ud->status) {
    case INIT:
        rtn = _mqtt_status_init(pack, client, buf, ud, status);
        break;
    case AUTH:
        rtn = _mqtt_status_auth(pack, client, buf, ud, status);
        break;
    case COMMAND:
        rtn = _mqtt_status_command(pack, client, buf, ud, status);
        break;
    }
    if (ERR_OK != rtn) {
        _mqtt_pkfree(pack);
        return NULL;
    }
    return pack;
}
