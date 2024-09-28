#include "protocol/custz.h"
#include "protocol/custz_head.h"
#include "protocol/protos.h"

typedef int32_t(*_head_decode)(buffer_ctx *buf, size_t *hlens, size_t *size, int32_t *status);
typedef char * (*_head_encode)(size_t dlens, size_t *hlens, size_t *size);

static _head_decode _decode = _custz_decode_flag;
static _head_encode _encode = _custz_encode_flag;

void *custz_unpack(buffer_ctx *buf, size_t *size, int32_t *status) {
    size_t hlens;
    if (ERR_OK != _decode(buf, &hlens, size, status)) {
        return NULL;
    }
    if (PACK_TOO_LONG(*size)) {
        BIT_SET(*status, PROTO_ERROR);
        return NULL;
    }
    size_t pklens = hlens + *size;
    if (pklens > buffer_size(buf)) {
        BIT_SET(*status, PROTO_MOREDATA);
        return NULL;
    }
    if (0 == *size) {
        ASSERTAB(hlens == buffer_drain(buf, hlens), "drain buffer error.");
        return NULL;
    }
    char *msg;
    MALLOC(msg, *size);
    ASSERTAB(*size == buffer_copyout(buf, hlens, msg, *size), "copy buffer error.");
    ASSERTAB(pklens == buffer_drain(buf, pklens), "drain buffer error.");
    return msg;
}
void *custz_pack(void *data, size_t lens, size_t *size) {
    size_t hlens;
    char *pack = _encode(lens, &hlens, size);
    if (0 != lens) {
        memcpy(pack + hlens, data, lens);
    }
    return pack;
}
