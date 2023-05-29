#include "proto/protos.h"
#include "proto/simple.h"
#include "proto/http.h"

void protos_pkfree(unpack_type type, void *data) {
    switch (type) {
    case UNPACK_HTTP:
        http_pkfree(data);
        break;
    default:
        FREE(data);
        break;
    }
}
void protos_exfree(ud_cxt *ud) {
    switch (ud->upktype) {
    case UNPACK_HTTP:
        http_exfree(ud->extra);
        break;
    default:
        FREE(ud->extra);
        break;
    }
}
static inline void *_unpack_default(buffer_ctx *buf, size_t *size, ud_cxt *ud) {
    size_t lens = buffer_size(buf);
    if (0 == lens) {
        return NULL;
    }
    void *unpack;
    MALLOC(unpack, lens);
    ASSERTAB(lens == buffer_remove(buf, unpack, lens), "copy buffer error.");
    *size = lens;
    return unpack;
}
void *protos_unpack(unpack_type type, buffer_ctx *buf, size_t *size, ud_cxt *ud, int32_t *closefd) {
    void *unpack;
    switch (type) {
    case UNPACK_RPC:
    case UNPACK_SIMPLE:
        unpack = simple_unpack(buf, size, ud, closefd);
        break;
    case UNPACK_HTTP:
        unpack = http_unpack(buf, size, ud, closefd);
        break;
    default:
        unpack = _unpack_default(buf, size, ud);
        break;
    }
    return unpack;
}
static inline void *_pack_default(void *data, size_t lens, size_t *size) {
    void *pack;
    MALLOC(pack, lens);
    memcpy(pack, data, lens);
    *size = lens;
    return pack;
}
void *protos_pack(pack_type type, void *data, size_t lens, size_t *size) {
    void *pack;
    switch (type) {
    case PACK_RPC:
    case PACK_SIMPLE:
        pack = simple_pack(data, lens, size);
        break;
    default:
        pack = _pack_default(data, lens, size);
        break;
    }
    return pack;
}
