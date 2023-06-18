#include "tasks.h"

int32_t task_startup(srey_ctx *ctx) {
    int32_t rtn = ERR_OK;
#if WITH_LUA
    rtn = _ltask_startup(ctx);
#endif
    return rtn;
}
