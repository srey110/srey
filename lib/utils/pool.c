#include "utils/pool.h"

#define POOL_DEFAULT_CAP  1024

void _pool_qu_nelfree(pool_ctx *pool, uint32_t nfree) {
    void **elem;
    for (uint32_t i = 0; i < nfree; i++) {
        elem = (void **)pptr_que_pop(&pool->qu);
        if (NULL == elem) {
            break;
        }
        ASAN_UNPOISON(*elem, pool->elsize);
        _pool_elfree(pool, *elem);
    }
}
void pool_init(pool_ctx *pool, size_t elsize, uint32_t capacity,
               uint32_t nkeep, int32_t flags, pool_cbs *elcbs) {
    ASSERTAB(elsize <= UINT32_MAX, "pool elsize overflow.");
    ZERO(pool, sizeof(pool_ctx));
    capacity = (0 == capacity ? POOL_DEFAULT_CAP : capacity);
    pool->elsize = (uint32_t)elsize;
    pool->nkeep = nkeep;
    pool->flags = flags;
    load_trend_init(&pool->trend);
    if (NULL != elcbs) {
        pool->elcbs = *elcbs;
    }
    pptr_que_init(&pool->qu, capacity);
}
void pool_free(pool_ctx *pool) {
    void *data = NULL;
    while (ERR_OK == _pool_qu_pop(pool, &data)) {
        _pool_elfree(pool, data);
    }
    pptr_que_free(&pool->qu);
}
