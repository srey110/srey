#include "proto/protos.h"
#include "proto/simple.h"
#include "proto/http.h"
#include "proto/websock.h"

void protos_pkfree(pack_type type, void *data) {
    if (NULL == data) {
        return;
    }
    switch (type) {
    case PACK_HTTP:
        http_pkfree(data);
        break;
    default:
        FREE(data);
        break;
    }
}
void protos_udfree(void *arg) {
    ud_cxt *ud = arg;
    switch (ud->pktype) {
    case PACK_HTTP:
        http_udfree(ud);
        break;
    default:
        FREE(ud->extra);
        break;
    }
}
static void *_unpack_default(buffer_ctx *buf, size_t *size, ud_cxt *ud) {
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
void protos_init(void) {
    _websock_init_key();
}
void *protos_unpack(ev_ctx *ev, SOCKET fd, uint64_t skid,
    buffer_ctx *buf, size_t *size, ud_cxt *ud, int32_t *closefd, int32_t *slice) {
    void *unpack;
    *size = 0;
    *slice = SLICE_NONE;
    switch (ud->pktype) {
    case PACK_RPC:
    case PACK_SIMPLE:
        unpack = simple_unpack(buf, size, ud, closefd);
        break;
    case PACK_HTTP:
        unpack = http_unpack(buf, size, ud, closefd, slice);
        break;
    case PACK_WEBSOCK:
        unpack = websock_unpack(ev, fd, skid, buf, size, ud, closefd, slice);
        break;
    default:
        unpack = _unpack_default(buf, size, ud);
        break;
    }
    return unpack;
}
