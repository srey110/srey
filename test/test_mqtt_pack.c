#include "test_mqtt_pack.h"
#include "lib.h"
#include "protocol/mqtt/mqtt_pack.h"
#include "protocol/mqtt/mqtt_struct.h"
#include "crypt/scram.h"

/* MQTT 状态机的 INIT/COMMAND 是 mqtt.c 内部 enum，定义在文件作用域；
 * 测试中按数值约定使用 0=INIT, 1=COMMAND。*/
#define _MQ_INIT     0
#define _MQ_COMMAND  1

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
    mqtt_pack_ctx *p = mqtt_unpack(0 /*server*/, &buf, &ud, &status);
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
    mqtt_pack_ctx *p = mqtt_unpack(0, &buf, &ud, &status);
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
static void test_mqtt_connack(CuTest *tc) {
    size_t lens = 0;
    char *pack = mqtt_pack_connack(MQTT_311, 1, 0, NULL, &lens);
    CuAssertPtrNotNull(tc, pack);

    buffer_ctx buf;
    _mq_to_buf(&buf, pack, lens);

    mqtt_ctx *mq;
    CALLOC(mq, 1, sizeof(mqtt_ctx));
    mq->version = MQTT_311;

    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = _MQ_INIT;
    ud.context = mq;

    int32_t status = PROT_INIT;
    mqtt_pack_ctx *p = mqtt_unpack(1 /*client*/, &buf, &ud, &status);
    CuAssertPtrNotNull(tc, p);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertIntEquals(tc, MQTT_CONNACK, p->fixhead.prot);

    mqtt_connack_varhead *vh = (mqtt_connack_varhead *)p->varhead;
    CuAssertIntEquals(tc, 1, vh->sesspresent);
    CuAssertIntEquals(tc, 0, vh->reason);

    _mqtt_pkfree(p);
    _mqtt_udfree(&ud);
    buffer_free(&buf);
}

static void _mqtt_pack_ack_test(CuTest *tc, mqtt_protversion ver, mqtt_prot expected,
    char *(*packer)(mqtt_protversion, int16_t, uint8_t, binary_ctx *, size_t *)) {
    size_t lens = 0;
    char *pack = packer(ver, 12345, 0x00, NULL, &lens);
    CuAssertPtrNotNull(tc, pack);

    buffer_ctx buf;
    _mq_to_buf(&buf, pack, lens);

    mqtt_ctx *mq;
    CALLOC(mq, 1, sizeof(mqtt_ctx));
    mq->version = ver;

    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = _MQ_COMMAND;
    ud.context = mq;

    int32_t status = PROT_INIT;
    mqtt_pack_ctx *p = mqtt_unpack(1, &buf, &ud, &status);
    CuAssertPtrNotNull(tc, p);
    CuAssertIntEquals(tc, (int)expected, (int)p->fixhead.prot);
    mqtt_pubackrel_varhead *vh = (mqtt_pubackrel_varhead *)p->varhead;
    CuAssertIntEquals(tc, 12345, vh->packid);

    _mqtt_pkfree(p);
    _mqtt_udfree(&ud);
    buffer_free(&buf);
}

