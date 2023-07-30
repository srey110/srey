#ifndef URLRAW_H_
#define URLRAW_H_

#include "macro.h"

#define URLEN_BLOCK_SIZE(s) (3 * (s) + 1)

char *url_encode(const char *str, const size_t len, char *out);
size_t url_decode(char *str, size_t len);

#endif//URLRAW_H_
