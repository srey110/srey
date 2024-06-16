#ifndef PADDING_H_
#define PADDING_H_

#include "base/macro.h"

typedef enum padding_model {
    NoPadding = 0x00,
    ZeroPadding,
    PKCS57,
    ISO10126,
    ANSIX923
}padding_model;

void _padding_data(padding_model padding, const void *data, size_t dlens, uint8_t *output, size_t reqlens);
uint8_t *_padding_key(const char *key, size_t klens, uint8_t *pdkey, size_t reqlens);

#endif//PADDING_H_
