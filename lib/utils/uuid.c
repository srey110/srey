#include "utils/uuid.h"
#include "utils/utils.h"

static atomic64_t _v7_last;// 上次发出的 (毫秒 << 12) | 计数器，全进程共用

// 第 i 个字节的十六进制文本前面是否有连字符
static inline int32_t _dash_before(int32_t i) {
    return 4 == i || 6 == i || 8 == i || 10 == i;
}
int32_t uuid_v4(char uuid[UUID_LENS]) {
    if (ERR_OK != csprng_rand(uuid, UUID_LENS)) {
        return ERR_FAILED;
    }
    uuid[6] = (char)(((uint8_t)uuid[6] & 0x0F) | 0x40);
    uuid[8] = (char)(((uint8_t)uuid[8] & 0x3F) | 0x80);
    return ERR_OK;
}
// 进入新毫秒时计数器取随机低 11 位（最高位留 0 防回绕）；同毫秒、回拨、计数器用满一律 last + 1
int32_t uuid_v7(char uuid[UUID_LENS]) {
    uint64_t cand, last, next;
    if (ERR_OK != csprng_rand(uuid + 6, 10)) {
        return ERR_FAILED;
    }
    cand = (nowms() << 12) | ((((uint8_t)uuid[6] << 8) | (uint8_t)uuid[7]) & 0x7FF);
    do {
        last = (uint64_t)ATOMIC64_GET(&_v7_last);
        next = (cand >> 12) > (last >> 12) ? cand : last + 1;
    } while (!ATOMIC64_CAS(&_v7_last, (atomic64_t)last, (atomic64_t)next));
    pack_integer(uuid, next >> 12, 6, 0);
    uuid[6] = (char)(0x70 | ((next >> 8) & 0x0F));
    uuid[7] = (char)(next & 0xFF);
    uuid[8] = (char)(((uint8_t)uuid[8] & 0x3F) | 0x80);
    return ERR_OK;
}
int32_t uuid_version(const char uuid[UUID_LENS]) {
    if (0x80 != ((uint8_t)uuid[8] & 0xC0)) {
        return 0;
    }
    return (uint8_t)uuid[6] >> 4;
}
uint64_t uuid_v7_ms(const char uuid[UUID_LENS]) {
    if (7 != uuid_version(uuid)) {
        return 0;
    }
    return (uint64_t)unpack_integer(uuid, 6, 0, 0);
}
void uuid_tostr(const char uuid[UUID_LENS], char out[UUID_STR_LENS]) {
    int32_t i;
    for (i = 0; i < UUID_LENS; i++) {
        if (_dash_before(i)) {
            *out++ = '-';
        }
        tohex(uuid + i, 1, out, 1);
        out += 2;
    }
}
int32_t uuid_fromstr(const char *str, size_t lens, char uuid[UUID_LENS]) {
    int32_t i, hi, lo;
    if (UUID_STR_LENS - 1 != lens) {
        return ERR_FAILED;
    }
    for (i = 0; i < UUID_LENS; i++) {
        if (_dash_before(i)) {
            if ('-' != *str) {
                return ERR_FAILED;
            }
            str++;
        }
        hi = fromhex(str[0]);
        lo = fromhex(str[1]);
        if (hi < 0 || lo < 0) {
            return ERR_FAILED;
        }
        uuid[i] = (char)((hi << 4) | lo);
        str += 2;
    }
    return ERR_OK;
}
