#ifndef STARTUP_H_
#define STARTUP_H_

#include "ltasks/ltask.h"

typedef struct config_ctx {
    uint8_t loglv;
    uint8_t logfile;
    uint16_t nnet;
    uint16_t nworker;
    uint16_t harborport;
    name_t harborname;
    name_t harborssl;
    size_t stack_size;
    char harborip[IP_LENS];
    char fmt[64];
    char harborkey[SIGN_KEY_LENS];
}config_ctx;

static int32_t task_startup(srey_ctx *ctx, config_ctx *config) {
    int32_t rtn = harbor_start(ctx, config->harborname, config->harborssl, config->harborip, config->harborport);
    if (ERR_OK != rtn) {
        return rtn;
    }
#if WITH_LUA
    rtn = _ltask_startup();
    if (ERR_OK != rtn) {
        return rtn;
    }
#endif
    return rtn;
};

#endif//STARTUP_H_
