#ifndef STRUCTS_H_
#define STRUCTS_H_

#include "macro.h"

struct ud_ctx
{
    uint32_t session;
    sid_t id;
    uintptr_t handle;
};
struct message_ctx
{
    uint32_t flags;
    uint32_t session;
    uint32_t size;
    sid_t id;
    void *data;
};

#endif//STRUCTS_H_
