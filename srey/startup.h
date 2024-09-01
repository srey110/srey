#ifndef STARTUP_H_
#define STARTUP_H_

#include "lbind/ltask.h"
#include "tasks/harbor.h"

typedef struct config_ctx {
    uint8_t loglv;
    uint16_t nnet;
    uint16_t nworker;
    uint16_t harborport;
    int32_t harbortimeout;
    name_t harborname;
    name_t harborssl;
    char dns[IP_LENS];
    char harborip[IP_LENS];
    char harborkey[128];
    char script[PATH_LENS];
}config_ctx;

static int32_t task_startup(loader_ctx *loader, config_ctx *config) {
    int32_t rtn = harbor_start(loader, config->harborname, config->harborssl,
        config->harborip, config->harborport, config->harborkey, config->harbortimeout);
    if (ERR_OK != rtn) {
        return rtn;
    }
#if WITH_LUA
    rtn = ltask_startup(config->script);
    if (ERR_OK != rtn) {
        return rtn;
    }
#endif
    return rtn;
};

#endif//STARTUP_H_
