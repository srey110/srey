#include "test_mqtt_pack.h"
#include "lib.h"
#include "protocol/mqtt/mqtt_pack.h"
#include "protocol/mqtt/mqtt_struct.h"
#include "protocol/varint.h"
#include "crypt/scram.h"

/* MQTT 状态机的 INIT/COMMAND 是 mqtt.c 内部 enum，定义在文件作用域；
 * 测试中按数值约定使用 0=INIT, 1=COMMAND。*/
#define _MQ_INIT     0
#define _MQ_COMMAND  1

// 解包桩共用的"无连接"标识: 取代旧的 (INVALID_SOCK, 0) 实参对
static sock_ctx _t_nosk = { INVALID_SOCK, INVALID_INDEX, 0 };
// 解包入口的 ev 与连接标识在测试里恒为空：只喂缓冲，不发包也不认连接。
// 三个恒定实参收进薄封装，签名再变时只改这里，不必逐个改调用点
static void *_t_mqtt_unpack(int32_t client, buffer_ctx *buf, ud_cxt *ud,
    size_t *size, int32_t *status) {
    return mqtt_unpack(NULL, &_t_nosk, client, buf, ud, size, status);
}

// mqtt 解包用的最小上下文：新建 mqtt_ctx 挂进清零的 ud，并摆到指定解析阶段。
// mqtt_ctx 由 _mqtt_udfree(&ud) 回收，调用方不必自己记指针
static void _mq_ud_init(ud_cxt *ud, mqtt_protversion ver, int32_t status) {
    ZERO(ud, sizeof(ud_cxt));
    ud->status = status;
    ud->context = mqtt_ctx_new(ver);
}

/* 公共辅助：把组包的 char* 写入 buffer，并返回 buffer */
static void _mq_to_buf(buffer_ctx *buf, char *pack, size_t lens) {
    buffer_init(buf);
    buffer_append(buf, pack, lens);
    FREE(pack);
}

/* =======================================================================
 * CONNECT —— v3.1.1 简单连接（无遗嘱、无认证）pack → unpack 往返
 * ======================================================================= */
static void test_mqtt_connect_311(CuTest *tc) {
    size_t lens = 0;
    char *pack = mqtt_pack_connect(MQTT_311, 1 /*cleanstart*/, 60 /*keepalive*/,
        "client_311", NULL, NULL, 0,
        NULL, NULL, 0, 0, 0,
        NULL, NULL, &lens);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, lens > 0);

    buffer_ctx buf;
    _mq_to_buf(&buf, pack, lens);

    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = _MQ_INIT;
    /* CONNECT 在 INIT 状态：unpack 内部会 CALLOC 一个 mqtt_ctx 写入 ud->context */

    int32_t status = PROT_INIT;
    mqtt_pack_ctx *p = _t_mqtt_unpack(0 /*server*/, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, p);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertIntEquals(tc, MQTT_CONNECT, p->fixhead.prot);
    CuAssertIntEquals(tc, MQTT_311, p->version);

    mqtt_connect_varhead *vh = (mqtt_connect_varhead *)p->varhead;
    CuAssertIntEquals(tc, 1,  vh->cleanstart);
    CuAssertIntEquals(tc, 60, vh->keepalive);
    CuAssertIntEquals(tc, 0,  vh->willflag);
    CuAssertIntEquals(tc, 0,  vh->userflag);
    CuAssertIntEquals(tc, 0,  vh->passwordflag);

    mqtt_connect_payload *pl = (mqtt_connect_payload *)p->payload;
    CuAssertStrEquals(tc, "client_311", pl->clientid);

    _mqtt_pkfree(p);
    _mqtt_udfree(&ud);
    buffer_free(&buf);
}

/* CONNECT 遗嘱载荷指针为空但长度非零：剩余长度按 wplens 记账，而 _mqtt_pack_lenstr 里的
 * binary_set_binary 遇 NULL 直接返回、只写出 2 字节长度前缀，对不上的字节会让对端把
 * 后一个报文的开头当成本包内容。组包侧须把长度归零，让报文自洽 */
static void test_mqtt_connect_will_null_payload(CuTest *tc) {
    size_t lens = 0;
    char *pack = mqtt_pack_connect(MQTT_311, 1, 60,
        "client_wnp", NULL, NULL, 0,
        "last/will", NULL, 7 /*指针为空却给了长度*/, 0, 0,
        NULL, NULL, &lens);
    CuAssertPtrNotNull(tc, pack);

    buffer_ctx buf;
    _mq_to_buf(&buf, pack, lens);
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = _MQ_INIT;
    int32_t status = PROT_INIT;
    mqtt_pack_ctx *p = _t_mqtt_unpack(0, &buf, &ud, NULL, &status);
    /* 解包侧的全局兜底是"消费掉的字节数须正好等于剩余长度"，错位会在那里被判协议错 */
    CuAssertPtrNotNull(tc, p);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    mqtt_connect_varhead *vh = (mqtt_connect_varhead *)p->varhead;
    CuAssertIntEquals(tc, 1, vh->willflag);
    /* 报文里的遗嘱载荷就是零长度 */
    mqtt_connect_payload *pl = (mqtt_connect_payload *)p->payload;
    CuAssertIntEquals(tc, 0, (int)pl->wplens);
    /* 整包被完整消费，缓冲不留残字节 */
    CuAssertIntEquals(tc, 0, (int)buffer_size(&buf));

    _mqtt_pkfree(p);
    _mqtt_udfree(&ud);
    buffer_free(&buf);
}

/* CONNECT 5.0 with user + password + will + keepalive */
static void test_mqtt_connect_50_full(CuTest *tc) {
    size_t lens = 0;
    char pwd[] = "secret";
    char will_payload[] = "byebye";
    char *pack = mqtt_pack_connect(MQTT_50, 1, 120,
        "client_50", "alice", pwd, sizeof(pwd) - 1,
        "last/will", will_payload, sizeof(will_payload) - 1, 1 /*willqos*/, 1 /*willretain*/,
        NULL, NULL, &lens);
    CuAssertPtrNotNull(tc, pack);

    buffer_ctx buf;
    _mq_to_buf(&buf, pack, lens);

    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = _MQ_INIT;

    int32_t status = PROT_INIT;
    mqtt_pack_ctx *p = _t_mqtt_unpack(0, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, p);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertIntEquals(tc, MQTT_CONNECT, p->fixhead.prot);
    CuAssertIntEquals(tc, MQTT_50, p->version);

    mqtt_connect_varhead *vh = (mqtt_connect_varhead *)p->varhead;
    CuAssertIntEquals(tc, 1,   vh->cleanstart);
    CuAssertIntEquals(tc, 120, vh->keepalive);
    CuAssertIntEquals(tc, 1,   vh->willflag);
    CuAssertIntEquals(tc, 1,   vh->willqos);
    CuAssertIntEquals(tc, 1,   vh->willretain);
    CuAssertIntEquals(tc, 1,   vh->userflag);
    CuAssertIntEquals(tc, 1,   vh->passwordflag);

    mqtt_connect_payload *pl = (mqtt_connect_payload *)p->payload;
    CuAssertStrEquals(tc, "client_50",  pl->clientid);
    CuAssertStrEquals(tc, "last/will",  pl->willtopic);
    CuAssertStrEquals(tc, "alice",      pl->user);
    CuAssertTrue(tc, pl->pslens == sizeof(pwd) - 1);
    CuAssertTrue(tc, 0 == memcmp(pl->password, pwd, pl->pslens));

    _mqtt_pkfree(p);
    _mqtt_udfree(&ud);
    buffer_free(&buf);
}

/* =======================================================================
 * CONNACK / PUBACK / PUBREC / PUBREL / PUBCOMP / DISCONNECT / AUTH
 * ======================================================================= */
