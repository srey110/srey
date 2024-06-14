#ifndef BASE64_H_
#define BASE64_H_

#include "base/macro.h"

#define B64EN_BLOCK_SIZE(s)   ((((s) + 2) / 3 * 4) + 1)
#define B64DE_BLOCK_SIZE(s)   (((s) / 4 * 3) + 1)

size_t b64_encode(const void *data, const size_t lens, char *out);
size_t b64_decode(const char *data, const size_t lens, char *out);

#endif//BASE64_H_
