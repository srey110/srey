#ifndef BASE64_H_
#define BASE64_H_

#include "macro.h"

#define B64_ENSIZE(s)   ((((s) + 2) / 3 * 4) + 1)
#define B64_DESIZE(s)   (((s) / 4 * 3) + 1)

size_t b64_encode(const char *buf, const size_t len, char *out);
size_t b64_decode(const char *buf, const size_t len, char *out);

#endif//BASE64_H_