static void _mqtt_connack_case(CuTest *tc, mqtt_protversion ver, int8_t sesspresent,
    uint8_t reason, binary_ctx *props) {
    size_t lens = 0;
    char *pack = mqtt_pack_connack(ver, sesspresent, reason, props, &lens);
    CuAssertPtrNotNull(tc, pack);

    buffer_ctx buf;
    _mq_to_buf(&buf, pack, lens);

    mqtt_ctx *mq = mqtt_ctx_new(ver);

    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = _MQ_INIT;
    ud.context = mq;

    int32_t status = PROT_INIT;
    mqtt_pack_ctx *p = _t_mqtt_unpack(1 /*client*/, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, p);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertIntEquals(tc, MQTT_CONNACK, p->fixhead.prot);

    mqtt_connack_varhead *vh = (mqtt_connack_varhead *)p->varhead;
    CuAssertIntEquals(tc, sesspresent, vh->sesspresent);
    CuAssertIntEquals(tc, reason, vh->reason);
    if (NULL != props && props->offset > 0) {
        CuAssertPtrNotNull(tc, vh->properties);
    }

    _mqtt_pkfree(p);
    _mqtt_udfree(&ud);
    buffer_free(&buf);
}
static void test_mqtt_connack(CuTest *tc) {
    _mqtt_connack_case(tc, MQTT_311, 1, 0, NULL);
    _mqtt_connack_case(tc, MQTT_311, 0, 0, NULL);
    // 5.0 的非零原因码走另一条编码路径。服务端拒绝连接的原因（0x85 客户端标识无效 /
    // 0x86 用户名密码错 / 0x87 未授权）以前一条都没测过——删掉解码端读 reason 那一步，
    // 客户端会把所有拒绝都读成 Success
    _mqtt_connack_case(tc, MQTT_50, 0, 0x85, NULL);
    _mqtt_connack_case(tc, MQTT_50, 1, 0x87, NULL);
    // 5.0 带属性段：走属性长度 varint + 属性体，也是零覆盖
    binary_ctx props;
    binary_init_write(&props, 0, 0);
    mqtt_props_fixnum(&props, SESSION_EXPIRY, 3600);
    _mqtt_connack_case(tc, MQTT_50, 0, 0, &props);
    binary_free(&props);
}

static void _mqtt_pack_ack_test(CuTest *tc, mqtt_protversion ver, mqtt_prot expected,
    uint8_t reason, char *(*packer)(mqtt_protversion, uint16_t, uint8_t, binary_ctx *, size_t *)) {
    size_t lens = 0;
    char *pack = packer(ver, 12345, reason, NULL, &lens);
    CuAssertPtrNotNull(tc, pack);

    buffer_ctx buf;
    _mq_to_buf(&buf, pack, lens);

    mqtt_ctx *mq = mqtt_ctx_new(ver);

    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = _MQ_COMMAND;
    ud.context = mq;

    int32_t status = PROT_INIT;
    mqtt_pack_ctx *p = _t_mqtt_unpack(1, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, p);
    CuAssertIntEquals(tc, (int)expected, (int)p->fixhead.prot);
    mqtt_pubackrel_varhead *vh = (mqtt_pubackrel_varhead *)p->varhead;
    CuAssertIntEquals(tc, 12345, vh->packid);
    // reason 必须回读。原来入参硬编码 0x00 且从不断言，而 5.0 在 reason=0 且无属性时
    // 走的是 §3.4.2.2.1 的 2 字节简化形式——与 3.1.1 逐字节相同，8 个子用例只产生
    // 4 种编码，version 形参等于没测，解码端 remaining_lens >= 3 那支也零覆盖
    CuAssertIntEquals(tc, (int)reason, (int)vh->reason);

    _mqtt_pkfree(p);
    _mqtt_udfree(&ud);
    buffer_free(&buf);
}

static void test_mqtt_acks(CuTest *tc) {
    _mqtt_pack_ack_test(tc, MQTT_311, MQTT_PUBACK,  0x00, mqtt_pack_puback);
    _mqtt_pack_ack_test(tc, MQTT_311, MQTT_PUBREC,  0x00, mqtt_pack_pubrec);
    _mqtt_pack_ack_test(tc, MQTT_311, MQTT_PUBREL,  0x00, mqtt_pack_pubrel);
    _mqtt_pack_ack_test(tc, MQTT_311, MQTT_PUBCOMP, 0x00, mqtt_pack_pubcomp);
    _mqtt_pack_ack_test(tc, MQTT_50,  MQTT_PUBACK,  0x00, mqtt_pack_puback);
    _mqtt_pack_ack_test(tc, MQTT_50,  MQTT_PUBREC,  0x00, mqtt_pack_pubrec);
    _mqtt_pack_ack_test(tc, MQTT_50,  MQTT_PUBREL,  0x00, mqtt_pack_pubrel);
    _mqtt_pack_ack_test(tc, MQTT_50,  MQTT_PUBCOMP, 0x00, mqtt_pack_pubcomp);
    // 非零原因码只有 5.0 有（3.1.1 的 ACK 就是两字节 packid，塞不进 reason），
    // 这四条把编码端 total = 2 + 1 与解码端 remaining_lens >= 3 两支带起来
    _mqtt_pack_ack_test(tc, MQTT_50,  MQTT_PUBACK,  0x10, mqtt_pack_puback);
    _mqtt_pack_ack_test(tc, MQTT_50,  MQTT_PUBREC,  0x80, mqtt_pack_pubrec);
    _mqtt_pack_ack_test(tc, MQTT_50,  MQTT_PUBREL,  0x92, mqtt_pack_pubrel);
    _mqtt_pack_ack_test(tc, MQTT_50,  MQTT_PUBCOMP, 0x92, mqtt_pack_pubcomp);
}

/* =======================================================================
 * PUBLISH —— 多 QoS 路径 + payload 往返
 * ======================================================================= */
static void test_mqtt_publish(CuTest *tc) {
    /* QoS 0：无 packid */
    size_t lens = 0;
    char payload[] = "publish-payload";
    char *pack = mqtt_pack_publish(MQTT_311, 0 /*retain*/, 0 /*qos*/, 0 /*dup*/,
        "sensor/temp", 0, payload, sizeof(payload) - 1, NULL, &lens);
    CuAssertPtrNotNull(tc, pack);

    buffer_ctx buf;
    _mq_to_buf(&buf, pack, lens);

    mqtt_ctx *mq = mqtt_ctx_new(MQTT_311);

    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = _MQ_COMMAND;
    ud.context = mq;

    int32_t status = PROT_INIT;
    mqtt_pack_ctx *p = _t_mqtt_unpack(0, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, p);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertIntEquals(tc, MQTT_PUBLISH, p->fixhead.prot);

    mqtt_publish_varhead *vh = (mqtt_publish_varhead *)p->varhead;
    CuAssertIntEquals(tc, 0, vh->qos);
    CuAssertIntEquals(tc, 0, vh->retain);
    CuAssertStrEquals(tc, "sensor/temp", vh->topic);

    mqtt_publish_payload *pl = (mqtt_publish_payload *)p->payload;
    CuAssertTrue(tc, (int)(sizeof(payload) - 1) == pl->lens);
    CuAssertTrue(tc, 0 == memcmp(pl->content, payload, pl->lens));

    _mqtt_pkfree(p);
    buffer_free(&buf);

    /* QoS 1：有 packid */
    pack = mqtt_pack_publish(MQTT_311, 1 /*retain*/, 1 /*qos*/, 0,
        "topic/x", 777, payload, sizeof(payload) - 1, NULL, &lens);
    CuAssertPtrNotNull(tc, pack);
    _mq_to_buf(&buf, pack, lens);

    status = PROT_INIT;
    ud.status = _MQ_COMMAND;
    p = _t_mqtt_unpack(0, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, p);
    vh = (mqtt_publish_varhead *)p->varhead;
    CuAssertIntEquals(tc, 1, vh->qos);
    CuAssertIntEquals(tc, 1, vh->retain);
    CuAssertIntEquals(tc, 777, vh->packid);

    _mqtt_pkfree(p);
    _mqtt_udfree(&ud);
    buffer_free(&buf);
}

