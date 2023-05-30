#ifndef PROTOS_H_
#define PROTOS_H_

#include "structs.h"
#include "buffer.h"

typedef enum unpack_type {
    UNPACK_NONE = 0x0,
    UNPACK_RPC,
    UNPACK_HTTP,
    UNPACK_SIMPLE,
}unpack_type;
typedef enum pack_type {
    PACK_NONE = 0x0,
    PACK_RPC,
    PACK_SIMPLE,
}pack_type;

void protos_pkfree(unpack_type type, void *data);
void protos_udfree(ud_cxt *ud);
void *protos_unpack(unpack_type type, buffer_ctx *buf, size_t *size, ud_cxt *ud, int32_t *closefd);
void *protos_pack(pack_type type, void *data, size_t lens, size_t *size);

#endif//PROTOS_H_
