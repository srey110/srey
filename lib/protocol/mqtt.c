#include "protocol/mqtt.h"
#include "protocol/protos.h"
#include "utils/utils.h"

typedef enum parse_status {
    INIT = 0,
    AUTH,
    COMMAND
}parse_status;
typedef enum mqtt_protoversion {//版本
    MQTT3 = 0x04,
    MQTT5 = 0x05
}mqtt_protoversion;
typedef struct mqtt_pack_propertie {//属性
    mqtt_propertie flag;//标识符
    int32_t nval;//数字值
    size_t slens;//sval长度
    size_t flens;//fval长度
    char *sval;//第二值(用户属性时为value)
    char fval[0];//第一值(用户属性时为key值)
}mqtt_pack_propertie;
ARRAY_DECL(mqtt_pack_propertie *, arr_propertie);
typedef struct mqtt_pack_fixhead {//固定头
    uint8_t flags;//标志
    mqtt_proto proto;//控制报文的类型
}mqtt_pack_fixhead;
typedef struct mqtt_connect_variablehead {
    int8_t version;//协议版本
    int8_t cleanstart;//新开始
    int8_t willflag;//遗嘱标志
    int8_t willqos;//遗嘱服务质量
    int8_t willretain;//遗嘱保留标志
    int8_t passwordflag;//密码标志
    int8_t userflag;//用户名标志
    uint16_t keepalive;//保持连接
    arr_propertie_ctx *properties;//属性
}mqtt_connect_variablehead;
typedef struct mqtt_connect_payload {
    char *clientid;//客户标识符
    arr_propertie_ctx *properties;//遗嘱属性
    char *willtopic;//遗嘱主题
    char *willpayload;//遗嘱载荷
    size_t wplens;//遗嘱载荷长度
    char *user;//用户名
    char *password;//密码
    size_t pslens;//密码长度
}mqtt_connect_payload;
typedef struct mqtt_pack_ctx {
    //固定报头
    mqtt_pack_fixhead fixhead;
    //可变报头
    void *variablehead;
    //载荷
    void *payload;
}mqtt_pack_ctx;

