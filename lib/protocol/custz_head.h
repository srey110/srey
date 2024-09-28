#ifndef CUSTZ_HEAD_H_
#define CUSTZ_HEAD_H_

#include "utils/buffer.h"

int32_t _custz_decode_fixed(buffer_ctx *buf, size_t *hlens, size_t *size, int32_t *status);
char *_custz_encode_fixed(size_t dlens, size_t *hlens, size_t *size);

int32_t _custz_decode_flag(buffer_ctx *buf, size_t *hlens, size_t *size, int32_t *status);
char *_custz_encode_flag(size_t dlens, size_t *hlens, size_t *size);

int32_t _custz_decode_variable(buffer_ctx *buf, size_t *hlens, size_t *size, int32_t *status);
char *_custz_encode_variable(size_t dlens, size_t *hlens, size_t *size);

#endif//CUSTZ_HEAD_H_
