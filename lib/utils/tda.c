#include "utils/tda.h"
#include "utils/utils.h"

void tda_init(tda_ctx *ctx, size_t overload_init) {
    ctx->overload_init = overload_init;
    ctx->overload_threshold = overload_init;
}
int32_t tda_check(tda_ctx *ctx, size_t overload) {
    if (0 == ctx->overload_init) {
        return 0;
    }
    if (overload < ctx->overload_init) {
        ctx->overload_threshold = ctx->overload_init;
        return 0;
    }
    int32_t triggered = 0;
    while (overload > ctx->overload_threshold) {
        triggered = 1;
        ctx->overload_threshold *= 2;
    }
    return triggered;
}
