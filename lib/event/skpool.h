#ifndef SOCK_POOL_H_
#define SOCK_POOL_H_

#include "containers/queue.h"

// sock_ctx 对象池，用于复用已分配的TCP连接上下文，减少内存分配开销
typedef struct skpool_ctx {
    qu_ptr_ctx pool; // 对象池队列
}skpool_ctx;

// 分配并初始化一个新的sock_ctx
struct sock_ctx *_new_sk(SOCKET fd, struct cbs_ctx *cbs, struct ud_cxt *ud);
// 释放sock_ctx及其所有资源
void _free_sk(struct sock_ctx *skctx);
// 清空sock_ctx以便放回池中复用（不释放内存）
void _clear_sk(struct sock_ctx *skctx);
// 重置sock_ctx的fd/cbs/ud以便从池中取出复用
void _reset_sk(struct sock_ctx *skctx, SOCKET fd, struct cbs_ctx *cbs, struct ud_cxt *ud);
// 初始化对象池，预留cnt个槽位
static inline void pool_init(skpool_ctx *pool, uint32_t cnt) {
    qu_ptr_init(&pool->pool, cnt);
};
// 从池中释放cnt个sock_ctx对象
static inline void _pool_nfree(skpool_ctx *pool, size_t cnt) {
    struct sock_ctx **sk;
    for (size_t i = 0; i < cnt; i++) {
        sk = (struct sock_ctx **)qu_ptr_pop(&pool->pool);
        _free_sk(*sk);
    }
};
// 释放对象池及其中所有对象
static inline void pool_free(skpool_ctx *pool) {
    _pool_nfree(pool, qu_ptr_size(&pool->pool));
    qu_ptr_free(&pool->pool);
};
// 将sock_ctx归还到对象池（先清空再入队）
static inline void pool_push(skpool_ctx *pool, struct sock_ctx *skctx) {
    _clear_sk(skctx);
    qu_ptr_push(&pool->pool, (void **)&skctx);
};
// 从对象池取出sock_ctx（池空则新建）
static inline struct sock_ctx *pool_pop(skpool_ctx *pool, SOCKET fd, struct cbs_ctx *cbs, struct ud_cxt *ud) {
    struct sock_ctx **tmp = (struct sock_ctx **)qu_ptr_pop(&pool->pool);
    if (NULL != tmp) {
        _reset_sk(*tmp, fd, cbs, ud);
        return *tmp;
    }
    return _new_sk(fd, cbs, ud);
};
// 收缩对象池，保留keep个对象，多余的释放
static inline void pool_shrink(skpool_ctx *pool, size_t keep) {
    size_t plsize = qu_ptr_size(&pool->pool);
    if (plsize > keep) {
        _pool_nfree(pool, plsize - keep);
    }
};

#endif//SOCK_POOL_H_