static void _mqtt_propertie_free(arr_propertie_ctx *properties) {
    if (NULL == properties) {
        return;
    }
    mqtt_pack_propertie *pkp;
    for (uint32_t i = 0; i < arr_propertie_size(properties); i++) {
        pkp = *arr_propertie_at(properties, i);
        FREE(pkp->sval);
        FREE(pkp);
    }
    arr_propertie_free(properties);
    FREE(properties);
}
static void _mqtt_connect_variablehead_free(void *data) {
    if (NULL == data) {
        return;
    }
    mqtt_connect_variablehead *vh = (mqtt_connect_variablehead *)data;
    _mqtt_propertie_free(vh->properties);
    FREE(vh);
}
static void _mqtt_connect_payload_free(void *data) {
    if (NULL == data) {
        return;
    }
    mqtt_connect_payload * pl = (mqtt_connect_payload *)data;
    FREE(pl->clientid);
    _mqtt_propertie_free(pl->properties);
    FREE(pl->willtopic);
    FREE(pl->willpayload);
    FREE(pl->user);
    FREE(pl->password);
    FREE(pl);
}
void _mqtt_pkfree(void *data) {
    if (NULL == data) {
        return;
    }
    mqtt_pack_ctx *pack = (mqtt_pack_ctx *)data;
    switch (pack->fixhead.proto) {
    case MQTT_CONNECT:
        _mqtt_connect_variablehead_free(pack->variablehead);
        _mqtt_connect_payload_free(pack->payload);
        break;
    case MQTT_CONNACK:
        break;
    case MQTT_PUBLISH:
        break;
    case MQTT_PUBACK:
        break;
    case MQTT_PUBREC:
        break;
    case MQTT_PUBREL:
        break;
    case MQTT_PUBCOMP:
        break;
    case MQTT_SUBSCRIBE:
        break;
    case MQTT_SUBACK:
        break;
    case MQTT_UNSUBSCRIBE:
        break;
    case MQTT_UNSUBACK:
        break;
    case MQTT_PINGREQ:
        break;
    case MQTT_PINGRESP:
        break;
    case MQTT_DISCONNECT:
        break;
    case MQTT_AUTH:
        break;
    }
    FREE(pack);
}
void _mqtt_udfree(ud_cxt *ud) {
}
static int32_t _variable_lens(buffer_ctx *buf, size_t off, size_t blens, size_t *rlens) {
    *rlens = 0;
    char c;
    int32_t mcl = 1;
    for (size_t i = 0; i < blens - off && i < 4; i++) {
        c = buffer_at(buf, i + off);
        *rlens += (c & 127) * mcl;
        if (!BIT_CHECK(c, 128)) {
            return (int32_t)(i + 1);
        }
        mcl *= 128;
    }
    return ERR_FAILED;
}
static int32_t _mqtt_data_number(buffer_ctx *buf, size_t lens, int32_t *num) {
    char tmp[4];
    ASSERTAB(lens <= sizeof(tmp), "too long.");
    if (lens != buffer_remove(buf, tmp, lens)) {
        return ERR_FAILED;
    }
    *num = (int32_t)unpack_integer(tmp, (int32_t)lens, 0, 0);
    return ERR_OK;
}
static int32_t _mqtt_data_variablenumber(buffer_ctx *buf, int32_t *num) {
    size_t val;
    int32_t occupy = _variable_lens(buf, 0, buffer_size(buf), &val);
    if (ERR_FAILED == occupy) {
        return ERR_FAILED;
    }
    ASSERTAB(occupy == buffer_drain(buf, occupy), "drain buffer failed.");
    *num = (int32_t)val;
    return occupy;
}
static mqtt_pack_propertie *_mqtt_data_string(buffer_ctx *buf, size_t *off) {
    int32_t num;
    if (ERR_OK != _mqtt_data_number(buf, 2, &num)) {
        return NULL;
    }
    (*off) += 2;
    mqtt_pack_propertie *propt;
    CALLOC(propt, 1, sizeof(mqtt_pack_propertie) + num + 1);
    if (num != buffer_remove(buf, propt->fval, num)) {
        FREE(propt);
        return NULL;
    }
    (*off) += num;
    propt->flens = num;
    return propt;
}
static char *_mqtt_data_string2(buffer_ctx *buf, int32_t *num) {
    if (ERR_OK != _mqtt_data_number(buf, 2, num)) {
        return NULL;
    }
    char *rtn;
    MALLOC(rtn, (*num) + 1);
    if ((*num) != buffer_remove(buf, rtn, (*num))) {
        FREE(rtn);
        return NULL;
    }
    rtn[(*num)] = '\0';
    return rtn;
}
static mqtt_pack_propertie *_mqtt_data_kv(buffer_ctx *buf, size_t *off) {
    //key
    int32_t num;
    if (ERR_OK != _mqtt_data_number(buf, 2, &num)) {
        return NULL;
    }
    (*off) += 2;
    mqtt_pack_propertie *propt;
    CALLOC(propt, 1, sizeof(mqtt_pack_propertie) + num + 1);
    if (num != buffer_remove(buf, propt->fval, (size_t)num)) {
        FREE(propt);
        return NULL;
    }
    (*off) += num;
    propt->flens = (size_t)num;
    //value
    if (ERR_OK != _mqtt_data_number(buf, 2, &num)) {
        FREE(propt);
        return NULL;
    }
    (*off) += 2;
    MALLOC(propt->sval, num + 1);
    if (num != buffer_remove(buf, propt->sval, (size_t)num)) {
        FREE(propt->sval);
        FREE(propt);
        return NULL;
    }
    propt->sval[num] = '\0';
    (*off) += num;
    propt->slens = (size_t)num;
    return propt;
}
//属性解析
static arr_propertie_ctx *_mqtt_properties(buffer_ctx *buf, int32_t *status) {
    size_t plens;
    int32_t occupy = _variable_lens(buf, 0, buffer_size(buf), &plens);//属性长度
    if (ERR_FAILED == occupy) {
        BIT_SET(*status, PROTO_ERROR);
        return NULL;
    }
    ASSERTAB(occupy == buffer_drain(buf, occupy), "drain buffer failed.");
    if (0 == plens) {
        return NULL;
    }
    if (plens > buffer_size(buf)) {
        BIT_SET(*status, PROTO_ERROR);
        return NULL;
    }
    char tmp[1];
    int8_t flag;
    int32_t num;
    size_t off;
    mqtt_pack_propertie *propt;
    arr_propertie_ctx *arrpropts;
    MALLOC(arrpropts, sizeof(arr_propertie_ctx));
    arr_propertie_init(arrpropts, 0);
    for (off = 0; off < plens;) {
        if (1 != buffer_remove(buf, tmp, 1)) {
            BIT_SET(*status, PROTO_ERROR);
            _mqtt_propertie_free(arrpropts);
            return NULL;
        }
        off++;
        propt = NULL;
        flag = tmp[0];
        switch (flag) {
        case PAYLOAD_FORMAT://载荷格式说明	字节	PUBLISH, Will Properties
        case REQPROBLEM_INFO://请求问题信息	字节	CONNECT
        case REQRESP_INFO://请求响应信息	字节	CONNECT
        case MAXIMUM_QOS://最大QoS	字节	CONNACK
        case RETAIN_AVAILABLE://保留属性可用性	字节	CONNACK
        case WILDCARD_SUBSCRIPTION://通配符订阅可用性	字节	CONNACK
        case SUBSCRIPTIONID_AVAILABLE://订阅标识符可用性	字节	CONNACK
        case SHARED_SUBSCRIPTION://共享订阅可用性	字节	CONNACK
            if (ERR_OK == _mqtt_data_number(buf, 1, &num)) {
                CALLOC(propt, 1, sizeof(mqtt_pack_propertie));
                propt->nval = num;
                off++;
            }
            break;
        case SERVER_KEEPALIVE://服务端保活时间	双字节整数	CONNACK
        case RECEIVE_MAXIMUM://接收最大数量	双字节整数	CONNECT, CONNACK
        case TOPICALIAS_MAXIMUM://主题别名最大长度	双字节整数	CONNECT, CONNACK
        case TOPIC_ALIAS://主题别名	双字节整数	PUBLISH
            if (ERR_OK == _mqtt_data_number(buf, 2, &num)) {
                CALLOC(propt, 1, sizeof(mqtt_pack_propertie));
                propt->nval = num;
                off += 2;
            }
            break;
        case MSG_EXPIRY://消息过期时间	四字节整数	PUBLISH, Will Properties
        case SESSION_EXPIRY://会话过期间隔	四字节整数	CONNECT, CONNACK, DISCONNECT
        case WILLDELAY_INTERVAL://遗嘱延时间隔	四字节整数	Will Properties
        case MAXIMUM_PACKETSIZE://最大报文长度	四字节整数	CONNECT, CONNACK
            if (ERR_OK == _mqtt_data_number(buf, 4, &num)) {
                CALLOC(propt, 1, sizeof(mqtt_pack_propertie));
                propt->nval = num;
                off += 4;
            }
            break;
        case SUBSCRIPTION_ID://定义标识符	变长字节整数	PUBLISH, SUBSCRIBE
            occupy = _mqtt_data_variablenumber(buf, &num);
            if (ERR_FAILED != occupy) {
                CALLOC(propt, 1, sizeof(mqtt_pack_propertie));
                propt->nval = num;
                off += occupy;
            }
            break;
        case CORRELATION_DATA://相关数据	二进制数据	PUBLISH, Will Properties
        case AUTH_DATA://认证数据	二进制数据	CONNECT, CONNACK, AUTH
            propt = _mqtt_data_string(buf, &off);
            break;
        case CONTENT_TYPE://内容类型	UTF-8编码字符串	PUBLISH, Will Properties
        case RESP_TOPIC://响应主题	UTF-8编码字符串	PUBLISH, Will Properties
        case CLIENT_ID://分配客户标识符	UTF-8编码字符串	CONNACK
        case AUTH_METHOD://认证方法	UTF-8编码字符串	CONNECT, CONNACK, AUTH
        case RESP_INFO://请求信息	UTF-8编码字符串	CONNACK
        case SERVER_REFERENCE://服务端参考	UTF-8编码字符串	CONNACK, DISCONNECT
        case REASON_STR://原因字符串	UTF-8编码字符串	CONNACK, PUBACK, PUBREC, PUBREL, PUBCOMP, SUBACK, UNSUBACK, DISCONNECT, AUTH
            propt = _mqtt_data_string(buf, &off);
            break;
        case USER_PROPERTY://用户属性	UTF-8字符串对	CONNECT, CONNACK, PUBLISH, Will Properties, PUBACK, PUBREC, PUBREL, PUBCOMP, SUBSCRIBE, SUBACK, UNSUBSCRIBE, UNSUBACK, DISCONNECT, AUTH
            propt = _mqtt_data_kv(buf, &off);
            break;
        }
        if (NULL == propt) {
            BIT_SET(*status, PROTO_ERROR);
            _mqtt_propertie_free(arrpropts);
            return NULL;
        }
        propt->flag = flag;
        arr_propertie_push_back(arrpropts, &propt);
    }
    if (off != plens) {
        BIT_SET(*status, PROTO_ERROR);
        _mqtt_propertie_free(arrpropts);
        return NULL;
    }
    return arrpropts;
}
static int32_t _mqtt_check_proto(buffer_ctx *buf) {
    int32_t num;
    if (ERR_OK != _mqtt_data_number(buf, 2, &num)) {//协议名长度
        return ERR_FAILED;
    }
    if (4 != num) {
        return ERR_FAILED;
    }
    char tmp[4];
    if (num != buffer_remove(buf, tmp, num)) {//协议名
        return ERR_FAILED;
    }
    if (0 != _memicmp(tmp, "mqtt", num)) {
        return ERR_FAILED;
    }
    if (1 != buffer_remove(buf, tmp, 1)) {//协议版本
        return ERR_FAILED;
    }
    if (MQTT3 != tmp[0]
        && MQTT5 != tmp[0]) {
        return ERR_FAILED;
    }
    return tmp[0];
}
//客户端到服务端  客户端请求连接服务端
static int32_t _mqtt_connect(mqtt_pack_ctx *pack, int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    if (client) {
        BIT_SET(*status, PROTO_ERROR);
        return ERR_FAILED;
    }
    //可变报头
    mqtt_connect_variablehead *vh;
    MALLOC(vh, sizeof(mqtt_connect_variablehead));
    vh->properties = NULL;
    pack->variablehead = vh;
    vh->version = _mqtt_check_proto(buf);
    if (ERR_FAILED == vh->version) {//协议名检查
        BIT_SET(*status, PROTO_ERROR);
        return ERR_FAILED;
    }
    int32_t num;
    if (ERR_OK != _mqtt_data_number(buf, 1, &num)) {//连接标志
        BIT_SET(*status, PROTO_ERROR);
        return ERR_FAILED;
    }
    char flags = (char)num;
    if (0 != BIT_GETN(flags, 0)) {//保留
        BIT_SET(*status, PROTO_ERROR);
        return ERR_FAILED;
    }
    vh->cleanstart = BIT_GETN(flags, 1);//新开始
    vh->willflag = BIT_GETN(flags, 2);//遗嘱标志
    vh->willqos = BIT_GETN(flags, 3);
    vh->willqos |= (BIT_GETN(flags, 4) << 1);//遗嘱服务质量
    vh->willretain = BIT_GETN(flags, 5);//遗嘱保留标志
    if (0 == vh->willflag && (0 != vh->willqos || 0 != vh->willretain)) {
        BIT_SET(*status, PROTO_ERROR);
        return ERR_FAILED;
    }
    vh->passwordflag = BIT_GETN(flags, 6);//密码标志
    vh->userflag = BIT_GETN(flags, 7);//用户名标志
    if (ERR_OK != _mqtt_data_number(buf, 2, &num)) {//保持连接
        BIT_SET(*status, PROTO_ERROR);
        return ERR_FAILED;
    }
    vh->keepalive = (uint16_t)num;
    if (MQTT5 == vh->version) {
        //属性
        vh->properties = _mqtt_properties(buf, status);
        if (NULL == vh->properties
            && BIT_CHECK(*status, PROTO_ERROR)) {
            return ERR_FAILED;
        }
    }
    //载荷
    mqtt_connect_payload *pl;
    CALLOC(pl, 1, sizeof(mqtt_connect_payload));
    pack->payload = pl;
    pl->clientid = _mqtt_data_string2(buf, &num);//客户标识符
    if (NULL == pl->clientid) {
        BIT_SET(*status, PROTO_ERROR);
        return ERR_FAILED;
    }
    if (vh->willflag) {
        if (MQTT5 == vh->version) {
            pl->properties = _mqtt_properties(buf, status);//遗嘱属性
            if (NULL == pl->properties
                && BIT_CHECK(*status, PROTO_ERROR)) {
                return ERR_FAILED;
            }
        }
        pl->willtopic = _mqtt_data_string2(buf, &num);//遗嘱主题
        if (NULL == pl->willtopic) {
            BIT_SET(*status, PROTO_ERROR);
            return ERR_FAILED;
        }
        pl->willpayload = _mqtt_data_string2(buf, &num);//遗嘱载荷
        if (NULL == pl->willpayload) {
            BIT_SET(*status, PROTO_ERROR);
            return ERR_FAILED;
        }
        pl->wplens = num;
    }
    if (vh->userflag) {
        pl->user = _mqtt_data_string2(buf, &num);
        if (NULL == pl->user) {
            BIT_SET(*status, PROTO_ERROR);
            return ERR_FAILED;
        }
    }
    if (vh->passwordflag) {
        pl->password = _mqtt_data_string2(buf, &num);
        if (NULL == pl->password) {
            BIT_SET(*status, PROTO_ERROR);
            return ERR_FAILED;
        }
        pl->pslens = num;
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
//两个方向都允许  认证信息交换
static int32_t _mqtt_auth(mqtt_pack_ctx *pack, int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    return ERR_OK;
}
//两个方向都允许  断开连接通知
static int32_t _mqtt_disconnect(mqtt_pack_ctx *pack, int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    return ERR_OK;
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
static int32_t _mqtt_commands(mqtt_pack_ctx *pack, int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    int32_t rtn = ERR_FAILED;
    switch (pack->fixhead.proto) {
    case MQTT_CONNECT:
        rtn = _mqtt_connect(pack, client, buf, ud, status);
        break;
    case MQTT_CONNACK:
        rtn = _mqtt_connack(pack, client, buf, ud, status);
        break;
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
    int32_t roccupy = _variable_lens(buf, 1, blens, &remaining_lens);//返回剩余长度占用字节数
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
    char val = buffer_at(buf, 0);
    pack->fixhead.proto = (mqtt_proto)(val >> 4);
    pack->fixhead.flags = (uint8_t)(val & 0x0F);
    ASSERTAB(1 + roccupy == buffer_drain(buf, 1 + roccupy), "drain buffer failed.");
    if (ERR_OK != _mqtt_commands(pack, client, buf, ud, status)) {
        _mqtt_pkfree(pack);
        return NULL;
    }
    return pack;
}
