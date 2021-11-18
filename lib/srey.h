#ifndef SREY_H_
#define SREY_H_

#include "netev/netev.h"

struct srey_ctx *srey_new();
void srey_free(struct srey_ctx *pctx);
void srey_loop(struct srey_ctx *pctx);

struct netev_ctx *srey_netev(struct srey_ctx *pctx);
struct tw_ctx *srey_tw(struct srey_ctx *pctx);


#endif//SREY_H_
