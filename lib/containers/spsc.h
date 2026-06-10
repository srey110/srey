#ifndef SPSC_H_
#define SPSC_H_

#include "base/macro.h"

//无锁单生产者单消费者有界队列 (SPSC Lock-Free Queue)
//用 union + char[64] 让 enq/deq 各占 64 字节
typedef union {
    atomic_t v;
    char     _pad[64];
} spsc_aln_t;
//无锁 SPSC 队列上下文
typedef struct spsc_ctx {
    uint32_t    capacity; //队列容量，必须为 2 的幂
    uint32_t    mask;     //capacity - 1，用于快速取模
    uint32_t    elsize;   //单元素字节数（init 时指定）
    uint32_t    stride;   //每槽位字节数 = ROUND_UP(elsize, 8)
    char        *cells;   //槽位数组基址（按 stride 步进寻址）
    spsc_aln_t  enq;      //入队位置计数器，producer 独占写、consumer 只读
    spsc_aln_t  deq;      //出队位置计数器，consumer 独占写、producer 只读
} spsc_ctx;
/// <summary>
/// 初始化队列
/// </summary>
/// <param name="q">spsc_ctx</param>
/// <param name="elsize">单元素字节数（按值存储，须 大于 0）</param>
/// <param name="capacity">期望容量，0 则使用默认值，非 2 的幂自动向上取整</param>
void spsc_init(spsc_ctx *q, size_t elsize, uint32_t capacity);
/// <summary>
/// 释放队列内部内存，不释放 q 本身
/// </summary>
/// <param name="q">spsc_ctx</param>
void spsc_free(spsc_ctx *q);
/// <summary>
/// 阻塞入队：从 data 拷贝 elsize 字节入队，队列满时自旋等待直到成功。
/// 约束：仅允许单一生产者线程调用，并发调用 push/trypush 行为未定义。
/// </summary>
/// <param name="q">spsc_ctx</param>
/// <param name="data">指向待入队元素的指针，不得为 NULL（拷贝 elsize 字节）</param>
void spsc_push(spsc_ctx *q, const void *data);
/// <summary>
/// 非阻塞入队：从 data 拷贝 elsize 字节入队，队列满时立即返回 ERR_FAILED。
/// 约束：仅允许单一生产者线程调用，并发调用 push/trypush 行为未定义。
/// </summary>
/// <param name="q">spsc_ctx</param>
/// <param name="data">指向待入队元素的指针，不得为 NULL（拷贝 elsize 字节）</param>
/// <returns>ERR_OK 成功，ERR_FAILED 队列已满</returns>
int32_t spsc_trypush(spsc_ctx *q, const void *data);
/// <summary>
/// 出队，非阻塞：拷贝 elsize 字节到 out。
/// 约束：仅允许单一消费者线程调用，并发调用 pop 行为未定义。
/// </summary>
/// <param name="q">spsc_ctx</param>
/// <param name="out">出参：接收出队元素的缓冲（至少 elsize 字节），仅 ERR_OK 时有效</param>
/// <returns>ERR_OK 成功，ERR_FAILED 队列为空</returns>
int32_t spsc_pop(spsc_ctx *q, void *out);
/// <summary>
/// 返回当前队列元素数量的近似值，并发下不精确
/// </summary>
/// <param name="q">spsc_ctx</param>
/// <returns>元素数量</returns>
uint32_t spsc_size(spsc_ctx *q);
/// <summary>
/// 返回队列最大容量
/// </summary>
/// <param name="q">spsc_ctx</param>
/// <returns>最大容量</returns>
static inline uint32_t spsc_capacity(const spsc_ctx *q) { return q->capacity; }

#endif//SPSC_H_
