#include "utils/pool.h"

#define POOL_NELFREE 128
#define POOL_DEFAULT_CAP  1024

static int32_t _pool_safe_trypush(void *qu, const void *data) {
    return fsqu_trypush((fsqu_ctx *)qu, data);
}
static int32_t _pool_safe_pop(void *qu, void *out) {
    return fsqu_pop((fsqu_ctx *)qu, out);
}
static void _pool_safe_qufree(void *qu) {
    fsqu_free((fsqu_ctx *)qu);
}
static void _pool_safe_nelfree(pool_ctx *pool, uint32_t nfree) {
    uint32_t i, n, npop, remain = nfree;
    void *elems[POOL_NELFREE];
    while (remain > 0 && pool_size(pool) > pool->nkeep) {
        npop = remain > POOL_NELFREE ? POOL_NELFREE : remain;
        n = fsqu_pop_batch((fsqu_ctx *)pool->cur_qu, elems, npop);
        for (i = 0; i < n; i++) {
            _pool_elfree(pool, elems[i]);
        }
        if (n < npop) {
            break;
        }
        remain -= n;
    }
}
static uint32_t _pool_safe_size(void *qu) {
    return fsqu_size((fsqu_ctx *)qu);
}
static uint32_t _pool_safe_capacity(void *qu) {
    return fsqu_capacity((fsqu_ctx *)qu);
}
static int32_t _pool_normal_trypush(void *qu, const void *data) {
    queue_ctx *q = (queue_ctx *)qu;
    if (q->size >= q->maxsize) {
        return ERR_FAILED;
    }
    queue_push(q, data);
    return ERR_OK;
}
static int32_t _pool_normal_pop(void *qu, void *out) {
    queue_ctx *q = (queue_ctx *)qu;
    void **elem = queue_pop(q);
    if (NULL == elem) {
        return ERR_FAILED;
    }
    *(void **)out = *elem;
    return ERR_OK;
}
static void _pool_normal_qufree(void *qu) {
    queue_free((queue_ctx *)qu);
}
static void _pool_normal_nelfree(pool_ctx *pool, uint32_t nfree) {
    void **elem;
    for (uint32_t i = 0; i < nfree; i++) {
        elem = queue_pop((queue_ctx *)pool->cur_qu);
        if (NULL == elem) {
            break;
        }
        _pool_elfree(pool, *elem);
    }
}
static uint32_t _pool_normal_size(void *qu) {
    return queue_size((queue_ctx *)qu);
}
static uint32_t _pool_normal_capacity(void *qu) {
    return queue_maxsize((queue_ctx *)qu);
}
void pool_init(pool_ctx *pool, size_t elsize, uint32_t capacity,
               uint32_t nkeep, int32_t thsafe, el_cbs *elcbs) {
    ZERO(pool, sizeof(pool_ctx));
    capacity = (0 == capacity ? POOL_DEFAULT_CAP : capacity);
    pool->elsize = (uint32_t)elsize;
    pool->nkeep = nkeep;
    load_trend_init(&pool->trend);
    if (NULL != elcbs) {
        pool->elcbs = *elcbs;
    }
    if (thsafe) {
        pool->cur_qu = &pool->qu.safe_qu;
        pool->_qu_trypush = _pool_safe_trypush;
        pool->_qu_pop = _pool_safe_pop;
        pool->_qu_free = _pool_safe_qufree;
        pool->_qu_nelfree = _pool_safe_nelfree;
        pool->_qu_size = _pool_safe_size;
        pool->_qu_capacity = _pool_safe_capacity;
        fsqu_init(&pool->qu.safe_qu, sizeof(void *), capacity);
    } else {
        pool->cur_qu = &pool->qu.normal_qu;
        pool->_qu_trypush = _pool_normal_trypush;
        pool->_qu_pop = _pool_normal_pop;
        pool->_qu_free = _pool_normal_qufree;
        pool->_qu_nelfree = _pool_normal_nelfree;
        pool->_qu_size = _pool_normal_size;
        pool->_qu_capacity = _pool_normal_capacity;
        queue_init(&pool->qu.normal_qu, sizeof(void *), capacity);
    }
}
void pool_free(pool_ctx *pool) {
    void *data = NULL;
    while (ERR_OK == pool->_qu_pop(pool->cur_qu, &data)) {
        _pool_elfree(pool, data);
    }
    pool->_qu_free(pool->cur_qu);
}
