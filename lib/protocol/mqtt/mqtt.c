#include "protocol/mqtt/mqtt.h"
#include "protocol/prots.h"
#include "utils/utils.h"

typedef enum parse_status {
    INIT = 0,
    COMMAND
}parse_status;

void _mqtt_pkfree(void *data) {
    if (NULL == data) {
        return;
    }
    mqtt_pack_ctx *pack = (mqtt_pack_ctx *)data;
    switch (pack->fixhead.prot) {
    case MQTT_CONNECT:
        _mqtt_connect_varhead_free(pack->varhead);
        _mqtt_connect_payload_free(pack->payload);
        break;
    case MQTT_CONNACK:
        _mqtt_connack_varhead_free(pack->varhead);
        break;
    case MQTT_PUBLISH:
        _mqtt_publish_varhead_free(pack->varhead);
        _mqtt_publish_payload_free(pack->payload);
        break;
    case MQTT_PUBACK:
        _mqtt_puback_varhead_free(pack->varhead);
        break;
    case MQTT_PUBREC:
        _mqtt_pubrec_varhead_free(pack->varhead);
        break;
    case MQTT_PUBREL:
        _mqtt_pubrel_varhead_free(pack->varhead);
        break;
    case MQTT_PUBCOMP:
        _mqtt_pubcomp_varhead_free(pack->varhead);
        break;
    case MQTT_SUBSCRIBE:
        _mqtt_subscribe_varhead_free(pack->varhead);
        _mqtt_subscribe_payload_free(pack->payload);
        break;
    case MQTT_SUBACK:
        _mqtt_suback_varhead_free(pack->varhead);
        _mqtt_suback_payload_free(pack->payload);
        break;
    case MQTT_UNSUBSCRIBE:
        _mqtt_unsubscribe_varhead_free(pack->varhead);
        _mqtt_unsubscribe_payload_free(pack->payload);
        break;
    case MQTT_UNSUBACK:
        _mqtt_unsuback_varhead_free(pack->varhead);
        _mqtt_unsuback_payload_free(pack->payload);
        break;
    case MQTT_PINGREQ:
        break;
    case MQTT_PINGRESP:
        break;
    case MQTT_DISCONNECT:
        _mqtt_disconnect_varhead_free(pack->varhead);
        break;
    case MQTT_AUTH:
        _mqtt_auth_varhead_free(pack->varhead);
        break;
    default:
        break;
    }
    FREE(pack);
}
void _mqtt_udfree(ud_cxt *ud) {
    if (NULL == ud->extra) {
        return;
    }
    mqtt_ctx *mq = (mqtt_ctx *)ud->extra;
    FREE(mq);
    ud->extra = NULL;
}
static int32_t _mqtt_varlens_decode(buffer_ctx *buf, size_t off, size_t blens, size_t *rlens) {
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
static int32_t _mqtt_data_fixnum(buffer_ctx *buf, size_t lens, int32_t *num) {
    char tmp[4];
    ASSERTAB(lens <= sizeof(tmp), "too long.");
    if (lens != buffer_remove(buf, tmp, lens)) {
        return ERR_FAILED;
    }
    if (1 == lens) {
        *num = tmp[0];
        return ERR_OK;
    }
    *num = (int32_t)unpack_integer(tmp, (int32_t)lens, 0, 0);
    return ERR_OK;
}
static int32_t _mqtt_data_varnum(buffer_ctx *buf, int32_t *num) {
    size_t val;
    int32_t occupy = _mqtt_varlens_decode(buf, 0, buffer_size(buf), &val);
    if (ERR_FAILED == occupy) {
        return ERR_FAILED;
    }
    ASSERTAB(occupy == buffer_drain(buf, occupy), "drain buffer failed.");
    *num = (int32_t)val;
    return occupy;
}
static mqtt_propertie *_mqtt_data_string(buffer_ctx *buf, size_t *off) {
    int32_t num;
    if (ERR_OK != _mqtt_data_fixnum(buf, 2, &num)) {
        return NULL;
    }
    (*off) += 2;
    mqtt_propertie *propt;
    CALLOC(propt, 1, sizeof(mqtt_propertie) + num + 1);
    if (num != buffer_remove(buf, propt->fval, num)) {
        FREE(propt);
        return NULL;
    }
    (*off) += num;
    propt->flens = num;
    return propt;
}
static char *_mqtt_data_string2(buffer_ctx *buf, int32_t *num) {
    if (ERR_OK != _mqtt_data_fixnum(buf, 2, num)) {
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
static mqtt_propertie *_mqtt_data_kv(buffer_ctx *buf, size_t *off) {
    //key
    int32_t num;
    if (ERR_OK != _mqtt_data_fixnum(buf, 2, &num)) {
        return NULL;
    }
    (*off) += 2;
    mqtt_propertie *propt;
    CALLOC(propt, 1, sizeof(mqtt_propertie) + num + 1);
    if (num != buffer_remove(buf, propt->fval, (size_t)num)) {
        FREE(propt);
        return NULL;
    }
    (*off) += num;
    propt->flens = num;
    //value
    if (ERR_OK != _mqtt_data_fixnum(buf, 2, &num)) {
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
    propt->slens = num;
    return propt;
}
//属性解析
static arr_propertie_ctx *_mqtt_properties(buffer_ctx *buf, int32_t *status, int32_t *total) {
    int32_t plens;
    int32_t occupy = _mqtt_data_varnum(buf, &plens);//属性长度
    if (ERR_FAILED == occupy 
        || plens > buffer_size(buf)) {
        BIT_SET(*status, PROT_ERROR);
        return NULL;
    }
    if (NULL != total) {
        *total = occupy + plens;
    }
    if (0 == plens) {
        return NULL;
    }
    int32_t num;
    size_t off;
    mqtt_prop_flag flag;
    mqtt_propertie *propt;
    arr_propertie_ctx *arrpropts;
    MALLOC(arrpropts, sizeof(arr_propertie_ctx));
    arr_propertie_init(arrpropts, 0);
    for (off = 0; off < plens;) {
        if (ERR_OK != _mqtt_data_fixnum(buf, 1, &num)) {
            BIT_SET(*status, PROT_ERROR);
            _mqtt_propertie_free(arrpropts);
            return NULL;
        }
        off++;
        propt = NULL;
        flag = num;
        switch (flag) {
        case PAYLOAD_FORMAT://0x01 载荷格式说明	字节	PUBLISH, Will Properties
        case REQPROBLEM_INFO://0x17 请求问题信息	字节	CONNECT
        case REQRESP_INFO://0x19 请求响应信息	字节	CONNECT
        case MAXIMUM_QOS://0x24 最大QoS	字节	CONNACK
        case RETAIN_AVAILABLE://0x25 保留属性可用性	字节	CONNACK
        case WILDCARD_SUBSCRIPTION://0x28 通配符订阅可用性	字节	CONNACK
        case SUBSCRIPTIONID_AVAILABLE://0x29 订阅标识符可用性	字节	CONNACK
        case SHARED_SUBSCRIPTION://0x2A 共享订阅可用性	字节	CONNACK
            if (ERR_OK == _mqtt_data_fixnum(buf, 1, &num)) {
                CALLOC(propt, 1, sizeof(mqtt_propertie));
                propt->nval = num;
                off++;
            }
            break;
        case SERVER_KEEPALIVE://0x13 服务端保活时间	双字节整数	CONNACK
        case RECEIVE_MAXIMUM://0x21 接收最大数量	双字节整数	CONNECT, CONNACK
        case TOPICALIAS_MAXIMUM://0x22 主题别名最大长度	双字节整数	CONNECT, CONNACK
        case TOPIC_ALIAS://0x23 主题别名	双字节整数	PUBLISH
            if (ERR_OK == _mqtt_data_fixnum(buf, 2, &num)) {
                CALLOC(propt, 1, sizeof(mqtt_propertie));
                propt->nval = num;
                off += 2;
            }
            break;
        case MSG_EXPIRY://0x02 消息过期时间	四字节整数	PUBLISH, Will Properties
        case SESSION_EXPIRY://0x11 会话过期间隔	四字节整数	CONNECT, CONNACK, DISCONNECT
        case WILLDELAY_INTERVAL://0x18 遗嘱延时间隔	四字节整数	Will Properties
        case MAXIMUM_PACKETSIZE://0x27 最大报文长度	四字节整数	CONNECT, CONNACK
            if (ERR_OK == _mqtt_data_fixnum(buf, 4, &num)) {
                CALLOC(propt, 1, sizeof(mqtt_propertie));
                propt->nval = num;
                off += 4;
            }
            break;
        case SUBSCRIPTION_ID://0x0B 定义标识符	变长字节整数	PUBLISH, SUBSCRIBE
            occupy = _mqtt_data_varnum(buf, &num);
            if (ERR_FAILED != occupy) {
                CALLOC(propt, 1, sizeof(mqtt_propertie));
                propt->nval = num;
                off += occupy;
            }
            break;
        case CORRELATION_DATA://0x09 相关数据	二进制数据	PUBLISH, Will Properties
        case AUTH_DATA://0x16 认证数据	二进制数据	CONNECT, CONNACK, AUTH
        case CONTENT_TYPE://0x03 内容类型	UTF-8编码字符串	PUBLISH, Will Properties
        case RESP_TOPIC://0x08 响应主题	UTF-8编码字符串	PUBLISH, Will Properties
        case CLIENT_ID://0x12 分配客户标识符	UTF-8编码字符串	CONNACK
        case AUTH_METHOD://0x15 认证方法	UTF-8编码字符串	CONNECT, CONNACK, AUTH
        case RESP_INFO://0x1A 请求信息	UTF-8编码字符串	CONNACK
        case SERVER_REFERENCE://0x1C 服务端参考	UTF-8编码字符串	CONNACK, DISCONNECT
        case REASON_STR://0x1F 原因字符串	UTF-8编码字符串	CONNACK, PUBACK, PUBREC, PUBREL, PUBCOMP, SUBACK, UNSUBACK, DISCONNECT, AUTH
            propt = _mqtt_data_string(buf, &off);
            break;
        case USER_PROPERTY://0x26 用户属性	UTF-8字符串对	CONNECT, CONNACK, PUBLISH, Will Properties, PUBACK, PUBREC, PUBREL, PUBCOMP, SUBSCRIBE, SUBACK, UNSUBSCRIBE, UNSUBACK, DISCONNECT, AUTH
            propt = _mqtt_data_kv(buf, &off);
            break;
        }
        if (NULL == propt) {
            BIT_SET(*status, PROT_ERROR);
            _mqtt_propertie_free(arrpropts);
            return NULL;
        }
        propt->flag = flag;
        arr_propertie_push_back(arrpropts, &propt);
    }
    if (off != plens) {
        BIT_SET(*status, PROT_ERROR);
        _mqtt_propertie_free(arrpropts);
        return NULL;
    }
    return arrpropts;
}
static int32_t _mqtt_check_prot(buffer_ctx *buf) {
    int32_t num;
    if (ERR_OK != _mqtt_data_fixnum(buf, 2, &num)) {//协议名长度
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
    if (1 != buffer_remove(buf, tmp, 1)) {//协议级别
        return ERR_FAILED;
    }
    if (MQTT_311 != tmp[0]
        && MQTT_50 != tmp[0]) {
        return ERR_FAILED;
    }
    return tmp[0];
}
//客户端到服务端  客户端请求连接服务端
static int32_t _mqtt_connect(mqtt_pack_ctx *pack, int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    if (client 
        || 0 != pack->fixhead.flags) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    //可变报头 协议名（Protocol Name），协议级别（Protocol Level），连接标志（Connect Flags），保持连接（Keep Alive）,
    //属性（Properties MQTT_50）
    mqtt_connect_varhead *vh;
    MALLOC(vh, sizeof(mqtt_connect_varhead));
    vh->properties = NULL;
    pack->varhead = vh;
    vh->version = _mqtt_check_prot(buf);
    if (ERR_FAILED == vh->version) {//协议名 协议级别 检查
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    pack->version = vh->version;
    int32_t num;
    if (ERR_OK != _mqtt_data_fixnum(buf, 1, &num)) {//连接标志
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    if (0 != BIT_GETN(num, 0)) {//保留标志位
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    vh->cleanstart = BIT_GETN(num, 1);//新开始
    vh->willflag = BIT_GETN(num, 2);//遗嘱标志
    vh->willqos = BIT_GETN(num, 3);
    vh->willqos |= (BIT_GETN(num, 4) << 1);//遗嘱服务质量
    vh->willretain = BIT_GETN(num, 5);//遗嘱保留标志
    if (0 == vh->willflag 
        && (0 != vh->willqos || 0 != vh->willretain)) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    vh->passwordflag = BIT_GETN(num, 6);//密码标志
    vh->userflag = BIT_GETN(num, 7);//用户名标志
    if (ERR_OK != _mqtt_data_fixnum(buf, 2, &num)) {//保持连接
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    vh->keepalive = (uint16_t)num;
    if (vh->version >= MQTT_50) {
        vh->properties = _mqtt_properties(buf, status, NULL);//属性
        if (NULL == vh->properties
            && BIT_CHECK(*status, PROT_ERROR)) {
            return ERR_FAILED;
        }
    }
    //载荷 客户标识符（Client Identifier）、遗嘱属性（Will Properties MQTT_50）、遗嘱主题（Will Topic）、遗嘱载荷（Will Payload）、
    //用户名（User Name）、密码（Password）
    mqtt_connect_payload *pl;
    CALLOC(pl, 1, sizeof(mqtt_connect_payload));
    pack->payload = pl;
    pl->clientid = _mqtt_data_string2(buf, &num);//客户标识符
    if (NULL == pl->clientid) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    if (vh->willflag) {
        if (vh->version >= MQTT_50) {
            pl->properties = _mqtt_properties(buf, status, NULL);//属性
            if (NULL == pl->properties
                && BIT_CHECK(*status, PROT_ERROR)) {
                return ERR_FAILED;
            }
        }
        pl->willtopic = _mqtt_data_string2(buf, &num);//遗嘱主题
        if (NULL == pl->willtopic) {
            BIT_SET(*status, PROT_ERROR);
            return ERR_FAILED;
        }
        pl->willpayload = _mqtt_data_string2(buf, &num);//遗嘱载荷
        if (NULL == pl->willpayload) {
            BIT_SET(*status, PROT_ERROR);
            return ERR_FAILED;
        }
        pl->wplens = num;
    }
    if (vh->userflag) {
        pl->user = _mqtt_data_string2(buf, &num);
        if (NULL == pl->user) {
            BIT_SET(*status, PROT_ERROR);
            return ERR_FAILED;
        }
    }
    if (vh->passwordflag) {
        pl->password = _mqtt_data_string2(buf, &num);
        if (NULL == pl->password) {
            BIT_SET(*status, PROT_ERROR);
            return ERR_FAILED;
        }
        pl->pslens = num;
    }
    mqtt_ctx *mq;
    CALLOC(mq, 1, sizeof(mqtt_ctx));
    mq->version = vh->version;
    ud->extra = mq;
    ud->status = COMMAND;
    return ERR_OK;
}
//服务端到客户端  连接报文确认
static int32_t _mqtt_connack(mqtt_pack_ctx *pack, int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    if (!client 
        || 0 != pack->fixhead.flags) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    //可变报头 连接确认标志（Connect Acknowledge Flags），连接原因码（Reason Code），属性（Properties MQTT_50）
    int32_t num;
    if (ERR_OK != _mqtt_data_fixnum(buf, 1, &num)) {//连接确认标志
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    if (0 != (num >> 1)) {//位7-1是保留位且必须设置为0
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    mqtt_connack_varhead *vh;
    CALLOC(vh, 1, sizeof(mqtt_connack_varhead));
    pack->varhead = vh;
    vh->sesspresent = BIT_GETN(num, 0);//会话存在
    if (ERR_OK != _mqtt_data_fixnum(buf, 1, &num)) {//连接原因码
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    vh->reason = (uint8_t)num;
    pack->version = ((mqtt_ctx *)ud->extra)->version;
    if (pack->version >= MQTT_50) {
        vh->properties = _mqtt_properties(buf, status, NULL);//属性
        if (NULL == vh->properties
            && BIT_CHECK(*status, PROT_ERROR)) {
            return ERR_FAILED;
        }
    }
    if (0x00 == vh->reason) {
        ud->status = COMMAND;
    }
    return ERR_OK;
}
//两个方向都允许  发布消息
static int32_t _mqtt_publish(mqtt_pack_ctx *pack, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    //可变报头 主题名（Topic Name），报文标识符（Packet Identifier），属性（Properties MQTT_50）
    int32_t num;
    char *topic = _mqtt_data_string2(buf, &num);//主题名
    if (NULL == topic) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    int32_t off = (2 + num);//主题名(2 + 主题名长度)
    mqtt_publish_varhead *vh;
    CALLOC(vh, 1, sizeof(mqtt_publish_varhead));
    pack->varhead = vh;
    vh->topic = topic;
    //解析固定报头标志
    vh->retain = BIT_GETN(pack->fixhead.flags, 0);
    vh->qos = BIT_GETN(pack->fixhead.flags, 1);
    vh->qos |= (BIT_GETN(pack->fixhead.flags, 2) << 1);
    vh->dup = BIT_GETN(pack->fixhead.flags, 3);
    if (1 == vh->qos 
        || 2 == vh->qos) {//只有当QoS等级是1或2时，报文标识符字段才能出现在报文中
        if (ERR_OK != _mqtt_data_fixnum(buf, 2, &num)) {//报文标识符
            BIT_SET(*status, PROT_ERROR);
            return ERR_FAILED;
        }
        off += 2;
        vh->packid = (int16_t)num;
    }
    pack->version = ((mqtt_ctx *)ud->extra)->version;
    if (pack->version >= MQTT_50) {
        vh->properties = _mqtt_properties(buf, status, &num);//属性
        if (NULL == vh->properties
            && BIT_CHECK(*status, PROT_ERROR)) {
            return ERR_FAILED;
        }
        off += num;
    }
    //载荷
    int32_t remain = (int32_t)pack->fixhead.remaining_lens - off;
    if (remain < 0) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    mqtt_publish_payload *pl;
    CALLOC(pl, 1, sizeof(mqtt_publish_payload) + remain + 1);
    pack->payload = pl;
    if (0 == remain) {
        return ERR_OK;
    }
    if (remain != buffer_remove(buf, pl->content, remain)) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    return ERR_OK;
}
//两个方向都允许  QoS 1消息发布收到确认
static int32_t _mqtt_puback(mqtt_pack_ctx *pack, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    if (0 != pack->fixhead.flags) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    //可变报头 报文标识符，[原因码(MQTT_50)，属性(MQTT_50)]
    int32_t num;
    if (ERR_OK != _mqtt_data_fixnum(buf, 2, &num)) {//报文标识符
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    mqtt_puback_varhead *vh;
    CALLOC(vh, 1, sizeof(mqtt_puback_varhead));
    pack->varhead = vh;
    vh->packid = (int16_t)num;
    pack->version = ((mqtt_ctx *)ud->extra)->version;
    if (pack->version < MQTT_50
        || 2 == pack->fixhead.remaining_lens) {//剩余长度为2，则表示使用原因码0x00（成功）
        return ERR_OK;
    }
    if (pack->fixhead.remaining_lens >= 3) {
        if (ERR_OK != _mqtt_data_fixnum(buf, 1, &num)) {//原因码
            BIT_SET(*status, PROT_ERROR);
            return ERR_FAILED;
        }
        vh->reason = (uint8_t)num;
    }
    if (pack->fixhead.remaining_lens >= 4) {
        vh->properties = _mqtt_properties(buf, status, NULL);//属性
        if (NULL == vh->properties
            && BIT_CHECK(*status, PROT_ERROR)) {
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
//两个方向都允许  发布收到（保证交付第一步）
static int32_t _mqtt_pubrec(mqtt_pack_ctx *pack, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    if (0 != pack->fixhead.flags) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    //可变报头 报文标识符，[原因码(MQTT_50)，属性(MQTT_50)]
    int32_t num;
    if (ERR_OK != _mqtt_data_fixnum(buf, 2, &num)) {//报文标识符
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    mqtt_pubrec_varhead *vh;
    CALLOC(vh, 1, sizeof(mqtt_pubrec_varhead));
    pack->varhead = vh;
    vh->packid = (int16_t)num;
    pack->version = ((mqtt_ctx *)ud->extra)->version;
    if (pack->version < MQTT_50
        || 2 == pack->fixhead.remaining_lens) {//剩余长度为2，则表示使用原因码0x00（成功
        return ERR_OK;
    }
    if (pack->fixhead.remaining_lens >= 3) {
        if (ERR_OK != _mqtt_data_fixnum(buf, 1, &num)) {//原因码
            BIT_SET(*status, PROT_ERROR);
            return ERR_FAILED;
        }
        vh->reason = (uint8_t)num;
    }
    if (pack->fixhead.remaining_lens >= 4) {
        vh->properties = _mqtt_properties(buf, status, NULL);//属性
        if (NULL == vh->properties
            && BIT_CHECK(*status, PROT_ERROR)) {
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
//两个方向都允许  发布释放（保证交付第二步）
static int32_t _mqtt_pubrel(mqtt_pack_ctx *pack, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    if (0x02 != pack->fixhead.flags) {//3，2，1，0位是保留位且必须分别设置为0，0，1，0
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    //可变报头 报文标识符，[原因码(MQTT_50)，属性(MQTT_50)]
    int32_t num;
    if (ERR_OK != _mqtt_data_fixnum(buf, 2, &num)) {//报文标识符
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    mqtt_pubrel_varhead *vh;
    CALLOC(vh, 1, sizeof(mqtt_pubrel_varhead));
    pack->varhead = vh;
    vh->packid = (int16_t)num;
    pack->version = ((mqtt_ctx *)ud->extra)->version;
    if (pack->version < MQTT_50
        || 2 == pack->fixhead.remaining_lens) {//剩余长度为2，则表示使用原因码0x00 （成功）
        return ERR_OK;
    }
    if (pack->fixhead.remaining_lens >= 3) {
        if (ERR_OK != _mqtt_data_fixnum(buf, 1, &num)) {//原因码
            BIT_SET(*status, PROT_ERROR);
            return ERR_FAILED;
        }
        vh->reason = (uint8_t)num;
    }
    if (pack->fixhead.remaining_lens >= 4) {
        vh->properties = _mqtt_properties(buf, status, NULL);//属性
        if (NULL == vh->properties
            && BIT_CHECK(*status, PROT_ERROR)) {
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
//两个方向都允许  QoS 2消息发布完成（保证交互第三步）
static int32_t _mqtt_pubcomp(mqtt_pack_ctx *pack, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    if (0 != pack->fixhead.flags) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    //可变报头 报文标识符，[原因码(MQTT_50)，属性(MQTT_50)]
    int32_t num;
    if (ERR_OK != _mqtt_data_fixnum(buf, 2, &num)) {//报文标识符
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    mqtt_pubcomp_varhead *vh;
    CALLOC(vh, 1, sizeof(mqtt_pubcomp_varhead));
    pack->varhead = vh;
    vh->packid = (int16_t)num;
    pack->version = ((mqtt_ctx *)ud->extra)->version;
    if (pack->version < MQTT_50
        || 2 == pack->fixhead.remaining_lens) {//剩余长度为2，则表示使用原因码0x00（成功）
        return ERR_OK;
    }
    if (pack->fixhead.remaining_lens >= 3) {
        if (ERR_OK != _mqtt_data_fixnum(buf, 1, &num)) {//原因码
            BIT_SET(*status, PROT_ERROR);
            return ERR_FAILED;
        }
        vh->reason = (uint8_t)num;
    }
    if (pack->fixhead.remaining_lens >= 4) {
        vh->properties = _mqtt_properties(buf, status, NULL);//属性
        if (NULL == vh->properties
            && BIT_CHECK(*status, PROT_ERROR)) {
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
//客户端到服务端  客户端订阅请求
static int32_t _mqtt_subscribe(mqtt_pack_ctx *pack, int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    if (client 
        || 0x02 != pack->fixhead.flags) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    //可变报头 报文标识符，属性(MQTT_50)
    int32_t num;
    if (ERR_OK != _mqtt_data_fixnum(buf, 2, &num)) {//报文标识符
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    mqtt_subscribe_varhead *vh;
    CALLOC(vh, 1, sizeof(mqtt_subscribe_varhead));
    pack->varhead = vh;
    vh->packid = (int16_t)num;
    num = 0;
    pack->version = ((mqtt_ctx *)ud->extra)->version;
    if (pack->version >= MQTT_50) {
        vh->properties = _mqtt_properties(buf, status, &num);//属性
        if (NULL == vh->properties
            && BIT_CHECK(*status, PROT_ERROR)) {
            return ERR_FAILED;
        }
    }
    //载荷
    char *topic;
    int32_t off;
    int32_t remain = (int32_t)pack->fixhead.remaining_lens - 2 - num;//剩余长度
    if (remain <= 0) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    subscribe_option *subop;
    mqtt_subscribe_payload *pl;
    CALLOC(pl, 1, sizeof(mqtt_subscribe_payload));
    pack->payload = pl;
    arr_subscribe_option_init(&pl->subop, 0);
    for (off = 0; off < remain;) {
        topic = _mqtt_data_string2(buf, &num);//主题
        if (NULL == topic) {
            BIT_SET(*status, PROT_ERROR);
            return ERR_FAILED;
        }
        off += (2 + num);
        if (ERR_OK != _mqtt_data_fixnum(buf, 1, &num)) {//订阅选项
            BIT_SET(*status, PROT_ERROR);
            FREE(topic);
            return ERR_FAILED;
        }
        off++;
        MALLOC(subop, sizeof(subscribe_option));
        subop->topic = topic;
        subop->qos = BIT_GETN(num, 0);//订阅选项 解析
        subop->qos |= (BIT_GETN(num, 1) << 1);
        if (pack->version >= MQTT_50) {
            subop->nl = BIT_GETN(num, 2);
            subop->rap = BIT_GETN(num, 3);
            subop->retain = BIT_GETN(num, 4);
            subop->retain |= (BIT_GETN(num, 5) << 1);
        }
        arr_subscribe_option_push_back(&pl->subop, &subop);
    }
    if (off != remain) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    return ERR_OK;
}
//服务端到客户端  订阅请求报文确认
static int32_t _mqtt_suback(mqtt_pack_ctx *pack, int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    if (!client || 0 != pack->fixhead.flags) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    //可变报头 报文标识符，属性(MQTT_50)
    int32_t num;
    if (ERR_OK != _mqtt_data_fixnum(buf, 2, &num)) {//报文标识符
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    mqtt_suback_varhead *vh;
    CALLOC(vh, 1, sizeof(mqtt_suback_varhead));
    pack->varhead = vh;
    vh->packid = (int16_t)num;
    num = 0;
    pack->version = ((mqtt_ctx *)ud->extra)->version;
    if (pack->version >= MQTT_50) {
        vh->properties = _mqtt_properties(buf, status, &num);//属性
        if (NULL == vh->properties
            && BIT_CHECK(*status, PROT_ERROR)) {
            return ERR_FAILED;
        }
    }
    //载荷
    num = (int32_t)pack->fixhead.remaining_lens - 2 - num;//计算剩余长度
    if (num <= 0) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    mqtt_suback_payload *pl;
    CALLOC(pl, 1, sizeof(mqtt_suback_payload) + num);
    pack->payload = pl;
    pl->rlens = num;
    if (num != buffer_remove(buf, pl->reasons, num)) {//原因码列表
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    return ERR_OK;
}
//客户端到服务端  客户端取消订阅请求
static int32_t _mqtt_unsubscribe(mqtt_pack_ctx *pack, int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    if (client 
        || 0x02 != pack->fixhead.flags) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    //可变报头 报文标识符，属性(MQTT_50)
    int32_t num;
    if (ERR_OK != _mqtt_data_fixnum(buf, 2, &num)) {//报文标识符
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    mqtt_unsubscribe_varhead *vh;
    CALLOC(vh, 1, sizeof(mqtt_unsubscribe_varhead));
    pack->varhead = vh;
    vh->packid = (int16_t)num;
    num = 0;
    pack->version = ((mqtt_ctx *)ud->extra)->version;
    if (pack->version >= MQTT_50) {
        vh->properties = _mqtt_properties(buf, status, &num);//属性
        if (NULL == vh->properties
            && BIT_CHECK(*status, PROT_ERROR)) {
            return ERR_FAILED;
        }
    }
    //载荷
    char *topic;
    int32_t off;
    int32_t remain = (int32_t)pack->fixhead.remaining_lens - 2 - num;//剩余长度
    if (remain <= 0) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    mqtt_unsubscribe_payload *pl;
    CALLOC(pl, 1, sizeof(mqtt_unsubscribe_payload));
    pack->payload = pl;
    arr_ptr_init(&pl->topics, 0);
    for (off = 0; off < remain;) {
        topic = _mqtt_data_string2(buf, &num);//主题
        if (NULL == topic) {
            BIT_SET(*status, PROT_ERROR);
            return ERR_FAILED;
        }
        off += (2 + num);
        arr_ptr_push_back(&pl->topics, (void **)&topic);
    }
    if (off != remain) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    return ERR_OK;
}
//服务端到客户端  取消订阅确认
static int32_t _mqtt_unsuback(mqtt_pack_ctx *pack, int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    if (!client 
        || 0 != pack->fixhead.flags) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    //可变报头 报文标识符，属性(MQTT_50)
    int32_t num;
    if (ERR_OK != _mqtt_data_fixnum(buf, 2, &num)) {//报文标识符
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    mqtt_unsuback_varhead *vh;
    CALLOC(vh, 1, sizeof(mqtt_unsuback_varhead));
    pack->varhead = vh;
    vh->packid = (int16_t)num;
    pack->version = ((mqtt_ctx *)ud->extra)->version;
    if (pack->version < MQTT_50) {
        return ERR_OK;
    }
    vh->properties = _mqtt_properties(buf, status, &num);//属性
    if (NULL == vh->properties
        && BIT_CHECK(*status, PROT_ERROR)) {
        return ERR_FAILED;
    }
    //载荷
    num = (int32_t)pack->fixhead.remaining_lens - 2 - num;//计算剩余长度
    if (num <= 0) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    mqtt_unsuback_payload *pl;
    CALLOC(pl, 1, sizeof(mqtt_unsuback_payload) + num);
    pack->payload = pl;
    pl->rlens = num;
    if (num != buffer_remove(buf, pl->reasons, num)) {//原因码列表
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    return ERR_OK;
}
//客户端到服务端  心跳请求
static int32_t _mqtt_ping(mqtt_pack_ctx *pack, int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    if (client 
        || 0 != pack->fixhead.flags) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    pack->version = ((mqtt_ctx *)ud->extra)->version;
    return ERR_OK;
}
//服务端到客户端  心跳响应
static int32_t _mqtt_pong(mqtt_pack_ctx *pack, int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    if (!client 
        || 0 != pack->fixhead.flags) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    pack->version = ((mqtt_ctx *)ud->extra)->version;
    return ERR_OK;
}
//两个方向都允许  断开连接通知
static int32_t _mqtt_disconnect(mqtt_pack_ctx *pack, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    if (0 != pack->fixhead.flags) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    mqtt_disconnect_varhead *vh;
    CALLOC(vh, 1, sizeof(mqtt_disconnect_varhead));
    pack->varhead = vh;
    BIT_SET(*status, PROT_CLOSE);
    pack->version = ((mqtt_ctx *)ud->extra)->version;
    if (pack->version < MQTT_50
        || 0 == pack->fixhead.remaining_lens) {//如果剩余长度小于1，则表示使用原因码0x00（正常断开）. 如果剩余长度小于2，属性长度使用0。
        return ERR_OK;
    }
    //可变报头
    int32_t num;
    if (ERR_OK != _mqtt_data_fixnum(buf, 1, &num)) {//断开原因码
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    vh->reason = (uint8_t)num;
    if (pack->fixhead.remaining_lens > 1) {
        vh->properties = _mqtt_properties(buf, status, NULL);//属性
        if (NULL == vh->properties
            && BIT_CHECK(*status, PROT_ERROR)) {
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
//两个方向都允许  认证信息交换
static int32_t _mqtt_auth(mqtt_pack_ctx *pack, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    if (0 != pack->fixhead.flags) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    pack->version = ((mqtt_ctx *)ud->extra)->version;
    //如果原因码为0x00（成功）并且没有属性字段，则可以省略原因码和属性长度。这种情况下，AUTH报文剩余长度为0。
    if (0 == pack->fixhead.remaining_lens) {
        mqtt_auth_varhead *vh;
        CALLOC(vh, 1, sizeof(mqtt_auth_varhead));
        pack->varhead = vh;
        return ERR_OK;
    }
    //可变报头
    int32_t num;
    if (ERR_OK != _mqtt_data_fixnum(buf, 1, &num)) {//认证原因码
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    mqtt_auth_varhead *vh;
    CALLOC(vh, 1, sizeof(mqtt_auth_varhead));
    pack->varhead = vh;
    vh->reason = (uint8_t)num;
    vh->properties = _mqtt_properties(buf, status, NULL);//属性
    if (NULL == vh->properties
        && BIT_CHECK(*status, PROT_ERROR)) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
static int32_t _mqtt_init(mqtt_pack_ctx *pack, int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    int32_t rtn = ERR_FAILED;
    switch (pack->fixhead.prot) {
    case MQTT_CONNECT:
        rtn = _mqtt_connect(pack, client, buf, ud, status);
        break;
    case MQTT_CONNACK:
        rtn = _mqtt_connack(pack, client, buf, ud, status);
        break;
    case MQTT_AUTH:
        if (client) {
            rtn = _mqtt_auth(pack, buf, ud, status);
        }
        break;
    default:
        BIT_SET(*status, PROT_ERROR);
        break;
    }
    return rtn;
}
static int32_t _mqtt_commands(mqtt_pack_ctx *pack, int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    int32_t rtn = ERR_FAILED;
    switch (pack->fixhead.prot) {
    case MQTT_PUBLISH:
        rtn = _mqtt_publish(pack, buf, ud, status);
        break;
    case MQTT_PUBACK:
        rtn = _mqtt_puback(pack, buf, ud, status);
        break;
    case MQTT_PUBREC:
        rtn = _mqtt_pubrec(pack, buf, ud, status);
        break;
    case MQTT_PUBREL:
        rtn = _mqtt_pubrel(pack, buf, ud, status);
        break;
    case MQTT_PUBCOMP:
        rtn = _mqtt_pubcomp(pack, buf, ud, status);
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
        rtn = _mqtt_disconnect(pack, buf, ud, status);
        break;
    case MQTT_AUTH:
        rtn = _mqtt_auth(pack, buf, ud, status);
        break;
    default:
        BIT_SET(*status, PROT_ERROR);
        break;
    }
    return rtn;
}
mqtt_pack_ctx *mqtt_unpack(int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    size_t blens = buffer_size(buf);
    if (blens < 2) {//固定头至少2字节
        BIT_SET(*status, PROT_MOREDATA);
        return NULL;
    }
    size_t remaining_lens;
    int32_t roccupy = _mqtt_varlens_decode(buf, 1, blens, &remaining_lens);//返回剩余长度占用字节数
    if (ERR_FAILED == roccupy) {
        if (blens >= 5) {//剩余长度最大4个字节
            BIT_SET(*status, PROT_ERROR);
            return NULL;
        }
        BIT_SET(*status, PROT_MOREDATA);
        return NULL;
    }
    size_t fhlens = 1 + roccupy;
    size_t total = fhlens + remaining_lens;
    if (blens < total) {
        BIT_SET(*status, PROT_MOREDATA);
        return NULL;
    }
    uint8_t val = (uint8_t)buffer_at(buf, 0);
    mqtt_pack_ctx *pack;
    CALLOC(pack, 1, sizeof(mqtt_pack_ctx));
    pack->fixhead.remaining_lens = remaining_lens;
    pack->fixhead.prot = (val >> 4);
    pack->fixhead.flags = (val & 0x0F);
    ASSERTAB(fhlens == buffer_drain(buf, fhlens), "drain buffer failed.");
    int32_t rtn = ERR_FAILED;
    switch (ud->status) {
    case INIT:
        rtn = _mqtt_init(pack, client, buf, ud, status);
        break;
    case COMMAND:
        rtn = _mqtt_commands(pack, client, buf, ud, status);
        break;
    }
    if (ERR_OK != rtn) {
        _mqtt_pkfree(pack);
        return NULL;
    }
    if (blens - buffer_size(buf) != total) {
        BIT_SET(*status, PROT_ERROR);
        _mqtt_pkfree(pack);
        return NULL;
    }
    return pack;
}
const char *mqtt_reason(mqtt_prot prot, int32_t code) {
    switch (code) {
    case 0x00:
        if (MQTT_CONNACK == prot || MQTT_PUBACK == prot || MQTT_PUBREC == prot || MQTT_PUBREL == prot
            || MQTT_PUBCOMP == prot || MQTT_UNSUBACK == prot || MQTT_AUTH == prot) {
            return "Success";//成功
        }
        if (MQTT_DISCONNECT == prot) {
            return "Normal disconnection";//正常断开
        }
        if (MQTT_SUBACK == prot) {
            return "Granted QoS 0";//授权的QoS 0
        }
        break;
    case 0x01:
        if (MQTT_SUBACK == prot) {
            return "Granted QoS 1";//授权的QoS 1
        }
        break;
    case 0x02:
        if (MQTT_SUBACK == prot) {
            return "Granted QoS 2";//授权的QoS 2
        }
        break;
    case 0x04:
        if (MQTT_DISCONNECT == prot) {
            return "Disconnect with Will Message";//包含遗嘱的断开
        }
        break;
    case 0x10:
        if (MQTT_PUBACK == prot || MQTT_PUBREC == prot) {
            return "No matching subscribers";//无匹配订阅
        }
        break;
    case 0x11:
        if (MQTT_UNSUBACK == prot) {
            return "No subscription existed";//订阅不存在
        }
        break;
    case 0x18:
        if (MQTT_AUTH == prot) {
            return "Continue authentication";//继续认证
        }
        break;
    case 0x19:
        if (MQTT_AUTH == prot) {
            return "Re-authenticate";//重新认证
        }
        break;
    case 0x80:
        if (MQTT_CONNACK == prot || MQTT_PUBACK == prot || MQTT_PUBREC == prot
            || MQTT_SUBACK == prot || MQTT_UNSUBACK == prot || MQTT_DISCONNECT == prot) {
            return "Unspecified error";//未指明的错误
        }
        break;
    case 0x81:
        if (MQTT_CONNACK == prot || MQTT_DISCONNECT == prot) {
            return "Malformed Packet";//无效报文
        }
        break;
    case 0x82:
        if (MQTT_CONNACK == prot || MQTT_DISCONNECT == prot) {
            return "Protocol Error";//协议错误
        }
        break;
    case 0x83:
        if (MQTT_CONNACK == prot || MQTT_PUBACK == prot || MQTT_PUBREC == prot
            || MQTT_SUBACK == prot || MQTT_UNSUBACK == prot || MQTT_DISCONNECT == prot) {
            return "Implementation specific error";//实现错误
        }
        break;
    case 0x84:
        if (MQTT_CONNACK == prot) {
            return "Unsupported Protocol Version";//协议版本不支持
        }
        break;
    case 0x85:
        if (MQTT_CONNACK == prot) {
            return "Client Identifier not valid";//客户标识符无效
        }
        break;
    case 0x86:
        if (MQTT_CONNACK == prot) {
            return "Bad User Name or Password";//用户名密码错误
        }
        break;
    case 0x87:
        if (MQTT_CONNACK == prot || MQTT_PUBACK == prot || MQTT_PUBREC == prot
            || MQTT_SUBACK == prot || MQTT_UNSUBACK == prot || MQTT_DISCONNECT == prot) {
            return "Not authorized";//未授权
        }
        break;
    case 0x88:
        if (MQTT_CONNACK == prot) {
            return "Server unavailable";//服务端不可用
        }
        break;
    case 0x89:
        if (MQTT_CONNACK == prot || MQTT_DISCONNECT == prot) {
            return "Server busy";//服务端正忙
        }
        break;
    case 0x8A:
        if (MQTT_CONNACK == prot) {
            return "Banned";//禁止
        }
        break;
    case 0x8B:
        if (MQTT_DISCONNECT == prot) {
            return "Server shutting down";//服务端关闭中
        }
        break;
    case 0x8C:
        if (MQTT_CONNACK == prot || MQTT_DISCONNECT == prot) {
            return "Bad authentication method";//无效的认证方法
        }
        break;
    case 0x8D:
        if (MQTT_DISCONNECT == prot) {
            return "Keep Alive timeout";//保活超时
        }
        break;
    case 0x8E:
        if (MQTT_DISCONNECT == prot) {
            return "Session taken over";//会话被接管
        }
        break;
    case 0x8F:
        if (MQTT_SUBACK == prot || MQTT_UNSUBACK == prot || MQTT_DISCONNECT == prot) {
            return "Topic Filter invalid";//主题过滤器无效
        }
        break;
    case 0x90:
        if (MQTT_CONNACK == prot || MQTT_PUBACK == prot || MQTT_PUBREC == prot || MQTT_DISCONNECT == prot) {
            return "Topic Name invalid";//主题名无效
        }
        break;
    case 0x91:
        if (MQTT_PUBACK == prot || MQTT_PUBREC == prot || MQTT_SUBACK == prot || MQTT_UNSUBACK == prot) {
            return "Packet Identifier in use";//报文标识符已被占用
        }
        break;
    case 0x92:
        if (MQTT_PUBREL == prot || MQTT_PUBCOMP == prot) {
            return "Packet Identifier not found";//报文标识符无效
        }
        break;
    case 0x93:
        if (MQTT_DISCONNECT == prot) {
            return "Receive Maximum exceeded";//接收超出最大数量
        }
        break;
    case 0x94:
        if (MQTT_DISCONNECT == prot) {
            return "Topic Alias invalid";//主题别名无效
        }
        break;
    case 0x95:
        if (MQTT_CONNACK == prot || MQTT_DISCONNECT == prot) {
            return "Packet too large";//报文过长
        }
        break;
    case 0x96:
        if (MQTT_DISCONNECT == prot) {
            return "Message rate too high";//消息太过频繁
        }
        break;
    case 0x97:
        if (MQTT_CONNACK == prot || MQTT_PUBACK == prot || MQTT_PUBREC == prot
            || MQTT_SUBACK == prot || MQTT_DISCONNECT == prot) {
            return "Quota exceeded";//超出配额
        }
        break;
    case 0x98:
        if (MQTT_DISCONNECT == prot) {
            return "Administrative action";//管理行为
        }
        break;
    case 0x99:
        if (MQTT_CONNACK == prot || MQTT_PUBACK == prot
            || MQTT_PUBREC == prot || MQTT_DISCONNECT == prot) {
            return "Payload format invalid";//载荷格式无效
        }
        break;
    case 0x9A:
        if (MQTT_CONNACK == prot || MQTT_DISCONNECT == prot) {
            return "Retain not supported";//不支持保留
        }
        break;
    case 0x9B:
        if (MQTT_CONNACK == prot || MQTT_DISCONNECT == prot) {
            return "QoS not supported";//不支持的QoS等级
        }
        break;
    case 0x9C:
        if (MQTT_CONNACK == prot || MQTT_DISCONNECT == prot) {
            return "Use another server";//(临时)使用其他服务端
        }
        break;
    case 0x9D:
        if (MQTT_CONNACK == prot || MQTT_DISCONNECT == prot) {
            return "Server moved";//服务端已(永久)移动
        }
        break;
    case 0x9E:
        if (MQTT_SUBACK == prot || MQTT_DISCONNECT == prot) {
            return "Shared Subscriptions not supported";//不支持共享订阅
        }
        break;
    case 0x9F:
        if (MQTT_CONNACK == prot || MQTT_DISCONNECT == prot) {
            return "Connection rate exceeded";//超出连接速率限制
        }
        break;
    case 0xA0:
        if (MQTT_DISCONNECT == prot) {
            return "Maximum connect time";//最大连接时间
        }
        break;
    case 0xA1:
        if (MQTT_SUBACK == prot || MQTT_DISCONNECT == prot) {
            return "Subscription Identifiers not supported";//不支持订阅标识符
        }
        break;
    case 0xA2:
        if (MQTT_SUBACK == prot || MQTT_DISCONNECT == prot) {
            return "Wildcard Subscriptions not supported";//不支持通配符订阅
        }
        break;
    }
    return "Unknown";
}
