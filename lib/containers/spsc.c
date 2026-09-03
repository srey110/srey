#include "containers/spsc.h"

#define SPSC_DEFAULT_CAP  1024

//按 pos 取槽位地址：基址 + (pos & mask) * stride
static inline char *_spsc_cell_at(spsc_ctx *q, uint32_t pos) {
    return _ringq_cell(&q->rq, pos);
}
void spsc_init(spsc_ctx *q, size_t elsize, uint32_t capacity) {
    ASSERTAB(NULL != q, ERRSTR_NULLP);
    ASSERTAB(elsize > 0 && elsize <= (size_t)UINT32_MAX - 7, ERRSTR_INVPARAM);
    //每槽位仅存 data，向上对齐到 8 字节，保证后续访问对齐
    _ringq_init(&q->rq, elsize, capacity, SPSC_DEFAULT_CAP,
        (uint32_t)ROUND_UP(elsize, 8));
}
void spsc_free(spsc_ctx *q) {
    if (NULL == q) {
        return;
    }
    _ringq_free(&q->rq);
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
    uint32_t enq = ATOMIC_GET(&q->rq.enq.v);
    uint32_t deq = ATOMIC_GET(&q->rq.deq.v);
    if (enq - deq >= q->rq.capacity) {
        return ERR_FAILED;
    }
    /* 写数据并发布（enq++ 通知消费者）。ATOMIC_SET 是足序写, 不许上面那次 memcpy
     * 下沉, 消费者读到新的 enq.v 就一定能看到数据。与 mpq_trypush 同一套约定。*/
    memcpy(_spsc_cell_at(q, enq), data, q->rq.elsize);
    ATOMIC_SET(&q->rq.enq.v, enq + 1);
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
    uint32_t deq = ATOMIC_GET(&q->rq.deq.v);
    uint32_t enq = ATOMIC_GET(&q->rq.enq.v);
    if (deq == enq) {
        return ERR_FAILED;
    }
    /* 拷出数据并推进 deq（释放该槽位给 producer 下一轮使用）。
     * ATOMIC_GET(enq.v) 是 acquire，保证此后 memcpy 能观察到生产者
     * 在 ATOMIC_SET(enq, enq+1) 之前写入的 cell 内容，无需对槽位数据本身加原子操作。*/
    memcpy(out, _spsc_cell_at(q, deq), q->rq.elsize);
    ATOMIC_SET(&q->rq.deq.v, deq + 1);
    return ERR_OK;
}
uint32_t spsc_size(spsc_ctx *q) {
    return _ringq_size(&q->rq);
}
