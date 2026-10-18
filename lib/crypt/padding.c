#include "crypt/padding.h"

int32_t _padding_data(padding_model padding, const void *data, size_t dlens, uint8_t *output, size_t reqlens) {
    if (dlens > reqlens) {
        return ERR_FAILED;// 装不下,output 一个字节都没写,调用方不能当它有内容
    }
    if (dlens > 0
        && NULL == data) {
        return ERR_FAILED;
    }
    if (dlens == reqlens) {
        if (dlens > 0) {
            memcpy(output, data, dlens);
        }
        return ERR_OK;
    }
    if (dlens > 0) {
        memcpy(output, data, dlens);
        output += dlens;
    }
    size_t remain = reqlens - dlens;
    switch (padding) {
    case NoPadding:
        break;// 有意不动填充区,保持调用方原样
    case ZeroPadding:
        ZERO(output, remain);
        break;
    case PKCS57:
        memset(output, (int32_t)remain, remain);
        break;
    case ISO10126:
        if (ERR_OK != csprng_rand(output, remain - 1)) {
            return ERR_FAILED;
        }
        output[remain - 1] = (uint8_t)remain;
        break;
    case ANSIX923:
        ZERO(output, remain - 1);
        output[remain - 1] = (uint8_t)remain;
        break;
    }
    return ERR_OK;
}
int32_t _padding_strip(padding_model padding, const uint8_t *block, size_t blk, uint8_t *pad) {
    uint8_t len = block[blk - 1];
    if (ISO10126 == padding) {
        if (len < 1 || len > blk) {
            return ERR_FAILED;
        }
        *pad = len;
        return ERR_OK;
    }
    // 循环长度固定 [1, blk)，mask 屏蔽非填充区；PKCS#7 期望全部 == len，ANSI X.923 前 N-1 字节 == 0。
    // 长度越界也折进 bad 而不提前返回：两条失败路径耗时不同就把"长度字节是否合法"漏了出去
    uint8_t expected = (PKCS57 == padding) ? len : (uint8_t)0;
    volatile uint8_t bad = (uint8_t)(0u - ((((uint32_t)len - 1u) | ((uint32_t)blk - (uint32_t)len)) >> 31));
    uint8_t mask;
    for (size_t j = 1; j < blk; j++) {
        mask = (uint8_t)(0u - (((uint32_t)j - (uint32_t)len) >> 31));// j < len 即在填充区
        bad |= mask & (block[blk - 1 - j] ^ expected);
    }
    if (0 != bad) {
        return ERR_FAILED;
    }
    *pad = len;
    return ERR_OK;
}
uint8_t *_padding_key(const char *key, size_t klens, uint8_t *pdkey, size_t reqlens) {
    if (klens < reqlens) {
        if (klens > 0) {
            memcpy(pdkey, key, klens);// klens 为 0 时 key 可以是 NULL, memcpy 不收 NULL
        }
        ZERO(pdkey + klens, reqlens - klens);
        return pdkey;
    }
    return (uint8_t *)key;// 够长即原样透传, 只有前 reqlens 字节会被用到
}
