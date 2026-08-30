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
/// 初始化平台自适应队列,由 FSQU_MPQ 宏控制使用那种方式
/// </summary>
/// <param name="fsqu">fsqu_ctx</param>
/// <param name="elsize">单元素字节数，须 大于 0</param>
/// <param name="capacity">期望容量，0 使用默认值。实际容量会向上取整：mpq 侧取到 2 的幂，
///   queue 侧取到偶数，所以 fsqu_capacity 可能大于这里给的值</param>
void fsqu_init(fsqu_ctx *fsqu, size_t elsize, uint32_t capacity);
/// <summary>
/// 释放队列内部内存，不释放 fsqu 本身
/// </summary>
/// <param name="fsqu">fsqu_ctx</param>
void fsqu_free(fsqu_ctx *fsqu);
/// <summary>
/// 非阻塞入队（多生产者）：队列满时不阻塞、不扩容、不落溢出层。同一实例上 fsqu_push 曾落过
/// 溢出层且尚未排空时同样失败——快路径空着也拒。这道守卫是 best-effort：读 novf 与随后的
/// 入队不是一个原子步，跨生产者顺序本就不保证
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
    int32_t rtn = queue_trypush(&fsqu->qu, data);
    spin_unlock(&fsqu->lck);
    return rtn;
#endif
}
/// <summary>
/// 入队单个元素（多生产者），永不阻塞、永不失败：mpq 侧满时降级到无界溢出层，该层只增不减，
/// 峰值容量保留到 fsqu_free。这是为消除自投递死锁有意接受的取舍，不是疏漏
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
    // 置位排在 queue_push 之前:生产者侧是免锁读 novf,排在后面会留出"元素已进溢出层、
    // novf 仍为 0"的窗口。收窄不等于消除,故上面的守卫按 best-effort 声明
    ATOMIC_ADD(&fsqu->novf, 1);
    queue_push(&fsqu->qu, data);
    spin_unlock(&fsqu->lck);
#else
    spin_lock(&fsqu->lck);
    queue_push(&fsqu->qu, data);
    spin_unlock(&fsqu->lck);