// PUBLISH 的 varhead / topic / 载荷现在与 pack 同处一块内存，摆放偏移随 topic 长度、
// qos（有无报文标识符）、v5 属性段而变。这里逐个走一遍边界，确认指针落点与内容都对。
// 建议用 ASan 构建跑：偏移算错时普通构建可能悄悄过去
// withprops 非 0 时给 v5 挂一段真属性（非空属性段才让载荷起点真的后移，
// off += num 的非平凡取值才走得到）
static void _mq_publish_case(CuTest *tc, mqtt_protversion version, int8_t qos, uint16_t packid,
                             const char *topic, const char *body, size_t blens, int32_t withprops) {
    binary_ctx props;
    binary_ctx *pprops = NULL;
    if (withprops) {
        binary_init_write(&props, 0, 64);
        CuAssertIntEquals(tc, ERR_OK, mqtt_props_fixnum(&props, PAYLOAD_FORMAT, 1));
        CuAssertIntEquals(tc, ERR_OK, mqtt_props_kv(&props, USER_PROPERTY, "k", 1, "v", 1));
        pprops = &props;
    }
    size_t lens = 0;
    char *pack = mqtt_pack_publish(version, 0, qos, 0, topic, packid,
                                   (char *)body, blens, pprops, &lens);
    if (withprops) {
        binary_free(&props);
    }
    CuAssertPtrNotNull(tc, pack);
    buffer_ctx buf;
    _mq_to_buf(&buf, pack, lens);
    ud_cxt ud;
    _mq_ud_init(&ud, version, _MQ_COMMAND);
    int32_t status = PROT_INIT;
    mqtt_pack_ctx *p = _t_mqtt_unpack(0, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, p);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    mqtt_publish_varhead *vh = (mqtt_publish_varhead *)p->varhead;
    mqtt_publish_payload *pl = (mqtt_publish_payload *)p->payload;
    CuAssertStrEquals(tc, topic, vh->topic);
    CuAssertIntEquals(tc, qos, vh->qos);
    if (1 == qos || 2 == qos) {
        CuAssertIntEquals(tc, packid, vh->packid);
    }
    CuAssertIntEquals(tc, (int)blens, pl->lens);
    if (blens > 0) {
        CuAssertTrue(tc, 0 == memcmp(pl->content, body, blens));
    }
    // 三段必须首尾相接地落在 pack 那一块里，且互不重叠
    CuAssertTrue(tc, (char *)vh == (char *)p + sizeof(mqtt_pack_ctx));
    CuAssertTrue(tc, vh->topic > (char *)vh);
    CuAssertTrue(tc, (char *)pl >= vh->topic + strlen(topic) + 1);
    CuAssertTrue(tc, 0 == ((uintptr_t)pl % sizeof(int32_t)));// 载荷头须 4 字节对齐
    if (withprops) {
        // 属性段非空时必须解出来，且载荷仍在它之后
        CuAssertPtrNotNull(tc, vh->properties);
        CuAssertIntEquals(tc, 2, (int)array_size(vh->properties));
    }
    _mqtt_pkfree(p);
    _mqtt_udfree(&ud);
    buffer_free(&buf);
}
// PUBLISH 单块分配的边界：空 topic / 空载荷 / 各 qos 档 / v5 属性段
static void test_mqtt_publish_block(CuTest *tc) {
    const char *body = "payload-bytes";
    size_t blens = strlen(body);
    // 1) qos 0/1/2：packid 有无会改变载荷在块内的起点
    _mq_publish_case(tc, MQTT_311, 0, 0, "a/b", body, blens, 0);
    _mq_publish_case(tc, MQTT_311, 1, 1, "a/b", body, blens, 0);
    _mq_publish_case(tc, MQTT_311, 2, 65535, "a/b", body, blens, 0);
    // 2) 空载荷（remain == 0，走 _mqtt_publish 的提前 return）
    _mq_publish_case(tc, MQTT_311, 0, 0, "only/topic", "", 0, 0);
    _mq_publish_case(tc, MQTT_311, 1, 7, "only/topic", "", 0, 0);
    // 3) 空 topic（长度前缀为 0，topic 只占一个 '\0'）
    _mq_publish_case(tc, MQTT_311, 0, 0, "", body, blens, 0);
    _mq_publish_case(tc, MQTT_311, 0, 0, "", "", 0, 0);
    // 4) topic 长度为奇数/偶数各来一次，覆盖载荷头的对齐补白分支
    _mq_publish_case(tc, MQTT_311, 0, 0, "x", body, blens, 0);
    _mq_publish_case(tc, MQTT_311, 0, 0, "xy", body, blens, 0);
    _mq_publish_case(tc, MQTT_311, 0, 0, "xyz", body, blens, 0);
    // 5) v5：属性段占 remaining_lens 的一部分，载荷起点随之后移
    _mq_publish_case(tc, MQTT_50, 0, 0, "v5/topic", body, blens, 0);
    _mq_publish_case(tc, MQTT_50, 2, 9, "v5/topic", body, blens, 0);
    // 6) v5 + 非空属性段：属性长度不再是 0，载荷起点随之再后移
    _mq_publish_case(tc, MQTT_50, 0, 0, "v5/topic", body, blens, 1);
    _mq_publish_case(tc, MQTT_50, 2, 9, "v5/topic", body, blens, 1);
    _mq_publish_case(tc, MQTT_50, 1, 3, "x", "", 0, 1);
}

// 回归：topic 长度前缀声称的字节数超过 remaining_lens 时须判协议错，不能照着写进块内
static void test_mqtt_publish_bad_topiclen(CuTest *tc) {
    // 固定头 0x30(PUBLISH,qos0) + remaining_lens=4 + topic 长度前缀 0xFFFF + 两字节凑数
    unsigned char raw[] = { 0x30, 0x04, 0xff, 0xff, 'a', 'b' };
    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, raw, sizeof(raw));
    ud_cxt ud;
    _mq_ud_init(&ud, MQTT_311, _MQ_COMMAND);
    int32_t status = PROT_INIT;
    mqtt_pack_ctx *p = _t_mqtt_unpack(0, &buf, &ud, NULL, &status);
    CuAssert(tc, "topic length beyond remaining_lens must be a protocol error",
        NULL == p && BIT_CHECK(status, PROT_ERROR));
    _mqtt_udfree(&ud);
    buffer_free(&buf);
}

/* =======================================================================
 * SUBSCRIBE / SUBACK / UNSUBSCRIBE / UNSUBACK
 * ======================================================================= */
// SUBSCRIBE / SUBACK / UNSUBSCRIBE / UNSUBACK 一整轮。
// 按版本参数化：3.1.1 的订阅选项只有 qos，5.0 还有 nl / rap / retain 三位与属性段，
// 只跑 3.1.1 的话 5.0 那几位的编解码一次都不执行
static void _mqtt_subunsub_case(CuTest *tc, mqtt_protversion ver) {
    const int8_t nl = (MQTT_50 == ver) ? 1 : 0;
    const int8_t rap = (MQTT_50 == ver) ? 1 : 0;
    const int8_t rh = (MQTT_50 == ver) ? 2 : 0;
    binary_ctx topics;
    binary_init_write(&topics, 0, 64);
    CuAssertIntEquals(tc, ERR_OK, mqtt_topics_subscribe(&topics, ver, "topic/a", 0, 0, 0, 0));
    CuAssertIntEquals(tc, ERR_OK, mqtt_topics_subscribe(&topics, ver, "topic/b", 1, nl, rap, rh));

    size_t lens = 0;
    char *pack = mqtt_pack_subscribe(ver, 0xAABB, &topics, NULL, &lens);
    CuAssertPtrNotNull(tc, pack);
    binary_free(&topics);

    buffer_ctx buf;
    _mq_to_buf(&buf, pack, lens);

    ud_cxt ud;
    _mq_ud_init(&ud, ver, _MQ_COMMAND);

    int32_t status = PROT_INIT;
    mqtt_pack_ctx *p = _t_mqtt_unpack(0, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, p);
    CuAssertIntEquals(tc, MQTT_SUBSCRIBE, p->fixhead.prot);
    mqtt_subreqresp_varhead *vh = (mqtt_subreqresp_varhead *)p->varhead;
    CuAssertIntEquals(tc, 0xAABB, vh->packid);

    mqtt_subscribe_payload *pl = (mqtt_subscribe_payload *)p->payload;
    CuAssertIntEquals(tc, 2, (int)array_size(&pl->subop));
    subscribe_option *opt0 = *(subscribe_option **)array_at(&pl->subop, 0);
    subscribe_option *opt1 = *(subscribe_option **)array_at(&pl->subop, 1);
    CuAssertStrEquals(tc, "topic/a", opt0->topic);
    CuAssertStrEquals(tc, "topic/b", opt1->topic);
    CuAssertIntEquals(tc, 1, opt1->qos);
    CuAssertIntEquals(tc, nl, opt1->nl);
    CuAssertIntEquals(tc, rap, opt1->rap);
    CuAssertIntEquals(tc, rh, opt1->retain);
    CuAssertIntEquals(tc, 0, opt0->qos);
    CuAssertIntEquals(tc, 0, opt0->nl);

    _mqtt_pkfree(p);
    buffer_free(&buf);

    /* SUBACK */
    uint8_t reasons[] = { 0x00, 0x01 };
    pack = mqtt_pack_suback(ver, 0xAABB, reasons, sizeof(reasons), NULL, &lens);
    CuAssertPtrNotNull(tc, pack);
    _mq_to_buf(&buf, pack, lens);

    status = PROT_INIT;
    ud.status = _MQ_COMMAND;
    p = _t_mqtt_unpack(1, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, p);
    CuAssertIntEquals(tc, MQTT_SUBACK, p->fixhead.prot);
    mqtt_reasonlist_payload *spl = (mqtt_reasonlist_payload *)p->payload;
    CuAssertTrue(tc, 2 == spl->rlens);
    CuAssertTrue(tc, 0x00 == spl->reasons[0]);
    CuAssertTrue(tc, 0x01 == spl->reasons[1]);
    _mqtt_pkfree(p);
    buffer_free(&buf);

    /* UNSUBSCRIBE */
    binary_ctx untopics;
    binary_init_write(&untopics, 0, 64);
    CuAssertIntEquals(tc, ERR_OK, mqtt_topics_unsubscribe(&untopics, "topic/a"));
    CuAssertIntEquals(tc, ERR_OK, mqtt_topics_unsubscribe(&untopics, "topic/b"));
    pack = mqtt_pack_unsubscribe(ver, 0xCCDD, &untopics, NULL, &lens);
    CuAssertPtrNotNull(tc, pack);
    binary_free(&untopics);
    _mq_to_buf(&buf, pack, lens);

    status = PROT_INIT;
    ud.status = _MQ_COMMAND;
    p = _t_mqtt_unpack(0, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, p);
    CuAssertIntEquals(tc, MQTT_UNSUBSCRIBE, p->fixhead.prot);
    mqtt_unsubscribe_payload *upl = (mqtt_unsubscribe_payload *)p->payload;
    CuAssertIntEquals(tc, 2, (int)array_size(&upl->topics));
    _mqtt_pkfree(p);
    buffer_free(&buf);

    /* UNSUBACK：3.1.1 仅 packid，5.0 带 reason 列表 */
    uint8_t unreasons[] = { 0x00, 0x11 };
    pack = (MQTT_50 == ver)
         ? mqtt_pack_unsuback(ver, 0xCCDD, unreasons, sizeof(unreasons), NULL, &lens)
         : mqtt_pack_unsuback(ver, 0xCCDD, NULL, 0, NULL, &lens);
    CuAssertPtrNotNull(tc, pack);
    _mq_to_buf(&buf, pack, lens);

    status = PROT_INIT;
    ud.status = _MQ_COMMAND;
    p = _t_mqtt_unpack(1, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, p);
    CuAssertIntEquals(tc, MQTT_UNSUBACK, p->fixhead.prot);
    if (MQTT_50 == ver) {
        mqtt_reasonlist_payload *upl2 = (mqtt_reasonlist_payload *)p->payload;
        CuAssertPtrNotNull(tc, upl2);
        CuAssertTrue(tc, 2 == upl2->rlens);
        CuAssertTrue(tc, 0x00 == upl2->reasons[0]);
        CuAssertTrue(tc, 0x11 == upl2->reasons[1]);
    }
    _mqtt_pkfree(p);
    _mqtt_udfree(&ud);
    buffer_free(&buf);
}
static void test_mqtt_subscribe(CuTest *tc) {
    _mqtt_subunsub_case(tc, MQTT_311);
    _mqtt_subunsub_case(tc, MQTT_50);
}

