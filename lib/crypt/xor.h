#ifndef XOR_H_
#define XOR_H_

#include "base/macro.h"

void *xor_encode(const char key[4], const size_t round, void *data, const size_t lens);
void *xor_decode(const char key[4], const size_t round, void *data, const size_t lens);

#endif//XOR_H_
