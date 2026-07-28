#ifndef TASK_MQTT_CLIENT_H_
#define TASK_MQTT_CLIENT_H_

#include "lib.h"

// 启动 MQTT 客户端测试任务，连接指定 broker 并走完完整消息流程：
// CONNECT→CONNACK→PUBLISH QoS0/1/2→SUBSCRIBE→服务端推送→UNSUBSCRIBE→PING→DISCONNECT。
// 全流程成功后将 *ok 置 1；host 为域名时自动 DNS 解析。
// pt 非 0 时输出每条指令的收发日志。
// clientid 取 "<name>-<随机6字节>"：同一 broker 上的多个客户端必须互异，否则按 MQTT 规定
// 后来者会让服务端踢掉先前的同名会话，表现为握手中途被静默关闭（非连接失败）；
// 故 name 需在同一 broker 内唯一，且前 22 字节不得相同（超长会被 SNPRINTF 截断）
void task_mqtt_client_start(loader_ctx *loader, const char *name,
    mqtt_protversion version, const char *host, uint16_t port, int32_t pt, int32_t *ok);

#endif//TASK_MQTT_CLIENT_H_
