#ifndef PROTOS_H_
#define PROTOS_H_

#include "structs.h"
#include "buffer.h"

typedef enum unpack_type {
    UNPACK_NONE = 0x0,
    UNPACK_SIMPLE,
    UNPACK_CNT,
}unpack_type;
typedef enum pack_type {
    PACK_NONE = 0x0,
    PACK_SIMPLE,
    PACK_CNT,
}pack_type;

void protos_init();
void *protos_unpack(unpack_type type, buffer_ctx *buf, size_t *size, ud_cxt *ud);
void *protos_pack(pack_type type, void *data, size_t lens, size_t *size);

#endif//PROTOS_H_
