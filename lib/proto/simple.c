#include "proto/simple.h"
#include "loger.h"

typedef uint32_t lens_t;
#define  ntoh  ntohl
#define MAX_DATALENS ONEK * 64

typedef struct simple_head_ctx {
    lens_t lens;//内容长度
    char data[0];
}simple_head_ctx;

static inline void _simple_ntoh(simple_head_ctx *pack, lens_t lens) {
    pack->lens = lens;
    //其他变量赋值
}
void *_simple_data(buffer_ctx *buf, size_t *size, ud_cxt *ud) {
    simple_head_ctx *pack = ud->extra;
    if (buffer_size(buf) < pack->lens) {
        return NULL;
    }
    ASSERTAB(pack->lens == buffer_remove(buf, pack->data, pack->lens), "copy buffer error.");
    *size = sizeof(simple_head_ctx) + pack->lens;
    ud->extra = NULL;
    return pack;
}
//simple_head_ctx + 内容
void *simple_unpack(buffer_ctx *buf, size_t *size, ud_cxt *ud, int32_t *closefd) {
    if (NULL == ud->extra) {
        if (buffer_size(buf) < sizeof(simple_head_ctx)) {
            return NULL;
        }
        size_t off = offsetof(simple_head_ctx, lens);
        lens_t lens;
        ASSERTAB(sizeof(lens) == buffer_copyout(buf, off, &lens, sizeof(lens)), "copy buffer error.");
        lens = (lens_t)ntoh(lens);
        if (lens > MAX_DATALENS) {
            *closefd = 1;
            LOG_WARN("data too long, %"PRIu64, lens);
            return NULL;
        }
        void *data;
        MALLOC(data, sizeof(simple_head_ctx) + lens);
        ASSERTAB(sizeof(simple_head_ctx) == buffer_remove(buf, data, sizeof(simple_head_ctx)), "copy buffer error.");
        _simple_ntoh(data, lens);
        if (0 == lens) {
            *size = sizeof(simple_head_ctx);
            return data;
        } else {
            ud->extra = data;
            return _simple_data(buf, size, ud);
        }
    } else {
        return _simple_data(buf, size, ud);
    }
}
void *simple_pack(void *data, size_t lens, size_t *size) {
    void *pack;
    *size = sizeof(simple_head_ctx) + lens;
    MALLOC(pack, *size);
    _simple_ntoh(pack, (lens_t)ntoh((lens_t)lens));
    if (lens > 0) {
        memcpy(((simple_head_ctx *)pack)->data, data, lens);
    }
    return pack;
}
size_t simple_hsize() {
    return sizeof(simple_head_ctx);
}
void *simple_data(void *data, size_t *lens) {
    simple_head_ctx *pack = data;
    *lens = pack->lens;
    return pack->data;
}
