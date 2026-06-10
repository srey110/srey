#ifndef MPQ_H_
#define MPQ_H_

#include "base/macro.h"

//无锁多生产者有界队列 (Multi-Producer Queue, Lock-Free)
//生产者侧固定多线程 CAS 抢 enq.v；消费者侧由调用方约定：
//  - mpq_pop    多消费者安全（内部 CAS 抢 deq.v）
//  - mpq_pop_sc 单消费者优化（无 CAS，要求调用方保证仅一个线程调用 pop_sc）
//单元素：序列号 + 柔性数据区（长度 = init 时的 elsize，按值存任意定长元素）
typedef struct mpq_cell {
    atomic_t  sequence;
    char      data[];
} mpq_cell;
//用 union + char[64] 让 enq/deq 各占 64 字节
typedef union {
    atomic_t v;
    char     _pad[64];
} mpq_aln_t;
//无锁多生产者队列上下文
typedef struct mpq_ctx {
    uint32_t   capacity; //队列容量，必须为 2 的幂
    uint32_t   mask;     //capacity - 1，用于快速取模
    uint32_t   elsize;   //单元素字节数（init 时指定）
    uint32_t   stride;   //每槽位字节数 = ROUND_UP(sizeof(atomic_t)+elsize, 8)
    char       *cells;   //槽位数组基址（按 stride 步进寻址，不可用下标索引）
    mpq_aln_t  enq;      //入队位置计数器，独占缓存行（多生产者 CAS 抢）
    mpq_aln_t  deq;      //出队位置计数器，独占缓存行
} mpq_ctx;
/// <summary>
/// 初始化队列
/// </summary>
/// <param name="q">mpq_ctx</param>
/// <param name="elsize">单元素字节数（按值存储，须 大于 0）</param>
/// <param name="capacity">期望容量，0 则使用默认值，非 2 的幂自动向上取整</param>
void mpq_init(mpq_ctx *q, size_t elsize, uint32_t capacity);
/// <summary>
/// 释放队列内部内存，不释放 q 本身
/// </summary>
/// <param name="q">mpq_ctx</param>
void mpq_free(mpq_ctx *q);
/// <summary>
/// 阻塞入队：从 data 拷贝 elsize 字节入队，队列满时自旋等待直到成功
/// </summary>
/// <param name="q">mpq_ctx</param>
/// <param name="data">指向待入队元素的指针，不得为 NULL（拷贝 elsize 字节）</param>
void mpq_push(mpq_ctx *q, const void *data);
/// <summary>
/// 非阻塞入队：从 data 拷贝 elsize 字节入队，队列满时立即返回 ERR_FAILED
/// </summary>
/// <param name="q">mpq_ctx</param>
/// <param name="data">指向待入队元素的指针，不得为 NULL（拷贝 elsize 字节）</param>
/// <returns>ERR_OK 成功，ERR_FAILED 队列已满</returns>
int32_t mpq_trypush(mpq_ctx *q, const void *data);
/// <summary>
/// 出队（多消费者安全）：拷贝 elsize 字节到 out。内部 CAS 抢占 deq.v，
/// 适用于多线程并发出队的场景。
/// </summary>
/// <param name="q">mpq_ctx</param>
/// <param name="out">出参：接收出队元素的缓冲（至少 elsize 字节），仅 ERR_OK 时有效</param>
/// <returns>ERR_OK 成功，ERR_FAILED 队列为空</returns>
int32_t mpq_pop(mpq_ctx *q, void *out);
/// <summary>
/// 出队（单消费者）：拷贝 elsize 字节到 out。消费者侧独占 deq.v、无 CAS，
/// 比 mpq_pop 快。约束：仅允许单一消费者线程调用，并发调用 pop_sc 行为未定义；
/// 同一队列上 pop 与 pop_sc 也不可混用（语义错乱）。
/// </summary>
/// <param name="q">mpq_ctx</param>
/// <param name="out">出参：接收出队元素的缓冲（至少 elsize 字节），仅 ERR_OK 时有效</param>
/// <returns>ERR_OK 成功，ERR_FAILED 队列为空</returns>
int32_t mpq_pop_sc(mpq_ctx *q, void *out);
/// <summary>
/// 返回当前队列元素数量的近似值，并发下不精确
/// </summary>
/// <param name="q">mpq_ctx</param>
/// <returns>元素数量</returns>
uint32_t mpq_size(mpq_ctx *q);
/// <summary>
/// 返回队列最大容量
/// </summary>
/// <param name="q">mpq_ctx</param>
/// <returns>最大容量</returns>
static inline uint32_t mpq_capacity(const mpq_ctx *q) { return q->capacity; }

#endif//MPQ_H_
