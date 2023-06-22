#include "proto/simple.h"

typedef uint32_t lens_t;
#define  ntoh  ntohl
#define  hton  htonl

//simple_head_ctx + 内容
typedef struct simple_pack_ctx {
    lens_t lens;//内容长度
    char data[0];
}simple_pack_ctx;

static inline void _simple_ntoh(simple_pack_ctx *pack, lens_t lens) {
    pack->lens = lens;
    //其他变量赋值
}
static inline void _simple_hton(simple_pack_ctx *pack, lens_t lens) {
    pack->lens = lens;
    //其他变量赋值
}
static inline simple_pack_ctx *_simple_data(buffer_ctx *buf, size_t *size, ud_cxt *ud) {
    simple_pack_ctx *pack = ud->extra;
    if (buffer_size(buf) < pack->lens) {
        return NULL;
    }
    ASSERTAB(pack->lens == buffer_remove(buf, pack->data, pack->lens), "copy buffer error.");
    *size = sizeof(simple_pack_ctx) + pack->lens;
    ud->extra = NULL;
    return pack;
}
simple_pack_ctx *simple_unpack(buffer_ctx *buf, size_t *size, ud_cxt *ud, int32_t *closefd) {
    if (NULL == ud->extra) {
        if (buffer_size(buf) < sizeof(simple_pack_ctx)) {
            return NULL;
        }
        lens_t dlens;
        ASSERTAB(sizeof(dlens) == 
            buffer_copyout(buf, offsetof(simple_pack_ctx, lens), &dlens, sizeof(dlens)), 
            "copy buffer error.");
        dlens = (lens_t)ntoh(dlens);
        if (PACK_TOO_LONG(dlens)) {
            *closefd = 1;
            return NULL;
        }
        simple_pack_ctx *pack;
        MALLOC(pack, sizeof(simple_pack_ctx) + dlens);
        ASSERTAB(sizeof(simple_pack_ctx) == buffer_remove(buf, pack, sizeof(simple_pack_ctx)), "copy buffer error.");
        _simple_ntoh(pack, dlens);
        if (0 == dlens) {
            *size = sizeof(simple_pack_ctx);
            return pack;
        } else {
            ud->extra = pack;
            return _simple_data(buf, size, ud);
        }
    } else {
        return _simple_data(buf, size, ud);
    }
}
simple_pack_ctx *simple_pack(void *data, size_t lens, size_t *size) {
    simple_pack_ctx *pack;
    *size = sizeof(simple_pack_ctx) + lens;
    MALLOC(pack, *size);
    _simple_hton(pack, (lens_t)hton((lens_t)lens));
    if (lens > 0) {
        memcpy(pack->data, data, lens);
    }
    return pack;
}
void *simple_data(simple_pack_ctx *pack, size_t *lens) {
    *lens = pack->lens;
    return pack->data;
}
