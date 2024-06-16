#include "crypt/padding.h"
#include "utils/utils.h"

void _padding_data(padding_model padding, const void *data, size_t dlens, uint8_t *output, size_t reqlens) {
    if (NULL != data
        && dlens > 0) {
        memcpy(output, data, dlens);
        output += dlens;
    }
    size_t remain = reqlens - dlens;
    switch (padding) {
    case ZeroPadding:
        ZERO(output, remain);
        break;
    case PKCS57:
        memset(output, (int32_t)remain, remain);
        break;
    case ISO10126:
        for (size_t i = 0; i < remain - 1; i++) {
            output[i] = (uint8_t)randrange(0, UCHAR_MAX + 1);
        }
        output[remain - 1] = (uint8_t)remain;
        break;
    case ANSIX923:
        ZERO(output, remain - 1);
        output[remain - 1] = (uint8_t)remain;
        break;
    default:
        break;
    }
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
