#include "proto/protos.h"
#include "proto/simple.h"

typedef void *(*_unpack)(buffer_ctx *buf, size_t *size, ud_cxt *ud);
typedef void *(*_pack)(void *data, size_t lens, size_t *size);
static _unpack _unpack_funcs[UNPACK_CNT];
static _pack _pack_funcs[UNPACK_CNT];

void protos_init() {
    _unpack_funcs[UNPACK_NONE] = NULL;
    _unpack_funcs[UNPACK_RPC] = simple_unpack;
    _unpack_funcs[UNPACK_SIMPLE] = simple_unpack;

    _pack_funcs[PACK_NONE] = NULL;
    _pack_funcs[PACK_RPC] = simple_pack;
    _pack_funcs[PACK_SIMPLE] = simple_pack;
}
void *protos_unpack(unpack_type type, buffer_ctx *buf, size_t *size, ud_cxt *ud) {
    _unpack func = _unpack_funcs[type];
    if (NULL == func) {
        size_t lens = buffer_size(buf);
        if (0 == lens) {
            return NULL;
        }
        void *unpack;
        MALLOC(unpack, lens);
        ASSERTAB(lens == buffer_remove(buf, unpack, lens), "copy buffer error.");
        *size = lens;
        return unpack;
    } else {
        return func(buf, size, ud);
    }
}
void *protos_pack(pack_type type, void *data, size_t lens, size_t *size) {
    _pack func = _pack_funcs[type];
    if (NULL == func) {
        void *pack;
        MALLOC(pack, lens);
        memcpy(pack, data, lens);
        *size = lens;
        return pack;
    } else {
        return func(data, lens, size);
    }
}