#endif
}
/// <summary>
/// 批量入队（多生产者），永不阻塞、永不失败
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
    ATOMIC_ADD(&fsqu->novf, nleft);// 同 fsqu_push:先置位再入队,收窄免锁读到 0 的窗口
    for (; i < count; i++) {
        queue_push(&fsqu->qu, src);
        src += elsize;
    }
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
// 快路径取完后从溢出层续取补齐
static inline void _fsqu_ovf_drain(fsqu_ctx *fsqu, char *dst, uint32_t max, uint32_t *n, int32_t mpqrtn) {
    if (*n >= max
        || 1 == mpqrtn
        || 0 == ATOMIC_GET(&fsqu->novf)) {
        return;
    }
    uint32_t elsize = fsqu->mpq.elsize;
    int32_t k = 0;
    void *elem;
    spin_lock(&fsqu->lck);
    while (*n < max
           && NULL != (elem = queue_pop(&fsqu->qu))) {
        memcpy(dst, elem, elsize);// queue_pop 的指针仅在下次 push 前有效，须锁内拷出
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
// 从溢出层取一个元素；取不到就把 mpq 的三态原样回传(ERR_FAILED 真空 / 1 有在途)
static inline int32_t _fsqu_ovf_pop(fsqu_ctx *fsqu, void *out, int32_t mpqrtn) {
    uint32_t n = 0;
    _fsqu_ovf_drain(fsqu, (char *)out, 1, &n, mpqrtn);
    return (0 != n) ? ERR_OK : mpqrtn;
}
// 批量出队的 MPQ 实现，fsqu_pop_batch / fsqu_pop_sc_batch 共用；sc 传字面量，分支被常量折叠。
// rtn 在每个出口都有确定值：取满 max 退出时是最后一次成功的 ERR_OK（drain 由 *n >= max 早退），
// max 为 0 时是这里的初值 —— _fsqu_ovf_drain 靠它决定要不要去溢出层续取
static inline uint32_t _fsqu_pop_batch_mpq(fsqu_ctx *fsqu, void *out, uint32_t max, int32_t sc) {
    uint32_t n = 0;
    uint32_t elsize = fsqu->mpq.elsize;
    char *dst = (char *)out;
    int32_t rtn = ERR_FAILED;
    while (n < max
           && ERR_OK == (rtn = (sc ? mpq_pop_sc(&fsqu->mpq, dst) : mpq_pop(&fsqu->mpq, dst)))) {
        dst += elsize;
        n++;
    }
    _fsqu_ovf_drain(fsqu, dst, max, &n, rtn);
    return n;
}
#endif
/// <summary>
/// 出队单个元素（多消费者）
/// </summary>
/// <param name="fsqu">fsqu_ctx</param>
/// <param name="out">出参：接收出队元素的缓冲（至少 elsize 字节），仅 ERR_OK 时有效</param>
/// <returns>三态，语义同 mpq_pop：ERR_OK 成功；ERR_FAILED 确实为空；1 有元素已被生产者
/// 抢占、尚未发布(queue+spin 后端不产生这一态)。只判 ERR_OK 的调用方当空处理即可；
/// 靠 fsqu_size 决定睡不睡的调用方必须区分 1 与 ERR_FAILED，退避用 spin_backoff</returns>
static inline int32_t fsqu_pop(fsqu_ctx *fsqu, void *out) {
#if FSQU_MPQ
    int32_t rtn = mpq_pop(&fsqu->mpq, out);
    if (ERR_OK == rtn) {
        return ERR_OK;
    }
    return _fsqu_ovf_pop(fsqu, out, rtn);
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
/// 批量出队（多消费者）
/// </summary>
/// <param name="fsqu">fsqu_ctx</param>
/// <param name="out">出参：接收出队元素的数组，至少 max * elsize 字节</param>
/// <param name="max">最多出队个数</param>
/// <returns>实际出队个数，0 到 max</returns>
static inline uint32_t fsqu_pop_batch(fsqu_ctx *fsqu, void *out, uint32_t max) {
#if FSQU_MPQ
    return _fsqu_pop_batch_mpq(fsqu, out, max, 0);
#else
    uint32_t n = 0;
    char *dst = (char *)out;
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
/// 出队单个元素（单消费者）：仅允许单一消费者线程调用，且不可与 fsqu_pop 混用(同 mpq_pop_sc)
/// </summary>
/// <param name="fsqu">fsqu_ctx</param>
/// <param name="out">出参：接收出队元素的缓冲（至少 elsize 字节），仅 ERR_OK 时有效</param>
/// <returns>三态，同 fsqu_pop</returns>
static inline int32_t fsqu_pop_sc(fsqu_ctx *fsqu, void *out) {
#if FSQU_MPQ
    int32_t rtn = mpq_pop_sc(&fsqu->mpq, out);
    if (ERR_OK == rtn) {
        return ERR_OK;
    }
    return _fsqu_ovf_pop(fsqu, out, rtn);
#else
    return fsqu_pop(fsqu, out);
#endif
}
/// <summary>
/// 批量出队（单消费者）：约束同 fsqu_pop_sc
/// </summary>
/// <param name="fsqu">fsqu_ctx</param>
/// <param name="out">出参：接收出队元素的数组，至少 max * elsize 字节</param>
/// <param name="max">最多出队个数</param>
/// <returns>实际出队个数，0 到 max</returns>
static inline uint32_t fsqu_pop_sc_batch(fsqu_ctx *fsqu, void *out, uint32_t max) {
#if FSQU_MPQ
    return _fsqu_pop_batch_mpq(fsqu, out, max, 1);
#else
    return fsqu_pop_batch(fsqu, out, max);
#endif
}
/// <summary>
/// 返回当前队列元素数量的近似值(含溢出层)：只会高估不会低估，不会把有元素报成 0
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
/// 返回队列容量；mpq 侧是快路径固定容量(降级到溢出层的阈值，不含无界的溢出层)，
/// queue 侧是当前已分配容量。调用方据此推导过载告警阈值
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
