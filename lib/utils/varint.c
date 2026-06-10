#include "utils/varint.h"

int32_t varint_encode_mqtt(uint32_t value, char buf[4]) {
    if (value >= 0x10000000) {
        return 0;
    }
    uint8_t byte;
    int32_t i = 0;
    do {
        byte = value % 0x80;
        value /= 0x80;
        if (0 != value) {
            BIT_SET(byte, 0x80);
        }
        buf[i++] = (char)byte;
    } while (value > 0);
    return i;
}
int32_t varint_decode_mqtt(buffer_ctx *buf, size_t off, size_t blens, size_t *value) {
    *value = 0;
    if (off >= blens) {
        return ERR_FAILED;
    }
    uint8_t byte;
    int32_t mcl = 1;
    for (size_t i = 0; i < blens - off && i < 4; i++) {
        byte = (uint8_t)buffer_at(buf, i + off);
        *value += (size_t)((byte & 0x7f) * mcl);
        if (!BIT_CHECK(byte, 0x80)) {
            return (int32_t)(i + 1);
        }
        mcl *= 0x80;
    }
    return ERR_FAILED;
}
