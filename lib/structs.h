#ifndef STRUCTS_H_
#define STRUCTS_H_

#include "macro.h"

struct ud_ctx
{    
    uintptr_t handle;
    uint64_t id;
    uint64_t session;
};
struct message_ctx
{
    uint32_t flags;    
    uint32_t size;
    uint64_t id;
    uint64_t session;
    void *data;
};

#endif//STRUCTS_H_
