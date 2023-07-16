#ifndef STARTUP_H_
#define STARTUP_H_

#include "ltasks/ltask.h"

static inline int32_t task_startup(srey_ctx *ctx) {
    int32_t rtn = ERR_OK;
#if WITH_LUA
    rtn = _ltask_startup();
#endif
    return rtn;
};

#endif//STARTUP_H_
