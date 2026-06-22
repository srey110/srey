#include "containers/spsc.h"
#include "utils/utils.h"

#define SPSC_DEFAULT_CAP  1024
#define SPSC_SPIN_MAX     64

//按 pos 取槽位地址：基址 + (pos & mask) * stride
static inline char *_spsc_cell_at(spsc_ctx *q, uint32_t pos) {
    return q->cells + (size_t)(pos & q->mask) * q->stride;
}
void spsc_init(spsc_ctx *q, size_t elsize, uint32_t capacity) {
    ASSERTAB(NULL != q, ERRSTR_NULLP);
    ASSERTAB(elsize > 0 && elsize <= (size_t)UINT32_MAX - 7, ERRSTR_INVPARAM);
    capacity = (0 == capacity) ? SPSC_DEFAULT_CAP : pow2_ceil(capacity);
    ASSERTAB(capacity >= 2, ERRSTR_INVPARAM);
    q->capacity = capacity;
    q->mask = capacity - 1;
    q->elsize = (uint32_t)elsize;
    //每槽位仅存 data，向上对齐到 8 字节，保证后续访问对齐
    q->stride = (uint32_t)ROUND_UP(elsize, 8);
    q->enq.v = 0;
    q->deq.v = 0;
    ASSERTAB((size_t)capacity <= SIZE_MAX / q->stride, "byte size overflow.");
    MALLOC(q->cells, (size_t)q->stride * capacity);
}
void spsc_free(spsc_ctx *q) {
    if (NULL == q) {
        return;
    }
    FREE(q->cells);
}
/*
 * 入队核心逻辑（单生产者）：
 *   producer 独占 enq.v，无并发推进者，无需 CAS。
 *   满/空判定：(enq - deq) ∈ [0, capacity]
 *     - enq == deq        → 空
 *     - enq - deq == cap  → 满
 *   读 deq 取保守快照即可：consumer 只会让 deq 增加（空间变多），
 *   旧 deq 看起来更紧、最坏只是误判为满。uint32_t 减法处理 wrap。
 */
//非阻塞入队：从 data 拷贝 elsize 字节，队列满时立即返回 ERR_FAILED
int32_t spsc_trypush(spsc_ctx *q, const void *data) {
    if (NULL == q || NULL == data) {
        return ERR_FAILED;
    }
    uint32_t enq = ATOMIC_GET(&q->enq.v);
    uint32_t deq = ATOMIC_GET(&q->deq.v);
    if (enq - deq >= q->capacity) {
        return ERR_FAILED;
    }
    /* 写入数据并发布（enq++ 通知消费者）。
     * memcpy 为普通写，但其后的 ATOMIC_SET 是 full barrier（release），
     * 与消费者侧 ATOMIC_GET（acquire）构成 synchronizes-with 关系，
     * 保证本次写对消费者可见，ARM 弱序架构下同样成立。*/
    memcpy(_spsc_cell_at(q, enq), data, q->elsize);
    ATOMIC_SET(&q->enq.v, enq + 1);
    return ERR_OK;
}
//阻塞入队：队列满时自旋等待直到成功；长时间满则 yield 退让
void spsc_push(spsc_ctx *q, const void *data) {
    ASSERTAB(NULL != q && NULL != data, ERRSTR_NULLP);
    uint32_t spins = 0;
    while (ERR_OK != spsc_trypush(q, data)) {
        if (++spins >= SPSC_SPIN_MAX) {
            spins = 0;
            THREAD_YIELD();
        } else {
            CPU_PAUSE();
        }
    }
}
/*
 * 出队核心逻辑（单消费者）：
 *   consumer 独占 deq.v，无并发推进者，无需 CAS。
 *   读 enq 取保守快照即可：producer 只会让 enq 增加（数据变多），
 *   旧 enq 看起来更少、最坏只是误判为空。uint32_t 减法处理 wrap。
 */
int32_t spsc_pop(spsc_ctx *q, void *out) {
    if (NULL == q || NULL == out) {
        return ERR_FAILED;
    }
    uint32_t deq = ATOMIC_GET(&q->deq.v);
    uint32_t enq = ATOMIC_GET(&q->enq.v);
    if (deq == enq) {
        return ERR_FAILED;
    }
    /* 拷出数据并推进 deq（释放该槽位给 producer 下一轮使用）。
     * ATOMIC_GET(enq.v) 是 acquire，保证此后 memcpy 能观察到生产者
     * 在 ATOMIC_SET(enq, enq+1) 之前写入的 cell 内容，无需对槽位数据本身加原子操作。*/
    memcpy(out, _spsc_cell_at(q, deq), q->elsize);
    ATOMIC_SET(&q->deq.v, deq + 1);
    return ERR_OK;
}
/*
 * 返回当前队列元素数量的近似值。
 * 并发场景下 enq 与 deq 分两次读取，结果仅供参考。
 * 无符号减法天然处理 uint32_t 绕回情形。
 */
uint32_t spsc_size(spsc_ctx *q) {
    uint32_t enq = ATOMIC_GET(&q->enq.v);
    uint32_t deq = ATOMIC_GET(&q->deq.v);
    uint32_t size = enq - deq;
    return size > q->capacity ? 0 : size;
}
