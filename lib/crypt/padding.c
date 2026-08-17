#include "crypt/padding.h"
#include "utils/utils.h"

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
        ASSERTAB(ERR_OK == csprng_rand(output, remain - 1), ERRORSTR(ERRNO));
        output[remain - 1] = (uint8_t)remain;
        break;
    case ANSIX923:
        ZERO(output, remain - 1);
        output[remain - 1] = (uint8_t)remain;
        break;
    }
    return ERR_OK;
}
uint8_t *_padding_key(const char *key, size_t klens, uint8_t *pdkey, size_t reqlens) {
    if (klens < reqlens) {
        memcpy(pdkey, key, klens);
        ZERO(pdkey + klens, reqlens - klens);
        return pdkey;
    } else {
        return (uint8_t *)key;
    }
}
