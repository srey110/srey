#ifndef XOR_H_
#define XOR_H_

#include "base/macro.h"

char *xor_encode(const char key[4], const size_t round, char *data, const size_t lens);
char *xor_decode(const char key[4], const size_t round, char *data, const size_t lens);

#endif//XOR_H_
