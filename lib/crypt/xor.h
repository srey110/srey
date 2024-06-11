#ifndef XOR_H_
#define XOR_H_

#include "base/macro.h"

char *xor_encode(const char key[4], const size_t round, char *buf, const size_t len);
char *xor_decode(const char key[4], const size_t round, char *buf, const size_t len);

#endif//XOR_H_
