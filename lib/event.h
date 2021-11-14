#ifndef EVENT_H_
#define EVENT_H_

#include "netev/netev.h"

struct event_ctx *event_new();
void event_free(struct event_ctx *pctx);
void event_loop(struct event_ctx *pctx);

struct netev_ctx *event_netev(struct event_ctx *pctx);
struct tw_ctx *event_tw(struct event_ctx *pctx);


#endif//EVENT_H_
