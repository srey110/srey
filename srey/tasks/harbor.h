#ifndef HARBOR_H_
#define HARBOR_H_

#include "lib.h"

int32_t harbor_start(scheduler_ctx *scheduler, name_t tname, name_t ssl,
    const char *host, uint16_t port, const char *key, int32_t ms);
void *harbor_pack(name_t task, int32_t call, uint8_t reqtype, const char *key, void *data, size_t size, size_t *lens);

#endif//HARBOR_H_
