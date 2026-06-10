#ifndef FSQU_H_
#define FSQU_H_

#include "containers/mpq.h"
#include "containers/queue.h"
#include "thread/spinlock.h"

typedef struct fsqu_ctx {
#if FSQU_MPQ
    mpq_ctx qu;
#else
    queue_ctx qu;
    spin_ctx lck;
#endif
}fsqu_ctx;

/// <summary>
/// 初始化平台自适应队列,由FSQU_MPQ宏控制
/// </summary>
/// <param name="fsqu">fsqu_ctx</param>
/// <param name="elsize">单元素字节数，须 大于 0</param>
/// <param name="capacity">期望容量，0 使用默认值（mpq 侧非 2 的幂自动向上取整）</param>
void fsqu_init(fsqu_ctx *fsqu, size_t elsize, uint32_t capacity);
/// <summary>
/// 释放队列内部内存，不释放 fsqu 本身
/// </summary>
/// <param name="fsqu">fsqu_ctx</param>
void fsqu_free(fsqu_ctx *fsqu);
/// <summary>
/// 非阻塞入队（多生产者安全）：队列满时立即返回 ERR_FAILED，不阻塞也不扩容
/// </summary>
/// <param name="fsqu">fsqu_ctx</param>
/// <param name="data">指向待入队元素的指针，拷贝 elsize 字节</param>
/// <returns>ERR_OK 成功，ERR_FAILED 队列已满</returns>
static inline int32_t fsqu_trypush(fsqu_ctx *fsqu, const void *data) {
#if FSQU_MPQ
    return mpq_trypush(&fsqu->qu, data);
#else
    spin_lock(&fsqu->lck);
    if (fsqu->qu.size >= fsqu->qu.maxsize) {
        spin_unlock(&fsqu->lck);
        return ERR_FAILED;
    }
    queue_push(&fsqu->qu, data);
    spin_unlock(&fsqu->lck);
    return ERR_OK;
#endif
}
/// <summary>
/// 入队单个元素（多生产者安全）；mpq 侧队列满时自旋阻塞，queue 侧自动扩容
/// </summary>
/// <param name="fsqu">fsqu_ctx</param>
/// <param name="data">指向待入队元素的指针，拷贝 elsize 字节</param>
static inline void fsqu_push(fsqu_ctx *fsqu, const void *data) {
#if FSQU_MPQ
    mpq_push(&fsqu->qu, data);
#else
    spin_lock(&fsqu->lck);
    queue_push(&fsqu->qu, data);
    spin_unlock(&fsqu->lck);
#endif
}
/// <summary>
/// 批量入队（多生产者安全）；queue 侧仅一次加锁循环入队，mpq 侧逐个入队（满则自旋阻塞）
/// </summary>
/// <param name="fsqu">fsqu_ctx</param>
/// <param name="data">指向连续元素数组的指针，拷贝 count * elsize 字节</param>
/// <param name="count">入队元素个数</param>
static inline void fsqu_push_batch(fsqu_ctx *fsqu, const void *data, uint32_t count) {
    uint32_t i;
    const char *src = (const char *)data;
#if FSQU_MPQ
    for (i = 0; i < count; i++) {
        mpq_push(&fsqu->qu, src);
        src += fsqu->qu.elsize;
    }
#else
    spin_lock(&fsqu->lck);
    for (i = 0; i < count; i++) {
        queue_push(&fsqu->qu, src);
        src += fsqu->qu.elsize;
    }
    spin_unlock(&fsqu->lck);
#endif
}
/// <summary>
/// 出队单个元素（多消费者安全），拷贝 elsize 字节到 out
/// </summary>
/// <param name="fsqu">fsqu_ctx</param>
/// <param name="out">出参：接收出队元素的缓冲（至少 elsize 字节），仅 ERR_OK 时有效</param>
/// <returns>ERR_OK 成功，ERR_FAILED 队列为空</returns>
static inline int32_t fsqu_pop(fsqu_ctx *fsqu, void *out) {
#if FSQU_MPQ
    return mpq_pop(&fsqu->qu, out);
#else
    spin_lock(&fsqu->lck);
    void *elem = queue_pop(&fsqu->qu);
    if (NULL == elem) {
        spin_unlock(&fsqu->lck);
        return ERR_FAILED;
    }
    memcpy(out, elem, fsqu->qu.elsize);
    spin_unlock(&fsqu->lck);
    return ERR_OK;
#endif
}
/// <summary>
/// 批量出队（多消费者安全），最多取 max 个；queue 侧仅一次加锁循环出队
/// </summary>
/// <param name="fsqu">fsqu_ctx</param>
/// <param name="out">出参：接收出队元素的数组，至少 max * elsize 字节</param>
/// <param name="max">最多出队个数</param>
/// <returns>实际出队个数，0 到 max</returns>
static inline uint32_t fsqu_pop_batch(fsqu_ctx *fsqu, void *out, uint32_t max) {
    uint32_t n = 0;
    char *dst = (char *)out;
#if FSQU_MPQ
    while (n < max && ERR_OK == mpq_pop(&fsqu->qu, dst)) {
        dst += fsqu->qu.elsize;
        n++;
    }
    return n;
#else
    void *elem;
    spin_lock(&fsqu->lck);
    while (n < max && NULL != (elem = queue_pop(&fsqu->qu))) {
        memcpy(dst, elem, fsqu->qu.elsize);
        dst += fsqu->qu.elsize;
        n++;
    }
    spin_unlock(&fsqu->lck);
    return n;
#endif
}
/// <summary>
/// 出队单个元素（单消费者），mpq 侧免 CAS 更快；要求仅单一消费者线程调用
/// </summary>
/// <param name="fsqu">fsqu_ctx</param>
/// <param name="out">出参：接收出队元素的缓冲（至少 elsize 字节），仅 ERR_OK 时有效</param>
/// <returns>ERR_OK 成功，ERR_FAILED 队列为空</returns>
static inline int32_t fsqu_pop_sc(fsqu_ctx *fsqu, void *out) {
#if FSQU_MPQ
    return mpq_pop_sc(&fsqu->qu, out);
#else
    return fsqu_pop(fsqu, out);
#endif
}
/// <summary>
/// 批量出队（单消费者），最多取 max 个，mpq 侧免 CAS；要求仅单一消费者线程调用
/// </summary>
/// <param name="fsqu">fsqu_ctx</param>
/// <param name="out">出参：接收出队元素的数组，至少 max * elsize 字节</param>
/// <param name="max">最多出队个数</param>
/// <returns>实际出队个数，0 到 max</returns>
static inline uint32_t fsqu_pop_sc_batch(fsqu_ctx *fsqu, void *out, uint32_t max) {
#if FSQU_MPQ
    uint32_t n = 0;
    char *dst = (char *)out;
    while (n < max && ERR_OK == mpq_pop_sc(&fsqu->qu, dst)) {
        dst += fsqu->qu.elsize;
        n++;
    }
    return n;
#else
    return fsqu_pop_batch(fsqu, out, max);
#endif
}
/// <summary>
/// 返回当前队列元素数量；并发下为近似值
/// </summary>
/// <param name="fsqu">fsqu_ctx</param>
/// <returns>元素数量</returns>
static inline uint32_t fsqu_size(fsqu_ctx *fsqu) {
#if FSQU_MPQ
    return mpq_size(&fsqu->qu);
#else
    spin_lock(&fsqu->lck);
    uint32_t n = queue_size(&fsqu->qu);
    spin_unlock(&fsqu->lck);
    return n;
#endif
}
/// <summary>
/// 返回队列容量；mpq 侧为固定容量，queue 侧为当前已分配容量
/// </summary>
/// <param name="fsqu">fsqu_ctx</param>
/// <returns>容量</returns>
static inline uint32_t fsqu_capacity(fsqu_ctx *fsqu) {
#if FSQU_MPQ
    return mpq_capacity(&fsqu->qu);
#else
    spin_lock(&fsqu->lck);
    uint32_t cap = fsqu->qu.maxsize;
    spin_unlock(&fsqu->lck);
    return cap;
#endif
}

#endif//FSQU_H_
