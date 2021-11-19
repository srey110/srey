#ifndef STRUCTS_H_
#define STRUCTS_H_

#include "macro.h"

struct ud_ctx
{
    uint32_t id;
    uintptr_t handle;
};
struct message_ctx
{
    uint32_t flags;
    int32_t idata;
    void *pdata;
    size_t uldata;
};

#endif//STRUCTS_H_
