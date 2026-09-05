#ifndef SPSC_H_
#define SPSC_H_

#include "containers/cont_pub.h"

//无锁单生产者单消费者有界队列 (SPSC Lock-Free Queue)
//无锁 SPSC 队列上下文
typedef struct spsc_ctx {
    ringq_ctx rq; //公共队列头，字段说明见 cont_pub.h；每槽只存数据
} spsc_ctx;
/// <summary>
/// 初始化队列
/// </summary>
/// <param name="q">spsc_ctx</param>
/// <param name="elsize">单元素字节数（按值存储，须 大于 0）</param>
/// <param name="capacity">期望容量，0 则使用默认值，非 2 的幂自动向上取整，下限为 2</param>
void spsc_init(spsc_ctx *q, size_t elsize, uint32_t capacity);
/// <summary>
/// 释放队列内部内存，不释放 q 本身
/// </summary>
/// <param name="q">spsc_ctx</param>
void spsc_free(spsc_ctx *q);
/// <summary>
/// 非阻塞入队。仅允许单一生产者线程调用，并发调用 trypush 行为未定义。
/// </summary>
/// <param name="q">spsc_ctx</param>
/// <param name="data">指向待入队元素的指针，不得为 NULL（拷贝 elsize 字节）</param>
/// <returns>ERR_OK 成功，ERR_FAILED 队列已满</returns>
int32_t spsc_trypush(spsc_ctx *q, const void *data);
/// <summary>
/// 出队，非阻塞。仅允许单一消费者线程调用，并发调用 pop 行为未定义。
/// </summary>
/// <param name="q">spsc_ctx</param>
/// <param name="out">出参：接收出队元素的缓冲（至少 elsize 字节），仅 ERR_OK 时有效</param>
/// <returns>ERR_OK 成功，ERR_FAILED 队列为空</returns>
int32_t spsc_pop(spsc_ctx *q, void *out);
/// <summary>
/// 返回当前队列元素数量的近似值：只会高估不会低估(上限 capacity)，不会把有元素报成 0。同 mpq_size
/// </summary>
/// <param name="q">spsc_ctx</param>
/// <returns>元素数量，取值 [0, capacity]</returns>
static inline uint32_t spsc_size(spsc_ctx *q) {
    return _ringq_size(&q->rq);
}
/// <summary>
/// 返回队列最大容量
/// </summary>
/// <param name="q">spsc_ctx</param>
/// <returns>最大容量</returns>
static inline uint32_t spsc_capacity(const spsc_ctx *q) {
    return q->rq.capacity;
}

#endif//SPSC_H_
