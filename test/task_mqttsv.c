#include "task_mqttsv.h"

static int32_t _prt = 1;
static int32_t _publish = 0;

static void _net_recv(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client, uint8_t slice, void *data, size_t size) {
    mqtt_pack_ctx *pack = (mqtt_pack_ctx *)data;
    switch (pack->fixhead.prot) {
    case MQTT_CONNECT: {
        if (_prt) {
            LOG_INFO("CONNECT");
        }
        //mqtt_connect_varhead *vh = pack->varhead;
        mqtt_connect_payload *pl = pack->payload;
        char *pk;
        size_t plens;
        if (pack->version >= MQTT_50) {
            binary_ctx props;
            binary_init(&props, NULL, 0, 0);
            mqtt_props_fixnum(&props, SESSION_EXPIRY, 120);
            mqtt_props_fixnum(&props, RECEIVE_MAXIMUM, 15000);
            mqtt_props_fixnum(&props, MAXIMUM_QOS, 1);//
            mqtt_props_binary(&props, CLIENT_ID, pl->clientid, (int32_t)strlen(pl->clientid));
            mqtt_props_kv(&props, USER_PROPERTY, "key1", 4, "val1", 4);
            mqtt_props_kv(&props, USER_PROPERTY, "key2", 4, "val2", 4);
            pk = mqtt_pack_connack(pack->version, 1, 0, &props, &plens);
            FREE(props.data);
        } else {
            pk = mqtt_pack_connack(pack->version, 1, 0, NULL, &plens);
        }
        if (NULL != pk) {
            ev_send(&task->loader->netev, fd, skid, pk, plens, 0);
        }
        break;
    }
    case MQTT_PUBLISH: {
        if (_prt) {
            LOG_INFO("PUBLISH");
        }
        char *pk = NULL;
        size_t lens;
        mqtt_publish_varhead *vh = pack->varhead;
        mqtt_publish_payload *pl = pack->payload;
        if (0 == STRCMP(pl->content, "bye")) {
            pk = mqtt_pack_disconnect(pack->version, 0, NULL, &lens);
            if (NULL != pk) {
                ev_send(&task->loader->netev, fd, skid, pk, lens, 0);
            }
            break;
        }
        if (0 == vh->qos) {
            break;
        }
        _publish++;
        binary_ctx props;
        binary_init(&props, NULL, 0, 0);
        mqtt_props_kv(&props, USER_PROPERTY, "key1", 4, "val1", 4);
        if (1 == vh->qos) {
            if (1 == _publish) {
                pk = mqtt_pack_puback(pack->version, vh->packid, 0, &props, &lens);
            }
            if (2 == _publish) {
                pk = mqtt_pack_puback(pack->version, vh->packid, 0x10, NULL, &lens);
            }
            if (3 == _publish) {
                pk = mqtt_pack_puback(pack->version, vh->packid, 0, NULL, &lens);
            }
        }
        if (2 == vh->qos) {
            if (1 == _publish) {
                pk = mqtt_pack_pubrec(pack->version, vh->packid, 0, &props, &lens);
            } else {
                pk = mqtt_pack_pubrec(pack->version, vh->packid, 0, NULL, &lens);
            }
        }
        FREE(props.data);
        if (NULL != pk) {
            ev_send(&task->loader->netev, fd, skid, pk, lens, 0);
        }
        if (3 == _publish) {
            _publish = 0;
        }
        break;
    }
    case MQTT_PUBREL: {
        if (_prt) {
            LOG_INFO("PUBREL");
        }
        char *pk = NULL;
        size_t lens;
        mqtt_pubrel_varhead *vh = pack->varhead;
        pk = mqtt_pack_pubcomp(pack->version, vh->packid, 0, NULL, &lens);
        if (NULL != pk) {
            ev_send(&task->loader->netev, fd, skid, pk, lens, 0);
        }
        break;
    }
    case MQTT_SUBSCRIBE: {
        if (_prt) {
            LOG_INFO("SUBSCRIBE");
        }
        mqtt_subscribe_varhead *vh = pack->varhead;
        char *pk = NULL;
        size_t lens;
        uint8_t reasons[1] = { 0 };
        binary_ctx props;
        binary_init(&props, NULL, 0, 0);
        mqtt_props_kv(&props, USER_PROPERTY, "key1", 4, "val1", 4);
        pk = mqtt_pack_suback(pack->version, vh->packid, reasons, 1, &props, &lens);
        if (NULL != pk) {
            ev_send(&task->loader->netev, fd, skid, pk, lens, 0);
        }
        FREE(props.data);
        break;
    }
    case MQTT_UNSUBSCRIBE: {
        if (_prt) {
            LOG_INFO("UNSUBSCRIBE");
        }
        mqtt_unsubscribe_varhead *vh = pack->varhead;
        char *pk = NULL;
        size_t lens;
        uint8_t reasons[1] = { 0 };
        binary_ctx props;
        binary_init(&props, NULL, 0, 0);
        mqtt_props_kv(&props, USER_PROPERTY, "key1", 4, "val1", 4);
        pk = mqtt_pack_unsuback(pack->version, vh->packid, reasons, 1, &props, &lens);
        if (NULL != pk) {
            ev_send(&task->loader->netev, fd, skid, pk, lens, 0);
        }
        FREE(props.data);
        break;
    }
    case MQTT_PINGREQ: {
        if (_prt) {
            LOG_INFO("PING");
        }
        char *pk = NULL;
        size_t lens;
        pk = mqtt_pack_pong(pack->version, &lens);
        if (NULL != pk) {
            ev_send(&task->loader->netev, fd, skid, pk, lens, 0);
        }
        break;
    }
    case MQTT_DISCONNECT: {
        if (_prt) {
            LOG_INFO("DISCONNECT");
        }
        break;
    }
    case MQTT_AUTH: {
        if (_prt) {
            LOG_INFO("AUTH");
        }
        break;
    }
    default:
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
