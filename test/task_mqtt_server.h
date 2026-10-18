#ifndef TASK_MQTT_SERVER_H_
#define TASK_MQTT_SERVER_H_

#include "lib.h"

// 启动本地 MQTT broker 测试任务，兼容 MQTT 3.1.1 和 5.0。
// 覆盖完整 broker 侧流程：CONNECT/PUBLISH QoS0~2/SUBSCRIBE/UNSUBSCRIBE/PING/DISCONNECT。
// PUBLISH QoS1 回带属性的 PUBACK、QoS2 回不带属性的 PUBREC，reason 恒 0。
// pt 非 0 时输出每条指令的收发日志
void task_mqtt_server_start(loader_ctx *loader, const char *name, uint16_t port, int32_t pt);

#endif//TASK_MQTT_SERVER_H_
