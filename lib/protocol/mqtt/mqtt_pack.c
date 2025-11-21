#include "protocol/mqtt/mqtt_pack.h"

static int32_t _mqtt_varlens_encode(uint32_t vlens, char buf[4]) {
    if (vlens >= 0x10000000) {
        return 0;
    }
    uint8_t byte;
    int32_t i = 0;
    do {
        byte = vlens % 0x80;
        vlens /= 0x80;
        if (0 != vlens) {
            BIT_SET(byte, 0x80);
        }
        buf[i++] = (char)byte;
    } while (vlens > 0);
    return i;
}
void mqtt_props_init(binary_ctx *props) {
    binary_init(props, NULL, 0, 0);
}
void mqtt_props_free(binary_ctx *props) {
    FREE(props->data);
}
int32_t mqtt_props_fixnum(binary_ctx *props, mqtt_prop_flag flag, int32_t val) {
    binary_set_int8(props, (int8_t)flag);
    switch (flag) {
    case PAYLOAD_FORMAT://0x01 载荷格式说明	字节	PUBLISH, Will Properties
    case REQPROBLEM_INFO://0x17 请求问题信息	字节	CONNECT
    case REQRESP_INFO://0x19 请求响应信息	字节	CONNECT
    case MAXIMUM_QOS://0x24 最大QoS	字节	CONNACK
    case RETAIN_AVAILABLE://0x25 保留属性可用性	字节	CONNACK
    case WILDCARD_SUBSCRIPTION://0x28 通配符订阅可用性	字节	CONNACK
    case SUBSCRIPTIONID_AVAILABLE://0x29 订阅标识符可用性	字节	CONNACK
    case SHARED_SUBSCRIPTION://0x2A 共享订阅可用性	字节	CONNACK
        binary_set_int8(props, (int8_t)val);
        break;
    case SERVER_KEEPALIVE://0x13 服务端保活时间	双字节整数	CONNACK
    case RECEIVE_MAXIMUM://0x21 接收最大数量	双字节整数	CONNECT, CONNACK
    case TOPICALIAS_MAXIMUM://0x22 主题别名最大长度	双字节整数	CONNECT, CONNACK
    case TOPIC_ALIAS://0x23 主题别名	双字节整数	PUBLISH
        binary_set_integer(props, val, 2, 0);
        break;
    case MSG_EXPIRY://0x02 消息过期时间	四字节整数	PUBLISH, Will Properties
    case SESSION_EXPIRY://0x11 会话过期间隔	四字节整数	CONNECT, CONNACK, DISCONNECT
    case WILLDELAY_INTERVAL://0x18 遗嘱延时间隔	四字节整数	Will Properties
    case MAXIMUM_PACKETSIZE://0x27 最大报文长度	四字节整数	CONNECT, CONNACK
        binary_set_integer(props, val, 4, 0);
        break;
    default:
        return ERR_FAILED;
    }
    return ERR_OK;
}
static int32_t _mqtt_props_varnum(binary_ctx *props, int32_t val) {
    char buf[4];
    int32_t lens = _mqtt_varlens_encode((uint32_t)val, buf);
    if (0 == lens) {
        return ERR_FAILED;
    }
    binary_set_string(props, buf, lens);
    return ERR_OK;
}
int32_t mqtt_props_varnum(binary_ctx *props, mqtt_prop_flag flag, int32_t val) {
    binary_set_int8(props, (int8_t)flag);
    switch (flag) {
    case SUBSCRIPTION_ID://0x0B 定义标识符	变长字节整数	PUBLISH, SUBSCRIBE
        _mqtt_props_varnum(props, val);
        break;
    default:
        return ERR_FAILED;
    }
    return ERR_OK;
}
int32_t mqtt_props_binary(binary_ctx *props, mqtt_prop_flag flag, void *data, int32_t lens) {
    binary_set_int8(props, (int8_t)flag);
    switch (flag) {
    case CORRELATION_DATA://0x09 相关数据	二进制数据	PUBLISH, Will Properties
    case AUTH_DATA://0x16 认证数据	二进制数据	CONNECT, CONNACK, AUTH
    case CONTENT_TYPE://0x03 内容类型	UTF-8编码字符串	PUBLISH, Will Properties
    case RESP_TOPIC://0x08 响应主题	UTF-8编码字符串	PUBLISH, Will Properties
    case CLIENT_ID://0x12 分配客户标识符	UTF-8编码字符串	CONNACK
    case AUTH_METHOD://0x15 认证方法	UTF-8编码字符串	CONNECT, CONNACK, AUTH
    case RESP_INFO://0x1A 请求信息	UTF-8编码字符串	CONNACK
    case SERVER_REFERENCE://0x1C 服务端参考	UTF-8编码字符串	CONNACK, DISCONNECT
    case REASON_STR://0x1F 原因字符串	UTF-8编码字符串	CONNACK, PUBACK, PUBREC, PUBREL, PUBCOMP, SUBACK, UNSUBACK, DISCONNECT, AUTH
        binary_set_integer(props, lens, 2, 0);
        binary_set_string(props, data, lens);
        break;
    default:
        return ERR_FAILED;
    }
    return ERR_OK;
}
int32_t mqtt_props_kv(binary_ctx *props, mqtt_prop_flag flag, void *key, size_t klens, void *val, size_t vlens) {
    binary_set_int8(props, (int8_t)flag);
    switch (flag) {
    case USER_PROPERTY://0x26 用户属性	UTF-8字符串对	CONNECT, CONNACK, PUBLISH, Will Properties, PUBACK, PUBREC, PUBREL, PUBCOMP, SUBSCRIBE, SUBACK, UNSUBSCRIBE, UNSUBACK, DISCONNECT, AUTH
        binary_set_integer(props, klens, 2, 0);
        binary_set_string(props, key, klens);
        binary_set_integer(props, vlens, 2, 0);
        binary_set_string(props, val, vlens);
        break;
    default:
        return ERR_FAILED;
    }
    return ERR_OK;
}
char *mqtt_pack_connack(mqtt_protversion version, int8_t sesspresent, int8_t reason, binary_ctx *props, size_t *lens) {
    int8_t fixhead = (MQTT_CONNACK << 4);//固定报头
    int8_t caflag = 0;//连接确认标志
    if (sesspresent) {
        BIT_SETN(caflag, 0, 1);
    }
    char propvlens[4];
    int32_t pvoccupy = 0;
    uint32_t total = 2;
    if (version >= MQTT_50) {
        if (NULL == props) {
            pvoccupy = _mqtt_varlens_encode(0, propvlens);
        } else {
            total += props->offset;
            pvoccupy = _mqtt_varlens_encode(props->offset, propvlens);
        }
        if (0 == pvoccupy) {
            return NULL;
        }
        total += pvoccupy;
    }
    char rmain[4];
    int32_t roccupy = _mqtt_varlens_encode(total, rmain);
    if (0 == roccupy) {
        return NULL;
    }
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_int8(&bwriter, fixhead);//固定报头
    binary_set_string(&bwriter, rmain, roccupy);//剩余长度
    binary_set_int8(&bwriter, caflag);//连接确认标志
    binary_set_int8(&bwriter, reason);//连接原因码
    if (version >= MQTT_50) {
        binary_set_string(&bwriter, propvlens, pvoccupy);
        if (NULL != props
            && 0 != props->offset) {
            binary_set_string(&bwriter, props->data, props->offset);
        }
    }
    *lens = bwriter.offset;
    return bwriter.data;
}
