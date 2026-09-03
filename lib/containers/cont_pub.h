#ifndef CONT_PUB_H_
#define CONT_PUB_H_

#include "base/structs.h"

// 定长槽位环形队列头，mpq 与 spsc 共用。两者字段本就完全相同，抽出来后 init / free / size
// 只有一份，改读写顺序或溢出守卫不会只改到其中一个（size 漏改的后果是漏唤醒，不报错）
typedef struct ringq_ctx {
    uint32_t      capacity; //队列容量，必须为 2 的幂
    uint32_t      mask;     //capacity - 1，用于快速取模
    uint32_t      elsize;   //单元素字节数（init 时指定）
    uint32_t      stride;   //每槽位字节数，由各队列自己算（mpq 每槽多一个序列号）
    char          *cells;   //槽位数组基址（按 stride 步进寻址，不可用下标索引）
    char          _pad0[CACHELINE_SIZE];//把上面这几个只读字段与 enq 隔开：每次推进 enq
                            //都会让别的核重读 mask/stride/elsize/cells，而收发每次都要用它们
    atomic_aln_t  enq;      //入队位置计数器（与 deq 各占一条 cache line）
    atomic_aln_t  deq;      //出队位置计数器
} ringq_ctx;

// 按 pos 取槽位地址：基址 + (pos & mask) * stride
static inline char *_ringq_cell(ringq_ctx *rq, uint32_t pos) {
    return rq->cells + (size_t)(pos & rq->mask) * rq->stride;
}
// 填好头部并分配槽位数组。capacity 为 0 用 defcap，否则向上取到 2 的幂、下限 2；
// stride 由调用方按自己的槽位布局算好传进来
static inline void _ringq_init(ringq_ctx *rq, size_t elsize, uint32_t capacity,
    uint32_t defcap, uint32_t stride) {
    rq->capacity = (0 == capacity) ? defcap : pow2_ceil(capacity < 2 ? 2 : capacity);
    rq->mask = rq->capacity - 1;
    rq->elsize = (uint32_t)elsize;
    rq->stride = stride;
    rq->enq.v = 0;
    rq->deq.v = 0;
    ASSERTAB((size_t)rq->capacity <= SIZE_MAX / rq->stride, "byte size overflow.");
    MALLOC(rq->cells, (size_t)rq->stride * rq->capacity);
}
static inline void _ringq_free(ringq_ctx *rq) {
    FREE(rq->cells);
}
/*
 * 元素数近似值，并发下不精确。必须先读 deq 再读 enq：两者单调递增且恒有 enq >= deq，
 * 这个顺序下下溢不可能。越界钳到 capacity 而非 0——调用方靠它判"还有没有活要干"，
 * 高估最多多醒一次，低估就是漏唤醒
 */
static inline uint32_t _ringq_size(ringq_ctx *rq) {
    uint32_t deq = ATOMIC_GET(&rq->deq.v);
    uint32_t enq = ATOMIC_GET(&rq->enq.v);
    uint32_t size = enq - deq;
    return size > rq->capacity ? rq->capacity : size;
}

#endif//CONT_PUB_H_
