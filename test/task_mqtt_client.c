#include "task_mqtt_client.h"

typedef struct mqtt_client_args {
    uint16_t port;
    mqtt_protversion version;
    int32_t prt;
    int32_t *ok;
    SOCKET fd;
    uint64_t skid;
    char host[64];
}mqtt_client_args;

// TCP 连接建立后发送 CONNECT 包，MQTT 5.0 时携带扩展连接属性和遗嘱属性
static void _net_connect(task_ctx *task, SOCKET fd, uint64_t skid, subtype_t pktype, int32_t erro) {
    (void)pktype;
    mqtt_client_args *arg = coro_get_arg(task);
    if (ERR_OK != erro) {
        LOG_ERROR("mqtt connect %s error.", arg->host);
        return;
    }
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
    char clientid[9];
    randstr(clientid, sizeof(clientid) - 1);
    char *pk = mqtt_pack_connect(arg->version, 1, 120, clientid,
                                 "admin", "123", 3,
                                 "srey/will", "will message", strlen("will message"), 1, 1,
                                 &connprop, &willprop, &lens);
    binary_free(&connprop);
    binary_free(&willprop);
    if (NULL != pk) {
        ev_send(&task->loader->netev, fd, skid, pk, lens, 0);
        if (arg->prt) {
            LOG_INFO("C->CONNECT");
        }
    }
}
// 发送 PUBLISH 包，qos 指定服务质量等级，payload 固定为测试字符串
static void _send_publish(mqtt_pack_ctx *pack, task_ctx *task, SOCKET fd, uint64_t skid, int8_t qos) {
    mqtt_client_args *arg = coro_get_arg(task);
    binary_ctx props;
    binary_init(&props, NULL, 0, 0);
    mqtt_props_kv(&props, USER_PROPERTY, "key1", 4, "val1", 4);
    size_t lens;
    char *pk = mqtt_pack_publish(pack->version, 1, qos, 1, "srey/will", (uint16_t)randrange(100, 20000), "publish payload", strlen("publish payload"), &props, &lens);
    binary_free(&props);
    if (NULL != pk) {
        ev_send(&task->loader->netev, fd, skid, pk, lens, 0);
        if (arg->prt) {
            LOG_INFO("C->PUBLISH QoS %d", qos);
        }
    }
}
// 收到服务端数据包，驱动完整的 MQTT 消息流程状态机
static void _net_recv(task_ctx *task, SOCKET fd, uint64_t skid,
     subtype_t pktype, uint8_t client, uint8_t slice, void *data, size_t size) {
    (void)pktype;
    (void)client;
    (void)slice;
    (void)size;
    mqtt_pack_ctx *pack = (mqtt_pack_ctx *)data;
    mqtt_client_args *arg = coro_get_arg(task);
    switch (pack->fixhead.prot) {
    case MQTT_CONNACK: {
        // 连接成功后依次发送 QoS0（无 ACK）和 QoS1，覆盖三种 QoS 路径
        mqtt_connack_varhead *vh = pack->varhead;
        if (arg->prt) {
            LOG_INFO("CONNACK<-S %d (%s)", vh->reason, mqtt_reason(pack->fixhead.prot, vh->reason));
        }
        if (0x00 != vh->reason) {
            break;
        }
        _send_publish(pack, task, fd, skid, 0);
        _send_publish(pack, task, fd, skid, 1);
        break;
    }
    case MQTT_PUBLISH: {
        // 接收服务端主动推送的消息（SUBACK 后服务端 QoS0 推送）
        mqtt_publish_varhead *vh = pack->varhead;
        mqtt_publish_payload *pl = pack->payload;
        if (arg->prt) {
            LOG_INFO("PUBLISH<-S from server, topic=%s content=%s", vh->topic, pl->content);
        }
        // QoS0 无需回复；若服务端推 QoS1/2 此处需扩展
        break;
    }
    case MQTT_PUBACK: {
        // QoS1 确认：reason 0x00 或 0x10（No matching subscribers）均视为成功，
        // 收到 PUBACK 后继续发送 QoS2 PUBLISH
        mqtt_pubackrel_varhead *vh = pack->varhead;
        if (arg->prt) {
            LOG_INFO("PUBACK<-S %d (%s)", (int32_t)vh->reason, mqtt_reason(pack->fixhead.prot, vh->reason));
        }
        if (0x00 == vh->reason || 0x10 == vh->reason) {
            _send_publish(pack, task, fd, skid, 2);
        }
        break;
    }
    case MQTT_PUBREC: {
        // QoS2 第二步：回复 PUBREL
        mqtt_pubackrel_varhead *vh = pack->varhead;
        if (arg->prt) {
            LOG_INFO("PUBREC<-S %d (%s)", (int32_t)vh->reason, mqtt_reason(pack->fixhead.prot, vh->reason));
        }
        if (0x00 == vh->reason || 0x10 == vh->reason) {
            size_t lens;
            char *pk = mqtt_pack_pubrel(pack->version, vh->packid, 0, NULL, &lens);
            if (NULL != pk) {
                ev_send(&task->loader->netev, fd, skid, pk, lens, 0);
                if (arg->prt) {
                    LOG_INFO("C->PUBREL");
                }
            }
        }
        break;
    }
    case MQTT_PUBCOMP: {
        // QoS2 完成后发起 SUBSCRIBE，验证订阅流程
        mqtt_pubackrel_varhead *vh = pack->varhead;
        if (arg->prt) {
            LOG_INFO("PUBCOMP<-S %d (%s)", (int32_t)vh->reason, mqtt_reason(pack->fixhead.prot, vh->reason));
        }
        binary_ctx topics;
        binary_init(&topics, NULL, 0, 0);
        if (ERR_OK != mqtt_topics_subscribe(&topics, pack->version, "/test/topic1", 1, 1, 1, 1)) {
            binary_free(&topics);
            break;
        }
        size_t lens;
        char *pk = mqtt_pack_subscribe(pack->version, (uint16_t)randrange(100, 20000), &topics, NULL, &lens);
        binary_free(&topics);
        if (NULL != pk) {
            ev_send(&task->loader->netev, fd, skid, pk, lens, 0);
            if (arg->prt) {
                LOG_INFO("C->SUBSCRIBE");
            }
        }
        break;
    }
    case MQTT_SUBACK: {
        // 订阅成功后立即发起 UNSUBSCRIBE，覆盖取消订阅路径
        mqtt_reasonlist_payload *pl = pack->payload;
        if (arg->prt) {
            LOG_INFO("SUBACK<-S %d (%s)", (int32_t)pl->reasons[0], mqtt_reason(pack->fixhead.prot, pl->reasons[0]));
        }
        if (0x00 == pl->reasons[0] || 0x01 == pl->reasons[0] || 0x02 == pl->reasons[0]) {
            binary_ctx topics;
            binary_init(&topics, NULL, 0, 0);
            if (ERR_OK != mqtt_topics_unsubscribe(&topics, "/test/topic1")) {
                binary_free(&topics);
                break;
            }
            size_t lens;
            char *pk = mqtt_pack_unsubscribe(pack->version, (uint16_t)randrange(100, 20000), &topics, NULL, &lens);
            binary_free(&topics);
            if (NULL != pk) {
                ev_send(&task->loader->netev, fd, skid, pk, lens, 0);
                if (arg->prt) {
                    LOG_INFO("C->UNSUBSCRIBE");
                }
            }
        }
        break;
    }
    case MQTT_UNSUBACK: {
        // 取消订阅成功后发送 PING，验证心跳路径
        mqtt_reasonlist_payload *pl = pack->payload;
        if (arg->prt) {
            if (NULL != pl) {
                LOG_INFO("UNSUBACK<-S %d (%s)", (int32_t)pl->reasons[0], mqtt_reason(pack->fixhead.prot, pl->reasons[0]));
            } else {
                LOG_INFO("UNSUBACK<-S");
            }
        }
        size_t lens;
        char *pk = mqtt_pack_ping(&lens);
        if (NULL != pk) {
            ev_send(&task->loader->netev, fd, skid, pk, lens, 0);
            if (arg->prt) {
                LOG_INFO("C->PING");
            }
        }
        break;
    }
    case MQTT_PINGRESP: {
        // 心跳往返完成，发送 DISCONNECT 正常断开，标记测试通过
        if (arg->prt) {
            LOG_INFO("PONG<-S");
        }
        size_t lens;
        char *pk = mqtt_pack_disconnect(pack->version, 0, NULL, &lens);
        if (NULL != pk) {
            ev_send(&task->loader->netev, fd, skid, pk, lens, 0);
            if (arg->prt) {
                LOG_INFO("C->DISCONNECT");
            }
            *(arg->ok) = 1;
            LOG_INFO("mqtt client tested.");
        }
        break;
    }
    default:
        break;
    }
}
static void _startup(task_ctx *task) {
    task_connected(task, _net_connect);
    task_recved(task, _net_recv);

    mqtt_client_args *arg = coro_get_arg(task);
    int32_t rtn;
    // 域名需先 DNS 解析，IP 直连
    if (ERR_OK != is_ipaddr(arg->host)) {
        size_t n;
        dns_ip *ips = dns_lookup(task, arg->host, 0, 1, &n);
        if (NULL == ips) {
            LOG_ERROR("dns_lookup error.");
            return;
        }
        rtn = mqtt_try_connect(task, NULL, ips[0].ip, arg->port, 0, arg->version, &arg->fd, &arg->skid);
        FREE(ips);
    } else {
        rtn = mqtt_try_connect(task, NULL, arg->host, arg->port, 0, arg->version, &arg->fd, &arg->skid);
    }
    if (ERR_OK != rtn) {
        LOG_WARN("task_connect %s error.", arg->host);
    }
}
void task_mqtt_client_start(loader_ctx *loader, const char *name,
     mqtt_protversion version, const char *host, uint16_t port, int32_t pt, int32_t *ok) {
    if (NULL == ok) {
        return;
    }
    mqtt_client_args *arg;
    MALLOC(arg, sizeof(mqtt_client_args));
    arg->port = port;
    arg->version = version;
    arg->prt = pt;
    arg->ok = ok;
    SNPRINTF(arg->host, sizeof(arg->host), "%s", host);
    coro_task_register(loader, name, 0, _startup, NULL, _free, arg);
}
