#include "utils/pool.h"

#define POOL_DEFAULT_CAP  1024
#define POOL_NELFREE      128 // 安全池收缩时单批出队上限

void _pool_qu_nelfree(pool_ctx *pool, uint32_t nfree) {
    void **elem;
    if (!BIT_CHECK(pool->flags, POOL_THSAFE)) {
        for (uint32_t i = 0; i < nfree; i++) {
            elem = (void **)pptr_que_pop(&pool->qu.normal_qu);
            if (NULL == elem) {
                break;
            }
            _pool_elfree(pool, *elem);
        }
        return;
    }
    uint32_t i, n, npop, remain = nfree;
    void *elems[POOL_NELFREE];
    while (remain > 0 && _pool_qu_size(pool) > pool->nkeep) {
        npop = remain > POOL_NELFREE ? POOL_NELFREE : remain;
        n = pfsq_pop_batch(&pool->qu.safe_qu, elems, npop);
        for (i = 0; i < n; i++) {
            _pool_elfree(pool, elems[i]);
        }
        if (n < npop) {
            break;
        }
        remain -= n;
    }
}
void pool_init(pool_ctx *pool, size_t elsize, uint32_t capacity,
               uint32_t nkeep, int32_t flags, pool_cbs *elcbs) {
    ZERO(pool, sizeof(pool_ctx));
    capacity = (0 == capacity ? POOL_DEFAULT_CAP : capacity);
    pool->elsize = (uint32_t)elsize;
    pool->nkeep = nkeep;
    pool->flags = flags;
    load_trend_init(&pool->trend);
    if (NULL != elcbs) {
        pool->elcbs = *elcbs;
    }
    if (BIT_CHECK(pool->flags, POOL_THSAFE)) {
        pfsq_init(&pool->qu.safe_qu, capacity);
    } else {
        pptr_que_init(&pool->qu.normal_qu, capacity);
    }
}
void pool_free(pool_ctx *pool) {
    void *data = NULL;
    void *elems[POOL_NELFREE];
    uint32_t i, n;
    if (BIT_CHECK(pool->flags, POOL_THSAFE)) {
        while ((n = pfsq_pop_batch(&pool->qu.safe_qu, elems, POOL_NELFREE)) > 0) {
            for (i = 0; i < n; i++) {
                _pool_elfree(pool, elems[i]);
            }
        }
        pfsq_free(&pool->qu.safe_qu);
        return;
    }
    while (ERR_OK == _pool_qu_pop(pool, &data)) {
        _pool_elfree(pool, data);
    }
    pptr_que_free(&pool->qu.normal_qu);
}