static void test_mqtt_acks(CuTest *tc) {
    _mqtt_pack_ack_test(tc, MQTT_311, MQTT_PUBACK,  mqtt_pack_puback);
    _mqtt_pack_ack_test(tc, MQTT_311, MQTT_PUBREC,  mqtt_pack_pubrec);
    _mqtt_pack_ack_test(tc, MQTT_311, MQTT_PUBREL,  mqtt_pack_pubrel);
    _mqtt_pack_ack_test(tc, MQTT_311, MQTT_PUBCOMP, mqtt_pack_pubcomp);
    _mqtt_pack_ack_test(tc, MQTT_50,  MQTT_PUBACK,  mqtt_pack_puback);
    _mqtt_pack_ack_test(tc, MQTT_50,  MQTT_PUBREC,  mqtt_pack_pubrec);
    _mqtt_pack_ack_test(tc, MQTT_50,  MQTT_PUBREL,  mqtt_pack_pubrel);
    _mqtt_pack_ack_test(tc, MQTT_50,  MQTT_PUBCOMP, mqtt_pack_pubcomp);
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

    mqtt_ctx *mq;
    CALLOC(mq, 1, sizeof(mqtt_ctx));
    mq->version = MQTT_311;

    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = _MQ_COMMAND;
    ud.context = mq;

    int32_t status = PROT_INIT;
    mqtt_pack_ctx *p = mqtt_unpack(0, &buf, &ud, &status);
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
    p = mqtt_unpack(0, &buf, &ud, &status);
    CuAssertPtrNotNull(tc, p);
    vh = (mqtt_publish_varhead *)p->varhead;
    CuAssertIntEquals(tc, 1, vh->qos);
    CuAssertIntEquals(tc, 1, vh->retain);
    CuAssertIntEquals(tc, 777, vh->packid);

    _mqtt_pkfree(p);
    _mqtt_udfree(&ud);
    buffer_free(&buf);
}

/* =======================================================================
 * SUBSCRIBE / SUBACK / UNSUBSCRIBE / UNSUBACK
 * ======================================================================= */
