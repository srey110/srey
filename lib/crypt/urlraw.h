#ifndef URLRAW_H_
#define URLRAW_H_

#include "base/macro.h"

#define URLEN_BLOCK_SIZE(s) (3 * (s) + 1)

char *url_encode(const char *data, const size_t lens, char *out);
size_t url_decode(char *data, size_t lens);

#endif//URLRAW_H_
