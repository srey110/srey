#include "protocol/prots_pub.h"

uint32_t parse_usec_frac(const char *str) {
    uint32_t usec = 0;
    const char *dot = strchr(str, '.');
    if (NULL != dot) {
        dot++;
        int32_t mult = 100000;
        for (int32_t i = 0; i < 6 && dot[i] >= '0' && dot[i] <= '9'; i++, mult /= 10) {
            usec += (uint32_t)((dot[i] - '0') * mult);
        }
    }
    return usec;
}
int32_t parse_colon_triple(const char *str, const uint32_t max[3], uint32_t val[3]) {
    uint32_t got[3] = { 0, 0, 0 };
    const char *p = str;
    const char *digit;
    uint64_t v;
    int32_t n = 0;
    while (n < 3) {
        digit = p;
        while ('0' <= *p
               && '9' >= *p) {
            p++;
        }
        if (p == digit) {
            break;
        }
        if (ERR_OK != strtou64(digit, (size_t)(p - digit), max[n], &v)) {
            // 超上界即整串作废,不留半截结果:调用方拿三段值算时刻,半截比全无更难查
            return 0;
        }
        got[n] = (uint32_t)v;
        n++;
        if (':' != *p) {
            break;
        }
        p++;
    }
    if (0 == n) {
        return 0;
    }
    val[0] = got[0];
    val[1] = got[1];
    val[2] = got[2];
    return n;
}
int32_t varint_encode_mqtt(uint32_t value, char buf[4]) {
    if (value > MQTT_VARINT_MAX) {
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
    size_t v = 0;
    uint8_t byte;
    if (off >= blens) {
        *value = 0;
        return ERR_FAILED;
    }
    for (size_t i = 0; i < blens - off && i < 4; i++) {
        byte = (uint8_t)buffer_at(buf, i + off);
        v |= (size_t)(byte & 0x7f) << (7 * i);
        if (!BIT_CHECK(byte, 0x80)) {
            *value = v;
            return (int32_t)(i + 1);
        }
    }
    *value = v;
    return ERR_FAILED;
}
