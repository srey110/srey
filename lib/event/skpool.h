#ifndef SOCK_POOL_H_
#define SOCK_POOL_H_

#include "queue.h"
#include "mutex.h"

QUEUE_DECL(struct sock_ctx *, qu_sock);
typedef struct skpool_ctx
{
    qu_sock pool;
    mutex_ctx lck;
}skpool_ctx;

struct sock_ctx * _new_sk(SOCKET fd, struct cbs_ctx *cbs, struct ud_cxt *ud);
void _free_sk(struct sock_ctx *skctx);
void _clear_sk(struct sock_ctx *skctx);
void _reset_sk(struct sock_ctx *skctx, SOCKET fd, struct cbs_ctx *cbs, struct ud_cxt *ud);
static inline void pool_init(skpool_ctx *pool, uint32_t cnt)
{
    mutex_init(&pool->lck);
    qu_sock_init(&pool->pool, cnt);
};
static inline void pool_free(skpool_ctx *pool)
{
    struct sock_ctx **sk;
    while (NULL != (sk = qu_sock_pop(&pool->pool)))
    {
        _free_sk(*sk);
    }
    qu_sock_free(&pool->pool);
    mutex_free(&pool->lck);
};
static inline void pool_push(skpool_ctx *pool, struct sock_ctx *skctx)
{
    _clear_sk(skctx);
    mutex_lock(&pool->lck);
    qu_sock_push(&pool->pool, &skctx);
    mutex_unlock(&pool->lck);
};
static inline struct sock_ctx *pool_pop(skpool_ctx *pool, SOCKET fd, struct cbs_ctx *cbs, struct ud_cxt *ud)
{
    struct sock_ctx *sk = NULL;
    mutex_lock(&pool->lck);
    struct sock_ctx **tmp = qu_sock_pop(&pool->pool);
    if (NULL != tmp)
    {
        sk = *tmp;
    }
    mutex_unlock(&pool->lck);
    if (NULL == sk)
    {
        sk = _new_sk(fd, cbs, ud);
    }
    else
    {
        _reset_sk(sk, fd, cbs, ud);
    }
    return sk;
};

#endif//SOCK_POOL_H_
