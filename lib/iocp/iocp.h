#ifndef IOCP_H_
#define IOCP_H_

#include "overlap.h"

#if defined(OS_WIN)

void netev_init(struct netev_ctx *pctx);
void netev_free(struct netev_ctx *pctx);
void netev_loop(struct netev_ctx *pctx);

#endif // OS_WIN
#endif//IOCP_H_
