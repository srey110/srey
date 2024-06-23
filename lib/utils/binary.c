#include "utils/binary.h"

void binary_init(binary_ctx *ctx, char *buf, size_t lens, size_t inc) {
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
void binary_offset(binary_ctx *ctx, size_t off) {
    ctx->offset = off;
}
static inline void _binary_expand(binary_ctx *ctx, size_t size) {
    size += ctx->offset + 1;
    if (size > ctx->size) {
        size_t lens = (ctx->size / 2) * 3;
        if (lens < size) {
            lens = size;
        }
        ctx->size = ROUND_UP(lens, ctx->inc);
        REALLOC(ctx->data, ctx->data, ctx->size);
    }
}
void binary_set_int8(binary_ctx *ctx, int8_t val) {
    _binary_expand(ctx, sizeof(val));
    (ctx->data + ctx->offset)[0] = val;
    ctx->offset += sizeof(val);
}
void binary_set_uint8(binary_ctx *ctx, uint8_t val) {
    _binary_expand(ctx, sizeof(val));
    (ctx->data + ctx->offset)[0] = (int8_t)val;
    ctx->offset += sizeof(val);
}
void binary_set_integer(binary_ctx *ctx, int64_t val, size_t lens, int32_t islittle) {
    _binary_expand(ctx, lens);
    pack_integer(ctx->data + ctx->offset, (uint64_t)val, (int32_t)lens, islittle);
    ctx->offset += lens;
}
void binary_set_uinteger(binary_ctx *ctx, uint64_t val, size_t lens, int32_t islittle) {
    _binary_expand(ctx, lens);
    pack_integer(ctx->data + ctx->offset, val, (int32_t)lens, islittle);
    ctx->offset += lens;
}
void binary_set_float(binary_ctx *ctx, float val, int32_t islittle) {
    _binary_expand(ctx, sizeof(val));
    pack_float(ctx->data + ctx->offset, val, islittle);
    ctx->offset += sizeof(val);
}
void binary_set_double(binary_ctx *ctx, double val, int32_t islittle) {
    _binary_expand(ctx, sizeof(val));
    pack_double(ctx->data + ctx->offset, val, islittle);
    ctx->offset += sizeof(val);
}
void binary_set_string(binary_ctx *ctx, const char *buf, size_t lens) {
    if (0 == lens
        || NULL == buf) {
        return;
    }
    _binary_expand(ctx, lens);
    memcpy(ctx->data + ctx->offset, buf, lens);
    ctx->offset += lens;
}
void binary_set_fill(binary_ctx *ctx, char val, size_t lens) {
    _binary_expand(ctx, lens);
    memset(ctx->data + ctx->offset, val, lens);
    ctx->offset += lens;
}
void binary_set_skip(binary_ctx *ctx, size_t lens) {
    _binary_expand(ctx, lens);
    ctx->offset += lens;
}
static void _binary_va(binary_ctx *ctx, const char *fmt, va_list args) {
    if (0 == ctx->size - ctx->offset) {
        _binary_expand(ctx, ctx->inc - 1);
    }
    int32_t rtn;
    size_t size;
    while (1) {
        size = ctx->size - ctx->offset;
        rtn = vsnprintf(ctx->data + ctx->offset, size, fmt, args);
        if (rtn < 0) {
            break;
        }
        if (rtn >= 0
            && rtn < (int32_t)size) {
            ctx->offset += rtn;
            break;
        }
        _binary_expand(ctx, rtn);
    }
}
void binary_set_va(binary_ctx *ctx, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    _binary_va(ctx, fmt, args);
    va_end(args);
}
char *binary_at(binary_ctx *ctx, size_t pos) {
    ASSERTAB(pos < ctx->size, "out of memory.");
    return ctx->data + pos;
}
int8_t binary_get_int8(binary_ctx *ctx) {
    ASSERTAB(ctx->offset + sizeof(int8_t) <= ctx->size, "out of memory.");
    int8_t val = (ctx->data + ctx->offset)[0];
    ctx->offset += sizeof(val);
    return val;
}
uint8_t binary_get_uint8(binary_ctx *ctx) {
    ASSERTAB(ctx->offset + sizeof(uint8_t) <= ctx->size, "out of memory.");
    uint8_t val = (uint8_t)(ctx->data + ctx->offset)[0];
    ctx->offset += sizeof(val);
    return val;
}
int64_t binary_get_integer(binary_ctx *ctx, size_t lens, int32_t islittle) {
    ASSERTAB(ctx->offset + lens <= ctx->size, "out of memory.");
    int64_t val = unpack_integer(ctx->data + ctx->offset, (int32_t)lens, islittle, 1);
    ctx->offset += lens;
    return val;
}
uint64_t binary_get_uinteger(binary_ctx *ctx, size_t lens, int32_t islittle) {
    ASSERTAB(ctx->offset + lens <= ctx->size, "out of memory.");
    uint64_t val = (uint64_t)unpack_integer(ctx->data + ctx->offset, (int32_t)lens, islittle, 0);
    ctx->offset += lens;
    return val;
}
float binary_get_float(binary_ctx *ctx, int32_t islittle) {
    ASSERTAB(ctx->offset + sizeof(float) <= ctx->size, "out of memory.");
    float val = unpack_float(ctx->data + ctx->offset, islittle);
    ctx->offset += sizeof(val);
    return val;
}
double binary_get_double(binary_ctx *ctx, int32_t islittle) {
    ASSERTAB(ctx->offset + sizeof(double) <= ctx->size, "out of memory.");
    double val = unpack_double(ctx->data + ctx->offset, islittle);
    ctx->offset += sizeof(val);
    return val;
}
char *binary_get_string(binary_ctx *ctx, size_t lens) {
    char *val = ctx->data + ctx->offset;
    if (0 == lens) {
        size_t slen = strlen(val) + 1;
        size_t remain = ctx->size - ctx->offset;
        ctx->offset += (slen > remain ? remain : slen);
    } else {
        ASSERTAB(ctx->offset + lens <= ctx->size, "out of memory.");
        ctx->offset += lens;
    }
    return val;
}
void binary_get_skip(binary_ctx *ctx, size_t lens) {
    ASSERTAB(ctx->offset + lens <= ctx->size, "out of memory.");
    ctx->offset += lens;
}
