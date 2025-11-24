#ifndef MQTT_H_
#define MQTT_H_

#include "protocol/mqtt/mqtt_pack.h"

void _mqtt_pkfree(void *data);
void _mqtt_udfree(ud_cxt *ud);
mqtt_pack_ctx *mqtt_unpack(int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status);
/// <summary>
/// 原因字符串。小于0x80的原因码指示某次操作成功完成，通常用0来表示。大于等于0x80的原因码用来指示操作失败。
/// </summary>
/// <param name="prot">mqtt_prot</param>
/// <param name="code">原因码</param>
/// <returns>char * 字符串</returns>
const char *mqtt_reason(mqtt_prot prot, int32_t code);

#endif//MQTT_H_
