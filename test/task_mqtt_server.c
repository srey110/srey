#include "task_mqtt_server.h"

static uint16_t _port = 0;
static int32_t _prt = 1;
// QoS1/2 PUBLISH 轮次计数，循环覆盖三种 PUBACK reason code 分支
static int32_t _publish = 0;

// 收到 MQTT 数据包，按协议类型分发处理
static void _net_recv(task_ctx *task, sk_id *sk, subtype_t pktype, uint8_t client, uint8_t slice, void *data, size_t size) {
    (void)pktype;
    (void)client;
    (void)slice;
    (void)size;
    mqtt_pack_ctx *pack = (mqtt_pack_ctx *)data;
    switch (pack->fixhead.prot) {
    case MQTT_CONNECT: {
        // 应答 CONNACK，MQTT 5.0 时携带扩展属性（session expiry、receive maximum、
        // maximum qos、client id 回显、user property）
        if (_prt) {
            LOG_INFO("CONNECT<-C");
        }
        mqtt_connect_payload *pl = pack->payload;
        char *pk;
        size_t plens;
        if (pack->version >= MQTT_50) {
            binary_ctx props;
            binary_init(&props, NULL, 0, 0);
            mqtt_props_fixnum(&props, SESSION_EXPIRY, 120);
            mqtt_props_fixnum(&props, RECEIVE_MAXIMUM, 15000);
            mqtt_props_fixnum(&props, MAXIMUM_QOS, 1);
            mqtt_props_binary(&props, CLIENT_ID, pl->clientid, (int32_t)strlen(pl->clientid));
            mqtt_props_kv(&props, USER_PROPERTY, "key1", 4, "val1", 4);
            mqtt_props_kv(&props, USER_PROPERTY, "key2", 4, "val2", 4);
            pk = mqtt_pack_connack(pack->version, 1, 0, &props, &plens);
            binary_free(&props);
        } else {
            pk = mqtt_pack_connack(pack->version, 1, 0, NULL, &plens);
        }
        if (NULL != pk) {
            if (_prt) {
                LOG_INFO("S->CONNACK");
            }
            ev_send(&task->loader->netev, sk->fd, sk->skid, pk, plens, 0);
        }
        break;
    }
    case MQTT_PUBLISH: {
        // QoS0 不回复；收到 "bye" 时主动发 DISCONNECT 结束会话；
        // QoS1 轮流覆盖三种 PUBACK：(1) reason=0x00 含属性、(2) reason=0x10、(3) reason=0x00 无属性；
        // QoS2 回复 PUBREC，首次携带属性，后续不带
        if (_prt) {
            LOG_INFO("PUBLISH<-C");
        }
        char *pk = NULL;
        size_t lens;
        mqtt_publish_varhead *vh = pack->varhead;
        mqtt_publish_payload *pl = pack->payload;
        if (0 == STRICMP(pl->content, "bye")) {
            pk = mqtt_pack_disconnect(pack->version, 0, NULL, &lens);
            if (NULL != pk) {
                if (_prt) {
                    LOG_INFO("S->DISCONNECT");
                }
                ev_send(&task->loader->netev, sk->fd, sk->skid, pk, lens, 0);
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
            if (_prt) {
                LOG_INFO("S->PUBACK");
            }
        }
        if (2 == vh->qos) {
            if (1 == _publish) {
                pk = mqtt_pack_pubrec(pack->version, vh->packid, 0, &props, &lens);
            } else {
                pk = mqtt_pack_pubrec(pack->version, vh->packid, 0, NULL, &lens);
            }
            if (_prt) {
                LOG_INFO("S->PUBREC");
            }
        }
        binary_free(&props);
        if (NULL != pk) {
            ev_send(&task->loader->netev, sk->fd, sk->skid, pk, lens, 0);
        }
        if (3 == _publish) {
            _publish = 0;
        }
        break;
    }
    case MQTT_PUBREL: {
        // QoS2 第三步：回复 PUBCOMP 完成握手
        if (_prt) {
            LOG_INFO("PUBREL<-C");
        }
        char *pk = NULL;
        size_t lens;
        mqtt_pubackrel_varhead *vh = pack->varhead;
        pk = mqtt_pack_pubcomp(pack->version, vh->packid, 0, NULL, &lens);
        if (NULL != pk) {
            if (_prt) {
                LOG_INFO("S->PUBCOMP");
            }
            ev_send(&task->loader->netev, sk->fd, sk->skid, pk, lens, 0);
        }
        break;
    }
    case MQTT_SUBSCRIBE: {
        // 回复 SUBACK（reason=0x00，含 user property），
        // 之后主动向订阅方推送一条 QoS0 PUBLISH，覆盖服务端→客户端 PUBLISH 路径
        if (_prt) {
            LOG_INFO("SUBSCRIBE<-C");
        }
        mqtt_subreqresp_varhead *vh = pack->varhead;
        char *pk = NULL;
        size_t lens;
        uint8_t reasons[1] = { 0 };
        binary_ctx props;
        binary_init(&props, NULL, 0, 0);
        mqtt_props_kv(&props, USER_PROPERTY, "key1", 4, "val1", 4);
        pk = mqtt_pack_suback(pack->version, vh->packid, reasons, 1, &props, &lens);
        if (NULL != pk) {
            if (_prt) {
                LOG_INFO("S->SUBACK");
            }
            ev_send(&task->loader->netev, sk->fd, sk->skid, pk, lens, 0);
        }
        binary_free(&props);
        pk = mqtt_pack_publish(pack->version, 0, 0, 0, "/test/topic1", 0, "server push", strlen("server push"), NULL, &lens);
        if (NULL != pk) {
            if (_prt) {
                LOG_INFO("S->PUBLISH QoS0");
            }
            ev_send(&task->loader->netev, sk->fd, sk->skid, pk, lens, 0);
        }
        break;
    }
    case MQTT_UNSUBSCRIBE: {
        // 回复 UNSUBACK（reason=0x00，含 user property）
        if (_prt) {
            LOG_INFO("UNSUBSCRIBE<-C");
        }
        mqtt_subreqresp_varhead *vh = pack->varhead;
        char *pk = NULL;
        size_t lens;
        uint8_t reasons[1] = { 0 };
        binary_ctx props;
        binary_init(&props, NULL, 0, 0);
        mqtt_props_kv(&props, USER_PROPERTY, "key1", 4, "val1", 4);
        pk = mqtt_pack_unsuback(pack->version, vh->packid, reasons, 1, &props, &lens);
        if (NULL != pk) {
            if (_prt) {
                LOG_INFO("S->UNSUBACK");
            }
            ev_send(&task->loader->netev, sk->fd, sk->skid, pk, lens, 0);
        }
        binary_free(&props);
        break;
    }
    case MQTT_PINGREQ: {
        // 心跳：回复 PINGRESP
        if (_prt) {
            LOG_INFO("PING<-C");
        }
        char *pk = NULL;
        size_t lens;
        pk = mqtt_pack_pong(&lens);
        if (NULL != pk) {
            if (_prt) {
                LOG_INFO("S->PINGRESP");
            }
            ev_send(&task->loader->netev, sk->fd, sk->skid, pk, lens, 0);
        }
        break;
    }
    case MQTT_DISCONNECT: {
        // 客户端主动断开，无需回复
        if (_prt) {
            LOG_INFO("DISCONNECT<-C");
        }
        break;
    }
    case MQTT_AUTH: {
        // 扩展认证（当前测试不启用，仅记录日志）
        if (_prt) {
            LOG_INFO("AUTH<-C");
        }
        break;
    }
    default:
        break;
    }
}
static void _startup(task_ctx *task) {
    task_recved(task, _net_recv);
    uint64_t id;
    if (ERR_OK != task_listen(task, PACK_MQTT, NULL, "0.0.0.0", _port, &id, 0)) {
        LOG_ERROR("mqtt server listen %d error.", (int32_t)_port);
    }
}
void task_mqtt_server_start(loader_ctx *loader, const char *name, uint16_t port, int32_t pt) {
    _port = port;
    _prt = pt;
    task_ctx *task = task_new(loader, name, 0, NULL, NULL, NULL);
    if (ERR_OK != task_register(task, _startup, NULL)) {
        task_free(task);
    }
}
