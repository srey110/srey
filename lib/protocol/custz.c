#include "protocol/custz.h"
#include "protocol/protos.h"

void *custz_unpack(buffer_ctx *buf, size_t *size, int32_t *status) {
    size_t total = buffer_size(buf);
    if (total < 1) {
        BIT_SET(*status, PROTO_MOREDATA);
        return NULL;
    }
    uint8_t flag = buffer_at(buf, 0);
    size_t hlens = sizeof(flag);
    if (flag <= 0xfc) {
        *size = flag;
    } else if (0xfd == flag) {
        char buf16[sizeof(uint16_t)];
        hlens += sizeof(buf16);
        if (hlens > total) {
            BIT_SET(*status, PROTO_MOREDATA);
            return NULL;
        }
        ASSERTAB(sizeof(buf16) == buffer_copyout(buf, sizeof(flag), buf16, sizeof(buf16)), "copy buffer error.");
        *size = (size_t)unpack_integer(buf16, sizeof(buf16), 0, 0);
    } else if (0xfe == flag) {
        char buf32[sizeof(uint32_t)];
        hlens += sizeof(buf32);
        if (hlens > total) {
            BIT_SET(*status, PROTO_MOREDATA);
            return NULL;
        }
        ASSERTAB(sizeof(buf32) == buffer_copyout(buf, sizeof(flag), buf32, sizeof(buf32)), "copy buffer error.");
        *size = (size_t)unpack_integer(buf32, sizeof(buf32), 0, 0);
    } else {
        char buf64[sizeof(uint64_t)];
        hlens += sizeof(buf64);
        if (hlens > total) {
            BIT_SET(*status, PROTO_MOREDATA);
            return NULL;
        }
        ASSERTAB(sizeof(buf64) == buffer_copyout(buf, sizeof(flag), buf64, sizeof(buf64)), "copy buffer error.");
        *size = (size_t)unpack_integer(buf64, sizeof(buf64), 0, 0);
    }
    if (PACK_TOO_LONG(*size)) {
        BIT_SET(*status, PROTO_ERROR);
        return NULL;
    }
    size_t pklens = hlens + *size;
    if (pklens > total) {
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
    char *pack;
    size_t hlens = sizeof(uint8_t);
    if (lens <= 0xfc) {
        *size = hlens + lens;
        MALLOC(pack, *size);
        pack[0] = (uint8_t)lens;
    } else if (lens > 0xfc && lens <= USHRT_MAX) {
        hlens += sizeof(uint16_t);
        *size = hlens + lens;
        MALLOC(pack, *size);
        pack[0] = 0xfd;
        pack_integer(pack + sizeof(uint8_t), lens, sizeof(uint16_t), 0);
    } else if (lens > USHRT_MAX && lens <= UINT_MAX) {
        hlens += sizeof(uint32_t);
        *size = hlens + lens;
        MALLOC(pack, *size);
        pack[0] = 0xfe;
        pack_integer(pack + sizeof(uint8_t), lens, sizeof(uint32_t), 0);
    } else {
        hlens += sizeof(uint64_t);
        *size = hlens + lens;
        MALLOC(pack, *size);
        pack[0] = 0xff;
        pack_integer(pack + sizeof(uint8_t), lens, sizeof(uint64_t), 0);
    }
    if (0 != lens) {
        memcpy(pack + hlens, data, lens);
    }
    return pack;
}
