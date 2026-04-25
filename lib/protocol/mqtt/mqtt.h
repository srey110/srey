#ifndef MQTT_H_
#define MQTT_H_

#include "protocol/mqtt/mqtt_pack.h"

// 释放 mqtt_pack_ctx 及其子结构的内存
void _mqtt_pkfree(void *data);
// 释放 ud_cxt 中挂载的 mqtt_ctx 上下文
void _mqtt_udfree(ud_cxt *ud);
/// <summary>
/// 从缓冲区中解析一个完整的 MQTT 数据包
/// </summary>
/// <param name="client">1 表示当前端为客户端，0 表示服务端</param>
/// <param name="buf">接收缓冲区</param>
/// <param name="ud">连接上下文，内部存储协议版本和解析状态</param>
/// <param name="status">解析结果标志位（PROT_MOREDATA / PROT_ERROR / PROT_CLOSE）</param>
/// <returns>解析成功返回 mqtt_pack_ctx*，数据不足或出错返回 NULL</returns>
mqtt_pack_ctx *mqtt_unpack(int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status);
/// <summary>
/// 原因字符串。小于0x80的原因码指示某次操作成功完成，通常用0来表示。大于等于0x80的原因码用来指示操作失败。
/// </summary>
/// <param name="prot">mqtt_prot</param>
/// <param name="code">原因码</param>
/// <returns>char * 字符串</returns>
const char *mqtt_reason(mqtt_prot prot, int32_t code);

#endif//MQTT_H_
