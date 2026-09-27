#include "utils/binary.h"

#define BINARY_INCREASE 256

void binary_init_write(binary_ctx *ctx, size_t lens, size_t inc) {
    ctx->offset = 0;
    if (0 == inc) {
        ctx->inc = BINARY_INCREASE;
    } else {
        // 上界必须挡：inc 是 size_t，直接转 uint32_t 时 2^32 会截成 0，
        // 而 inc==0 是读模式的标记——data 从此不再被 binary_free 释放，
        // 且任何 binary_set_* 都会撞上只读断言
        ASSERTAB(inc <= INT32_MAX, ERRSTR_INVPARAM);
        ctx->inc = pow2_ceil((uint32_t)(inc < 2 ? 2 : inc));
    }
    if (0 == lens) {
        ctx->size = ctx->inc;
    } else {
        ctx->size = ROUND_UP(lens + 1, ctx->inc);
    }
    MALLOC(ctx->data, ctx->size);
}
void binary_init_read(binary_ctx *ctx, char *buf, size_t lens) {
    //inc=0 一身二职：既让 _binary_expand 拒绝扩容,也让 binary_free 不去释放这块外部内存
    ctx->offset = 0;
    ctx->inc = 0;
    ctx->size = lens;
    ctx->data = buf;
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
void _binary_grow(binary_ctx *ctx, size_t size) {
    //inc==0 标记外部托管 buf：不接管所有权，任何 binary_set_* 都会从 offset 起改写调用方内存，
    //超出容量时还要对栈/静态/异分配器内存调 REALLOC(UB)
    ASSERTAB(0 != ctx->inc, "read-mode buffer is read-only: use binary_init_write for writable mode");
    ASSERTAB(size <= SIZE_MAX - ctx->offset - 1, "binary buffer size overflow");
    size += ctx->offset + 1;
    if (size > ctx->size) {
        size_t lens = ctx->size * 2;
        if (lens < size) {
            lens = size;
        }
        ctx->size = ROUND_UP(lens, ctx->inc);
        // 翻倍与取整都可能溢出回绕成 0，而 _realloc(0) 按契约释放并返回 NULL，下面就 memmove 到空指针
        ASSERTAB(0 != ctx->size, "binary buffer size overflow");
        REALLOC(ctx->data, ctx->data, ctx->size);
    }
}
void binary_set_uint(binary_ctx *ctx, uint64_t val, uint32_t base) {
    ASSERTAB(base >= 2 && base <= 16, ERRSTR_INVPARAM);
    char buf[64];
    char *p = buf + sizeof(buf);
    do {
        *--p = "0123456789abcdef"[val % base];
        val /= base;
    } while (0 != val);
    binary_set_binary(ctx, p, (size_t)(buf + sizeof(buf) - p));
}
void binary_set_va(binary_ctx *ctx, const char *fmt, ...) {
    //外部托管下 ctx->inc==0，ctx->inc-1 下溢为 SIZE_MAX 会让后续逻辑错乱，提前拒绝
    ASSERTAB(0 != ctx->inc, "read-mode buffer cannot binary_set_va: use binary_init_write for writable mode");
    if (0 == ctx->size - ctx->offset) {
        _binary_expand(ctx, ctx->inc - 1);
    }
    int32_t rtn;
    size_t size;
    va_list args, tmp;
    va_start(args, fmt);
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
    va_end(args);
}
