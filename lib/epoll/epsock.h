#ifndef EPSOCKET_H_
#define EPSOCKET_H_

#include "evtype.h"

#if defined(OS_LINUX)
typedef struct netev_ctx
{
    int32_t thcnt;
    int *epoll;
    struct thread_ctx *thepoll;
}netev_ctx;
#endif // OS_LINUX
#endif//EPSOCKET_H_