/* =======================================================================
 * PING / PONG —— 固定 2 字节
 * ======================================================================= */
static void test_mqtt_ping_pong(CuTest *tc) {
    size_t lens = 0;
    char *pack = mqtt_pack_ping(&lens);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, 2 == (int)lens);
    /* 0xC0 = PINGREQ (12<<4) */
    CuAssertTrue(tc, 0xC0 == (uint8_t)pack[0]);
    CuAssertTrue(tc, 0x00 == (uint8_t)pack[1]);

    buffer_ctx buf;
    _mq_to_buf(&buf, pack, lens);
    ud_cxt ud;
    _mq_ud_init(&ud, MQTT_311, _MQ_COMMAND);

    int32_t status = PROT_INIT;
    mqtt_pack_ctx *p = _t_mqtt_unpack(0, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, p);
    CuAssertIntEquals(tc, MQTT_PINGREQ, p->fixhead.prot);
    _mqtt_pkfree(p);
    buffer_free(&buf);

    /* PONG */
    pack = mqtt_pack_pong(&lens);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, 0xD0 == (uint8_t)pack[0]);/* PINGRESP (13<<4) */
    _mq_to_buf(&buf, pack, lens);

    status = PROT_INIT;
    ud.status = _MQ_COMMAND;
    p = _t_mqtt_unpack(1, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, p);
    CuAssertIntEquals(tc, MQTT_PINGRESP, p->fixhead.prot);
    _mqtt_pkfree(p);
    _mqtt_udfree(&ud);
    buffer_free(&buf);
}

/* =======================================================================
 * DISCONNECT —— v3.1.1 仅固定头；v5.0 含 reason + props
 * ======================================================================= */
static void test_mqtt_disconnect(CuTest *tc) {
    /* 3.1.1：组包后只有 2 字节固定头 */
    size_t lens = 0;
    char *pack = mqtt_pack_disconnect(MQTT_311, 0, NULL, &lens);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, 2 == (int)lens);
    CuAssertTrue(tc, 0xE0 == (uint8_t)pack[0]);/* DISCONNECT (14<<4) */

    buffer_ctx buf;
    _mq_to_buf(&buf, pack, lens);

    ud_cxt ud;
    _mq_ud_init(&ud, MQTT_311, _MQ_COMMAND);

    int32_t status = PROT_INIT;
    mqtt_pack_ctx *p = _t_mqtt_unpack(0, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, p);
    CuAssertIntEquals(tc, MQTT_DISCONNECT, p->fixhead.prot);
    _mqtt_pkfree(p);
    _mqtt_udfree(&ud);
    buffer_free(&buf);

    /* 5.0：含 reason 字段 */
    pack = mqtt_pack_disconnect(MQTT_50, 0x04 /*遗嘱断开*/, NULL, &lens);
    CuAssertPtrNotNull(tc, pack);
    _mq_to_buf(&buf, pack, lens);

    _mq_ud_init(&ud, MQTT_50, _MQ_COMMAND);
    status = PROT_INIT;

    p = _t_mqtt_unpack(0, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, p);
    CuAssertIntEquals(tc, MQTT_DISCONNECT, p->fixhead.prot);
    mqtt_reason_varhead *vh = (mqtt_reason_varhead *)p->varhead;
    CuAssertIntEquals(tc, 0x04, vh->reason);
    _mqtt_pkfree(p);
    _mqtt_udfree(&ud);
    buffer_free(&buf);
}

/* =======================================================================
 * AUTH (MQTT 5.0)
 * ======================================================================= */
static void test_mqtt_auth(CuTest *tc) {
    size_t lens = 0;
    char *pack = mqtt_pack_auth(MQTT_50, 0x18 /*继续认证*/, NULL, &lens);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, 0xF0 == (uint8_t)pack[0]);/* AUTH (15<<4) */

    buffer_ctx buf;
    _mq_to_buf(&buf, pack, lens);

    ud_cxt ud;
    _mq_ud_init(&ud, MQTT_50, _MQ_INIT);

    int32_t status = PROT_INIT;
    mqtt_pack_ctx *p = _t_mqtt_unpack(0, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, p);
    CuAssertIntEquals(tc, MQTT_AUTH, p->fixhead.prot);
    mqtt_reason_varhead *vh = (mqtt_reason_varhead *)p->varhead;
    CuAssertIntEquals(tc, 0x18, vh->reason);
    _mqtt_pkfree(p);
    _mqtt_udfree(&ud);
    buffer_free(&buf);
}

/* _mqtt_properties 的三条拒收路径。AUTH 是最小的带属性段载体（reason + 属性长度 + 属性体），
 * 而紧随其后那条紧凑形式用例连属性长度字段都没有，所以这三支此前全零覆盖：
 *   ① plens 撒谎（声明得比缓冲区还长）—— 删掉那句边界判定，异常 server 用一个撒谎的
 *      属性长度就能让逐项解析越过报文边界读进下一帧的字节
 *   ② 未知 property id —— 改成 continue 而不是置 PROT_ERROR，会在
 *      `for (off = 0; off < plens;)` 上造成死循环（off 不前进）
 *   ③ 逐项解完 off != plens（最后一项越过了声明的属性长度）
 * 三条都只断言"被拒"，不断言走的是哪一支：maxlens 收紧时 ③ 可能先在 ② 那支被拦下 */
