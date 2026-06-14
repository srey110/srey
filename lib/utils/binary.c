#include "utils/binary.h"

#define BINARY_INCREASE 256

void binary_init(binary_ctx *ctx, char *buf, size_t lens, size_t inc) {
    ctx->offset = 0;
    if (NULL == buf) {
        //内部托管：分配可扩容 buffer
        if (0 == inc) {
            ctx->inc = BINARY_INCREASE;
        } else {
            ctx->inc = ROUND_UP(inc, 2);
        }
        if (0 == lens) {
            ctx->size = ctx->inc;
        } else {
            ctx->size = ROUND_UP(lens, ctx->inc);
        }
        MALLOC(ctx->data, ctx->size);
    } else {
        //外部托管：禁止扩容（_binary_expand 中以 inc==0 为标记 ASSERT 拒绝）
        //调用方契约：传入外部 buf 后只能用 binary_get_* / binary_offset / binary_at 等不扩容的接口
        ctx->inc = 0;
        ctx->size = lens;
        ctx->data = buf;
    }
}
void binary_free(binary_ctx *ctx) {
    //仅内部托管（inc!=0）才持有 data 的所有权，需释放；外部托管 buf 由调用方负责
    if (0 != ctx->inc && NULL != ctx->data) {
        FREE(ctx->data);
    }
    ctx->data = NULL;
    ctx->size = 0;
    ctx->offset = 0;
}
void binary_offset(binary_ctx *ctx, size_t off) {
    ctx->offset = off;
}
// 扩展缓冲区，确保有足够空间写入 size 字节（仅内部托管可扩容）
static inline void _binary_expand(binary_ctx *ctx, size_t size) {
    ASSERTAB(size <= SIZE_MAX - ctx->offset - 1, "binary buffer size overflow");
    size += ctx->offset + 1;
    if (size > ctx->size) {
        //inc==0 标记外部托管 buf：对栈/静态/异分配器内存调 REALLOC 是 UB，必须明确拒绝
        ASSERTAB(0 != ctx->inc, "external buffer cannot expand: use binary_init(NULL,...) for writable mode");
        size_t lens = ctx->size * 2;
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
void binary_set_string(binary_ctx *ctx, const char *buf) {
    if (NULL == buf) {
        return;
    }
    size_t lens = strlen(buf);
    _binary_expand(ctx, lens + 1);
    memcpy(ctx->data + ctx->offset, buf, lens);
    ctx->offset += lens;
    ctx->data[ctx->offset] = '\0';
    ctx->offset++;
}
void binary_set_binary(binary_ctx *ctx, const char *buf, size_t lens) {
    if (NULL == buf || 0 == lens) {
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
// 使用 va_list 格式化字符串写入缓冲区，自动扩容（仅内部托管）
static void _binary_va(binary_ctx *ctx, const char *fmt, va_list args) {
    //外部托管下 ctx->inc==0，ctx->inc-1 下溢为 SIZE_MAX 会让后续逻辑错乱，提前拒绝
    ASSERTAB(0 != ctx->inc, "external buffer cannot binary_set_va: use binary_init(NULL,...) for writable mode");
    if (0 == ctx->size - ctx->offset) {
        _binary_expand(ctx, ctx->inc - 1);
    }
    int32_t rtn;
    size_t size;
    va_list tmp;
    while (1) {
        size = ctx->size - ctx->offset;
        va_copy(tmp, args);
        rtn = vsnprintf(ctx->data + ctx->offset, size, fmt, tmp);
        va_end(tmp);
        if (rtn < 0) {
            break;
        }
        if ((size_t)rtn < size) {
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
    //先减后比，避免攻击者构造极大 lens 让 offset+lens size_t 溢出绕过断言
    ASSERTAB(lens <= ctx->size - ctx->offset, "out of memory.");
    int64_t val = unpack_integer(ctx->data + ctx->offset, (int32_t)lens, islittle, 1);
    ctx->offset += lens;
    return val;
}
uint64_t binary_get_uinteger(binary_ctx *ctx, size_t lens, int32_t islittle) {
    ASSERTAB(lens <= ctx->size - ctx->offset, "out of memory.");
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
char *binary_get_string(binary_ctx *ctx) {
    char *val = ctx->data + ctx->offset;
    size_t remain = ctx->size - ctx->offset;
    size_t slen = strnlen(val, remain);
    ASSERTAB(slen < remain, "out of memory.");
    ctx->offset += slen + 1;
    return val;
}
char *binary_get_binary(binary_ctx *ctx, size_t lens) {
    ASSERTAB(lens <= ctx->size - ctx->offset, "out of memory.");
    if (0 == lens) {
        return NULL;
    }
    char *val = ctx->data + ctx->offset;
    ctx->offset += lens;
    return val;
}
void binary_get_skip(binary_ctx *ctx, size_t lens) {
    ASSERTAB(lens <= ctx->size - ctx->offset, "out of memory.");
    ctx->offset += lens;
}
