#include "proto/custz.h"
#include "proto/protos.h"

typedef uint32_t lens_t;
#define  ntoh  ntohl
#define  hton  htonl

//simple_head_ctx + 内容
typedef struct custz_pack_ctx {
    lens_t lens;//内容长度
    char data[0];
}custz_pack_ctx;

static void _custz_ntoh(custz_pack_ctx *pack, lens_t lens) {
    pack->lens = lens;
    //其他变量赋值
}
static void _custz_hton(custz_pack_ctx *pack, lens_t lens) {
    pack->lens = lens;
    //其他变量赋值
}
static custz_pack_ctx *_custz_data(buffer_ctx *buf, size_t *size, ud_cxt *ud, int32_t *status) {
    custz_pack_ctx *pack = ud->extra;
    if (buffer_size(buf) < pack->lens) {
        BIT_SET(*status, PROTO_MOREDATA);
        return NULL;
    }
    ASSERTAB(pack->lens == buffer_remove(buf, pack->data, pack->lens), "copy buffer error.");
    *size = sizeof(custz_pack_ctx) + pack->lens;
    ud->extra = NULL;
    return pack;
}
custz_pack_ctx *custz_unpack(buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status) {
    if (NULL == ud->extra) {
        if (buffer_size(buf) < sizeof(custz_pack_ctx)) {
            BIT_SET(*status, PROTO_MOREDATA);
            return NULL;
        }
        lens_t dlens;
        ASSERTAB(sizeof(dlens) == buffer_copyout(buf, offsetof(custz_pack_ctx, lens), &dlens, sizeof(dlens)), "copy buffer error.");
        dlens = (lens_t)ntoh(dlens);
        if (PACK_TOO_LONG(dlens)) {
            BIT_SET(*status, PROTO_ERROR);
            return NULL;
        }
        custz_pack_ctx *pack;
        MALLOC(pack, sizeof(custz_pack_ctx) + dlens);
        ASSERTAB(sizeof(custz_pack_ctx) == buffer_remove(buf, pack, sizeof(custz_pack_ctx)), "copy buffer error.");
        _custz_ntoh(pack, dlens);
        if (0 == dlens) {
            *size = sizeof(custz_pack_ctx);
            return pack;
        } else {
            ud->extra = pack;
            return _custz_data(buf, size, ud, status);
        }
    } else {
        return _custz_data(buf, size, ud, status);
    }
}
custz_pack_ctx *custz_pack(void *data, size_t lens, size_t *size) {
    custz_pack_ctx *pack;
    *size = sizeof(custz_pack_ctx) + lens;
    MALLOC(pack, *size);
    _custz_hton(pack, (lens_t)hton((lens_t)lens));
    if (lens > 0) {
        memcpy(pack->data, data, lens);
    }
    return pack;
}
void *custz_data(custz_pack_ctx *pack, size_t *lens) {
    *lens = pack->lens;
    return pack->data;
}