static void _mqtt_props_reject(CuTest *tc, char *wire, size_t rlen) {
    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, wire, rlen);
    ud_cxt ud;
    _mq_ud_init(&ud, MQTT_50, _MQ_INIT);
    int32_t status = PROT_INIT;
    mqtt_pack_ctx *p = _t_mqtt_unpack(0, &buf, &ud, NULL, &status);
    CuAssertPtrEquals(tc, NULL, p);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
    _mqtt_udfree(&ud);
    buffer_free(&buf);
}
static void test_mqtt_props_reject(CuTest *tc) {
    /* ① 属性长度声明 0x64，实际只跟了 1 字节 */
    static char lie[] = { (char)0xF0, 0x03, 0x18, 0x64, 0x00 };
    _mqtt_props_reject(tc, lie, sizeof(lie));
    /* ② 属性长度 1，id 0x7F 不在 property id 表里（表内最大 0x2A SHARED_SUBSCRIPTION） */
    static char unknown[] = { (char)0xF0, 0x03, 0x18, 0x01, 0x7F };
    _mqtt_props_reject(tc, unknown, sizeof(unknown));
    /* ③ 属性长度声明 3，而一个 SESSION_EXPIRY(0x11) 要吃 1+4=5 字节，越过声明长度 */
    static char overrun[] = { (char)0xF0, 0x07, 0x18, 0x03,
                                    0x11, 0x00, 0x00, 0x0E, 0x10 };
    _mqtt_props_reject(tc, overrun, sizeof(overrun));
}

/* MQTT 5.0 §3.15.2.2.1：第三方 broker/client 可用紧凑形式 [0xF0, 0x01, reason]
 * (remaining_length=1, 仅 reason 无属性)。本项目 encoder 总写 remaining_length=2，
 * 但 decoder 必须接受这种合法紧凑形式而非误判为协议错。 */
static void test_mqtt_auth_compact_no_props(CuTest *tc) {
    /* 手工构造 3 字节 AUTH wire：encoder 不产生此形式 */
    char wire[3] = { (char)0xF0, 0x01, 0x18 };
    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, wire, sizeof(wire));

    ud_cxt ud;
    _mq_ud_init(&ud, MQTT_50, _MQ_INIT);

    int32_t status = PROT_INIT;
    mqtt_pack_ctx *p = _t_mqtt_unpack(0, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, p);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertIntEquals(tc, MQTT_AUTH, p->fixhead.prot);
    mqtt_reason_varhead *vh = (mqtt_reason_varhead *)p->varhead;
    CuAssertIntEquals(tc, 0x18, vh->reason);
    CuAssertPtrEquals(tc, NULL, vh->properties);/* 紧凑形式无属性 */
    _mqtt_pkfree(p);
    _mqtt_udfree(&ud);
    buffer_free(&buf);
}

/* =======================================================================
 * mqtt_props_* —— 属性编码（5.0）
 * ======================================================================= */
static void test_mqtt_props(CuTest *tc) {
    binary_ctx props;
    binary_init_write(&props, 0, 64);

    /* fixnum 三档宽度各喂一个，别都走 4 字节那支：
       1 字节 MAXIMUM_QOS=0x24 / 2 字节 RECEIVE_MAXIMUM=0x21 / 4 字节 SESSION_EXPIRY=0x11 */
    CuAssertIntEquals(tc, ERR_OK, mqtt_props_fixnum(&props, MAXIMUM_QOS, 2));
    CuAssertIntEquals(tc, ERR_OK, mqtt_props_fixnum(&props, RECEIVE_MAXIMUM, 0xBEEF));
    CuAssertIntEquals(tc, ERR_OK, mqtt_props_fixnum(&props, SESSION_EXPIRY, 3600));
    /* varnum (变长)：SUBSCRIPTION_ID = 0x0B */
    CuAssertIntEquals(tc, ERR_OK, mqtt_props_varnum(&props, SUBSCRIPTION_ID, 128));
    /* binary：AUTH_DATA = 0x16 */
    CuAssertIntEquals(tc, ERR_OK, mqtt_props_binary(&props, AUTH_DATA, "DATA", 4));
    /* kv：USER_PROPERTY = 0x26 */
    CuAssertIntEquals(tc, ERR_OK, mqtt_props_kv(&props, USER_PROPERTY, "k", 1, "v", 1));

    /* 至少写入了几个字节 */
    CuAssertTrue(tc, props.offset > 0);

    /* 编出来的字节要喂回 mqtt_unpack 读一遍。原来只有上面那几条返回码 + offset > 0，
       编码器把值写成 0、把 4 字节写成 2 字节、kv 漏写 value，全都照样 ERR_OK；
       解码侧的 1/2/4 字节数值分支、SUBSCRIPTION_ID 的变长分支、USER_PROPERTY 的 kv 分支
       在确定性 CuTest 里也就一次没执行过。AUTH 是最小的带属性段载体 */
    // 下面 wire[1] 与 wire[3] 都按 1 字节 varint 拼，卡住两者里更紧的 remaining
    CuAssertTrue(tc, 2 + props.offset < 128);
    char *wire;
    size_t wlens = 4 + props.offset;
    MALLOC(wire, wlens);
    wire[0] = (char)0xF0;
    wire[1] = (char)(2 + props.offset);// remaining：reason(1) + 属性长度 varint(1) + 属性体
    wire[2] = 0x18;// reason
    wire[3] = (char)props.offset;// 属性长度
    memcpy(wire + 4, props.data, props.offset);

    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, wire, wlens);
    FREE(wire);
    ud_cxt ud;
    _mq_ud_init(&ud, MQTT_50, _MQ_INIT);
    int32_t status = PROT_INIT;
    mqtt_pack_ctx *p = _t_mqtt_unpack(0, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, p);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    mqtt_reason_varhead *vh = (mqtt_reason_varhead *)p->varhead;
    CuAssertPtrNotNull(tc, vh->properties);
    CuAssertIntEquals(tc, 6, (int)array_size(vh->properties));

    mqtt_propertie *pr;
    int32_t seen = 0;
    for (uint32_t i = 0; i < array_size(vh->properties); i++) {
        pr = *(mqtt_propertie **)array_at(vh->properties, (int32_t)i);
        CuAssertPtrNotNull(tc, pr);
        switch (pr->flag) {
        case MAXIMUM_QOS:
            CuAssertTrue(tc, 2 == pr->nval);// 1 字节档
            seen |= 1;
            break;
        case RECEIVE_MAXIMUM:
            CuAssertTrue(tc, 0xBEEF == pr->nval);// 2 字节档：写成 1 或 4 字节这里就不对
            seen |= 16;
            break;
        case SESSION_EXPIRY:
            CuAssertTrue(tc, 3600 == pr->nval);// 4 字节档
            seen |= 32;
            break;
        case SUBSCRIPTION_ID:
            CuAssertTrue(tc, 128 == pr->nval);// 128 恰好跨到 2 字节 varint
            seen |= 2;
            break;
        case AUTH_DATA:
            CuAssertTrue(tc, 4 == pr->flens && 0 == memcmp("DATA", pr->fval, 4));
            seen |= 4;
            break;
        case USER_PROPERTY:
            CuAssertTrue(tc, 1 == pr->flens && 'k' == pr->fval[0]);
            CuAssertTrue(tc, 1 == pr->slens && NULL != pr->sval && 'v' == pr->sval[0]);
            seen |= 8;
            break;
        default:
            CuFail(tc, "unexpected property flag");
            break;
        }
    }
    CuAssertIntEquals(tc, 63, seen);

    _mqtt_pkfree(p);
    _mqtt_udfree(&ud);
    buffer_free(&buf);
    binary_free(&props);
}

/* =======================================================================
 * mqtt_reason —— 已知码字符串
 * ======================================================================= */
static void test_mqtt_reason(CuTest *tc) {
    /* 0x00 在不同报文类型下含义不同 */
    CuAssertStrEquals(tc, "Success",              mqtt_reason(MQTT_CONNACK, 0x00));
    CuAssertStrEquals(tc, "Success",              mqtt_reason(MQTT_PUBACK,  0x00));
    CuAssertStrEquals(tc, "Normal disconnection", mqtt_reason(MQTT_DISCONNECT, 0x00));
    CuAssertStrEquals(tc, "Granted QoS 0",        mqtt_reason(MQTT_SUBACK,  0x00));
    CuAssertStrEquals(tc, "Granted QoS 1",        mqtt_reason(MQTT_SUBACK,  0x01));
    CuAssertStrEquals(tc, "Granted QoS 2",        mqtt_reason(MQTT_SUBACK,  0x02));

    /* 未知码返回非 NULL 兜底字符串 */
    const char *unknown = mqtt_reason(MQTT_CONNACK, 0xff);
    CuAssertPtrNotNull(tc, unknown);
}

