#include "proto/protos.h"
#include "proto/simple.h"
#include "proto/http.h"
#include "proto/websock.h"

void protos_pkfree(pack_type type, void *data) {
    switch (type) {
    case PACK_HTTP:
        http_pkfree(data);
        break;
    default:
        FREE(data);
        break;
    }
}
void protos_udfree(ud_cxt *ud) {
    switch (ud->pktype) {
    case PACK_HTTP:
        http_udfree(ud);
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
static inline void _check_retry(void *unpack, ud_cxt *ud, int32_t *closefd) {
    if (0 != *closefd) {
        return;
    }
    if (NULL == unpack) {
        if (++ud->nretry >= MAX_RETRYCNT) {
            *closefd = 1;
        }
    } else {
        ud->nretry = 0;
    }
}
void *protos_unpack(ev_ctx *ev, SOCKET fd, buffer_ctx *buf, size_t *size, ud_cxt *ud, int32_t *closefd) {
    void *unpack;
    *size = 0;
    switch (ud->pktype) {
    case PACK_RPC:
    case PACK_SIMPLE:
        unpack = simple_unpack(buf, size, ud, closefd);
        break;
    case PACK_HTTP:
        unpack = http_unpack(buf, size, ud, closefd);
        break;
    case PACK_WEBSOCK:
        unpack = websock_unpack(ev, fd, buf, size, ud, closefd);
        break;
    default:
        unpack = _unpack_default(buf, size, ud);
        break;
    }
    _check_retry(unpack, ud, closefd);
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
    case PACK_WEBSOCK:
        pack = websock_pack(data, lens, size);
        break;
    default:
        pack = _pack_default(data, lens, size);
        break;
    }
    return pack;
}
int32_t protos_handshake(ev_ctx *ev, SOCKET fd, ud_cxt *ud, void *hscb) {
    int32_t rtn = ERR_OK;
    switch (ud->pktype) {
    case PACK_WEBSOCK:
        ud->hscb = hscb;
        if (0 == ud->svside) {
            rtn = websock_client_reqhs(ev, fd, ud);
        }
        break;
    default:
        break;
    }
    return rtn;
}

