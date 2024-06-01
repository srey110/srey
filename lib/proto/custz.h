#ifndef CUSTOMIZE_H_
#define CUSTOMIZE_H_

#include "structs.h"
#include "buffer.h"

struct custz_pack_ctx *custz_unpack(buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status);
struct custz_pack_ctx *custz_pack(void *data, size_t lens, size_t *size);

void *custz_data(struct custz_pack_ctx *pack, size_t *lens);

#endif//CUSTOMIZE_H_