/* =======================================================================
 * SCRAM-SHA-256 over MQTT 5.0 AUTH —— 完整 SASL 四步握手
 *
 * 流程：
 *  step 1  client → server : AUTH(0x18) { AUTH_METHOD, AUTH_DATA=clientFirst }
 *  step 2  server → client : AUTH(0x18) { AUTH_METHOD, AUTH_DATA=serverFirst }
 *  step 3  client → server : AUTH(0x18) { AUTH_METHOD, AUTH_DATA=clientFinal }
 *  step 4  server → client : AUTH(0x00) { AUTH_METHOD, AUTH_DATA=serverFinal }
 *
 * 终态：client = SCRAM_REMOTE_FINAL，server = SCRAM_LOCAL_FINAL。
 * ======================================================================= */

/* 从 AUTH 报文 properties 中提取 AUTH_METHOD / AUTH_DATA 两个字段 */
static void _mqtt_extract_auth(mqtt_pack_ctx *p,
    const char **method, size_t *mlen,
    const char **data,   size_t *dlen) {
    *method = NULL; *mlen = 0;
    *data   = NULL; *dlen = 0;
    mqtt_reason_varhead *vh = (mqtt_reason_varhead *)p->varhead;
    if (NULL == vh || NULL == vh->properties) {
        return;
    }
    uint32_t n = array_size(vh->properties);
    for (uint32_t i = 0; i < n; i++) {
        mqtt_propertie *prop = *(mqtt_propertie **)array_at(vh->properties, i);
        if (AUTH_METHOD == prop->flag) {
            *method = prop->fval;
            *mlen   = prop->flens;
        } else if (AUTH_DATA == prop->flag) {
            *data = prop->fval;
            *dlen = prop->flens;
        }
    }
}

/* 把 (method, data) 打包到 AUTH(reason, ...) 后写入 buffer。
   tc 必须传真的：CuAssert 失败时 CuFailInternal 首句就是 tc->failed = 1 */
static void _mqtt_pack_auth_sasl(CuTest *tc, buffer_ctx *out,
    uint8_t reason, const char *method, const char *data, size_t dlen) {
    binary_ctx props;
    binary_init_write(&props, 0, 64);
    CuAssert(tc, "props: AUTH_METHOD",
        ERR_OK == mqtt_props_binary(&props, AUTH_METHOD, (void *)method, strlen(method)));
    CuAssert(tc, "props: AUTH_DATA",
        ERR_OK == mqtt_props_binary(&props, AUTH_DATA, (void *)data, dlen));

    size_t lens = 0;
    char *pack = mqtt_pack_auth(MQTT_50, reason, &props, &lens);
    binary_free(&props);

    buffer_init(out);
    buffer_append(out, pack, lens);
    FREE(pack);
}

/* 解一个 AUTH 报文，验证 method 字段 = 期望值，返回 pack 与 data 切片（pack 由调用方释放）*/
static mqtt_pack_ctx *_mqtt_unpack_auth_sasl(CuTest *tc, buffer_ctx *buf,
    mqtt_ctx *mq, int32_t client_role,
    const char *expect_method,
    const char **out_data, size_t *out_dlen) {
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = _MQ_INIT;/* AUTH 走 _mqtt_init 路径 */
    ud.context = mq;

    int32_t status = PROT_INIT;
    mqtt_pack_ctx *p = _t_mqtt_unpack(client_role, buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, p);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertIntEquals(tc, MQTT_AUTH, p->fixhead.prot);

    const char *m; size_t ml;
    _mqtt_extract_auth(p, &m, &ml, out_data, out_dlen);
    CuAssertPtrNotNull(tc, m);
    CuAssertTrue(tc, strlen(expect_method) == ml);
    CuAssertTrue(tc, 0 == memcmp(m, expect_method, ml));
    return p;
}

static void test_mqtt_auth_scram_sha256(CuTest *tc) {
    /* 固定 salt + iter，让握手结果可重现 */
    static const char salt[16] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10
    };
    const char *METHOD = "SCRAM-SHA-256";

    scram_ctx *cli = scram_init(METHOD, 1);
    scram_ctx *srv = scram_init(METHOD, 0);
    CuAssertPtrNotNull(tc, cli);
    CuAssertPtrNotNull(tc, srv);
    scram_set_user(cli, "alice", 5);
    scram_set_pwd(cli, "correcthorsebatterystaple", 25);
    scram_set_pwd(srv, "correcthorsebatterystaple", 25);
    scram_set_salt(srv, (char *)salt, sizeof(salt));
    scram_set_iter(srv, 4096);

    /* mqtt_ctx 由两端各持一份，AUTH 报文以 MQTT 5.0 编码，version 必须一致 */
    mqtt_ctx mq_srv; ZERO(&mq_srv, sizeof(mq_srv)); mq_srv.version = MQTT_50;
    mqtt_ctx mq_cli; ZERO(&mq_cli, sizeof(mq_cli)); mq_cli.version = MQTT_50;

    /* ── step 1：客户端发 clientFirst ─────────────────────────── */
    char *cf = scram_first_message(cli);
    CuAssertPtrNotNull(tc, cf);

    buffer_ctx buf;
    _mqtt_pack_auth_sasl(tc, &buf, 0x18, METHOD, cf, strlen(cf));

    const char *got_data; size_t got_dlen;
    mqtt_pack_ctx *p = _mqtt_unpack_auth_sasl(tc, &buf, &mq_srv, 0 /*server*/,
        METHOD, &got_data, &got_dlen);
    mqtt_reason_varhead *vh = (mqtt_reason_varhead *)p->varhead;
    CuAssertIntEquals(tc, 0x18, vh->reason);

    /* 服务端解析 clientFirst */
    char *copy;
    MALLOC(copy, got_dlen + 1);
    memcpy(copy, got_data, got_dlen);
    copy[got_dlen] = '\0';
    CuAssertIntEquals(tc, ERR_OK,
        scram_parse_first_message(srv, copy, got_dlen));
    FREE(copy);
    _mqtt_pkfree(p);
    buffer_free(&buf);
    FREE(cf);

    /* ── step 2：服务端发 serverFirst ─────────────────────────── */
    char *sf = scram_first_message(srv);
    CuAssertPtrNotNull(tc, sf);
    _mqtt_pack_auth_sasl(tc, &buf, 0x18, METHOD, sf, strlen(sf));

    p = _mqtt_unpack_auth_sasl(tc, &buf, &mq_cli, 1 /*client*/,
        METHOD, &got_data, &got_dlen);

    MALLOC(copy, got_dlen + 1);
    memcpy(copy, got_data, got_dlen);
    copy[got_dlen] = '\0';
    CuAssertIntEquals(tc, ERR_OK,
        scram_parse_first_message(cli, copy, got_dlen));
    FREE(copy);
    _mqtt_pkfree(p);
    buffer_free(&buf);
    FREE(sf);

    /* ── step 3：客户端发 clientFinal ─────────────────────────── */
    char *clf = scram_final_message(cli);
    CuAssertPtrNotNull(tc, clf);
    /* clf 是 client-final，含 ClientProof；按 scram.h 的契约擦除后释放 */
    _mqtt_pack_auth_sasl(tc, &buf, 0x18, METHOD, clf, strlen(clf));

    p = _mqtt_unpack_auth_sasl(tc, &buf, &mq_srv, 0,
        METHOD, &got_data, &got_dlen);

    MALLOC(copy, got_dlen + 1);
    memcpy(copy, got_data, got_dlen);
    copy[got_dlen] = '\0';
    CuAssertIntEquals(tc, ERR_OK,
        scram_check_final_message(srv, copy, got_dlen));
    FREE(copy);
    _mqtt_pkfree(p);
    buffer_free(&buf);
    SECURE_FREE(clf, strlen(clf) + 1);

    /* ── step 4：服务端发 serverFinal（reason=0x00=Success）──── */
    char *svf = scram_final_message(srv);
    CuAssertPtrNotNull(tc, svf);
    _mqtt_pack_auth_sasl(tc, &buf, 0x00, METHOD, svf, strlen(svf));

    p = _mqtt_unpack_auth_sasl(tc, &buf, &mq_cli, 1,
        METHOD, &got_data, &got_dlen);
    vh = (mqtt_reason_varhead *)p->varhead;
    CuAssertIntEquals(tc, 0x00, vh->reason);

    MALLOC(copy, got_dlen + 1);
    memcpy(copy, got_data, got_dlen);
    copy[got_dlen] = '\0';
    CuAssertIntEquals(tc, ERR_OK,
        scram_check_final_message(cli, copy, got_dlen));
    FREE(copy);
    _mqtt_pkfree(p);
    buffer_free(&buf);
    SECURE_FREE(svf, strlen(svf) + 1);

    /* 终态：客户端验证完服务端签名 = REMOTE_FINAL；
     *       服务端发出 v= 消息后 = LOCAL_FINAL */
    CuAssertIntEquals(tc, SCRAM_REMOTE_FINAL, (int)cli->status);
    CuAssertIntEquals(tc, SCRAM_LOCAL_FINAL,  (int)srv->status);
    /* 服务端可还原用户名 */
    CuAssertStrEquals(tc, "alice", scram_get_user(srv));

    scram_free(cli);
    scram_free(srv);
}

