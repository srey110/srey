#include "base/sbyteswap.h"
#include "base/macro_log.h"

#define _MC ((1 << CHAR_BIT) - 1) //字节掩码（0xff），用于逐字节提取整数

void pack_integer(char *buf, uint64_t val, int32_t size, int32_t islittle) {
    ASSERTAB(size > 0, "pack_integer size must be positive.");
    buf[islittle ? 0 : size - 1] = (int8_t)(val & _MC);
    for (int32_t i = 1; i < size; i++) {
        val >>= CHAR_BIT;
        buf[islittle ? i : size - 1 - i] = (int8_t)(val & _MC);
    }
}
int64_t unpack_integer(const char *buf, int32_t size, int32_t islittle, int32_t issigned) {
    if (size <= 0) {
        return 0;
    }
    uint64_t rtn = 0;
    int32_t limit = (size <= (int32_t)sizeof(uint64_t)) ? size : (int32_t)sizeof(uint64_t);
    for (int32_t i = limit - 1; i >= 0; i--) {
        rtn <<= CHAR_BIT;
        rtn |= (uint64_t)(uint8_t)buf[islittle ? i : size - 1 - i];
    }
    if (size < (int32_t)sizeof(uint64_t)) {
        if (issigned) {
            uint64_t mask = 1llu << (size * CHAR_BIT - 1);
            rtn = ((rtn ^ mask) - mask);
        }
    }
    return (int64_t)rtn;
}
void pack_float(char *buf, float val, int32_t islittle) {
    uint32_t u;
    memcpy(&u, &val, sizeof(u));
    if (islittle != IS_LITTLE) {
        u = byteswap32(u);
    }
    memcpy(buf, &u, sizeof(u));
}
float unpack_float(const char *buf, int32_t islittle) {
    uint32_t u;
    float rtn;
    memcpy(&u, buf, sizeof(u));
    if (islittle != IS_LITTLE) {
        u = byteswap32(u);
    }
    memcpy(&rtn, &u, sizeof(rtn));
    return rtn;
}
void pack_double(char *buf, double val, int32_t islittle) {
    uint64_t u;
    memcpy(&u, &val, sizeof(u));
    if (islittle != IS_LITTLE) {
        u = byteswap64(u);
    }
    memcpy(buf, &u, sizeof(u));
}
double unpack_double(const char *buf, int32_t islittle) {
    uint64_t u;
    double rtn;
    memcpy(&u, buf, sizeof(u));
    if (islittle != IS_LITTLE) {
        u = byteswap64(u);
    }
    memcpy(&rtn, &u, sizeof(rtn));
    return rtn;
}
#if !defined(OS_WIN) && !defined(OS_DARWIN) && !defined(OS_AIX)
uint64_t ntohll(uint64_t val) {
    return IS_LITTLE ? byteswap64(val) : val;
}
uint64_t htonll(uint64_t val) {
    return ntohll(val);
}
#endif//OS_WIN OS_DARWIN OS_AIX
