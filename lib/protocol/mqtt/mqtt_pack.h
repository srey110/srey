#ifndef MQTT_PACK_H_
#define MQTT_PACK_H_

#include "utils/binary.h"
#include "protocol/mqtt/mqtt_struct.h"

void mqtt_props_init(binary_ctx *props);
void mqtt_props_free(binary_ctx *props);
int32_t mqtt_props_fixnum(binary_ctx *props, mqtt_prop_flag flag, int32_t val);
int32_t mqtt_props_varnum(binary_ctx *props, mqtt_prop_flag flag, int32_t val);
int32_t mqtt_props_binary(binary_ctx *props, mqtt_prop_flag flag, void *data, int32_t lens);
int32_t mqtt_props_kv(binary_ctx *props, mqtt_prop_flag flag, void *key, size_t klens, void *val, size_t vlens);

char *mqtt_pack_connack(mqtt_protversion version, int8_t sesspresent, int8_t reason, binary_ctx *props, size_t *lens);

#endif//MQTT_PACK_H_
