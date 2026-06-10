#ifndef TASK_MQTT_CLIENT_H_
#define TASK_MQTT_CLIENT_H_

#include "lib.h"

// 启动 MQTT 客户端测试任务，连接指定 broker 并走完完整消息流程：
// CONNECT→CONNACK→PUBLISH QoS0/1/2→SUBSCRIBE→服务端推送→UNSUBSCRIBE→PING→DISCONNECT。
// 全流程成功后将 *ok 置 1；host 为域名时自动 DNS 解析。
// pt 非 0 时输出每条指令的收发日志
void task_mqtt_client_start(loader_ctx *loader, const char *name,
    mqtt_protversion version, const char *host, uint16_t port, int32_t pt, int32_t *ok);

#endif//TASK_MQTT_CLIENT_H_
