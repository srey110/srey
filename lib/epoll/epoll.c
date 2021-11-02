#include "netapi.h"

#if defined(OS_LINUX)

typedef struct cmd_ctx
{
    SOCKET sock;
}cmd_ctx;

struct netev_ctx *netev_new()
{

}
void netev_free(struct netev_ctx *pctx)
{

}
void netev_loop(struct netev_ctx *pctx)
{

}

#endif // OS_LINUX
