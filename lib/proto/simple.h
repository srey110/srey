#ifndef SIMPLE_H_
#define SIMPLE_H_

#include "structs.h"
#include "buffer.h"

void *simple_unpack(buffer_ctx *buf, size_t *size, ud_cxt *ud, int32_t *closefd);
void *simple_pack(void *data, size_t lens, size_t *size);

#endif//SIMPLE_H_
