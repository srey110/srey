#include "utils/uuid.h"
#include "utils/utils.h"

static atomic64_t _v7_last;// 上次发出的 (毫秒 << 12) | 计数器，全进程共用

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
// 按 8-4-4-4-12 分五段转，连字符写在段间；最后一段的 tohex 顺带写结尾 '\0'
void uuid_tostr(const char uuid[UUID_LENS], char out[UUID_STR_LENS]) {
    tohex(uuid, 4, out, 1);
    out[8] = '-';
    tohex(uuid + 4, 2, out + 9, 1);
    out[13] = '-';
    tohex(uuid + 6, 2, out + 14, 1);
    out[18] = '-';
    tohex(uuid + 8, 2, out + 19, 1);
    out[23] = '-';
    tohex(uuid + 10, 6, out + 24, 1);
}
// 布局固定：先认四个连字符，再按固定偏移解 16 个字节。非法字符 fromhex 返负数，
// 各字节的结果按位或起来最后只判一次(失败时 uuid 内容无定义，契约允许)
int32_t uuid_fromstr(const char *str, size_t lens, char uuid[UUID_LENS]) {
    static const uint8_t pos[UUID_LENS] = { 0, 2, 4, 6, 9, 11, 14, 16, 19, 21, 24, 26, 28, 30, 32, 34 };
    int32_t i, hi, lo, bad = 0;
    if (UUID_STR_LENS - 1 != lens
        || '-' != str[8] || '-' != str[13] || '-' != str[18] || '-' != str[23]) {
        return ERR_FAILED;
    }
    for (i = 0; i < UUID_LENS; i++) {
        hi = fromhex(str[pos[i]]);
        lo = fromhex(str[pos[i] + 1]);
        bad |= hi | lo;
        uuid[i] = (char)(((uint32_t)hi << 4) | (uint32_t)lo);
    }
    return bad < 0 ? ERR_FAILED : ERR_OK;
}