static void test_mqtt_subscribe(CuTest *tc) {
    binary_ctx topics;
    binary_init(&topics, NULL, 0, 64);
    mqtt_topics_subscribe(&topics, MQTT_311, "topic/a", 0, 0, 0, 0);
    mqtt_topics_subscribe(&topics, MQTT_311, "topic/b", 1, 0, 0, 0);

    size_t lens = 0;
    char *pack = mqtt_pack_subscribe(MQTT_311, 0xAABB, &topics, NULL, &lens);
    CuAssertPtrNotNull(tc, pack);
    binary_free(&topics);

    buffer_ctx buf;
    _mq_to_buf(&buf, pack, lens);

    mqtt_ctx *mq;
    CALLOC(mq, 1, sizeof(mqtt_ctx));
    mq->version = MQTT_311;
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = _MQ_COMMAND;
    ud.context = mq;

    int32_t status = PROT_INIT;
    mqtt_pack_ctx *p = mqtt_unpack(0, &buf, &ud, &status);
    CuAssertPtrNotNull(tc, p);
    CuAssertIntEquals(tc, MQTT_SUBSCRIBE, p->fixhead.prot);
    mqtt_subreqresp_varhead *vh = (mqtt_subreqresp_varhead *)p->varhead;
    CuAssertIntEquals(tc, (int16_t)0xAABB, vh->packid);

    mqtt_subscribe_payload *pl = (mqtt_subscribe_payload *)p->payload;
    CuAssertIntEquals(tc, 2, (int)array_size(&pl->subop));
    subscribe_option *opt0 = *(subscribe_option **)array_at(&pl->subop, 0);
    subscribe_option *opt1 = *(subscribe_option **)array_at(&pl->subop, 1);
    CuAssertStrEquals(tc, "topic/a", opt0->topic);
    CuAssertStrEquals(tc, "topic/b", opt1->topic);
    CuAssertIntEquals(tc, 1, opt1->qos);

    _mqtt_pkfree(p);
    buffer_free(&buf);

    /* SUBACK */
    uint8_t reasons[] = { 0x00, 0x01 };
    pack = mqtt_pack_suback(MQTT_311, 0xAABB, reasons, sizeof(reasons), NULL, &lens);
    CuAssertPtrNotNull(tc, pack);
    _mq_to_buf(&buf, pack, lens);

    status = PROT_INIT;
    ud.status = _MQ_COMMAND;
    p = mqtt_unpack(1, &buf, &ud, &status);
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
    binary_init(&untopics, NULL, 0, 64);
    mqtt_topics_unsubscribe(&untopics, "topic/a");
    mqtt_topics_unsubscribe(&untopics, "topic/b");
    pack = mqtt_pack_unsubscribe(MQTT_311, 0xCCDD, &untopics, NULL, &lens);
    CuAssertPtrNotNull(tc, pack);
    binary_free(&untopics);
    _mq_to_buf(&buf, pack, lens);

    status = PROT_INIT;
    ud.status = _MQ_COMMAND;
    p = mqtt_unpack(0, &buf, &ud, &status);
    CuAssertPtrNotNull(tc, p);
    CuAssertIntEquals(tc, MQTT_UNSUBSCRIBE, p->fixhead.prot);
    mqtt_unsubscribe_payload *upl = (mqtt_unsubscribe_payload *)p->payload;
    CuAssertIntEquals(tc, 2, (int)array_size(&upl->topics));
    _mqtt_pkfree(p);
    buffer_free(&buf);

    /* UNSUBACK (3.1.1 仅 packid) */
    pack = mqtt_pack_unsuback(MQTT_311, 0xCCDD, NULL, 0, NULL, &lens);
    CuAssertPtrNotNull(tc, pack);
    _mq_to_buf(&buf, pack, lens);

    status = PROT_INIT;
    ud.status = _MQ_COMMAND;
    p = mqtt_unpack(1, &buf, &ud, &status);
    CuAssertPtrNotNull(tc, p);
    CuAssertIntEquals(tc, MQTT_UNSUBACK, p->fixhead.prot);
    _mqtt_pkfree(p);
    _mqtt_udfree(&ud);
    buffer_free(&buf);
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
    mqtt_ctx *mq;
    CALLOC(mq, 1, sizeof(mqtt_ctx));
    mq->version = MQTT_311;
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = _MQ_COMMAND;
    ud.context = mq;

    int32_t status = PROT_INIT;
    mqtt_pack_ctx *p = mqtt_unpack(0, &buf, &ud, &status);
    CuAssertPtrNotNull(tc, p);
    CuAssertIntEquals(tc, MQTT_PINGREQ, p->fixhead.prot);
    _mqtt_pkfree(p);
    buffer_free(&buf);

    /* PONG */
    pack = mqtt_pack_pong(&lens);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, 0xD0 == (uint8_t)pack[0]);  /* PINGRESP (13<<4) */
    _mq_to_buf(&buf, pack, lens);

    status = PROT_INIT;
    ud.status = _MQ_COMMAND;
    p = mqtt_unpack(1, &buf, &ud, &status);
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
    CuAssertTrue(tc, 0xE0 == (uint8_t)pack[0]);  /* DISCONNECT (14<<4) */

    buffer_ctx buf;
    _mq_to_buf(&buf, pack, lens);

    mqtt_ctx *mq;
    CALLOC(mq, 1, sizeof(mqtt_ctx));
    mq->version = MQTT_311;
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = _MQ_COMMAND;
    ud.context = mq;

    int32_t status = PROT_INIT;
    mqtt_pack_ctx *p = mqtt_unpack(0, &buf, &ud, &status);
    CuAssertPtrNotNull(tc, p);
    CuAssertIntEquals(tc, MQTT_DISCONNECT, p->fixhead.prot);
    _mqtt_pkfree(p);
    _mqtt_udfree(&ud);
    buffer_free(&buf);

    /* 5.0：含 reason 字段 */
    pack = mqtt_pack_disconnect(MQTT_50, 0x04 /*遗嘱断开*/, NULL, &lens);
    CuAssertPtrNotNull(tc, pack);
    _mq_to_buf(&buf, pack, lens);

    CALLOC(mq, 1, sizeof(mqtt_ctx));
    mq->version = MQTT_50;
    ZERO(&ud, sizeof(ud));
    ud.status = _MQ_COMMAND;
    ud.context = mq;
    status = PROT_INIT;

    p = mqtt_unpack(0, &buf, &ud, &status);
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
    CuAssertTrue(tc, 0xF0 == (uint8_t)pack[0]);  /* AUTH (15<<4) */

    buffer_ctx buf;
    _mq_to_buf(&buf, pack, lens);

    mqtt_ctx *mq;
    CALLOC(mq, 1, sizeof(mqtt_ctx));
    mq->version = MQTT_50;
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = _MQ_INIT;
    ud.context = mq;

    int32_t status = PROT_INIT;
    mqtt_pack_ctx *p = mqtt_unpack(0, &buf, &ud, &status);
    CuAssertPtrNotNull(tc, p);
    CuAssertIntEquals(tc, MQTT_AUTH, p->fixhead.prot);
    mqtt_reason_varhead *vh = (mqtt_reason_varhead *)p->varhead;
    CuAssertIntEquals(tc, 0x18, vh->reason);
    _mqtt_pkfree(p);
    _mqtt_udfree(&ud);
    buffer_free(&buf);
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

    mqtt_ctx *mq;
    CALLOC(mq, 1, sizeof(mqtt_ctx));
    mq->version = MQTT_50;
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.status = _MQ_INIT;
    ud.context = mq;

    int32_t status = PROT_INIT;
    mqtt_pack_ctx *p = mqtt_unpack(0, &buf, &ud, &status);
    CuAssertPtrNotNull(tc, p);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertIntEquals(tc, MQTT_AUTH, p->fixhead.prot);
    mqtt_reason_varhead *vh = (mqtt_reason_varhead *)p->varhead;
    CuAssertIntEquals(tc, 0x18, vh->reason);
    CuAssertPtrEquals(tc, NULL, vh->properties);  /* 紧凑形式无属性 */
    _mqtt_pkfree(p);
    _mqtt_udfree(&ud);
    buffer_free(&buf);
}

