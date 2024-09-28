#include "protocol/custz_head.h"
#include "protocol/protos.h"
#include "utils/utils.h"

#define CUSTZ_FIXED_LENS 4 //固定头长度

int32_t _custz_decode_fixed(buffer_ctx *buf, size_t *hlens, size_t *size, int32_t *status) {
    if (buffer_size(buf) < CUSTZ_FIXED_LENS) {
        BIT_SET(*status, PROTO_MOREDATA);
        return ERR_FAILED;
    }
    *hlens = CUSTZ_FIXED_LENS;
    char head[CUSTZ_FIXED_LENS];
    ASSERTAB(sizeof(head) == buffer_copyout(buf, 0, head, sizeof(head)), "copy buffer error.");
    *size = (size_t)unpack_integer(head, sizeof(head), 0, 0);
    return ERR_OK;
}
char *_custz_encode_fixed(size_t dlens, size_t *hlens, size_t *size) {
    *hlens = CUSTZ_FIXED_LENS;
    *size = *hlens + dlens;
    char *pack;
    MALLOC(pack, *size);
    pack_integer(pack, dlens, CUSTZ_FIXED_LENS, 0);
    return pack;
}
int32_t _custz_decode_flag(buffer_ctx *buf, size_t *hlens, size_t *size, int32_t *status) {
    if (buffer_size(buf) < 1){
        BIT_SET(*status, PROTO_MOREDATA);
        return ERR_FAILED;
    }
    uint8_t flag = buffer_at(buf, 0);
    *hlens = sizeof(flag);
    if (flag <= 0xfc) {
        *size = flag;
    } else if (0xfd == flag) {
        char buf16[sizeof(uint16_t)];
        *hlens += sizeof(buf16);
        if (*hlens > buffer_size(buf)) {
            BIT_SET(*status, PROTO_MOREDATA);
            return ERR_FAILED;
        }
        ASSERTAB(sizeof(buf16) == buffer_copyout(buf, sizeof(flag), buf16, sizeof(buf16)), "copy buffer error.");
        *size = (size_t)unpack_integer(buf16, sizeof(buf16), 0, 0);
    } else if (0xfe == flag) {
        char buf32[sizeof(uint32_t)];
        *hlens += sizeof(buf32);
        if (*hlens > buffer_size(buf)) {
            BIT_SET(*status, PROTO_MOREDATA);
            return ERR_FAILED;
        }
        ASSERTAB(sizeof(buf32) == buffer_copyout(buf, sizeof(flag), buf32, sizeof(buf32)), "copy buffer error.");
        *size = (size_t)unpack_integer(buf32, sizeof(buf32), 0, 0);
    } else {
        char buf64[sizeof(uint64_t)];
        *hlens += sizeof(buf64);
        if (*hlens > buffer_size(buf)) {
            BIT_SET(*status, PROTO_MOREDATA);
            return ERR_FAILED;
        }
        ASSERTAB(sizeof(buf64) == buffer_copyout(buf, sizeof(flag), buf64, sizeof(buf64)), "copy buffer error.");
        *size = (size_t)unpack_integer(buf64, sizeof(buf64), 0, 0);
    }
    return ERR_OK;
}
char *_custz_encode_flag(size_t dlens, size_t *hlens, size_t *size) {
    char *pack;
    *hlens = sizeof(uint8_t);
    if (dlens <= 0xfc) {
        *size = *hlens + dlens;
        MALLOC(pack, *size);
        pack[0] = (uint8_t)dlens;
    } else if (dlens > 0xfc && dlens <= USHRT_MAX) {
        *hlens += sizeof(uint16_t);
        *size = *hlens + dlens;
        MALLOC(pack, *size);
        pack[0] = 0xfd;
        pack_integer(pack + sizeof(uint8_t), dlens, sizeof(uint16_t), 0);
    } else if (dlens > USHRT_MAX && dlens <= UINT_MAX) {
        *hlens += sizeof(uint32_t);
        *size = *hlens + dlens;
        MALLOC(pack, *size);
        pack[0] = 0xfe;
        pack_integer(pack + sizeof(uint8_t), dlens, sizeof(uint32_t), 0);
    } else {
        *hlens += sizeof(uint64_t);
        *size = *hlens + dlens;
        MALLOC(pack, *size);
        pack[0] = 0xff;
        pack_integer(pack + sizeof(uint8_t), dlens, sizeof(uint64_t), 0);
    }
    return pack;
}
int32_t _custz_decode_variable(buffer_ctx *buf, size_t *hlens, size_t *size, int32_t *status) {
    if (buffer_size(buf) < 1) {
        BIT_SET(*status, PROTO_MOREDATA);
        return ERR_FAILED;
    }
    *size = 0;
    uint8_t byte;
    size_t mcl = 1;
    for (size_t i = 0; i < buffer_size(buf) && i < 8; i++) {
        byte = (uint8_t)buffer_at(buf, i);
        *size += (byte & 0x7f) * mcl;
        if (!BIT_CHECK(byte, 0x80)) {
            *hlens = i + 1;
            return ERR_OK;
        }
        mcl *= 0x80;
    }
    if (buffer_size(buf) >= 8) {
        BIT_SET(*status, PROTO_ERROR);
    } else {
        BIT_SET(*status, PROTO_MOREDATA);
    }
    return ERR_FAILED;
}
char *_custz_encode_variable(size_t dlens, size_t *hlens, size_t *size) {
    *hlens = 0;
    *size = dlens;
    char *pack;
    MALLOC(pack, 8 + dlens);
    uint8_t byte;
    do {
        byte = dlens % 0x80;
        dlens /= 0x80;
        if (0 != dlens) {
            BIT_SET(byte, 0x80);
        }
        pack[(*hlens)++] = byte;
    } while (dlens > 0);
    *size += *hlens;
    return pack;
}
