#include "containers/spsc.h"
#include "utils/utils.h"

#define SPSC_DEFAULT_CAP  1024

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
 * 入队核心（单生产者）：独占 enq.v 无需 CAS。满/空判定 (enq - deq) ∈ [0, capacity]，
 * 相等为空、差等于 cap 为满，uint32_t 减法自然处理回绕。
 * 读 deq 取保守快照即可：consumer 只会让 deq 增加，旧值最坏只是误判为满。
 * 队满立即 ERR_FAILED，拷 elsize 字节
 */
int32_t spsc_trypush(spsc_ctx *q, const void *data) {
    if (NULL == q || NULL == data) {
        return ERR_FAILED;
    }
    uint32_t enq = ATOMIC_GET(&q->enq.v);
    uint32_t deq = ATOMIC_GET(&q->deq.v);
    if (enq - deq >= q->capacity) {
        return ERR_FAILED;
    }
    /* 写数据并发布（enq++ 通知消费者）。ATOMIC_SET 是足序写, 不许上面那次 memcpy
     * 下沉, 消费者读到新的 enq.v 就一定能看到数据。与 mpq_trypush 同一套约定。*/
    memcpy(_spsc_cell_at(q, enq), data, q->elsize);
    ATOMIC_SET(&q->enq.v, enq + 1);
    return ERR_OK;
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
 * 返回当前队列元素数量的近似值，并发下不精确。
 * 必须先读 deq 再读 enq：两者都单调递增且恒有 enq >= deq，先读的 deq 必不大于后读的 enq，
 * 下溢不可能。越界钳到 capacity 而非 0——调用方拿它判"还有没有活要干"，
 * 高估最多多醒一次，低估就是漏唤醒。与 mpq_size 同一约定。
 */
uint32_t spsc_size(spsc_ctx *q) {
    uint32_t deq = ATOMIC_GET(&q->deq.v);
    uint32_t enq = ATOMIC_GET(&q->enq.v);
    uint32_t size = enq - deq;
    return size > q->capacity ? q->capacity : size;
}
