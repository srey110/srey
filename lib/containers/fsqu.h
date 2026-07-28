#ifndef FSQU_H_
#define FSQU_H_

#include "containers/mpq.h"
#include "containers/queue.h"
#include "thread/spinlock.h"

typedef struct fsqu_ctx {
#if FSQU_MPQ
    atomic_t novf;// 溢出层元素数镜像，锁外无锁读（push 粘滞判定 / pop、size 快路径）
#endif
    spin_ctx lck;// 保护 qu
    queue_ctx qu;// MPQ=0：主队列；MPQ=1：mpq 满时的溢出层
#if FSQU_MPQ
    mpq_ctx mpq;// 无锁有界环，满则降级到 qu
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
/// 非阻塞入队（多生产者安全）：队列满时立即返回 ERR_FAILED，不阻塞、不扩容、不落溢出层。
/// 同一实例上若 fsqu_push 曾落过溢出层且尚未排空，本函数同样返回 ERR_FAILED——
/// 否则新元素会经快路径插到更早的溢出元素之前，破坏 FIFO
/// </summary>
/// <param name="fsqu">fsqu_ctx</param>
/// <param name="data">指向待入队元素的指针，拷贝 elsize 字节</param>
/// <returns>ERR_OK 成功；ERR_FAILED 快路径已满，或溢出层非空(见上)</returns>
static inline int32_t fsqu_trypush(fsqu_ctx *fsqu, const void *data) {
#if FSQU_MPQ
    // 与 fsqu_push 同守粘滞规则：溢出层非空期间一律拒绝，免新元素排到更早的溢出元素之前。
    if (0 != ATOMIC_GET(&fsqu->novf)) {
        return ERR_FAILED;
    }
    return mpq_trypush(&fsqu->mpq, data);
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
/// 入队单个元素（多生产者安全），永不阻塞、永不失败：
/// mpq 侧满时降级到溢出层（加锁、无界），queue 侧自动扩容。
/// 溢出层只增不减——queue_push 倍增后无收缩路径，峰值容量保留到 fsqu_free；
/// 这是为消除自投递死锁（消费者自身即生产者时阻塞入队即硬死锁）有意接受的取舍，不是疏漏
/// </summary>
/// <param name="fsqu">fsqu_ctx</param>
/// <param name="data">指向待入队元素的指针，拷贝 elsize 字节</param>
static inline void fsqu_push(fsqu_ctx *fsqu, const void *data) {
#if FSQU_MPQ
    // 粘滞降级：溢出层非空期间不得再走 mpq，否则新元素会插到更早的溢出元素之前
    if (0 == ATOMIC_GET(&fsqu->novf)
        && ERR_OK == mpq_trypush(&fsqu->mpq, data)) {
        return;
    }
    spin_lock(&fsqu->lck);
    queue_push(&fsqu->qu, data);
    ATOMIC_ADD(&fsqu->novf, 1);
    spin_unlock(&fsqu->lck);
#else
    spin_lock(&fsqu->lck);
    queue_push(&fsqu->qu, data);
    spin_unlock(&fsqu->lck);
#endif
}
/// <summary>
/// 批量入队（多生产者安全），永不阻塞、永不失败；queue 侧仅一次加锁循环入队，
/// mpq 侧先尽量走快路径，首个入不下的起整批余量一次加锁落溢出层
/// </summary>
/// <param name="fsqu">fsqu_ctx</param>
/// <param name="data">指向连续元素数组的指针，拷贝 count * elsize 字节</param>
/// <param name="count">入队元素个数</param>
static inline void fsqu_push_batch(fsqu_ctx *fsqu, const void *data, uint32_t count) {
    uint32_t i = 0;
    const char *src = (const char *)data;
#if FSQU_MPQ
    uint32_t elsize = fsqu->mpq.elsize;
    uint32_t nleft;
    // 粘滞降级：溢出层非空时整批直落溢出，不与更早的溢出元素交错
    if (0 == ATOMIC_GET(&fsqu->novf)) {
        while (i < count
               && ERR_OK == mpq_trypush(&fsqu->mpq, src)) {
            src += elsize;
            i++;
        }
    }
    if (i >= count) {
        return;
    }
    nleft = count - i;
    spin_lock(&fsqu->lck);
    for (; i < count; i++) {
        queue_push(&fsqu->qu, src);
        src += elsize;
    }
    ATOMIC_ADD(&fsqu->novf, nleft);
    spin_unlock(&fsqu->lck);
#else
    spin_lock(&fsqu->lck);
    for (i = 0; i < count; i++) {
        queue_push(&fsqu->qu, src);
        src += fsqu->qu.elsize;
    }
    spin_unlock(&fsqu->lck);
#endif
}
#if FSQU_MPQ
// 从溢出层取一个元素（快路径已空时调用）：novf 为 0 即判空免锁
static inline int32_t _fsqu_ovf_pop(fsqu_ctx *fsqu, void *out) {
    if (0 == ATOMIC_GET(&fsqu->novf)) {
        return ERR_FAILED;
    }
    spin_lock(&fsqu->lck);
    void *elem = queue_pop(&fsqu->qu);
    if (NULL == elem) {
        spin_unlock(&fsqu->lck);
        return ERR_FAILED;// novf 为过期非 0，另一消费者已取走
    }
    memcpy(out, elem, fsqu->qu.elsize);// queue_pop 的指针仅在下次 push 前有效，须锁内拷出
    ATOMIC_ADD(&fsqu->novf, (atomic_t)-1);
    spin_unlock(&fsqu->lck);
    return ERR_OK;
}
// 快路径取完后从溢出层续取补齐（否则调用方按 0 判空会漏掉溢出）：
// dst 为快路径填完后的写入位置，*n 传入已取个数、返回补齐后的个数
static inline void _fsqu_ovf_drain(fsqu_ctx *fsqu, char *dst, uint32_t max, uint32_t *n) {
    if (*n >= max
        || 0 == ATOMIC_GET(&fsqu->novf)) {
        return;
    }
    uint32_t elsize = fsqu->mpq.elsize;
    int32_t k = 0;
    void *elem;
    spin_lock(&fsqu->lck);
    while (*n < max
           && NULL != (elem = queue_pop(&fsqu->qu))) {
        memcpy(dst, elem, elsize);
        dst += elsize;
        (*n)++;
        k++;
    }
    if (0 != k) {
        // 批量一次扣减，省 k-1 次原子操作
        ATOMIC_ADD(&fsqu->novf, -k);
    }
    spin_unlock(&fsqu->lck);
}
#endif
/// <summary>
/// 出队单个元素（多消费者安全），拷贝 elsize 字节到 out
/// </summary>
/// <param name="fsqu">fsqu_ctx</param>
/// <param name="out">出参：接收出队元素的缓冲（至少 elsize 字节），仅 ERR_OK 时有效</param>
/// <returns>ERR_OK 成功，ERR_FAILED 队列为空</returns>
static inline int32_t fsqu_pop(fsqu_ctx *fsqu, void *out) {
#if FSQU_MPQ
    if (ERR_OK == mpq_pop(&fsqu->mpq, out)) {
        return ERR_OK;
    }
    return _fsqu_ovf_pop(fsqu, out);
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
    uint32_t elsize = fsqu->mpq.elsize;
    while (n < max && ERR_OK == mpq_pop(&fsqu->mpq, dst)) {
        dst += elsize;
        n++;
    }
    _fsqu_ovf_drain(fsqu, dst, max, &n);
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
    if (ERR_OK == mpq_pop_sc(&fsqu->mpq, out)) {
        return ERR_OK;
    }
    return _fsqu_ovf_pop(fsqu, out);
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
    uint32_t elsize = fsqu->mpq.elsize;
    char *dst = (char *)out;
    while (n < max && ERR_OK == mpq_pop_sc(&fsqu->mpq, dst)) {
        dst += elsize;
        n++;
    }
    _fsqu_ovf_drain(fsqu, dst, max, &n);
    return n;
#else
    return fsqu_pop_batch(fsqu, out, max);
#endif
}
/// <summary>
/// 返回当前队列元素数量（含溢出层）；并发下为近似值
/// </summary>
/// <param name="fsqu">fsqu_ctx</param>
/// <returns>元素数量</returns>
static inline uint32_t fsqu_size(fsqu_ctx *fsqu) {
#if FSQU_MPQ
    // 须含溢出层：调用方以此判空决定重调度/休眠，漏算会让溢出元素滞留
    return mpq_size(&fsqu->mpq) + (uint32_t)ATOMIC_GET(&fsqu->novf);
#else
    spin_lock(&fsqu->lck);
    uint32_t n = queue_size(&fsqu->qu);
    spin_unlock(&fsqu->lck);
    return n;
#endif
}
/// <summary>
/// 返回队列容量；mpq 侧为快路径固定容量（即降级到溢出层的阈值，不含无界的溢出层），
/// queue 侧为当前已分配容量。调用方据此推导过载告警阈值
/// </summary>
/// <param name="fsqu">fsqu_ctx</param>
/// <returns>容量</returns>
static inline uint32_t fsqu_capacity(fsqu_ctx *fsqu) {
#if FSQU_MPQ
    return mpq_capacity(&fsqu->mpq);
#else
    spin_lock(&fsqu->lck);
    uint32_t cap = fsqu->qu.maxsize;
    spin_unlock(&fsqu->lck);
    return cap;
#endif
}

#endif//FSQU_H_
