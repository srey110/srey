#include "proto/simple.h"

typedef uint32_t head_t;
#define  ntoh  ntohl

//head_t(内容长度) + 内容
void *simple_unpack(buffer_ctx *buf, size_t *size, ud_cxt *ud) {
    size_t bufsize = buffer_size(buf);
    if (bufsize < sizeof(head_t)) {
        return NULL;
    }
    head_t lens;
    ASSERTAB(sizeof(lens) == buffer_copyout(buf, &lens, sizeof(lens)), "copy buffer error.");
    lens = (head_t)ntoh(lens);
    if (lens + sizeof(lens) > bufsize) {
        return NULL;
    }

    ASSERTAB(sizeof(lens) == buffer_drain(buf, sizeof(lens)), "drain buffer error.");
    void *data;
    MALLOC(data, lens);
    ASSERTAB(lens == buffer_remove(buf, data, lens), "copy buffer error.");
    *size = lens;
    return data;
}
void *simple_pack(void *data, size_t lens, size_t *size) {
    char *pack;
    *size = lens + sizeof(head_t);
    MALLOC(pack, *size);
    head_t head = (head_t)ntoh((head_t)lens);
    memcpy(pack, &head, sizeof(head));
    memcpy(pack + sizeof(head), data, lens);
    return pack;
}
