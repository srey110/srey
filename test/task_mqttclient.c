#include "task_mqttclient.h"

static int32_t _prt = 1;
static SOCKET _fd = INVALID_SOCK;
static uint64_t _skid;
static mqtt_protversion _version = MQTT_50;

static void _net_close(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client) {
    LOG_INFO("DISCONNECTED");
}
static void _net_connect(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, int32_t erro) {
    binary_ctx connprop;
    binary_init(&connprop, NULL, 0, 0);
    mqtt_props_fixnum(&connprop, SESSION_EXPIRY, 120);
    mqtt_props_fixnum(&connprop, RECEIVE_MAXIMUM, 20000);
    mqtt_props_kv(&connprop, USER_PROPERTY, "key1", 4, "val1", 4);
    binary_ctx willprop;
    binary_init(&willprop, NULL, 0, 0);
    mqtt_props_fixnum(&willprop, WILLDELAY_INTERVAL, 60);
    mqtt_props_kv(&willprop, USER_PROPERTY, "key2", 4, "val2", 4);
    size_t lens;
    char *pk = mqtt_pack_connect(_version, 1, 120, "mqtt_srey123",
        "admin", "123", 3,
        "srey/will", "will message", strlen("will message"), 1, 1,
        &connprop, &willprop, &lens);
    FREE(connprop.data);
    FREE(willprop.data);
    if (NULL != pk) {
        ev_send(&task->loader->netev, fd, skid, pk, lens, 0);
        LOG_INFO("->CONNECT");
    }
}
static void _send_publish(mqtt_pack_ctx *pack, task_ctx *task, SOCKET fd, uint64_t skid, int8_t qos) {
    binary_ctx props;
    binary_init(&props, NULL, 0, 0);
    mqtt_props_kv(&props, USER_PROPERTY, "key1", 4, "val1", 4);
    size_t lens;
    char *pk = mqtt_pack_publish(pack->version, 1, qos, 1, "srey/will", (int16_t)randrange(100, 20000), "publish payload", strlen("publish payload"), &props, &lens);
    FREE(props.data);
    if (NULL != pk) {
        ev_send(&task->loader->netev, fd, skid, pk, lens, 0);
        LOG_INFO("->PUBLISH QOS %d", qos);
    }
}
static void _net_recv(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client, uint8_t slice, void *data, size_t size) {
    mqtt_pack_ctx *pack = (mqtt_pack_ctx *)data;
    switch (pack->fixhead.prot) {
    case MQTT_CONNACK: {
        mqtt_connack_varhead *vh = pack->varhead;
        LOG_INFO("<-CONNACK %d (%s)", vh->reason, mqtt_reason(pack->fixhead.prot, vh->reason));
        if (0x00 != vh->reason) {
            break;
        } 
        _send_publish(pack, task, fd, skid, 1);
        break;
    }
    case MQTT_PUBACK: {
        mqtt_puback_varhead *vh = pack->varhead;
        LOG_INFO("<-PUBACK %d (%s)", (int32_t)vh->reason, mqtt_reason(pack->fixhead.prot, vh->reason));
        if (0x00 == vh->reason || 0x10 == vh->reason) {
            _send_publish(pack, task, fd, skid, 2);
        }
        break;
    }
    case MQTT_PUBREC: {
        mqtt_pubrec_varhead *vh = pack->varhead;
        LOG_INFO("<-PUBREC %d (%s)", (int32_t)vh->reason, mqtt_reason(pack->fixhead.prot, vh->reason));
        if (0x00 == vh->reason || 0x10 == vh->reason) {
            size_t lens;
            char *pk = mqtt_pack_pubrel(pack->version, vh->packid, 0, NULL, &lens);
            if (NULL != pk) {
                ev_send(&task->loader->netev, fd, skid, pk, lens, 0);
                LOG_INFO("->PUBREL");
            }
        }
        break;
    }
    case MQTT_PUBCOMP: {
        mqtt_pubcomp_varhead *vh = pack->varhead;
        LOG_INFO("<-PUBCOMP %d (%s)", (int32_t)vh->reason, mqtt_reason(pack->fixhead.prot, vh->reason));
        binary_ctx topics;
        binary_init(&topics, NULL, 0, 0);
        mqtt_topics_subscribe(&topics, pack->version, "/test/topic1", 1, 1, 1, 1);
        size_t lens;
        char *pk = mqtt_pack_subscribe(pack->version, (int16_t)randrange(100, 20000), &topics, NULL, &lens);
        FREE(topics.data);
        if (NULL != pk) {
            ev_send(&task->loader->netev, fd, skid, pk, lens, 0);
            LOG_INFO("->SUBSCRIBE");
        }
        break;
    }
    case MQTT_SUBACK: {
        mqtt_suback_varhead *vh = pack->varhead;
        mqtt_suback_payload *pl = pack->payload;
        LOG_INFO("<-SUBACK %d (%s)", (int32_t)pl->reasons[0], mqtt_reason(pack->fixhead.prot, pl->reasons[0]));
        if (0x00 == pl->reasons[0] || 0x01 == pl->reasons[0] || 0x02 == pl->reasons[0]) {
            binary_ctx topics;
            binary_init(&topics, NULL, 0, 0);
            mqtt_topics_unsubscribe(&topics, "/test/topic1");
            size_t lens;
            char *pk = mqtt_pack_unsubscribe(pack->version, (int16_t)randrange(100, 20000), &topics, NULL, &lens);
            FREE(topics.data);
            if (NULL != pk) {
                ev_send(&task->loader->netev, fd, skid, pk, lens, 0);
                LOG_INFO("->UNSUBSCRIBE");
            }
        }
        break;
    }
    case MQTT_UNSUBACK: {
        mqtt_unsuback_varhead *vh = pack->varhead;
        mqtt_unsuback_payload *pl = pack->payload;
        if (NULL != pl) {
            LOG_INFO("<-UNSUBACK %d (%s)", (int32_t)pl->reasons[0], mqtt_reason(pack->fixhead.prot, pl->reasons[0]));
        } else {
            LOG_INFO("<-UNSUBACK");
        }
        size_t lens;
        char *pk = mqtt_pack_ping(pack->version, &lens);
        if (NULL != pk) {
            ev_send(&task->loader->netev, _fd, _skid, pk, lens, 0);
            LOG_INFO("->PING");
        }
        break;
    }
    case MQTT_PINGRESP: {
        LOG_INFO("<-PONG");
        size_t lens;
        char *pk = mqtt_pack_disconnect(pack->version, 0, NULL, &lens);
        if (NULL != pk) {
            ev_send(&task->loader->netev, _fd, _skid, pk, lens, 0);
            LOG_INFO("->DISCONNECT");
        }
        break;
    }
    }
}
static void _startup(task_ctx *task) {
    on_closed(task, _net_close);
    on_connected(task, _net_connect);
    on_recved(task, _net_recv);
    size_t n;
    dns_ip *ips = dns_lookup(task, "broker.emqx.io", 0, &n);
    if (NULL == ips) {
        if (_prt) {
            LOG_WARN("dns_lookup error.");
        }
        return;
    }
    mqtt_ctx *mq;
    CALLOC(mq, 1, sizeof(mqtt_ctx));
    mq->version = _version;
    coro_sleep(task, 2000);
    _fd = task_connect(task, PACK_MQTT, NULL, ips[0].ip, 1883, &_skid, 0, mq);
    FREE(ips);
}
void task_mqtt_client_start(loader_ctx *loader, name_t name, int32_t pt) {
    _prt = pt;
    task_ctx *task = task_new(loader, name, NULL, NULL, NULL);
    task_register(task, _startup, NULL);
}