// mqtt_struct.c 所有 _free 函数 NULL safety 直调，确保异常分支不崩溃
static void test_mqtt_struct_null_free(CuTest *tc) {
    (void)tc;
    // 所有 _free 入口对 NULL 都应早返，连续调用不崩
    _mqtt_propertie_free(NULL);
    _mqtt_connect_varhead_free(NULL);
    _mqtt_connect_payload_free(NULL);
    _mqtt_connack_varhead_free(NULL);
    _mqtt_pubackrel_varhead_free(NULL);
    _mqtt_subreqresp_varhead_free(NULL);
    _mqtt_subscribe_payload_free(NULL);
    _mqtt_unsubscribe_payload_free(NULL);
    _mqtt_reasonlist_payload_free(NULL);
    _mqtt_reason_varhead_free(NULL);
}

// mqtt_struct.c 各 _free 函数空 properties + 空字符串字段释放路径
static void test_mqtt_struct_empty_free(CuTest *tc) {
    (void)tc;
    // connect varhead：properties 为 NULL，应仅 FREE 自身
    mqtt_connect_varhead *cvh;
    CALLOC(cvh, 1, sizeof(*cvh));
    _mqtt_connect_varhead_free(cvh);
    // connect payload：clientid/willtopic/willpayload/user/password 都是 NULL，FREE(NULL) 安全
    mqtt_connect_payload *cpl;
    CALLOC(cpl, 1, sizeof(*cpl));
    _mqtt_connect_payload_free(cpl);
    // publish 没有独立的 varhead/payload free：两者都摆在 mqtt_pack_ctx 那一整块里
    // subscribe payload：subop 数组为空时也应正常释放
    mqtt_subscribe_payload *spl;
    CALLOC(spl, 1, sizeof(*spl));
    array_init(&spl->subop, sizeof(subscribe_option *), 0);
    _mqtt_subscribe_payload_free(spl);
    // unsubscribe payload：topics 数组为空
    mqtt_unsubscribe_payload *upl;
    CALLOC(upl, 1, sizeof(*upl));
    array_init(&upl->topics, sizeof(char *), 0);
    _mqtt_unsubscribe_payload_free(upl);
}

// mqtt_struct.c _mqtt_propertie_free 释放含 sval 与不含 sval 的混合数组
static void test_mqtt_struct_propertie_free(CuTest *tc) {
    (void)tc;
    array_ctx *props;
    MALLOC(props, sizeof(*props));
    array_init(props, sizeof(mqtt_propertie *), 0);
    // 元素 1：含 sval 字符串
    mqtt_propertie *p1;
    CALLOC(p1, 1, sizeof(*p1));
    MALLOC(p1->sval, 8);
    memcpy(p1->sval, "topic1", 7);
    array_push_back(props, &p1);
    // 元素 2：sval 为 NULL（int 类型属性）
    mqtt_propertie *p2;
    CALLOC(p2, 1, sizeof(*p2));
    p2->sval = NULL;
    array_push_back(props, &p2);
    // 释放后 props/p1->sval/p1/p2 应全部归还，ASan 下应无泄漏
    _mqtt_propertie_free(props);
}

/* =======================================================================
 * 畸形报文拒绝 —— QoS=3 / 订阅选项保留位非零等违规报文必须以 PROT_ERROR 拒收
 * ======================================================================= */
/* server 端 unpack 应返回 NULL 并置 PROT_ERROR。
 * COMMAND 阶段(PUBLISH/SUBSCRIBE)需预置带 version 的 mqtt_ctx；
 * INIT 阶段(CONNECT)的 context 在 will 校验之后才创建，传 NULL 即可。 */
static void _mq_assert_reject(CuTest *tc, int32_t init_status, int32_t ver,
                              uint8_t *raw, size_t rlen) {
    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, raw, rlen);

    mqtt_ctx *mq = NULL;
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = init_status;
    if (_MQ_COMMAND == init_status) {
        mq = mqtt_ctx_new((mqtt_protversion)ver);
        ud.context = mq;
    }

    int32_t status = PROT_INIT;
    mqtt_pack_ctx *p = _t_mqtt_unpack(0 /*server*/, &buf, &ud, NULL, &status);
    CuAssertPtrEquals(tc, NULL, p);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));

    _mqtt_udfree(&ud);
    buffer_free(&buf);
}

static void test_mqtt_malformed_reject(CuTest *tc) {
    /* PUBLISH QoS=3（两 QoS 位同时为 1，MQTT-3.3.1-4）
     * 0x36=PUBLISH|flags(qos3=0x06)；剩余7=主题(2+3)+载荷(2)，QoS=3 无报文标识符 */
    uint8_t pub_qos3[] = { 0x36, 0x07, 0x00, 0x03, 't','o','p', 'h','i' };
    _mq_assert_reject(tc, _MQ_COMMAND, MQTT_311, pub_qos3, sizeof(pub_qos3));

    /* SUBSCRIBE 订阅选项 QoS=3（MQTT-3.8.3-4）
     * 0x82=SUBSCRIBE|0x02；剩余7=报文标识符(2)+主题(2+2)+选项(1)，选项=0x03 */
    uint8_t sub_qos3[] = { 0x82, 0x07, 0x00, 0x01, 0x00, 0x02, 'a','b', 0x03 };
    _mq_assert_reject(tc, _MQ_COMMAND, MQTT_311, sub_qos3, sizeof(sub_qos3));

    /* SUBSCRIBE 选项保留位非零（3.1.1 bit2-7 须为 0）：选项=0x04 */
    uint8_t sub_rsv[] = { 0x82, 0x07, 0x00, 0x01, 0x00, 0x02, 'a','b', 0x04 };
    _mq_assert_reject(tc, _MQ_COMMAND, MQTT_311, sub_rsv, sizeof(sub_rsv));

    /* CONNECT WillFlag=1 且 WillQoS=3（MQTT-3.1.2-14）
     * 连接标志 0x1C=willflag(bit2)|willqos3(bit3,4)；剩余23 */
    uint8_t conn_willqos3[] = {
        0x10, 0x17,
        0x00, 0x04, 'M','Q','T','T', 0x04, 0x1C, 0x00, 0x3C,
        0x00, 0x03, 'c','i','d',
        0x00, 0x02, 'w','t',
        0x00, 0x02, 'w','p'
    };
    _mq_assert_reject(tc, _MQ_INIT, MQTT_311, conn_willqos3, sizeof(conn_willqos3));
}

/* ======================================================================= */

/* 空 clientid 的 CONNECT —— 旧 binary_set_string 在 lens==0 时曾多写 1 个 NUL，
 * 使 remaining length 与实际 body 字节不符、解包后 buffer 残留 1 字节导致后续帧错位 */
