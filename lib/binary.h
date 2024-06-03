#ifndef BINARY_H_
#define BINARY_H_

#include "structs.h"
#include "utils.h"

#define BINARY_INCREASE 256

typedef struct binary_ctx {
    char *data;
    size_t inc;
    size_t size;
    size_t offset;
}binary_ctx;

static inline void binary_init(binary_ctx *ctx, char *buf, size_t lens, size_t inc) {
    ASSERTAB(BINARY_INCREASE > 0 && 0 == BINARY_INCREASE % 2, "");
    ctx->offset = 0;
    if (0 == inc) {
        ctx->inc = BINARY_INCREASE;
    } else {
        ctx->inc = ROUND_UP(inc, 2);
    }
    if (NULL == buf) {
        if (0 == lens) {
            ctx->size = ctx->inc;
        } else {
            ctx->size = ROUND_UP(lens, ctx->inc);
        }
        MALLOC(ctx->data, ctx->size);
    } else {
        ctx->size = lens;
        ctx->data = buf;
    }
}
static inline void binary_offset(binary_ctx *ctx, size_t off) {
    ctx->offset = off;
}
static inline void _binary_expand(binary_ctx *ctx, size_t size) {
    size += ctx->offset;
    if (size > ctx->size) {
        size_t lens = (ctx->size / 2) * 3;
        if (lens < size) {
            lens = size;
        }
        ctx->size = ROUND_UP(lens, ctx->inc);
        REALLOC(ctx->data, ctx->data, ctx->size);
    }
}
static inline void binary_set_int8(binary_ctx *ctx, int8_t val) {
    _binary_expand(ctx, sizeof(val));
    (ctx->data + ctx->offset)[0] = val;
    ctx->offset += sizeof(val);
}
static inline void binary_set_uint8(binary_ctx *ctx, uint8_t val) {
    _binary_expand(ctx, sizeof(val));
    (ctx->data + ctx->offset)[0] = (int8_t)val;
    ctx->offset += sizeof(val);
}
static inline void binary_set_integer(binary_ctx *ctx, int64_t val, size_t lens, int32_t islittle) {
    _binary_expand(ctx, lens);
    pack_integer(ctx->data + ctx->offset, (uint64_t)val, (int32_t)lens, islittle);
    ctx->offset += lens;
}
static inline void binary_set_uinteger(binary_ctx *ctx, uint64_t val, size_t lens, int32_t islittle) {
    _binary_expand(ctx, lens);
    pack_integer(ctx->data + ctx->offset, val, (int32_t)lens, islittle);
    ctx->offset += lens;
}
static inline void binary_set_float(binary_ctx *ctx, float val, int32_t islittle) {
    _binary_expand(ctx, sizeof(val));
    pack_float(ctx->data + ctx->offset, val, islittle);
    ctx->offset += sizeof(val);
}
static inline void binary_set_double(binary_ctx *ctx, double val, int32_t islittle) {
    _binary_expand(ctx, sizeof(val));
    pack_double(ctx->data + ctx->offset, val, islittle);
    ctx->offset += sizeof(val);
}
static inline void binary_set_string(binary_ctx *ctx, const char *buf, size_t lens) {
    _binary_expand(ctx, lens);
    memcpy(ctx->data + ctx->offset, buf, lens);
    ctx->offset += lens;
}
static inline void binary_set_fill(binary_ctx *ctx, char val, size_t lens) {
    _binary_expand(ctx, lens);
    memset(ctx->data + ctx->offset, val, lens);
    ctx->offset += lens;
}
static inline void binary_set_skip(binary_ctx *ctx, size_t lens) {
    _binary_expand(ctx, lens);
    ctx->offset += lens;
}
static inline int8_t binary_get_int8(binary_ctx *ctx) {
    ASSERTAB(ctx->offset + sizeof(int8_t) <= ctx->size, "out of memory.");
    int8_t val = (ctx->data + ctx->offset)[0];
    ctx->offset += sizeof(val);
    return val;
}
static inline uint8_t binary_get_uint8(binary_ctx *ctx) {
    ASSERTAB(ctx->offset + sizeof(uint8_t) <= ctx->size, "out of memory.");
    uint8_t val = (uint8_t)(ctx->data + ctx->offset)[0];
    ctx->offset += sizeof(val);
    return val;
}
static inline int16_t binary_get_int16(binary_ctx *ctx, size_t lens, int32_t islittle) {
    ASSERTAB(ctx->offset + lens <= ctx->size, "out of memory.");
    int16_t val = (int16_t)unpack_integer(ctx->data + ctx->offset, (int32_t)lens, islittle, 1);
    ctx->offset += lens;
    return val;
}
static inline uint16_t binary_get_uint16(binary_ctx *ctx, size_t lens, int32_t islittle) {
    ASSERTAB(ctx->offset + lens <= ctx->size, "out of memory.");
    uint16_t val = (uint16_t)unpack_integer(ctx->data + ctx->offset, (int32_t)lens, islittle, 0);
    ctx->offset += lens;
    return val;
}
static inline int32_t binary_get_int32(binary_ctx *ctx, size_t lens, int32_t islittle) {
    ASSERTAB(ctx->offset + lens <= ctx->size, "out of memory.");
    int32_t val = (int32_t)unpack_integer(ctx->data + ctx->offset, (int32_t)lens, islittle, 1);
    ctx->offset += lens;
    return val;
}
static inline uint32_t binary_get_uint32(binary_ctx *ctx, size_t lens, int32_t islittle) {
    ASSERTAB(ctx->offset + lens <= ctx->size, "out of memory.");
    uint32_t val = (uint32_t)unpack_integer(ctx->data + ctx->offset, (int32_t)lens, islittle, 0);
    ctx->offset += lens;
    return val;
}
static inline int64_t binary_get_int64(binary_ctx *ctx, size_t lens, int32_t islittle) {
    ASSERTAB(ctx->offset + lens <= ctx->size, "out of memory.");
    int64_t val = unpack_integer(ctx->data + ctx->offset, (int32_t)lens, islittle, 1);
    ctx->offset += lens;
    return val;
}
static inline uint64_t binary_get_uint64(binary_ctx *ctx, size_t lens, int32_t islittle) {
    ASSERTAB(ctx->offset + lens <= ctx->size, "out of memory.");
    uint64_t val = (uint64_t)unpack_integer(ctx->data + ctx->offset, (int32_t)lens, islittle, 0);
    ctx->offset += lens;
    return val;
}
static inline float binary_get_float(binary_ctx *ctx, int32_t islittle) {
    ASSERTAB(ctx->offset + sizeof(float) <= ctx->size, "out of memory.");
    float val = unpack_float(ctx->data + ctx->offset, islittle);
    ctx->offset += sizeof(val);
    return val;
}
static inline double binary_get_double(binary_ctx *ctx, int32_t islittle) {
    ASSERTAB(ctx->offset + sizeof(double) <= ctx->size, "out of memory.");
    double val = unpack_double(ctx->data + ctx->offset, islittle);
    ctx->offset += sizeof(val);
    return val;
}
static inline char *binary_get_string(binary_ctx *ctx, size_t lens) {
    char *val = ctx->data + ctx->offset;
    if (0 == lens) {
        size_t slen = strlen(val) + 1;
        ASSERTAB(ctx->offset + slen <= ctx->size, "out of memory.");
        ctx->offset += slen;
    } else {
        ASSERTAB(ctx->offset + lens <= ctx->size, "out of memory.");
        ctx->offset += lens;
    }
    return val;
}
static inline void binary_get_skip(binary_ctx *ctx, size_t lens) {
    ASSERTAB(ctx->offset + lens <= ctx->size, "out of memory.");
    ctx->offset += lens;
}

#endif//BINARY_H_
