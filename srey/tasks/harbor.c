#include "tasks/harbor.h"

static void _harbor_startup(ctask_ctx *ctask) {
    PRINT("_harbor_startup");
}
int32_t harbor_register(srey_ctx *ctx, name_t name) {
    return ctask_register(srey, NULL, name, 0, 0, _harbor_startup, NULL, NULL);
}
