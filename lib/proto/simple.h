#ifndef SIMPLE_H_
#define SIMPLE_H_

#include "structs.h"
#include "buffer.h"

struct simple_pack_ctx *simple_unpack(buffer_ctx *buf, size_t *size, ud_cxt *ud, int32_t *closefd);
struct simple_pack_ctx *simple_pack(void *data, size_t lens, size_t *size);

void *simple_data(struct simple_pack_ctx *pack, size_t *lens);

#endif//SIMPLE_H_
