#include "protocol/mqtt/mqtt.h"
#include "utils/utils.h"
#include "protocol/prots_pub.h"

//https://mqtt.p2hp.com/mqtt311
//https://mqtt.p2hp.com/mqtt-5-0
typedef enum parse_status {
    INIT = 0,
    COMMAND
}parse_status;
// PUBLISH 单块分配的富余量：topic 与载荷各一个结尾 NUL，加载荷头最多 3 字节对齐补白
#define MQTT_PUB_SLACK 8
// PUBLISH 块里可变报头之后的偏移：v5 属性块预留在这里(见 _mqtt_publish_prelens)，按 8 对齐给属性块用
#define MQTT_PUB_PREOFF ROUND_UP(sizeof(mqtt_pack_ctx) + sizeof(mqtt_publish_varhead), 8)
#define MQTT_PUB_MERGE_LIM 1024 // 属性块并进 PUBLISH 块后跨过这个尺寸而原先不跨就不并：glibc 线程缓存只收 1032 字节以内的块
#define MQTT_SPLIT_HEAD 256 // 跨节点的包拷出来解析时先用栈上这块，装不下才 MALLOC
#define MQTT_PROP_FIT 4 // 属性小区按段长放大时保证前这么多个属性放得下(见 _mqtt_prop_cap)
#define MQTT_PROP_SLACK (sizeof(mqtt_propertie) + 8) // 一个属性在小区里比线上多占的字节上界：条目头、两个 NUL、按 8 取整
// 非 PUBLISH 包的可变报头就放在 pack 块尾(见 _mqtt_parse)，按最大的那种留位置；PUBLISH 的见 _mqtt_publish_blk
typedef union mqtt_vh_slot {
    mqtt_connect_varhead connect;
    mqtt_connack_varhead connack;
    mqtt_pubackrel_varhead pubackrel;
    mqtt_subreqresp_varhead subreqresp;
    mqtt_reason_varhead reason;
}mqtt_vh_slot;
// 连接上下文只记协议版本、建好后不再改，故每个版本一个全局只读实例，不按连接分配(见 mqtt_ctx_new)
static const mqtt_ctx _mqtt_ctx311 = { MQTT_311 };
static const mqtt_ctx _mqtt_ctx50 = { MQTT_50 };

