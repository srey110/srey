#ifndef MD2_H_
#define MD2_H_

#include "base/macro.h"

#define MD2_BLOCK_SIZE 16

typedef struct md2_ctx {
   unsigned char data[16];
   unsigned char state[48];
   unsigned char checksum[16];
   int32_t lens;
} md2_ctx;

void md2_init(md2_ctx *md2);
void md2_update(md2_ctx *md2, const unsigned char *data, size_t lens);
void md2_final(md2_ctx *md2, unsigned char hash[MD2_BLOCK_SIZE]);

#endif//MD2_H_
