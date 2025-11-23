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
void mqtt_topics_subscribe(binary_ctx *topics, mqtt_protversion version, const char *topic,
    int8_t qos, int8_t nl, int8_t rap, int8_t retain) {
    int8_t subop = 0;
    BIT_SETN(subop, 0, (BIT_GETN(qos, 0)));
    BIT_SETN(subop, 1, (BIT_GETN(qos, 1)));//QoS
    if (version >= MQTT_50) {
        BIT_SETN(subop, 2, nl);//NL(No Local)
        BIT_SETN(subop, 3, rap);//RAP(Retain As Published)
        BIT_SETN(subop, 4, (BIT_GETN(retain, 0)));
        BIT_SETN(subop, 5, (BIT_GETN(retain, 1)));//Retain Handling
    }
    size_t tlens = strlen(topic);
    binary_set_integer(topics, tlens, 2, 0);
    binary_set_string(topics, topic, tlens);
    binary_set_int8(topics, subop);
}
void mqtt_topics_unsubscribe(binary_ctx *topics, const char *topic) {
    size_t tlens = strlen(topic);
    binary_set_integer(topics, tlens, 2, 0);
    binary_set_string(topics, topic, tlens);
}
static int32_t _mqtt_props_varlens(mqtt_protversion version, binary_ctx *props, char vlens[4], uint32_t *off) {
    if (version < MQTT_50) {
        return 0;
    }
    int32_t occupy;
    if (NULL == props) {
        occupy = _mqtt_varlens_encode(0, vlens);
    } else {
        (*off) += (uint32_t)props->offset;
        occupy = _mqtt_varlens_encode((uint32_t)props->offset, vlens);
    }
    if (0 == occupy) {
        return ERR_FAILED;
    }
    (*off) += occupy;
    return occupy;
}
char *mqtt_pack_connect(mqtt_protversion version, int8_t cleanstart, int16_t keepalive, const char *clientid,
    const char *user, char *password, size_t pwlens,
    const char *willtopic, char *willpayload, size_t wplens, int8_t willqos, int8_t willretain,
    binary_ctx *connprops, binary_ctx *willprops, size_t *lens) {
    int8_t fixhead = (MQTT_CONNECT << 4);//固定报头
    //连接标志
    size_t ulens = 0;
    if (NULL != user) {
        ulens = strlen(user);
    }
    int8_t userflag = 0 == ulens ? 0 : 1;
    int8_t passwordflag = (NULL == password || 0 == pwlens) ? 0 : 1;
    size_t wtlens = 0;
    if (NULL != willtopic) {
        wtlens = strlen(willtopic);
    }
    int8_t willflag = 0 == wtlens ? 0 : 1;
    int8_t connflags = 0;//连接标志
    BIT_SETN(connflags, 1, cleanstart);//Clean Start
    BIT_SETN(connflags, 2, willflag);//Will Flag
    if (willflag) {
        BIT_SETN(connflags, 3, (BIT_GETN(willqos, 0)));
        BIT_SETN(connflags, 4, (BIT_GETN(willqos, 1)));//Will QoS
        BIT_SETN(connflags, 5, willretain);//Will Retain
    }
    BIT_SETN(connflags, 6, passwordflag);//Password Flag
    BIT_SETN(connflags, 7, userflag);//User Name Flag
    //计算剩余长度
    uint32_t total = 10;//协议名(2+4) + 协议版本(1) + 连接标志(1) + 保持连接(2)
    char cpvlens[4];
    int32_t cpoccupy = _mqtt_props_varlens(version, connprops, cpvlens, &total);
    if (ERR_FAILED == cpoccupy) {
        return NULL;
    }
    size_t cidlens = strlen(clientid);
    total += ((uint32_t)cidlens + 2);//客户标识符
    char wpvlens[4];
    int32_t wpoccupy = 0;
    if (willflag) {
        wpoccupy = _mqtt_props_varlens(version, willprops, wpvlens, &total);
        if (ERR_FAILED == wpoccupy) {
            return NULL;
        }
        total += ((uint32_t)wtlens + 2);//遗嘱主题
        total += ((uint32_t)wplens + 2);//遗嘱载荷
    }
    if (userflag) {
        total += ((uint32_t)ulens + 2);//用户名
    }
    if (passwordflag) {
        total += ((uint32_t)pwlens + 2);//密码
    }
    //编码剩余长度
    char rmain[4];
    int32_t roccupy = _mqtt_varlens_encode(total, rmain);
    if (0 == roccupy) {
        return NULL;
    }
    //打包
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_int8(&bwriter, fixhead);//固定报头
    binary_set_string(&bwriter, rmain, roccupy);//剩余长度
    binary_set_integer(&bwriter, 4, 2, 0);//协议名长度
    binary_set_string(&bwriter, "MQTT", 4);//协议名
    binary_set_int8(&bwriter, version);//协议版本
    binary_set_int8(&bwriter, connflags);//连接标志
    binary_set_integer(&bwriter, keepalive, 2, 0);//保持连接
    if (version >= MQTT_50) {
        binary_set_string(&bwriter, cpvlens, cpoccupy);//属性长度
        if (NULL != connprops
            && 0 != connprops->offset) {
            binary_set_string(&bwriter, connprops->data, connprops->offset);//属性
        }
    }
    binary_set_integer(&bwriter, cidlens, 2, 0);
    binary_set_string(&bwriter, clientid, cidlens);//客户标识符
    if (willflag) {
        if (version >= MQTT_50) {
            binary_set_string(&bwriter, wpvlens, wpoccupy);//属性长度
            if (NULL != willprops
                && 0 != willprops->offset) {
                binary_set_string(&bwriter, willprops->data, willprops->offset);//属性
            }
        }
        binary_set_integer(&bwriter, wtlens, 2, 0);
        binary_set_string(&bwriter, willtopic, wtlens);//遗嘱主题
        binary_set_integer(&bwriter, wplens, 2, 0);
        binary_set_string(&bwriter, willpayload, wplens);//遗嘱载荷
    }
    if (userflag) {
        binary_set_integer(&bwriter, ulens, 2, 0);
        binary_set_string(&bwriter, user, ulens);//用户名
    }
    if (passwordflag) {
        binary_set_integer(&bwriter, pwlens, 2, 0);
        binary_set_string(&bwriter, password, pwlens);//密码 
    }
    *lens = bwriter.offset;
    return bwriter.data;
}
char *mqtt_pack_connack(mqtt_protversion version, int8_t sesspresent, uint8_t reason, binary_ctx *props, size_t *lens) {
    int8_t fixhead = (MQTT_CONNACK << 4);//固定报头
    int8_t caflag = 0;//连接确认标志
    if (sesspresent) {
        BIT_SETN(caflag, 0, 1);
    }
    //计算剩余长度
    uint32_t total = 2;//连接确认标志(1) + 连接原因码(1)
    char pvlens[4];
    int32_t pvoccupy = _mqtt_props_varlens(version, props, pvlens, &total);
    if (ERR_FAILED == pvoccupy) {
        return NULL;
    }
    //编码剩余长度
    char rmain[4];
    int32_t roccupy = _mqtt_varlens_encode(total, rmain);
    if (0 == roccupy) {
        return NULL;
    }
    //打包
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_int8(&bwriter, fixhead);//固定报头
    binary_set_string(&bwriter, rmain, roccupy);//剩余长度
    binary_set_int8(&bwriter, caflag);//连接确认标志
    binary_set_uint8(&bwriter, reason);//连接原因码
    if (version >= MQTT_50) {//属性
        binary_set_string(&bwriter, pvlens, pvoccupy);//属性长度
        if (NULL != props
            && 0 != props->offset) {
            binary_set_string(&bwriter, props->data, props->offset);//属性
        }
    }
    *lens = bwriter.offset;
    return bwriter.data;
}
char *mqtt_pack_publish(mqtt_protversion version, int8_t retain, int8_t qos, int8_t dup,
    const char *topic, int16_t packid, char *payload, size_t pllens, binary_ctx *props, size_t *lens) {
    int8_t fixhead = (MQTT_PUBLISH << 4);//固定报头
    //固定报头标志
    BIT_SETN(fixhead, 0, retain);//保留标志
    BIT_SETN(fixhead, 1, (BIT_GETN(qos, 0)));
    BIT_SETN(fixhead, 2, (BIT_GETN(qos, 1)));//服务质量等级
    BIT_SETN(fixhead, 3, dup);//重发标志
    //计算剩余长度
    size_t tlens = strlen(topic);
    uint32_t total = 2 + (uint32_t)tlens;//主题名
    if (1 == qos
        || 2 == qos) {//只有当QoS等级是1或2时，报文标识符字段才能出现在报文中
        total += 2;//报文标识符(2)
    }
    char pvlens[4];
    int32_t pvoccupy = _mqtt_props_varlens(version, props, pvlens, &total);
    if (ERR_FAILED == pvoccupy) {
        return NULL;
    }
    if (NULL != payload) {
        total += (uint32_t)pllens;
    }
    //编码剩余长度
    char rmain[4];
    int32_t roccupy = _mqtt_varlens_encode(total, rmain);
    if (0 == roccupy) {
        return NULL;
    }
    //打包
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_int8(&bwriter, fixhead);//固定报头
    binary_set_string(&bwriter, rmain, roccupy);//剩余长度
    binary_set_integer(&bwriter, tlens, 2, 0);
    binary_set_string(&bwriter, topic, tlens);//主题名
    if (1 == qos
        || 2 == qos) {
        binary_set_integer(&bwriter, packid, 2, 0);//报文标识符
    }
    if (version >= MQTT_50) {//属性
        binary_set_string(&bwriter, pvlens, pvoccupy);//属性长度
        if (NULL != props
            && 0 != props->offset) {
            binary_set_string(&bwriter, props->data, props->offset);//属性
        }
    }
    if (NULL != payload
        && 0 != pllens) {
        binary_set_string(&bwriter, payload, pllens);//载荷
    }
    *lens = bwriter.offset;
    return bwriter.data;
}
char *mqtt_pack_puback(mqtt_protversion version, int16_t packid, uint8_t reason, binary_ctx *props, size_t *lens) {
    int8_t fixhead = (MQTT_PUBACK << 4);//固定报头
    //计算剩余长度
    uint32_t total;
    char pvlens[4];
    int32_t pvoccupy = 0;
    if (version < MQTT_50) {
        total = 2;//报文标识符(2)
    } else {
        if (0x00 == reason
            && (NULL == props || 0 == props->offset)) {
            total = 2;//报文标识符(2)
        } else {
            total = 2 + 1;//报文标识符(2) + 原因码(MQTT_50 1)
            if (NULL != props
                && 0 != props->offset) {
                pvoccupy = _mqtt_props_varlens(version, props, pvlens, &total);
                if (ERR_FAILED == pvoccupy) {
                    return NULL;
                }
            }
        }
    }
    //编码剩余长度
    char rmain[4];
    int32_t roccupy = _mqtt_varlens_encode(total, rmain);
    if (0 == roccupy) {
        return NULL;
    }
    //打包
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_int8(&bwriter, fixhead);//固定报头
    binary_set_string(&bwriter, rmain, roccupy);//剩余长度
    binary_set_integer(&bwriter, packid, 2, 0);//报文标识符
    if (2 == total) {
        *lens = bwriter.offset;
        return bwriter.data;
    }
    if (total >= 3) {
        binary_set_uint8(&bwriter, reason);//原因码
    }
    if (total >= 4) {
        binary_set_string(&bwriter, pvlens, pvoccupy);//属性长度
        if (NULL != props
            && 0 != props->offset) {
            binary_set_string(&bwriter, props->data, props->offset);//属性
        }
    }
    *lens = bwriter.offset;
    return bwriter.data;
} 
char *mqtt_pack_pubrec(mqtt_protversion version, int16_t packid, uint8_t reason, binary_ctx *props, size_t *lens) {
    int8_t fixhead = (MQTT_PUBREC << 4);//固定报头
    //计算剩余长度
    uint32_t total;
    char pvlens[4];
    int32_t pvoccupy = 0;
    if (version < MQTT_50) {
        total = 2;//报文标识符(2)
    } else {
        if (0x00 == reason
            && (NULL == props || 0 == props->offset)) {
            total = 2;//报文标识符(2)
        } else {
            total = 2 + 1;//报文标识符(2) + 原因码(MQTT_50 1)
            if (NULL != props
                && 0 != props->offset) {
                pvoccupy = _mqtt_props_varlens(version, props, pvlens, &total);
                if (ERR_FAILED == pvoccupy) {
                    return NULL;
                }
            }
        }
    }
    //编码剩余长度
    char rmain[4];
    int32_t roccupy = _mqtt_varlens_encode(total, rmain);
    if (0 == roccupy) {
        return NULL;
    }
    //打包
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_int8(&bwriter, fixhead);//固定报头
    binary_set_string(&bwriter, rmain, roccupy);//剩余长度
    binary_set_integer(&bwriter, packid, 2, 0);//报文标识符
    if (2 == total) {
        *lens = bwriter.offset;
        return bwriter.data;
    }
    if (total >= 3) {
        binary_set_uint8(&bwriter, reason);//原因码
    }
    if (total >= 4) {
        binary_set_string(&bwriter, pvlens, pvoccupy);//属性长度
        if (NULL != props
            && 0 != props->offset) {
            binary_set_string(&bwriter, props->data, props->offset);//属性
        }
    }
    *lens = bwriter.offset;
    return bwriter.data;
}
char *mqtt_pack_pubrel(mqtt_protversion version, int16_t packid, uint8_t reason, binary_ctx *props, size_t *lens) {
    int8_t fixhead = (MQTT_PUBREL << 4);//固定报头
    BIT_SETN(fixhead, 1, 1);//第3，2，1，0位是保留位，必须被设置为0，0，1，0
    //计算剩余长度
    uint32_t total;
    char pvlens[4];
    int32_t pvoccupy = 0;
    if (version < MQTT_50) {
        total = 2;//报文标识符(2)
    } else {
        if (0x00 == reason
            && (NULL == props || 0 == props->offset)) {
            total = 2;//报文标识符(2)
        } else {
            total = 2 + 1;//报文标识符(2) + 原因码(MQTT_50 1)
            if (NULL != props
                && 0 != props->offset) {
                pvoccupy = _mqtt_props_varlens(version, props, pvlens, &total);
                if (ERR_FAILED == pvoccupy) {
                    return NULL;
                }
            }
        }
    }
    //编码剩余长度
    char rmain[4];
    int32_t roccupy = _mqtt_varlens_encode(total, rmain);
    if (0 == roccupy) {
        return NULL;
    }
    //打包
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_int8(&bwriter, fixhead);//固定报头
    binary_set_string(&bwriter, rmain, roccupy);//剩余长度
    binary_set_integer(&bwriter, packid, 2, 0);//报文标识符
    if (2 == total) {
        *lens = bwriter.offset;
        return bwriter.data;
    }
    if (total >= 3) {
        binary_set_uint8(&bwriter, reason);//原因码
    }
    if (total >= 4) {
        binary_set_string(&bwriter, pvlens, pvoccupy);//属性长度
        if (NULL != props
            && 0 != props->offset) {
            binary_set_string(&bwriter, props->data, props->offset);//属性
        }
    }
    *lens = bwriter.offset;
    return bwriter.data;
}
char *mqtt_pack_pubcomp(mqtt_protversion version, int16_t packid, uint8_t reason, binary_ctx *props, size_t *lens) {
    int8_t fixhead = (MQTT_PUBCOMP << 4);//固定报头
    //计算剩余长度
    uint32_t total;
    char pvlens[4];
    int32_t pvoccupy = 0;
    if (version < MQTT_50) {
        total = 2;//报文标识符(2)
    } else {
        if (0x00 == reason
            && (NULL == props || 0 == props->offset)) {
            total = 2;//报文标识符(2)
        } else {
            total = 2 + 1;//报文标识符(2) + 原因码(MQTT_50 1)
            if (NULL != props
                && 0 != props->offset) {
                pvoccupy = _mqtt_props_varlens(version, props, pvlens, &total);
                if (ERR_FAILED == pvoccupy) {
                    return NULL;
                }
            }
        }
    }
    //编码剩余长度
    char rmain[4];
    int32_t roccupy = _mqtt_varlens_encode(total, rmain);
    if (0 == roccupy) {
        return NULL;
    }
    //打包
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_int8(&bwriter, fixhead);//固定报头
    binary_set_string(&bwriter, rmain, roccupy);//剩余长度
    binary_set_integer(&bwriter, packid, 2, 0);//报文标识符
    if (2 == total) {
        *lens = bwriter.offset;
        return bwriter.data;
    }
    if (total >= 3) {
        binary_set_uint8(&bwriter, reason);//原因码
    }
    if (total >= 4) {
        binary_set_string(&bwriter, pvlens, pvoccupy);//属性长度
        if (NULL != props
            && 0 != props->offset) {
            binary_set_string(&bwriter, props->data, props->offset);//属性
        }
    }
    *lens = bwriter.offset;
    return bwriter.data;
}
char *mqtt_pack_subscribe(mqtt_protversion version, int16_t packid, binary_ctx *topics, binary_ctx *props, size_t *lens) {
    int8_t fixhead = (MQTT_SUBSCRIBE << 4);//固定报头
    BIT_SETN(fixhead, 1, 1);//第3，2，1，0位是保留位，必须被设置为0，0，1，0
    //计算剩余长度
    uint32_t total = 2;//报文标识符(2)
    char pvlens[4];
    int32_t pvoccupy = _mqtt_props_varlens(version, props, pvlens, &total);
    if (ERR_FAILED == pvoccupy) {
        return NULL;
    }
    total += (uint32_t)topics->offset;
    //编码剩余长度
    char rmain[4];
    int32_t roccupy = _mqtt_varlens_encode(total, rmain);
    if (0 == roccupy) {
        return NULL;
    }
    //打包
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_int8(&bwriter, fixhead);//固定报头
    binary_set_string(&bwriter, rmain, roccupy);//剩余长度
    binary_set_integer(&bwriter, packid, 2, 0);//报文标识符
    if (version >= MQTT_50) {
        binary_set_string(&bwriter, pvlens, pvoccupy);//属性长度
        if (NULL != props
            && 0 != props->offset) {
            binary_set_string(&bwriter, props->data, props->offset);//属性
        }
    }
    binary_set_string(&bwriter, topics->data, topics->offset);
    *lens = bwriter.offset;
    return bwriter.data;
}
char *mqtt_pack_suback(mqtt_protversion version, int16_t packid, uint8_t *reasons, size_t rslens, binary_ctx *props, size_t *lens) {
    int8_t fixhead = (MQTT_SUBACK << 4);//固定报头
    //计算剩余长度
    uint32_t total = 2;//报文标识符(2)
    char pvlens[4];
    int32_t pvoccupy = _mqtt_props_varlens(version, props, pvlens, &total);
    if (ERR_FAILED == pvoccupy) {
        return NULL;
    }
    total += (uint32_t)rslens;
    //编码剩余长度
    char rmain[4];
    int32_t roccupy = _mqtt_varlens_encode(total, rmain);
    if (0 == roccupy) {
        return NULL;
    }
    //打包
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_int8(&bwriter, fixhead);//固定报头
    binary_set_string(&bwriter, rmain, roccupy);//剩余长度
    binary_set_integer(&bwriter, packid, 2, 0);//报文标识符
    if (version >= MQTT_50) {
        binary_set_string(&bwriter, pvlens, pvoccupy);//属性长度
        if (NULL != props
            && 0 != props->offset) {
            binary_set_string(&bwriter, props->data, props->offset);//属性
        }
    }
    binary_set_string(&bwriter, (const char *)reasons, rslens);
    *lens = bwriter.offset;
    return bwriter.data;
}
char *mqtt_pack_unsubscribe(mqtt_protversion version, int16_t packid, binary_ctx *topics, binary_ctx *props, size_t *lens) {
    int8_t fixhead = (MQTT_UNSUBSCRIBE << 4);//固定报头
    BIT_SETN(fixhead, 1, 1);//第3，2，1，0位是保留位，必须被设置为0，0，1，0
    //计算剩余长度
    uint32_t total = 2;//报文标识符(2)
    char pvlens[4];
    int32_t pvoccupy = _mqtt_props_varlens(version, props, pvlens, &total);
    if (ERR_FAILED == pvoccupy) {
        return NULL;
    }
    total += (uint32_t)topics->offset;
    //编码剩余长度
    char rmain[4];
    int32_t roccupy = _mqtt_varlens_encode(total, rmain);
    if (0 == roccupy) {
        return NULL;
    }
    //打包
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_int8(&bwriter, fixhead);//固定报头
    binary_set_string(&bwriter, rmain, roccupy);//剩余长度
    binary_set_integer(&bwriter, packid, 2, 0);//报文标识符
    if (version >= MQTT_50) {
        binary_set_string(&bwriter, pvlens, pvoccupy);//属性长度
        if (NULL != props
            && 0 != props->offset) {
            binary_set_string(&bwriter, props->data, props->offset);//属性
        }
    }
    binary_set_string(&bwriter, topics->data, topics->offset);
    *lens = bwriter.offset;
    return bwriter.data;
}
char *mqtt_pack_unsuback(mqtt_protversion version, int16_t packid, uint8_t *reasons, size_t rslens, binary_ctx *props, size_t *lens) {
    int8_t fixhead = (MQTT_UNSUBACK << 4);//固定报头
    //计算剩余长度
    uint32_t total = 2;//报文标识符(2)
    char pvlens[4];
    int32_t pvoccupy = _mqtt_props_varlens(version, props, pvlens, &total);
    if (ERR_FAILED == pvoccupy) {
        return NULL;
    }
    if (version >= MQTT_50) {
        total += (uint32_t)rslens;
    }
    //编码剩余长度
    char rmain[4];
    int32_t roccupy = _mqtt_varlens_encode(total, rmain);
    if (0 == roccupy) {
        return NULL;
    }
    //打包
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_int8(&bwriter, fixhead);//固定报头
    binary_set_string(&bwriter, rmain, roccupy);//剩余长度
    binary_set_integer(&bwriter, packid, 2, 0);//报文标识符
    if (version >= MQTT_50) {
        binary_set_string(&bwriter, pvlens, pvoccupy);//属性长度
        if (NULL != props
            && 0 != props->offset) {
            binary_set_string(&bwriter, props->data, props->offset);//属性
        }
        binary_set_string(&bwriter, (const char *)reasons, rslens);
    }
    *lens = bwriter.offset;
    return bwriter.data;
}
char *mqtt_pack_ping(mqtt_protversion version, size_t *lens) {
    int8_t fixhead = (MQTT_PINGREQ << 4);//固定报头
    //编码剩余长度
    char rmain[4];
    int32_t roccupy = _mqtt_varlens_encode(0, rmain);
    if (0 == roccupy) {
        return NULL;
    }
    //打包
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_int8(&bwriter, fixhead);//固定报头
    binary_set_string(&bwriter, rmain, roccupy);//剩余长度
    *lens = bwriter.offset;
    return bwriter.data;
}
char *mqtt_pack_pong(mqtt_protversion version, size_t *lens) {
    int8_t fixhead = (MQTT_PINGRESP << 4);//固定报头
    //编码剩余长度
    char rmain[4];
    int32_t roccupy = _mqtt_varlens_encode(0, rmain);
    if (0 == roccupy) {
        return NULL;
    }
    //打包
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_int8(&bwriter, fixhead);//固定报头
    binary_set_string(&bwriter, rmain, roccupy);//剩余长度
    *lens = bwriter.offset;
    return bwriter.data;
}
char *mqtt_pack_disconnect(mqtt_protversion version, uint8_t reason, binary_ctx *props, size_t *lens) {
    int8_t fixhead = (MQTT_DISCONNECT << 4);//固定报头
    //计算剩余长度
    uint32_t total;
    char pvlens[4];
    int32_t pvoccupy = 0;
    if (version < MQTT_50) {
        total = 0;
    } else {
        if (0x00 == reason
            && (NULL == props || 0 == props->offset)) {
            total = 0;
        } else {
            total = 1;
            if (NULL != props
                && 0 != props->offset) {
                pvoccupy = _mqtt_props_varlens(version, props, pvlens, &total);
                if (ERR_FAILED == pvoccupy) {
                    return NULL;
                }
            }
        }
    }
    //编码剩余长度
    char rmain[4];
    int32_t roccupy = _mqtt_varlens_encode(total, rmain);
    if (0 == roccupy) {
        return NULL;
    }
    //打包
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_int8(&bwriter, fixhead);//固定报头
    binary_set_string(&bwriter, rmain, roccupy);//剩余长度
    if (0 == total) {
        *lens = bwriter.offset;
        return bwriter.data;
    }
    if (total >= 1) {
        binary_set_uint8(&bwriter, reason);//原因码
    }
    if (total >= 2) {
        binary_set_string(&bwriter, pvlens, pvoccupy);//属性长度
        if (NULL != props
            && 0 != props->offset) {
            binary_set_string(&bwriter, props->data, props->offset);//属性
        }
    }
    *lens = bwriter.offset;
    return bwriter.data;
}
char *mqtt_pack_auth(mqtt_protversion version, uint8_t reason, binary_ctx *props, size_t *lens) {
    if (version < MQTT_50) {
        return NULL;
    }
    int8_t fixhead = (MQTT_AUTH << 4);//固定报头
    //计算剩余长度
    uint32_t total;
    char pvlens[4];
    int32_t pvoccupy = 0;
    //如果原因码为0x00（成功）并且没有属性字段，则可以省略原因码和属性长度。这种情况下，AUTH报文剩余长度为0。
    if (0x00 == reason
        && (NULL == props || 0 == props->offset)) {
        total = 0;
    } else {
        total = 1;//原因码(1)
        pvoccupy = _mqtt_props_varlens(version, props, pvlens, &total);
        if (ERR_FAILED == pvoccupy) {
            return NULL;
        }
    }
    //编码剩余长度
    char rmain[4];
    int32_t roccupy = _mqtt_varlens_encode(total, rmain);
    if (0 == roccupy) {
        return NULL;
    }
    //打包
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_int8(&bwriter, fixhead);//固定报头
    binary_set_string(&bwriter, rmain, roccupy);//剩余长度
    if (0 != total) {
        binary_set_uint8(&bwriter, reason);//原因码
        binary_set_string(&bwriter, pvlens, pvoccupy);//属性长度
        if (NULL != props
            && 0 != props->offset) {
            binary_set_string(&bwriter, props->data, props->offset);//属性
        }
    }
    *lens = bwriter.offset;
    return bwriter.data;
}
