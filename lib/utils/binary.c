#include "utils/binary.h"

#define BINARY_INCREASE 256

void binary_init(binary_ctx *ctx, char *buf, size_t lens, size_t inc) {
    ctx->offset = 0;
    if (NULL == buf) {
        //内部托管：分配可扩容 buffer
        if (0 == inc) {
            ctx->inc = BINARY_INCREASE;
        } else {
            // 上界必须挡：inc 是 size_t，直接转 uint32_t 时 2^32 会截成 0，
            // 而 inc==0 是"外部托管"的标记——data 从此不再被 binary_free 释放，
            // 且任何 binary_set_* 都会撞上只读断言
            ASSERTAB(inc <= INT32_MAX, ERRSTR_INVPARAM);
            ctx->inc = pow2_ceil((uint32_t)(inc < 2 ? 2 : inc));
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
void binary_set_va(binary_ctx *ctx, const char *fmt, ...) {
    //外部托管下 ctx->inc==0，ctx->inc-1 下溢为 SIZE_MAX 会让后续逻辑错乱，提前拒绝
    ASSERTAB(0 != ctx->inc, "external buffer cannot binary_set_va: use binary_init(NULL,...) for writable mode");
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
