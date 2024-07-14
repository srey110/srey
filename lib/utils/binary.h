#ifndef BINARY_H_
#define BINARY_H_

#include "base/structs.h"
#include "utils/utils.h"

#define BINARY_INCREASE 256

//连续内存读写
typedef struct binary_ctx {
    char *data;
    size_t inc;
    size_t size;
    size_t offset;
}binary_ctx;
//初始化
void binary_init(binary_ctx *ctx, char *buf, size_t lens, size_t inc);
//设置偏移值
void binary_offset(binary_ctx *ctx, size_t off);
//写入数据
void binary_set_int8(binary_ctx *ctx, int8_t val);
void binary_set_uint8(binary_ctx *ctx, uint8_t val);
void binary_set_integer(binary_ctx *ctx, int64_t val, size_t lens, int32_t islittle);
void binary_set_uinteger(binary_ctx *ctx, uint64_t val, size_t lens, int32_t islittle);
void binary_set_float(binary_ctx *ctx, float val, int32_t islittle);
void binary_set_double(binary_ctx *ctx, double val, int32_t islittle);
void binary_set_string(binary_ctx *ctx, const char *buf, size_t lens);
void binary_set_fill(binary_ctx *ctx, char val, size_t lens);
void binary_set_skip(binary_ctx *ctx, size_t lens);
void binary_set_va(binary_ctx *ctx, const char *fmt, ...);
//读取数据
char *binary_at(binary_ctx *ctx, size_t pos);
int8_t binary_get_int8(binary_ctx *ctx);
uint8_t binary_get_uint8(binary_ctx *ctx);
int64_t binary_get_integer(binary_ctx *ctx, size_t lens, int32_t islittle);
uint64_t binary_get_uinteger(binary_ctx *ctx, size_t lens, int32_t islittle);
float binary_get_float(binary_ctx *ctx, int32_t islittle);
double binary_get_double(binary_ctx *ctx, int32_t islittle);
char *binary_get_string(binary_ctx *ctx, size_t lens);
void binary_get_skip(binary_ctx *ctx, size_t lens);

#endif//BINARY_H_
