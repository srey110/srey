#ifndef MPMC_H_
#define MPMC_H_

#include "base/macro.h"

/*
 * 无锁多生产者多消费者有界队列 (MPMC Lock-Free Queue)
 *
 * 算法：Dmitry Vyukov 序列号 MPMC 算法
 *   - 每个槽位持有一个序列号，用于追踪槽位的读写代数
 *   - 入队：CAS 抢占 enq_pos，写入数据后发布 sequence = pos+1
 *   - 出队：CAS 抢占 deq_pos，读取数据后释放 sequence = pos+capacity
 *
 * 特性：
 *   - 真正无锁，所有操作均为 wait-free（有界容量下）
 *   - 容量固定，初始化后不扩容，满时 mpmc_trypush 返回 ERR_FAILED，mpmc_push 自旋等待
 *   - 容量必须为 2 的幂，若传入非幂次将自动向上对齐
 *   - enq_pos / deq_pos 各占独立缓存行，消除伪共享
 *
 * 用法示例：
 *   mpmc_ctx q;
 *   mpmc_init(&q, 1024);
 *   mpmc_push(&q, ptr);
 *   mpmc_trypush(&q, ptr);
 *   void *p = mpmc_pop(&q);
 *   mpmc_free(&q);
 */

 /* 单个槽位：序列号 + 数据指针 */
typedef struct mpmc_cell {
    atomic_t  sequence;
    void     *data;
} mpmc_cell;
/*
 * 用 union + char[64] 将原子计数器独占一条缓存行，防止伪共享。
 * 要求队列本身按 64 字节对齐时效果最佳：
 *   mpmc_ctx *q;
 *   posix_memalign((void**)&q, 64, sizeof(mpmc_ctx));
 */
typedef union {
    atomic_t v;
    char     _pad[64];
} mpmc_aln_t;
/* 无锁 MPMC 队列上下文 */
typedef struct mpmc_ctx {
    mpmc_cell  *cells;    /* 槽位数组 */
    uint32_t    capacity; /* 队列容量，必须为 2 的幂 */
    uint32_t    mask;     /* capacity - 1，用于快速取模 */
    mpmc_aln_t  enq;      /* 入队位置计数器，独占缓存行 */
    mpmc_aln_t  deq;      /* 出队位置计数器，独占缓存行 */
} mpmc_ctx;
/// <summary>
/// 初始化队列
/// </summary>
/// <param name="q">mpmc_ctx</param>
/// <param name="capacity">期望容量，0 则使用默认值，非 2 的幂自动向上取整</param>
void mpmc_init(mpmc_ctx *q, uint32_t capacity);
/// <summary>
/// 释放队列内部内存，不释放 q 本身
/// </summary>
/// <param name="q">mpmc_ctx</param>
void mpmc_free(mpmc_ctx *q);
/// <summary>
/// 阻塞入队：队列满时自旋等待直到成功
/// </summary>
/// <param name="q">mpmc_ctx</param>
/// <param name="data">数据指针，不得为 NULL</param>
void mpmc_push(mpmc_ctx *q, void *data);
/// <summary>
/// 非阻塞入队：队列满时立即返回 ERR_FAILED
/// </summary>
/// <param name="q">mpmc_ctx</param>
/// <param name="data">数据指针，不得为 NULL</param>
/// <returns>ERR_OK 成功，ERR_FAILED 队列已满</returns>
int32_t mpmc_trypush(mpmc_ctx *q, void *data);
/// <summary>
/// 出队，非阻塞
/// </summary>
/// <param name="q">mpmc_ctx</param>
/// <returns>数据指针，队列为空返回 NULL</returns>
void* mpmc_pop(mpmc_ctx *q);
/// <summary>
/// 返回当前队列元素数量的近似值，并发下不精确
/// </summary>
/// <param name="q">mpmc_ctx</param>
/// <returns>元素数量</returns>
uint32_t mpmc_size(mpmc_ctx *q);
/// <summary>
/// 返回队列最大容量
/// </summary>
/// <param name="q">mpmc_ctx</param>
/// <returns>最大容量</returns>
static inline uint32_t mpmc_capacity(const mpmc_ctx *q) { return q->capacity; }

#endif//MPMC_H_
