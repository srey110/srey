#include "task_mqttsv.h"

static int32_t _prt = 1;

static void _net_recv(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client, uint8_t slice, void *data, size_t size) {
    mqtt_pack_ctx *pack = (mqtt_pack_ctx *)data;
    switch (pack->fixhead.prot) {
    case MQTT_CONNECT: {
        mqtt_connect_varhead *vh = pack->varhead;
        mqtt_connect_payload *pl = pack->payload;
        size_t plens;
        if (pack->version >= MQTT_50) {
            binary_ctx props;
            mqtt_props_init(&props);
            mqtt_props_fixnum(&props, SESSION_EXPIRY, 120);
            mqtt_props_fixnum(&props, RECEIVE_MAXIMUM, 15000);
            mqtt_props_fixnum(&props, MAXIMUM_QOS, 1);//
            mqtt_props_binary(&props, CLIENT_ID, "mqtt_cid123", (int32_t)strlen("mqtt_cid123"));
            mqtt_props_kv(&props, USER_PROPERTY, "key1", 4, "val1", 4);
            mqtt_props_kv(&props, USER_PROPERTY, "key2", 4, "val2", 4);
            char *pk = mqtt_pack_connack(pack->version, 1, 0, &props, &plens);
            mqtt_props_free(&props);
            if (NULL != pk) {
                ev_send(&task->loader->netev, fd, skid, pk, plens, 0);
            }
        } else {
            char *pk = mqtt_pack_connack(pack->version, 1, 0, NULL, &plens);
            if (NULL != pk) {
                ev_send(&task->loader->netev, fd, skid, pk, plens, 0);
            }
        }
        break;
    }
    case MQTT_CONNACK:
        if (_prt) {
            LOG_INFO("MQTT Á´½Ó³É¹¦.");
        }        
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
}
static void _startup(task_ctx *task) {
    on_recved(task, _net_recv);
    uint64_t id;
    task_listen(task, PACK_MQTT, NULL, "0.0.0.0", 15005, &id, 0);
}
void task_mqtt_sv_start(loader_ctx *loader, name_t name, int32_t pt) {
    _prt = pt;
    task_ctx *task = task_new(loader, name, NULL, NULL, NULL);
    task_register(task, _startup, NULL);
}
