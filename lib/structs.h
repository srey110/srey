#ifndef STRUCTS_H_
#define STRUCTS_H_

#include "macro.h"

struct ud_ctx
{
    uint32_t session;
    uintptr_t handle;
    uint64_t id;
};
struct message_ctx
{
    uint32_t flags;
    uint32_t session;
    uint32_t size;
    uint64_t id;
    void *data;
};

#endif//STRUCTS_H_
