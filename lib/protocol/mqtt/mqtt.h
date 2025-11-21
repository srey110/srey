#ifndef MQTT_H_
#define MQTT_H_

#include "protocol/mqtt/mqtt_struct.h"

void _mqtt_pkfree(void *data);
void _mqtt_udfree(ud_cxt *ud);
mqtt_pack_ctx *mqtt_unpack(int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status);

#endif//MQTT_H_