static void test_mqtt_connect_empty_clientid(CuTest *tc) {
    size_t lens = 0;
    char *pack = mqtt_pack_connect(MQTT_311, 1, 60,
        "" /*空 clientid，MQTT 3.1.1 §3.1.3.1 合法*/, NULL, NULL, 0,
        NULL, NULL, 0, 0, 0,
        NULL, NULL, &lens);
    CuAssertPtrNotNull(tc, pack);
    /* 可变头 10 + clientid(长度前缀 2 + 体 0) = remaining 12；总字节 1+1+12=14，修复前多写 NUL 会得 15 */
    CuAssertIntEquals(tc, 12, (uint8_t)pack[1]);
    CuAssertIntEquals(tc, 14, (int32_t)lens);

    buffer_ctx buf;
    _mq_to_buf(&buf, pack, lens);
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = _MQ_INIT;
    int32_t status = PROT_INIT;
    mqtt_pack_ctx *p = _t_mqtt_unpack(0 /*server*/, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, p);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    /* 解包完整消费整个包，无残留（修复前残留 1 个 NUL）*/
    CuAssertIntEquals(tc, 0, (int32_t)buffer_size(&buf));
    _mqtt_pkfree(p);
    _mqtt_udfree(&ud);
    buffer_free(&buf);

    /* MQTT 3.1.1 [MQTT-3.1.3-7]：空 clientid + cleanstart=0 是禁止组合，必须被拒（返回 NULL）*/
    size_t rejlens = 0;
    char *rej = mqtt_pack_connect(MQTT_311, 0 /*cleanstart=0*/, 60,
        "", NULL, NULL, 0,
        NULL, NULL, 0, 0, 0,
        NULL, NULL, &rejlens);
    CuAssertTrue(tc, NULL == rej);

    /* NULL clientid 等价于零长度：cleanstart=1 时正常打包(cidlens=0)，全程不解引用 NULL */
    size_t nlens = 0;
    char *npack = mqtt_pack_connect(MQTT_311, 1, 60,
        NULL, NULL, NULL, 0,
        NULL, NULL, 0, 0, 0,
        NULL, NULL, &nlens);
    CuAssertPtrNotNull(tc, npack);
    CuAssertIntEquals(tc, 14, (int32_t)nlens);
    FREE(npack);
}
/* =======================================================================
 * varint —— MQTT 7-bit 变长编解码 + off>=blens 边界(回归)
 * ======================================================================= */
static void test_varint(CuTest *tc) {
    char enc[4];
    // 编码：字节数与上界溢出
    CuAssertIntEquals(tc, 1, varint_encode_mqtt(0, enc));
    CuAssertIntEquals(tc, 1, varint_encode_mqtt(127, enc));
    CuAssertIntEquals(tc, 2, varint_encode_mqtt(128, enc));
    CuAssertIntEquals(tc, 4, varint_encode_mqtt(0x0FFFFFFF, enc));
    CuAssertIntEquals(tc, 0, varint_encode_mqtt(0x10000000, enc));// 超 256MB-1 上界

    // 编解码往返：300 → 2 字节
    buffer_ctx b;
    buffer_init(&b);
    int32_t n = varint_encode_mqtt(300, enc);
    CuAssertIntEquals(tc, 2, n);
    buffer_append(&b, enc, (size_t)n);
    size_t val = 0;
    CuAssertIntEquals(tc, 2, varint_decode_mqtt(&b, 0, buffer_size(&b), &val));
    CuAssertTrue(tc, 300 == val);
    buffer_free(&b);

    // 4 字节全延续位(0x80)；不能用字符串字面量："\x80\x80" 会被当成单个十六进制转义
    char allcont[4] = { (char)0x80, (char)0x80, (char)0x80, (char)0x80 };
    buffer_init(&b);
    buffer_append(&b, allcont, sizeof(allcont));

    // 4 字节内未结束 → ERR_FAILED
    val = 1;
    CuAssertIntEquals(tc, ERR_FAILED, varint_decode_mqtt(&b, 0, buffer_size(&b), &val));
    // off == blens：可读字节为 0 → ERR_FAILED
    CuAssertIntEquals(tc, ERR_FAILED, varint_decode_mqtt(&b, 4, buffer_size(&b), &val));
    // off > blens(回归点)：blens-off 无符号回绕,修复前越界读 buffer_at,修复后直接 ERR_FAILED
    val = 12345;
    CuAssertIntEquals(tc, ERR_FAILED, varint_decode_mqtt(&b, 9, buffer_size(&b), &val));
    CuAssertTrue(tc, 0 == val);// 失败路径仍清零 *value
    buffer_free(&b);

    // 四段边界逐字节比对 + 往返：只验字节数的话,字节序或延续位标志写错照样通过
    const struct { uint32_t val; int32_t n; unsigned char enc[4]; } vecs[] = {
        { 0,         1, { 0x00 } },
        { 1,         1, { 0x01 } },
        { 127,       1, { 0x7F } },
        { 128,       2, { 0x80, 0x01 } },
        { 16383,     2, { 0xFF, 0x7F } },
        { 16384,     3, { 0x80, 0x80, 0x01 } },
        { 2097151,   3, { 0xFF, 0xFF, 0x7F } },
        { 2097152,   4, { 0x80, 0x80, 0x80, 0x01 } },
        { 268435455, 4, { 0xFF, 0xFF, 0xFF, 0x7F } }
    };
    buffer_ctx vb;
    size_t vval;
    int32_t vi, vk, vn;
    for (vi = 0; vi < (int32_t)ARRAY_SIZE(vecs); vi++) {
        ZERO(enc, sizeof(enc));
        vn = varint_encode_mqtt(vecs[vi].val, enc);
        CuAssertIntEquals(tc, vecs[vi].n, vn);
        for (vk = 0; vk < vn; vk++) {
            CuAssertIntEquals(tc, (int)vecs[vi].enc[vk], (int)(unsigned char)enc[vk]);
        }
        buffer_init(&vb);
        buffer_append(&vb, enc, (size_t)vn);
        CuAssertIntEquals(tc, vn, varint_decode_mqtt(&vb, 0, buffer_size(&vb), &vval));
        CuAssertTrue(tc, (size_t)vecs[vi].val == vval);
        buffer_free(&vb);
    }

    // 单字节延续位置起但可读量耗尽
    char trunc1[1] = { (char)0x80 };
    buffer_init(&vb);
    buffer_append(&vb, trunc1, sizeof(trunc1));
    CuAssertIntEquals(tc, ERR_FAILED, varint_decode_mqtt(&vb, 0, buffer_size(&vb), &vval));
    buffer_free(&vb);

    // 从非零 off 起解：前置两字节噪声不影响取值
    char noise[6] = { (char)0xAA, (char)0xBB, (char)0xFF, (char)0xFF, (char)0xFF, (char)0x7F };
    buffer_init(&vb);
    buffer_append(&vb, noise, sizeof(noise));
    CuAssertIntEquals(tc, 4, varint_decode_mqtt(&vb, 2, buffer_size(&vb), &vval));
    CuAssertTrue(tc, 268435455 == vval);
    buffer_free(&vb);

    // blens 小于实际可读量：按 blens 判截断,而非按 buffer 真实长度
    char two[2] = { (char)0x80, (char)0x01 };
    buffer_init(&vb);
    buffer_append(&vb, two, sizeof(two));
    CuAssertIntEquals(tc, ERR_FAILED, varint_decode_mqtt(&vb, 0, 1, &vval));
    CuAssertIntEquals(tc, 2, varint_decode_mqtt(&vb, 0, 2, &vval));
    CuAssertTrue(tc, 128 == vval);
    buffer_free(&vb);
}

void test_mqtt_pack(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_mqtt_connect_311);
    SUITE_ADD_TEST(suite, test_mqtt_connect_empty_clientid);
    SUITE_ADD_TEST(suite, test_mqtt_connect_will_null_payload);
    SUITE_ADD_TEST(suite, test_mqtt_connect_50_full);
    SUITE_ADD_TEST(suite, test_mqtt_connack);
    SUITE_ADD_TEST(suite, test_mqtt_acks);
    SUITE_ADD_TEST(suite, test_mqtt_publish);
    SUITE_ADD_TEST(suite, test_mqtt_publish_block);
    SUITE_ADD_TEST(suite, test_mqtt_publish_bad_topiclen);
    SUITE_ADD_TEST(suite, test_mqtt_subscribe);
    SUITE_ADD_TEST(suite, test_mqtt_malformed_reject);
    SUITE_ADD_TEST(suite, test_mqtt_ping_pong);
    SUITE_ADD_TEST(suite, test_mqtt_disconnect);
    SUITE_ADD_TEST(suite, test_mqtt_auth);
    SUITE_ADD_TEST(suite, test_mqtt_props_reject);
    SUITE_ADD_TEST(suite, test_mqtt_auth_compact_no_props);
    SUITE_ADD_TEST(suite, test_mqtt_auth_scram_sha256);
    SUITE_ADD_TEST(suite, test_mqtt_props);
    SUITE_ADD_TEST(suite, test_mqtt_reason);
    SUITE_ADD_TEST(suite, test_mqtt_struct_null_free);
    SUITE_ADD_TEST(suite, test_mqtt_struct_empty_free);
    SUITE_ADD_TEST(suite, test_mqtt_struct_propertie_free);
    SUITE_ADD_TEST(suite, test_varint);
}
