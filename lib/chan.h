#ifndef CHAN_H_
#define CHAN_H_

#include "queue.h"
#include "mutex.h"
#include "cond.h"

typedef struct chan_ctx
{
    int32_t closed;
    int32_t rwaiting;
    int32_t wwaiting;
    mutex_ctx mmutex;//¶ÁÐ´ÐÅºÅËø
    cond_ctx rcond;
    cond_ctx wcond;
    qu_void queue;
}chan_ctx;

void chan_init(chan_ctx *ctx, const size_t maxsize);
void chan_free(chan_ctx *ctx);
void chan_close(chan_ctx *ctx);
int32_t chan_send(chan_ctx *ctx, void *data);
int32_t chan_trysend(chan_ctx *ctx, void *data);
int32_t chan_recv(chan_ctx *ctx, void **data);
int32_t chan_tryrecv(chan_ctx *ctx, void **data);
size_t chan_size(chan_ctx *ctx);
int32_t chan_closed(chan_ctx *ctx);

#endif//CHAN_H_