// 释放 PUBLISH 的 v5 属性块：预留在块内的(地址恰是 MQTT_PUB_PREOFF 处，另开的块不可能落在别的块里)只放它另开的部分
static inline void _mqtt_publish_props_free(mqtt_pack_ctx *pack) {
    mprop_arr *props = ((mqtt_publish_varhead *)pack->varhead)->properties;
    if ((char *)props == (char *)pack + MQTT_PUB_PREOFF) {
        _mqtt_prop_blk_release(props);
    } else {
        _mqtt_propertie_free(props);
    }
}
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
        // 布局见 _mqtt_publish_blk：只有 v5 属性块可能另开。
        // varhead 为 NULL 表示没走到 _mqtt_publish
        if (NULL != pack->varhead) {
            _mqtt_publish_props_free(pack);
        }
        break;
    case MQTT_PUBACK:
    case MQTT_PUBREC:
    case MQTT_PUBREL:
    case MQTT_PUBCOMP:
        _mqtt_pubackrel_varhead_free(pack->varhead);
        break;
    case MQTT_SUBSCRIBE:
    case MQTT_UNSUBSCRIBE:
        _mqtt_subreqresp_varhead_free(pack->varhead);
        FREE(pack->payload);
        break;
    case MQTT_SUBACK:
    case MQTT_UNSUBACK:
        _mqtt_subreqresp_varhead_free(pack->varhead);
        break;
    case MQTT_PINGREQ:
        break;
    case MQTT_PINGRESP:
        break;
    case MQTT_DISCONNECT:
    case MQTT_AUTH:
        _mqtt_reason_varhead_free(pack->varhead);
        break;
    default:
        break;
    }
    FREE(pack);
}
mqtt_ctx *mqtt_ctx_new(mqtt_protversion version) {
    if (MQTT_311 == version) {
        return (mqtt_ctx *)&_mqtt_ctx311;
    }
    if (MQTT_50 == version) {
        return (mqtt_ctx *)&_mqtt_ctx50;
    }
    LOG_WARN("mqtt unsupported protocol version %d.", (int32_t)version);
    return NULL;
}
void mqtt_ctx_free(void *ctx) {
    (void)ctx;
}
void _mqtt_udfree(ud_cxt *ud) {
    mqtt_ctx_free(ud->context);
    ud->context = NULL;
}
int32_t _mqtt_may_resume(void *data) {
    if (NULL == data) {
        return ERR_OK;
    }
    mqtt_pack_ctx *pack = data;
    // 服务端主动推的包不是任何一次请求的响应,交给等待者会让请求-响应从此错开一格:
    // PUBLISH 是投递订阅,PUBREL 是入站 QoS2 的第二步,DISCONNECT 是服务端单方通知
    if (MQTT_PUBLISH == pack->fixhead.prot
        || MQTT_PUBREL == pack->fixhead.prot
        || MQTT_DISCONNECT == pack->fixhead.prot) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
// 从读区拷出 lens 字节到 out
static inline int32_t _mqtt_data_copy(binary_ctx *br, void *out, size_t lens) {
    if (!binary_have(br, lens)) {
        return ERR_FAILED;
    }
    memcpy(out, br->data + br->offset, lens);
    binary_get_skip(br, lens);
    return ERR_OK;
}
// 从读区读取固定长度大端无符号整数（1/2字节），存入 num
static inline int32_t _mqtt_data_fixnum(binary_ctx *br, size_t lens, int32_t *num) {
    if (!binary_have(br, lens)) {
        return ERR_FAILED;
    }
    if (1 == lens) {
        //强制 unsigned 解读：避免 char 默认符号性平台差异（部分 ARM 默认 unsigned char）
        //当前所有调用方对低 8 位的位提取/截断都正确，但属性值 nval(int32_t) 跨平台一致性需保证
        *num = binary_get_uint8(br);
        return ERR_OK;
    }
    *num = (int32_t)binary_get_uinteger(br, lens, 0);
    return ERR_OK;
}
// 从读区读取 4 字节无符号大端整数（用于 MQTT v5 四字节属性，避免 int32 截断负数）
static inline int32_t _mqtt_data_u32(binary_ctx *br, int64_t *num) {
    if (!binary_have(br, 4)) {
        return ERR_FAILED;
    }
    *num = (int64_t)binary_get_uinteger(br, 4, 0);
    return ERR_OK;
}
// 从读区读取可变长度整数，返回占用字节数，失败返回 ERR_FAILED（失败不消耗字节）
static inline int32_t _mqtt_data_varnum(binary_ctx *br, int32_t *num) {
    size_t val = 0;
    int32_t mcl = 1;
    int32_t i;
    uint8_t byte;
    for (i = 0; i < 4; i++) {
        if (!binary_have(br, (uint64_t)i + 1)) {
            return ERR_FAILED;
        }
        byte = (uint8_t)br->data[br->offset + (size_t)i];
        val += (size_t)((byte & 0x7f) * mcl);
        if (!BIT_CHECK(byte, 0x80)) {
            binary_get_skip(br, (size_t)i + 1);
            *num = (int32_t)val;
            return i + 1;
        }
        mcl *= 0x80;
    }
    return ERR_FAILED;
}
// 读 2 字节长度前缀，并卡住"声明长度不得超过读区剩余字节"。
// 读完就按这个长度分配的字段都走它，必须先卡再分配；有更紧判定的(协议名定长 4、PUBLISH 主题名
// 不超 remaining_lens)自己判
static inline int32_t _mqtt_data_lens(binary_ctx *br, int32_t *num) {
    if (ERR_OK != _mqtt_data_fixnum(br, 2, num)
        || !binary_have(br, (uint64_t)(*num))) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
// 把长度已由 _mqtt_data_lens 取到(且已卡过剩余字节)的 num 字节拷进 dst 并补 NUL。
// utf8 非 0 表示 UTF-8 字符串字段。MQTT-1.5.4-2：含 U+0000 即为非法报文，必须拒收(返 ERR_FAILED)——这些字段
// 解析后只剩 char*、长度不再保留，放过去就会在第一个 NUL 处截断，"victim\0evil" 与 "victim"
// 塌缩成同一个 clientid / topic。遗嘱载荷与密码是二进制字段，允许含 NUL，传 0
static inline int32_t _mqtt_data_str_to(binary_ctx *br, char *dst, int32_t num, int32_t utf8) {
    memcpy(dst, br->data + br->offset, (size_t)num);
    dst[num] = '\0';
    binary_get_skip(br, (size_t)num);
    return (utf8 && NULL != memchr(dst, '\0', (size_t)num)) ? ERR_FAILED : ERR_OK;
}
// 属性段长 plens 对应的小区字节数。属性线上至少 2 字节一个，每个在小区里多占不超过 MQTT_PROP_SLACK，
// 故前 MQTT_PROP_FIT 个一定放得下；不小于 MQTT_PROP_ARENA，属性短的包块长与原先一样
static inline uint32_t _mqtt_prop_cap(int32_t plens) {
    size_t n = (size_t)plens / 2;
    if (n > MQTT_PROP_FIT) {
        n = MQTT_PROP_FIT;
    }
    size_t cap = (size_t)plens + MQTT_PROP_SLACK * n;
    return (uint32_t)(cap < MQTT_PROP_ARENA ? MQTT_PROP_ARENA : cap);
}
// 属性数组连同前几个属性放进同一块：数组头、MQTT_PROP_SLOTS 个指针槽、cap 字节的小区。
// 小区放不下的属性、槽用满后的指针数组各自另开，释放见 _mqtt_propertie_free
static mqtt_propertie *_mqtt_prop_mem(mqtt_prop_blk *blk, size_t lens) {
    mqtt_propertie *propt;
    lens = ROUND_UP(lens, 8);
    if (lens <= blk->cap - blk->used) {
        propt = (mqtt_propertie *)((char *)blk->arena + blk->used);
        blk->used += (uint32_t)lens;
        return propt;
    }
    MALLOC(propt, lens);
    return propt;
}
// 读一个属性并放进 blk。失败返回 NULL：越出读区、未知 id。
// 字符串属性 fval 存值；用户属性 fval 存 key、sval 存 value(value 的长度前缀紧跟在 key 后面，先偷看它)，
// 两段都在同一次分配里，长度都卡"不超过读区剩余字节"，口径同 _mqtt_data_lens
static mqtt_propertie *_mqtt_prop_one(binary_ctx *br, mqtt_prop_blk *blk) {
    int32_t num, klen, vlen;
    int64_t nval = 0;
    size_t extra = 0;
    char *kv;
    mqtt_prop_flag flag;
    mqtt_propertie *propt;
    if (ERR_OK != _mqtt_data_fixnum(br, 1, &num)) {
        return NULL;
    }
    flag = num;
    klen = vlen = -1;
    switch (flag) {
    case PAYLOAD_FORMAT://0x01 载荷格式说明	字节	PUBLISH, Will Properties
    case REQPROBLEM_INFO://0x17 请求问题信息	字节	CONNECT
    case REQRESP_INFO://0x19 请求响应信息	字节	CONNECT
    case MAXIMUM_QOS://0x24 最大QoS	字节	CONNACK
    case RETAIN_AVAILABLE://0x25 保留属性可用性	字节	CONNACK
    case WILDCARD_SUBSCRIPTION://0x28 通配符订阅可用性	字节	CONNACK
    case SUBSCRIPTIONID_AVAILABLE://0x29 订阅标识符可用性	字节	CONNACK
    case SHARED_SUBSCRIPTION://0x2A 共享订阅可用性	字节	CONNACK
        if (ERR_OK != _mqtt_data_fixnum(br, 1, &num)) {
            return NULL;
        }
        nval = num;
        break;
    case SERVER_KEEPALIVE://0x13 服务端保活时间	双字节整数	CONNACK
    case RECEIVE_MAXIMUM://0x21 接收最大数量	双字节整数	CONNECT, CONNACK
    case TOPICALIAS_MAXIMUM://0x22 主题别名最大长度	双字节整数	CONNECT, CONNACK
    case TOPIC_ALIAS://0x23 主题别名	双字节整数	PUBLISH
        if (ERR_OK != _mqtt_data_fixnum(br, 2, &num)) {
            return NULL;
        }
        nval = num;
        break;
    case MSG_EXPIRY://0x02 消息过期时间	四字节整数	PUBLISH, Will Properties
    case SESSION_EXPIRY://0x11 会话过期间隔	四字节整数	CONNECT, CONNACK, DISCONNECT
    case WILLDELAY_INTERVAL://0x18 遗嘱延时间隔	四字节整数	Will Properties
    case MAXIMUM_PACKETSIZE://0x27 最大报文长度	四字节整数	CONNECT, CONNACK
        if (ERR_OK != _mqtt_data_u32(br, &nval)) {
            return NULL;
        }
        break;
    case SUBSCRIPTION_ID://0x0B 定义标识符	变长字节整数	PUBLISH, SUBSCRIBE
        if (ERR_FAILED == _mqtt_data_varnum(br, &num)) {
            return NULL;
        }
        nval = num;
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
        if (ERR_OK != _mqtt_data_lens(br, &klen)) {
            return NULL;
        }
        extra = (size_t)klen + 1;
        break;
    case USER_PROPERTY://0x26 用户属性	UTF-8字符串对	CONNECT, CONNACK, PUBLISH, Will Properties, PUBACK, PUBREC, PUBREL, PUBCOMP, SUBSCRIBE, SUBACK, UNSUBSCRIBE, UNSUBACK, DISCONNECT, AUTH
        if (ERR_OK != _mqtt_data_lens(br, &klen)
            || !binary_have(br, (uint64_t)klen + 2)) {
            return NULL;
        }
        vlen = (int32_t)read_be16(br->data + br->offset + klen);
        if (!binary_have(br, (uint64_t)klen + 2 + (uint64_t)vlen)) {
            return NULL;
        }
        extra = (size_t)klen + 1 + (size_t)vlen + 1;
        break;
    default:
        return NULL;
    }
    propt = _mqtt_prop_mem(blk, sizeof(mqtt_propertie) + extra);
    ZERO(propt, sizeof(mqtt_propertie));
    propt->flag = flag;
    propt->nval = nval;
    if (klen >= 0) {
        kv = br->data + br->offset;
        memcpy(propt->fval, kv, (size_t)klen);
        propt->fval[klen] = '\0';
        propt->flens = (size_t)klen;
        if (vlen >= 0) {
            propt->sval = propt->fval + klen + 1;
            memcpy(propt->sval, kv + klen + 2, (size_t)vlen);
            propt->sval[vlen] = '\0';
            propt->slens = (size_t)vlen;
            binary_get_skip(br, (size_t)klen + 2 + (size_t)vlen);
        } else {
            binary_get_skip(br, (size_t)klen);
        }
    }
    return propt;
}
// 解析出错时丢弃属性块：是调用方预留的 pre 就只放它另开的部分，块本身归调用方
static void _mqtt_prop_blk_drop(mqtt_prop_blk *blk, mqtt_prop_blk *pre) {
    if (blk == pre) {
        _mqtt_prop_blk_release(&blk->arr);
    } else {
        _mqtt_prop_blk_free(&blk->arr);
    }
}
// 属性解析。maxlens 传本报文的 fixhead.remaining_lens——属性段是报文的一部分,界要按本报文取,
// 不能按整个接收缓冲。只按 id 决定读几个字节,两件事未查:同一属性重复出现、属性 id 与当前
// 报文类型不匹配(按 MQTT-5.0 §2.2.2.2 两者都算 Protocol Error)。数组按 wire 顺序原样交上层,
// 重复属性会出现多个同 id 元素,上层若只读先遇到的那个,取到的可能不是对端的本意。
// pre 非 NULL 是调用方预留好的属性块(cap 已填)，装得下就用它，否则照常另开
static mprop_arr *_mqtt_properties(binary_ctx *br, int32_t *status, int32_t *total, size_t maxlens,
                                   mqtt_prop_blk *pre) {
    int32_t plens;
    int32_t occupy = _mqtt_data_varnum(br, &plens);//属性长度
    if (ERR_FAILED == occupy
        || (size_t)plens > maxlens
        || !binary_have(br, (uint64_t)plens)) {
        BIT_SET(*status, PROT_ERROR);
        return NULL;
    }
    SET_PTR(total, occupy + plens);
    if (0 == plens) {
        return NULL;
    }
    size_t end = br->offset + (size_t)plens;
    mqtt_propertie *propt;
    mqtt_prop_blk *blk;
    mqtt_propertie **p;
    uint32_t cap = _mqtt_prop_cap(plens);
    if (NULL != pre
        && cap <= pre->cap) {
        blk = pre;
    } else {
        MALLOC(blk, sizeof(mqtt_prop_blk) + cap);
        blk->cap = cap;
    }
    blk->arr.size = 0;
    blk->arr.maxsize = MQTT_PROP_SLOTS;
    blk->arr.ptr = blk->slots;
    blk->used = 0;
    while (br->offset < end) {
        propt = _mqtt_prop_one(br, blk);
        if (NULL == propt) {
            BIT_SET(*status, PROT_ERROR);
            _mqtt_prop_blk_drop(blk, pre);
            return NULL;
        }
        if (blk->arr.size == blk->arr.maxsize
            && blk->arr.ptr == blk->slots) {
            MALLOC(p, sizeof(mqtt_propertie *) * MQTT_PROP_SLOTS * 2);
            memcpy(p, blk->slots, sizeof(blk->slots));
            blk->arr.ptr = p;
            blk->arr.maxsize = MQTT_PROP_SLOTS * 2;
        }
        mprop_arr_push_back(&blk->arr, &propt);
    }
    if (br->offset != end) {
        BIT_SET(*status, PROT_ERROR);
        _mqtt_prop_blk_drop(blk, pre);
        return NULL;
    }
    return &blk->arr;
}
// CONNECT 载荷里 clientid / 遗嘱主题 / 遗嘱载荷 / 用户名 / 密码这 5 个串顺序排进载荷结构体后面那块，*cur 为下一个串的起点；
// 块按本包剩余字节 + 5 个 NUL 开，装得下。密码排在最后，释放时擦到它为止(见 _mqtt_connect_payload_free)
static inline char *_mqtt_connect_str(binary_ctx *br, char **cur, int32_t *num, int32_t utf8) {
    char *dst = *cur;
    if (ERR_OK != _mqtt_data_lens(br, num)
        || ERR_OK != _mqtt_data_str_to(br, dst, *num, utf8)) {
        return NULL;
    }
    *cur = dst + *num + 1;
    return dst;
}
// 验证 CONNECT 报文中的协议名和协议版本，成功返回版本号，失败返回 ERR_FAILED
static inline int32_t _mqtt_check_prot(binary_ctx *br) {
    int32_t num;
    if (ERR_OK != _mqtt_data_fixnum(br, 2, &num)) {//协议名长度
        return ERR_FAILED;
    }
    if (4 != num) {
        return ERR_FAILED;
    }
    if (!binary_have(br, (size_t)num)) {//协议名
        return ERR_FAILED;
    }
    if (0 != memcasecmp(binary_get_binary(br, (size_t)num), "mqtt", num)) {
        return ERR_FAILED;
    }
    if (!binary_have(br, 1)) {//协议级别
        return ERR_FAILED;
    }
    int8_t ver = binary_get_int8(br);
    if (MQTT_311 != ver && MQTT_50 != ver) {
        return ERR_FAILED;
    }
    return ver;
}
//客户端到服务端  客户端请求连接服务端
static int32_t _mqtt_connect(mqtt_pack_ctx *pack, int32_t client, binary_ctx *br, ud_cxt *ud, int32_t *status) {
    if (client
        || 0 != pack->fixhead.flags) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    if (NULL != ud->context) {
        LOG_WARN("mqtt connect on a connection that already has a context, mqtt_ws_bind is for the client direction only.");
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    //可变报头 协议名（Protocol Name），协议级别（Protocol Level），连接标志（Connect Flags），保持连接（Keep Alive）,
    //属性（Properties MQTT_50）
    mqtt_connect_varhead *vh = (mqtt_connect_varhead *)(pack + 1);
    pack->varhead = vh;
    vh->version = _mqtt_check_prot(br);
    if (ERR_FAILED == vh->version) {//协议名 协议级别 检查
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    pack->version = vh->version;
    int32_t num;
    if (ERR_OK != _mqtt_data_fixnum(br, 1, &num)) {//连接标志
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
    vh->willqos |= (BIT_GETN(num, 4) << 1);//遗嘱服务质量 2位最大值3
    vh->willretain = BIT_GETN(num, 5);//遗嘱保留标志
    if ((0 == vh->willflag && (0 != vh->willqos || 0 != vh->willretain))
        || (0 != vh->willflag && 3 == vh->willqos)) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    vh->passwordflag = BIT_GETN(num, 6);//密码标志
    vh->userflag = BIT_GETN(num, 7);//用户名标志
    if (vh->version < MQTT_50
        && 0 == vh->userflag
        && 0 != vh->passwordflag) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    if (ERR_OK != _mqtt_data_fixnum(br, 2, &num)) {//保持连接
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    vh->keepalive = (uint16_t)num;
    if (vh->version >= MQTT_50) {
        vh->properties = _mqtt_properties(br, status, NULL, pack->fixhead.remaining_lens, NULL);//属性
        if (NULL == vh->properties
            && BIT_CHECK(*status, PROT_ERROR)) {
            return ERR_FAILED;
        }
    }
    //载荷 客户标识符（Client Identifier）、遗嘱属性（Will Properties MQTT_50）、遗嘱主题（Will Topic）、遗嘱载荷（Will Payload）、
    //用户名（User Name）、密码（Password）
    mqtt_connect_payload *pl;
    MALLOC(pl, sizeof(mqtt_connect_payload) + binary_remain(br) + 5);
    ZERO(pl, sizeof(mqtt_connect_payload));
    pack->payload = pl;
    char *cur = (char *)(pl + 1);
    pl->clientid = _mqtt_connect_str(br, &cur, &num, 1);//客户标识符
    if (NULL == pl->clientid) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    if (vh->willflag) {
        if (vh->version >= MQTT_50) {
            pl->properties = _mqtt_properties(br, status, NULL, pack->fixhead.remaining_lens, NULL);//属性
            if (NULL == pl->properties
                && BIT_CHECK(*status, PROT_ERROR)) {
                return ERR_FAILED;
            }
        }
        pl->willtopic = _mqtt_connect_str(br, &cur, &num, 1);//遗嘱主题
        if (NULL == pl->willtopic) {
            BIT_SET(*status, PROT_ERROR);
            return ERR_FAILED;
        }
        pl->willpayload = _mqtt_connect_str(br, &cur, &num, 0);//遗嘱载荷
        if (NULL == pl->willpayload) {
            BIT_SET(*status, PROT_ERROR);
            return ERR_FAILED;
        }
        pl->wplens = num;
    }
    if (vh->userflag) {
        pl->user = _mqtt_connect_str(br, &cur, &num, 1);
        if (NULL == pl->user) {
            BIT_SET(*status, PROT_ERROR);
            return ERR_FAILED;
        }
    }
    if (vh->passwordflag) {
        pl->password = _mqtt_connect_str(br, &cur, &num, 0);
        if (NULL == pl->password) {
            BIT_SET(*status, PROT_ERROR);
            return ERR_FAILED;
        }
        pl->pslens = num;
    }
    mqtt_ctx *mq = mqtt_ctx_new((mqtt_protversion)vh->version);
    if (NULL == mq) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    ud->context = mq;
    ud->status = COMMAND;
    return ERR_OK;
}
//服务端到客户端  连接报文确认
static int32_t _mqtt_connack(mqtt_pack_ctx *pack, int32_t client, binary_ctx *br, ud_cxt *ud, int32_t *status) {
    if (!client
        || 0 != pack->fixhead.flags) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    if (NULL == ud->context) {
        LOG_WARN("mqtt connack without context, mqtt over ws client must inject one by mqtt_ws_bind.");
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    //可变报头 连接确认标志（Connect Acknowledge Flags），连接原因码（Reason Code），属性（Properties MQTT_50）
    int32_t num;
    if (ERR_OK != _mqtt_data_fixnum(br, 1, &num)) {//连接确认标志
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    if (0 != (num >> 1)) {//位7-1是保留位且必须设置为0
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    mqtt_connack_varhead *vh = (mqtt_connack_varhead *)(pack + 1);
    pack->varhead = vh;
    vh->sesspresent = BIT_GETN(num, 0);//会话存在
    if (ERR_OK != _mqtt_data_fixnum(br, 1, &num)) {//连接原因码
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    vh->reason = (uint8_t)num;
    pack->version = ((mqtt_ctx *)ud->context)->version;
    if (pack->version >= MQTT_50) {
        vh->properties = _mqtt_properties(br, status, NULL, pack->fixhead.remaining_lens, NULL);//属性
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
// PUBLISH 那一整块的大小：pack / varhead / [v5 属性块 prelens] / topic / 载荷合在一起，偏移由 _mqtt_publish 边解析边定。
// 容量按上界给——topic 与载荷之和不超过 remaining_lens，MQTT_PUB_SLACK 覆盖两个结尾 NUL 与对齐补白。
// 分配点与块内越界断言共用这一处，两边各抄一份算式的话，改一边漏一边编译器看不出来
static inline size_t _mqtt_publish_blk(size_t remaining_lens, size_t prelens) {
    return MQTT_PUB_PREOFF + prelens + sizeof(mqtt_publish_payload) + remaining_lens + MQTT_PUB_SLACK;
}
// v5 PUBLISH 要给属性块预留的字节数(0 为不留)。先偷看属性段长(主题长 + 主题 + 报文标识符之后那个 varint)，
// 只为定块长、不判错：读不出或越界就不留，错由正式解析报。并进去会让块跨过 MQTT_PUB_MERGE_LIM 而原先不跨时也不留，
// 最小的属性块都会跨过去的先判掉，不必偷看
static size_t _mqtt_publish_prelens(binary_ctx *br, size_t fhlens, size_t remaining_lens) {
    binary_ctx view = *br;
    size_t off = fhlens + 2;
    size_t base = _mqtt_publish_blk(remaining_lens, 0);
    size_t lens;
    int32_t plens;
    if (off > br->size
        || (base <= MQTT_PUB_MERGE_LIM && base + sizeof(mqtt_prop_blk) + MQTT_PROP_ARENA > MQTT_PUB_MERGE_LIM)) {
        return 0;
    }
    off += (size_t)read_be16(br->data + fhlens);
    if (0 != ((uint8_t)br->data[0] & 0x06)) {
        off += 2;
    }
    if (off >= br->size) {
        return 0;
    }
    view.offset = off;
    if (ERR_FAILED == _mqtt_data_varnum(&view, &plens)
        || 0 == plens
        || !binary_have(&view, (uint64_t)plens)) {
        return 0;
    }
    lens = sizeof(mqtt_prop_blk) + _mqtt_prop_cap(plens);
    if (base <= MQTT_PUB_MERGE_LIM
        && base + lens > MQTT_PUB_MERGE_LIM) {
        return 0;
    }
    return lens;
}
//两个方向都允许  发布消息。spill 非 NULL 时读区只到载荷起点，载荷从 spill 里直接拷进块内(见 _mqtt_publish_hlens)，
//读区记账照样走过这段载荷。pre 非 NULL 是 _mqtt_parse 在块内给属性预留的块(见 _mqtt_publish_prelens)
static int32_t _mqtt_publish(mqtt_pack_ctx *pack, binary_ctx *br, buffer_ctx *spill, mqtt_prop_blk *pre,
                             int32_t *status) {
    // 布局见 _mqtt_publish_blk：这里只按解析顺序定块内偏移，不再各自 malloc。
    // pack 头与 varhead 已由 _mqtt_parse 清零；载荷区不清(随即被写满)，载荷头与结尾 NUL 在下面补
    size_t prelens = (NULL == pre) ? 0 : sizeof(mqtt_prop_blk) + pre->cap;
    mqtt_publish_varhead *vh = (mqtt_publish_varhead *)((char *)pack + sizeof(mqtt_pack_ctx));
    pack->varhead = vh;
    char *slot = (char *)pack + MQTT_PUB_PREOFF + prelens;
    //可变报头 主题名（Topic Name），报文标识符（Packet Identifier），属性（Properties MQTT_50）
    int32_t num;
    if (ERR_OK != _mqtt_data_fixnum(br, 2, &num)//主题名长度
        || num < 0
        || num > (int32_t)pack->fixhead.remaining_lens) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    if (ERR_OK != _mqtt_data_copy(br, slot, (size_t)num)) {//主题名就地读进块内
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    // 主题名也是 UTF-8 字段, 内嵌 NUL 同样得拒, 理由见 _mqtt_data_str_to;
    // 它不走那个读取器(就地读进块内), 得在这自己判
    if (NULL != memchr(slot, '\0', (size_t)num)) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    slot[num] = '\0';
    vh->topic = slot;
    slot += num + 1;
    int32_t off = (2 + num);//主题名(2 + 主题名长度)
    //解析固定报头标志
    vh->retain = BIT_GETN(pack->fixhead.flags, 0);
    vh->qos = BIT_GETN(pack->fixhead.flags, 1);
    vh->qos |= (BIT_GETN(pack->fixhead.flags, 2) << 1);
    vh->dup = BIT_GETN(pack->fixhead.flags, 3);
    if (3 == vh->qos) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    if (1 == vh->qos
        || 2 == vh->qos) {//只有当QoS等级是1或2时，报文标识符字段才能出现在报文中
        if (ERR_OK != _mqtt_data_fixnum(br, 2, &num)) {//报文标识符
            BIT_SET(*status, PROT_ERROR);
            return ERR_FAILED;
        }
        off += 2;
        vh->packid = (uint16_t)num;
    }
    if (pack->version >= MQTT_50) {
        vh->properties = _mqtt_properties(br, status, &num, pack->fixhead.remaining_lens, pre);//属性
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
    // 载荷头首字段是 int32_t，要 4 字节对齐；topic 长度任意，故按块内偏移补齐。
    // pack 来自 MALLOC（最大对齐），加 4 的倍数仍是 4 对齐
    size_t ploff = ROUND_UP((size_t)(slot - (char *)pack), sizeof(int32_t));
    // 块够不够是跨 _mqtt_parse 与本函数的不变式，钉一道
    ASSERTAB(ploff + sizeof(mqtt_publish_payload) + (size_t)remain + 1
             <= _mqtt_publish_blk(pack->fixhead.remaining_lens, prelens),
             "publish block overflow.");
    mqtt_publish_payload *pl = (mqtt_publish_payload *)((char *)pack + ploff);
    pack->payload = pl;
    pl->lens = 0;
    pl->content[remain] = '\0';
    if (0 == remain) {
        return ERR_OK;
    }
    if (NULL != spill) {
        if (br->offset != br->size) {
            BIT_SET(*status, PROT_ERROR);
            return ERR_FAILED;
        }
        ASSERTAB((size_t)remain == buffer_copyout(spill, br->size, pl->content, (size_t)remain), "copy buffer failed.");
        br->size += (size_t)remain;
        br->offset = br->size;
    } else if (ERR_OK != _mqtt_data_copy(br, pl->content, (size_t)remain)) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    pl->lens = remain;
    return ERR_OK;
}
static int32_t _mqtt_pubackrel_common(mqtt_pack_ctx *pack, binary_ctx *br,
                                      int32_t *status, uint8_t expected_flags) {
    if (expected_flags != pack->fixhead.flags) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    //可变报头 报文标识符，[原因码(MQTT_50)，属性(MQTT_50)]
    int32_t num;
    if (ERR_OK != _mqtt_data_fixnum(br, 2, &num)) {//报文标识符
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    mqtt_pubackrel_varhead *vh = (mqtt_pubackrel_varhead *)(pack + 1);
    pack->varhead = vh;
    vh->packid = (uint16_t)num;
    if (pack->version < MQTT_50
        || 2 == pack->fixhead.remaining_lens) {//剩余长度为2，则表示使用原因码0x00（成功）
        return ERR_OK;
    }
    //MQTT v5.0 §3.4.2.2.1：剩余长度<4 时不存在 Property Length 字段，properties 默认按 0 处理。
    //因此 remaining_lens==3 仅读 reason 不读 properties，与编码端简化形式严格对称。
    //PUBREC/PUBREL/PUBCOMP 同等规则（§3.5/3.6/3.7.2.2.1），DISCONNECT 见 §3.14.2.2.1。
    if (pack->fixhead.remaining_lens >= 3) {
        if (ERR_OK != _mqtt_data_fixnum(br, 1, &num)) {//原因码
            BIT_SET(*status, PROT_ERROR);
            return ERR_FAILED;
        }
        vh->reason = (uint8_t)num;
    }
    if (pack->fixhead.remaining_lens >= 4) {
        vh->properties = _mqtt_properties(br, status, NULL, pack->fixhead.remaining_lens, NULL);//属性
        if (NULL == vh->properties
            && BIT_CHECK(*status, PROT_ERROR)) {
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
// SUBSCRIBE / SUBACK / UNSUBSCRIBE / UNSUBACK 四个报文共用的可变报头：
// 方向与 flags 校验 → 报文标识符 → 建 varhead → MQTT_50 才读属性 → 算出载荷剩余长度。
// expclient 非 0 表示这个报文只该由客户端收到（服务端发来的确认），为 0 则只该由服务端收到。
// *remain 可能 <= 0，由调用方按各自协议判定是否合法（3.1.1 的 UNSUBACK 就是 0）
static int32_t _mqtt_subunsub_varhead(mqtt_pack_ctx *pack, int32_t client, int32_t expclient,
                                      int32_t expflags, binary_ctx *br, int32_t *status,
                                      int32_t *remain) {
    if ((0 != client) != (0 != expclient)
        || expflags != pack->fixhead.flags) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    int32_t num;
    if (ERR_OK != _mqtt_data_fixnum(br, 2, &num)) {//报文标识符
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    mqtt_subreqresp_varhead *vh = (mqtt_subreqresp_varhead *)(pack + 1);
    pack->varhead = vh;
    vh->packid = (uint16_t)num;
    num = 0;
    if (pack->version >= MQTT_50) {
        vh->properties = _mqtt_properties(br, status, &num, pack->fixhead.remaining_lens, NULL);//属性
        if (NULL == vh->properties
            && BIT_CHECK(*status, PROT_ERROR)) {
            return ERR_FAILED;
        }
    }
    *remain = (int32_t)pack->fixhead.remaining_lens - 2 - num;
    return ERR_OK;
}
// 非 PUBLISH 包那一整块的大小：pack 头 + 可变报头槽，SUBACK / UNSUBACK 再加原因码表(不超过 remaining_lens)。
// 分配点(_mqtt_parse)与 _mqtt_reasonlist 的越界断言共用这一处
static inline size_t _mqtt_vh_blk(int32_t prot, size_t remaining_lens) {
    size_t lens = sizeof(mqtt_pack_ctx) + sizeof(mqtt_vh_slot);
    if (MQTT_SUBACK == prot
        || MQTT_UNSUBACK == prot) {
        lens += sizeof(mqtt_reasonlist_payload) + remaining_lens;
    }
    return lens;
}
// SUBACK / UNSUBACK 的载荷：长度为 remain 的一串原因码，放在 pack 块里可变报头之后(块长见 _mqtt_vh_blk)
static int32_t _mqtt_reasonlist(mqtt_pack_ctx *pack, binary_ctx *br, int32_t *status, int32_t remain) {
    if (remain <= 0) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    size_t ploff = sizeof(mqtt_pack_ctx) + sizeof(mqtt_vh_slot);
    ASSERTAB(ploff + sizeof(mqtt_reasonlist_payload) + (size_t)remain
             <= _mqtt_vh_blk(pack->fixhead.prot, pack->fixhead.remaining_lens),
             "reason list block overflow.");
    mqtt_reasonlist_payload *pl = (mqtt_reasonlist_payload *)((char *)pack + ploff);
    pack->payload = pl;
    pl->rlens = remain;
    if (ERR_OK != _mqtt_data_copy(br, pl->reasons, (size_t)remain)) {//原因码列表
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    return ERR_OK;
}
// 数出载荷里结构上放得下的条目数(每条 = 2 字节长度 + 主题 + tail 字节选项，整条不越出 remain)与主题总长。
// 只看结构不判合法：逐条解析的界与这里相同(读区恰好止于 remain)，推进到的条目只会是这里数到的前缀
static int32_t _mqtt_topic_count(binary_ctx *br, int32_t remain, int32_t tail, size_t *tlens) {
    const uint8_t *p = (const uint8_t *)br->data + br->offset;
    int32_t off = 0;
    int32_t cnt = 0;
    int32_t num;
    *tlens = 0;
    while (off + 2 <= remain) {
        num = (int32_t)read_be16(p + off);
        if (off + 2 + num + tail > remain) {
            break;
        }
        off += 2 + num + tail;
        *tlens += (size_t)num;
        cnt++;
    }
    return cnt;
}
//客户端到服务端  客户端订阅请求。载荷一块：载荷头、指针槽、各订阅项、各主题串依次排开，条目数先由 _mqtt_topic_count 数出；
//逐条先卡整条(含选项字节)放得下再拷主题，与计数同界，块才不会写出界
static int32_t _mqtt_subscribe(mqtt_pack_ctx *pack, int32_t client, binary_ctx *br, int32_t *status) {
    int32_t remain;
    if (ERR_OK != _mqtt_subunsub_varhead(pack, client, 0, 0x02, br, status, &remain)) {
        return ERR_FAILED;
    }
    if (remain <= 0) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    //载荷
    char *topic;
    int32_t num;
    int32_t opt;
    int32_t off;
    size_t tlens;
    subscribe_option *subop, *opts;
    mqtt_subscribe_payload *pl;
    int32_t cnt = _mqtt_topic_count(br, remain, 1, &tlens);
    MALLOC(pl, sizeof(mqtt_subscribe_payload)
        + (sizeof(subscribe_option *) + sizeof(subscribe_option)) * (size_t)cnt + tlens + (size_t)cnt);
    pack->payload = pl;
    pl->subop.size = 0;
    pl->subop.maxsize = (uint32_t)cnt;
    pl->subop.ptr = (subscribe_option **)(pl + 1);
    opts = (subscribe_option *)(pl->subop.ptr + cnt);
    topic = (char *)(opts + cnt);
    for (off = 0; off < remain;) {
        if (ERR_OK != _mqtt_data_lens(br, &num)//主题
            || !binary_have(br, (uint64_t)num + 1)) {
            BIT_SET(*status, PROT_ERROR);
            return ERR_FAILED;
        }
        ASSERTAB(pl->subop.size < pl->subop.maxsize, "subscribe block overflow.");
        if (ERR_OK != _mqtt_data_str_to(br, topic, num, 1)) {
            BIT_SET(*status, PROT_ERROR);
            return ERR_FAILED;
        }
        off += (2 + num);
        if (ERR_OK != _mqtt_data_fixnum(br, 1, &opt)) {//订阅选项
            BIT_SET(*status, PROT_ERROR);
            return ERR_FAILED;
        }
        off++;
        if (3 == (opt & 0x03)
            || (pack->version >= MQTT_50 && (3 == ((opt >> 4) & 0x03) || 0 != (opt & 0xC0)))
            || (pack->version < MQTT_50 && 0 != (opt & 0xFC))) {
            BIT_SET(*status, PROT_ERROR);
            return ERR_FAILED;
        }
        subop = opts + pl->subop.size;
        ZERO(subop, sizeof(subscribe_option));
        subop->topic = topic;
        subop->qos = BIT_GETN(opt, 0);//订阅选项 解析
        subop->qos |= (BIT_GETN(opt, 1) << 1);
        if (pack->version >= MQTT_50) {
            subop->nl = BIT_GETN(opt, 2);
            subop->rap = BIT_GETN(opt, 3);
            subop->retain = BIT_GETN(opt, 4);
            subop->retain |= (BIT_GETN(opt, 5) << 1);
        }
        pl->subop.ptr[pl->subop.size++] = subop;
        topic += num + 1;
    }
    if (off != remain) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    return ERR_OK;
}
//服务端到客户端  订阅请求报文确认
static int32_t _mqtt_suback(mqtt_pack_ctx *pack, int32_t client, binary_ctx *br, int32_t *status) {
    int32_t remain;
    if (ERR_OK != _mqtt_subunsub_varhead(pack, client, 1, 0, br, status, &remain)) {
        return ERR_FAILED;
    }
    return _mqtt_reasonlist(pack, br, status, remain);
}
//客户端到服务端  客户端取消订阅请求。载荷一块：载荷头、指针槽、各主题串依次排开，条目数先由 _mqtt_topic_count 数出
static int32_t _mqtt_unsubscribe(mqtt_pack_ctx *pack, int32_t client, binary_ctx *br, int32_t *status) {
    int32_t remain;
    if (ERR_OK != _mqtt_subunsub_varhead(pack, client, 0, 0x02, br, status, &remain)) {
        return ERR_FAILED;
    }
    if (remain <= 0) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    //载荷
    char *topic;
    int32_t num;
    int32_t off;
    size_t tlens;
    mqtt_unsubscribe_payload *pl;
    int32_t cnt = _mqtt_topic_count(br, remain, 0, &tlens);
    MALLOC(pl, sizeof(mqtt_unsubscribe_payload) + sizeof(char *) * (size_t)cnt + tlens + (size_t)cnt);
    pack->payload = pl;
    pl->topics.size = 0;
    pl->topics.maxsize = (uint32_t)cnt;
    pl->topics.ptr = (char **)(pl + 1);
    topic = (char *)(pl->topics.ptr + cnt);
    for (off = 0; off < remain;) {
        if (ERR_OK != _mqtt_data_lens(br, &num)
            || off + 2 + num > remain) {
            BIT_SET(*status, PROT_ERROR);
            return ERR_FAILED;
        }
        ASSERTAB(pl->topics.size < pl->topics.maxsize, "unsubscribe block overflow.");
        if (ERR_OK != _mqtt_data_str_to(br, topic, num, 1)) {//主题
            BIT_SET(*status, PROT_ERROR);
            return ERR_FAILED;
        }
        off += (2 + num);
        pl->topics.ptr[pl->topics.size++] = topic;
        topic += num + 1;
    }
    if (off != remain) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    return ERR_OK;
}
//服务端到客户端  取消订阅确认
static int32_t _mqtt_unsuback(mqtt_pack_ctx *pack, int32_t client, binary_ctx *br, int32_t *status) {
    int32_t remain;
    if (ERR_OK != _mqtt_subunsub_varhead(pack, client, 1, 0, br, status, &remain)) {
        return ERR_FAILED;
    }
    // 3.1.1 的 UNSUBACK 只有报文标识符，没有原因码列表（SUBACK 那边 3.1.1 是有返回码的）
    if (pack->version < MQTT_50) {
        return ERR_OK;
    }
    return _mqtt_reasonlist(pack, br, status, remain);
}
//客户端到服务端  心跳请求
static int32_t _mqtt_ping(mqtt_pack_ctx *pack, int32_t client, int32_t *status) {
    if (client
        || 0 != pack->fixhead.flags) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    return ERR_OK;
}
//服务端到客户端  心跳响应
static int32_t _mqtt_pong(mqtt_pack_ctx *pack, int32_t client, int32_t *status) {
    if (!client
        || 0 != pack->fixhead.flags) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    return ERR_OK;
}
//两个方向都允许  断开连接通知
static int32_t _mqtt_disconnect(mqtt_pack_ctx *pack, binary_ctx *br, int32_t *status) {
    if (0 != pack->fixhead.flags) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    mqtt_reason_varhead *vh = (mqtt_reason_varhead *)(pack + 1);
    pack->varhead = vh;
    BIT_SET(*status, PROT_CLOSE);
    if (pack->version < MQTT_50
        || 0 == pack->fixhead.remaining_lens) {//如果剩余长度小于1，则表示使用原因码0x00（正常断开）. 如果剩余长度小于2，属性长度使用0。
        return ERR_OK;
    }
    //可变报头
    int32_t num;
    if (ERR_OK != _mqtt_data_fixnum(br, 1, &num)) {//断开原因码
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    vh->reason = (uint8_t)num;
    if (pack->fixhead.remaining_lens > 1) {
        vh->properties = _mqtt_properties(br, status, NULL, pack->fixhead.remaining_lens, NULL);//属性
        if (NULL == vh->properties
            && BIT_CHECK(*status, PROT_ERROR)) {
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
//两个方向都允许  认证信息交换
static int32_t _mqtt_auth(mqtt_pack_ctx *pack, binary_ctx *br, ud_cxt *ud, int32_t *status) {
    if (0 != pack->fixhead.flags) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    if (NULL == ud->context) {
        // 客户端在 CONNACK 之前收到 AUTH（如服务端先发挑战），mqtt_ctx 尚未挂上
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    pack->version = ((mqtt_ctx *)ud->context)->version;
    if (pack->version < MQTT_50) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    //如果原因码为0x00（成功）并且没有属性字段，则可以省略原因码和属性长度。这种情况下，AUTH报文剩余长度为0。
    if (0 == pack->fixhead.remaining_lens) {
        mqtt_reason_varhead *vh = (mqtt_reason_varhead *)(pack + 1);
        pack->varhead = vh;
        return ERR_OK;
    }
    //可变报头
    int32_t num;
    if (ERR_OK != _mqtt_data_fixnum(br, 1, &num)) {//认证原因码
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    mqtt_reason_varhead *vh = (mqtt_reason_varhead *)(pack + 1);
    pack->varhead = vh;
    vh->reason = (uint8_t)num;
    if (pack->fixhead.remaining_lens > 1) {
        vh->properties = _mqtt_properties(br, status, NULL, pack->fixhead.remaining_lens, NULL);
        if (NULL == vh->properties
            && BIT_CHECK(*status, PROT_ERROR)) {
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
// 连接握手阶段分发：处理 CONNECT / CONNACK / AUTH 报文
static int32_t _mqtt_init(mqtt_pack_ctx *pack, int32_t client, binary_ctx *br, ud_cxt *ud, int32_t *status) {
    int32_t rtn = ERR_FAILED;
    switch (pack->fixhead.prot) {
    case MQTT_CONNECT:
        rtn = _mqtt_connect(pack, client, br, ud, status);
        break;
    case MQTT_CONNACK:
        rtn = _mqtt_connack(pack, client, br, ud, status);
        break;
    case MQTT_AUTH:
        rtn = _mqtt_auth(pack, br, ud, status);
        break;
    default:
        BIT_SET(*status, PROT_ERROR);
        break;
    }
    return rtn;
}
// 命令阶段分发：处理 PUBLISH / PUB* / SUBSCRIBE / UNSUBSCRIBE / PING / DISCONNECT / AUTH 报文；spill 与 pre 只给 PUBLISH
static int32_t _mqtt_commands(mqtt_pack_ctx *pack, int32_t client, binary_ctx *br, buffer_ctx *spill,
                              mqtt_prop_blk *pre, ud_cxt *ud, int32_t *status) {
    int32_t rtn = ERR_FAILED;
    // 版本在这里统一填，不由各 handler 自己抄：_mqtt_parse 分配时包头已清零，漏抄一处就静默得
    // version==0、下游按 3.1.1 处理而不报错。connack 不走这里(它正是确立版本的那条)；
    // auth 两条路都能到，它自己那份保留，两处赋的是同一个值
    pack->version = ((mqtt_ctx *)ud->context)->version;
    switch (pack->fixhead.prot) {
    case MQTT_PUBLISH:
        rtn = _mqtt_publish(pack, br, spill, pre, status);
        break;
    //两个方向都允许：PUBACK(QoS1 确认)、PUBREC(QoS2 第一步)、PUBCOMP(QoS2 第三步)期望 flags 为 0
    case MQTT_PUBACK:
    case MQTT_PUBREC:
    case MQTT_PUBCOMP:
        rtn = _mqtt_pubackrel_common(pack, br, status, 0);
        break;
    //PUBREL(QoS2 第二步)的 3，2，1，0 位是保留位且必须分别为 0，0，1，0
    case MQTT_PUBREL:
        rtn = _mqtt_pubackrel_common(pack, br, status, 0x02);
        break;
    case MQTT_SUBSCRIBE:
        rtn = _mqtt_subscribe(pack, client, br, status);
        break;
    case MQTT_SUBACK:
        rtn = _mqtt_suback(pack, client, br, status);
        break;
    case MQTT_UNSUBSCRIBE:
        rtn = _mqtt_unsubscribe(pack, client, br, status);
        break;
    case MQTT_UNSUBACK:
        rtn = _mqtt_unsuback(pack, client, br, status);
        break;
    case MQTT_PINGREQ:
        rtn = _mqtt_ping(pack, client, status);
        break;
    case MQTT_PINGRESP:
        rtn = _mqtt_pong(pack, client, status);
        break;
    case MQTT_DISCONNECT:
        rtn = _mqtt_disconnect(pack, br, status);
        break;
    case MQTT_AUTH:
        rtn = _mqtt_auth(pack, br, ud, status);
        break;
    default:
        BIT_SET(*status, PROT_ERROR);
        break;
    }
    return rtn;
}
// 在读区上解一整包。读区 br 是本包那段连续内存(首节点里的本包或拷出的本包)，越界判定一律按本包；
// spill 非 NULL 时读区只拷到 PUBLISH 载荷起点，载荷还在 spill 里(见 _mqtt_publish_hlens)。
// fhlens 为固定头长度。解到哪算到哪：返回后 br->offset 即该从接收缓冲排掉的字节数，成败都是
static mqtt_pack_ctx *_mqtt_parse(int32_t client, binary_ctx *br, buffer_ctx *spill, ud_cxt *ud, int32_t *status,
                                  size_t fhlens, size_t remaining_lens) {
    uint8_t val = (uint8_t)br->data[0];
    mqtt_pack_ctx *pack;
    mqtt_prop_blk *pre = NULL;
    size_t prelens = 0;
    if (MQTT_PUBLISH == (val >> 4)
        && COMMAND == ud->status) {
        if (((mqtt_ctx *)ud->context)->version >= MQTT_50) {
            prelens = _mqtt_publish_prelens(br, fhlens, remaining_lens);
        }
        MALLOC(pack, _mqtt_publish_blk(remaining_lens, prelens));
        ZERO(pack, sizeof(mqtt_pack_ctx) + sizeof(mqtt_publish_varhead));
        if (0 != prelens) {
            pre = (mqtt_prop_blk *)((char *)pack + MQTT_PUB_PREOFF);
            pre->cap = (uint32_t)(prelens - sizeof(mqtt_prop_blk));
        }
    } else {
        MALLOC(pack, _mqtt_vh_blk(val >> 4, remaining_lens));
        ZERO(pack, sizeof(mqtt_pack_ctx) + sizeof(mqtt_vh_slot));
    }
    pack->fixhead.remaining_lens = remaining_lens;
    pack->fixhead.prot = (val >> 4);
    pack->fixhead.flags = (val & 0x0F);
    binary_get_skip(br, fhlens);
    int32_t rtn = ERR_FAILED;
    switch (ud->status) {
    case INIT:
        rtn = _mqtt_init(pack, client, br, ud, status);
        break;
    case COMMAND:
        rtn = _mqtt_commands(pack, client, br, spill, pre, ud, status);
        break;
    default:
        //ud->status 当前仅在本文件内被赋值为 INIT/COMMAND，理论不可达；
        //此分支用于防御未来扩展或外部破坏场景，确保不会静默吞帧。
        BIT_SET(*status, PROT_ERROR);
        break;
    }
    if (ERR_OK != rtn) {
        _mqtt_pkfree(pack);
        return NULL;
    }
    if (fhlens + remaining_lens != br->offset) {
        BIT_SET(*status, PROT_ERROR);
        _mqtt_pkfree(pack);
        return NULL;
    }
    return pack;
}
// 跨节点的命令阶段 PUBLISH 只拷出载荷之前的头部，载荷由 _mqtt_publish 直接从接收缓冲拷进块内，省掉整包那次拷贝。
// 按固定头的 QoS 位(非 0 才有报文标识符)、主题名长度与 v5 属性长度算出载荷起点；不是这类包、
// 或算出来没有载荷/越界，返回 0 走整包拷出，出错口径不变
static size_t _mqtt_publish_hlens(buffer_ctx *buf, ud_cxt *ud, size_t fhlens, size_t total) {
    uint8_t val = (uint8_t)buffer_at(buf, 0);
    char two[2];
    size_t hlens, plens;
    int32_t occ;
    if (MQTT_PUBLISH != (val >> 4)
        || COMMAND != ud->status
        || fhlens + sizeof(two) > total) {
        return 0;
    }
    ASSERTAB(sizeof(two) == buffer_copyout(buf, fhlens, two, sizeof(two)), "copy buffer failed.");
    hlens = fhlens + sizeof(two) + (size_t)read_be16(two);
    if (0 != (val & 0x06)) {
        hlens += 2;
    }
    if (((mqtt_ctx *)ud->context)->version >= MQTT_50) {
        occ = varint_decode_mqtt(buf, hlens, total, &plens);
        if (ERR_FAILED == occ) {
            return 0;
        }
        hlens += (size_t)occ + plens;
    }
    return hlens < total ? hlens : 0;
}
void *mqtt_unpack(ev_ctx *ev, sock_ctx *sk, int32_t client,
    buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status) {
    (void)ev; (void)sk; (void)size;
    size_t blens = buffer_size(buf);
    if (blens < 2) {//固定头至少2字节
        BIT_SET(*status, PROT_MOREDATA);
        return NULL;
    }
    char head[MQTT_SPLIT_HEAD];
    char *tmp = NULL;
    buffer_ctx *spill = NULL;
    size_t lens, nlens, remaining_lens = 0;
    int32_t num;
    int32_t frozen = 1;
    int32_t roccupy = ERR_FAILED;
    IOV_TYPE iov;
    binary_ctx br;
    buffer_get(buf, blens, &iov, 1);
    nlens = (size_t)iov.IOV_LEN_FIELD;
    if (nlens >= 2) {
        binary_init_read(&br, (char *)iov.IOV_PTR_FIELD, nlens);
        br.offset = 1;
        roccupy = _mqtt_data_varnum(&br, &num);
        if (ERR_FAILED != roccupy) {
            remaining_lens = (size_t)num;
        }
    }
    if (ERR_FAILED == roccupy) {
        buffer_commit_get(buf, 0);
        frozen = 0;
        if (nlens < 5
            && nlens < blens) {
            roccupy = varint_decode_mqtt(buf, 1, blens, &remaining_lens);//返回剩余长度占用字节数
        }
        if (ERR_FAILED == roccupy) {
            BIT_SET(*status, blens >= 5 ? PROT_ERROR : PROT_MOREDATA);//剩余长度最大4个字节
            return NULL;
        }
    }
    size_t fhlens = 1 + roccupy;
    size_t total = fhlens + remaining_lens;
    if (total > MQTT_MAX_PACK_LENS
        || blens < total) {
        if (frozen) {
            buffer_commit_get(buf, 0);
        }
        BIT_SET(*status, total > MQTT_MAX_PACK_LENS ? PROT_ERROR : PROT_MOREDATA);
        return NULL;
    }
    if (frozen
        && nlens >= total) {
        binary_init_read(&br, (char *)iov.IOV_PTR_FIELD, total);
    } else {
        if (frozen) {
            buffer_commit_get(buf, 0);
        }
        lens = _mqtt_publish_hlens(buf, ud, fhlens, total);
        if (0 == lens) {
            lens = total;
        } else {
            spill = buf;
        }
        tmp = head;
        if (lens > sizeof(head)) {
            MALLOC(tmp, lens);
        }
        ASSERTAB(lens == buffer_copyout(buf, 0, tmp, lens), "copy buffer failed.");
        binary_init_read(&br, tmp, lens);
    }
    mqtt_pack_ctx *pack = _mqtt_parse(client, &br, spill, ud, status, fhlens, remaining_lens);
    if (NULL == tmp) {
        buffer_commit_get(buf, br.offset);
    } else {
        if (tmp != head) {
            FREE(tmp);
        }
        ASSERTAB(br.offset == buffer_drain(buf, br.offset), "drain buffer failed.");
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