/* =======================================================================
 * mqtt_props_* —— 属性编码（5.0）
 * ======================================================================= */
static void test_mqtt_props(CuTest *tc) {
    binary_ctx props;
    binary_init(&props, NULL, 0, 64);

    /* fixnum (4 字节)：SESSION_EXPIRY = 0x11 */
    CuAssertIntEquals(tc, ERR_OK, mqtt_props_fixnum(&props, SESSION_EXPIRY, 3600));
    /* varnum (变长)：SUBSCRIPTION_ID = 0x0B */
    CuAssertIntEquals(tc, ERR_OK, mqtt_props_varnum(&props, SUBSCRIPTION_ID, 128));
    /* binary：AUTH_DATA = 0x16 */
    CuAssertIntEquals(tc, ERR_OK, mqtt_props_binary(&props, AUTH_DATA, "DATA", 4));
    /* kv：USER_PROPERTY = 0x26 */
    CuAssertIntEquals(tc, ERR_OK, mqtt_props_kv(&props, USER_PROPERTY, "k", 1, "v", 1));

    /* 至少写入了几个字节 */
    CuAssertTrue(tc, props.offset > 0);

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

/* 把 (method, data) 打包到 AUTH(reason, ...) 后写入 buffer */
static void _mqtt_pack_auth_sasl(buffer_ctx *out,
    uint8_t reason, const char *method, const char *data, size_t dlen) {
    binary_ctx props;
    binary_init(&props, NULL, 0, 64);
    CuAssert(NULL, "props: AUTH_METHOD",
        ERR_OK == mqtt_props_binary(&props, AUTH_METHOD, (void *)method, strlen(method)));
    CuAssert(NULL, "props: AUTH_DATA",
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
    ud.status = _MQ_INIT;  /* AUTH 走 _mqtt_init 路径 */
    ud.context = mq;

    int32_t status = PROT_INIT;
    mqtt_pack_ctx *p = mqtt_unpack(client_role, buf, &ud, &status);
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
    scram_set_user(cli, "alice");
    scram_set_pwd(cli, "correcthorsebatterystaple");
    scram_set_pwd(srv, "correcthorsebatterystaple");
    scram_set_salt(srv, (char *)salt, sizeof(salt));
    scram_set_iter(srv, 4096);

    /* mqtt_ctx 由两端各持一份，AUTH 报文以 MQTT 5.0 编码，version 必须一致 */
    mqtt_ctx mq_srv; ZERO(&mq_srv, sizeof(mq_srv)); mq_srv.version = MQTT_50;
    mqtt_ctx mq_cli; ZERO(&mq_cli, sizeof(mq_cli)); mq_cli.version = MQTT_50;

    /* ── step 1：客户端发 clientFirst ─────────────────────────── */
    char *cf = scram_first_message(cli);
    CuAssertPtrNotNull(tc, cf);

    buffer_ctx buf;
    _mqtt_pack_auth_sasl(&buf, 0x18, METHOD, cf, strlen(cf));

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
    _mqtt_pack_auth_sasl(&buf, 0x18, METHOD, sf, strlen(sf));

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
    _mqtt_pack_auth_sasl(&buf, 0x18, METHOD, clf, strlen(clf));

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
    FREE(clf);

    /* ── step 4：服务端发 serverFinal（reason=0x00=Success）──── */
    char *svf = scram_final_message(srv);
    CuAssertPtrNotNull(tc, svf);
    _mqtt_pack_auth_sasl(&buf, 0x00, METHOD, svf, strlen(svf));

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
    FREE(svf);

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
    _mqtt_publish_varhead_free(NULL);
    _mqtt_publish_payload_free(NULL);
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
    // publish varhead：properties + topic 均 NULL
    mqtt_publish_varhead *pvh;
    CALLOC(pvh, 1, sizeof(*pvh));
    _mqtt_publish_varhead_free(pvh);
    // publish payload：仅 FREE
    mqtt_publish_payload *ppl;
    CALLOC(ppl, 1, sizeof(*ppl));
    _mqtt_publish_payload_free(ppl);
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
        CALLOC(mq, 1, sizeof(mqtt_ctx));
        mq->version = ver;
        ud.context = mq;
    }

    int32_t status = PROT_INIT;
    mqtt_pack_ctx *p = mqtt_unpack(0 /*server*/, &buf, &ud, &status);
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
    mqtt_pack_ctx *p = mqtt_unpack(0 /*server*/, &buf, &ud, &status);
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
void test_mqtt_pack(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_mqtt_connect_311);
    SUITE_ADD_TEST(suite, test_mqtt_connect_empty_clientid);
    SUITE_ADD_TEST(suite, test_mqtt_connect_50_full);
    SUITE_ADD_TEST(suite, test_mqtt_connack);
    SUITE_ADD_TEST(suite, test_mqtt_acks);
    SUITE_ADD_TEST(suite, test_mqtt_publish);
    SUITE_ADD_TEST(suite, test_mqtt_subscribe);
    SUITE_ADD_TEST(suite, test_mqtt_malformed_reject);
    SUITE_ADD_TEST(suite, test_mqtt_ping_pong);
    SUITE_ADD_TEST(suite, test_mqtt_disconnect);
    SUITE_ADD_TEST(suite, test_mqtt_auth);
    SUITE_ADD_TEST(suite, test_mqtt_auth_compact_no_props);
    SUITE_ADD_TEST(suite, test_mqtt_auth_scram_sha256);
    SUITE_ADD_TEST(suite, test_mqtt_props);
    SUITE_ADD_TEST(suite, test_mqtt_reason);
    SUITE_ADD_TEST(suite, test_mqtt_struct_null_free);
    SUITE_ADD_TEST(suite, test_mqtt_struct_empty_free);
    SUITE_ADD_TEST(suite, test_mqtt_struct_propertie_free);
}
