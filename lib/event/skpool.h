#ifndef SOCK_POOL_H_
#define SOCK_POOL_H_

#include "queue.h"

QUEUE_DECL(struct sock_ctx *, qu_sock);
typedef struct skpool_ctx
{
    qu_sock pool;
}skpool_ctx;

struct sock_ctx * _new_sk(SOCKET fd, struct cbs_ctx *cbs, struct ud_cxt *ud);
void _free_sk(struct sock_ctx *skctx);
void _clear_sk(struct sock_ctx *skctx);
void _reset_sk(struct sock_ctx *skctx, SOCKET fd, struct cbs_ctx *cbs, struct ud_cxt *ud);
static inline void pool_init(skpool_ctx *pool, uint32_t cnt)
{
    qu_sock_init(&pool->pool, cnt);
};
static inline void _pool_nfree(skpool_ctx *pool, size_t cnt)
{
    struct sock_ctx **sk;
    for (size_t i = 0; i < cnt; i++)
    {
        sk = qu_sock_pop(&pool->pool);
        _free_sk(*sk);
    }
};
static inline void pool_free(skpool_ctx *pool)
{
    _pool_nfree(pool, qu_sock_size(&pool->pool));
    qu_sock_free(&pool->pool);
};
static inline void pool_push(skpool_ctx *pool, struct sock_ctx *skctx)
{
    _clear_sk(skctx);
    qu_sock_push(&pool->pool, &skctx);
};
static inline struct sock_ctx *pool_pop(skpool_ctx *pool, SOCKET fd, struct cbs_ctx *cbs, struct ud_cxt *ud)
{
    struct sock_ctx **tmp = qu_sock_pop(&pool->pool);
    if (NULL != tmp)
    {
        _reset_sk(*tmp, fd, cbs, ud);
        return *tmp;
    }
    return _new_sk(fd, cbs, ud);
};
static inline void pool_shrink(skpool_ctx *pool, size_t keep)
{
    size_t plsize = qu_sock_size(&pool->pool);
    if (plsize > keep)
    {
        _pool_nfree(pool, plsize - keep);
    }
};

#endif//SOCK_POOL_H_
