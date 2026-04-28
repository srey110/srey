#include "containers/mspc.h"

#define MSPC_DEFAULT_CAP  1024
#define MSPC_SPIN_MAX     64

/* 将 n 向上取整到最近的 2 的幂；n 已经是 2 的幂则原值返回 */
static uint32_t _pow2_ceil(uint32_t n) {
    if (0 == n || 0 == (n & (n - 1))) {
        return n;
    }
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return n + 1;
}
void mspc_init(mspc_ctx *q, uint32_t capacity) {
    ASSERTAB(NULL != q, ERRSTR_NULLP);
    capacity = (0 == capacity) ? MSPC_DEFAULT_CAP : _pow2_ceil(capacity);
    ASSERTAB(capacity >= 2, ERRSTR_INVPARAM);
    q->capacity = capacity;
    q->mask     = capacity - 1;
    q->enq.v    = 0;
    q->deq.v    = 0;
    MALLOC(q->cells, sizeof(mspc_cell) * capacity);
    ASSERTAB(NULL != q->cells, "mspc_init: malloc failed.");
    /* 初始化每个槽位的序列号为其下标，表示"可入队"状态 */
    for (uint32_t i = 0; i < capacity; i++) {
        q->cells[i].sequence = i;
        q->cells[i].data     = NULL;
    }
}
void mspc_free(mspc_ctx *q) {
    if (NULL == q) {
        return;
    }
    FREE(q->cells);
}
/*
 * 入队核心逻辑（Vyukov MPMC 序列号算法）：
 *
 *   每个槽位的 sequence 追踪该槽位当前所处的"代"：
 *     - sequence == pos          → 槽位空闲，可被当前入队者抢占
 *     - sequence == pos + 1      → 槽位已写入，等待出队者消费
 *     - sequence == pos + cap    → 槽位已消费，可进入下一轮入队
 *
 *   signed diff = (int32_t)(sequence - pos)：
 *     diff == 0  → 本轮可入队，尝试 CAS 抢占 enq_pos
 *     diff  < 0  → 槽位尚未被消费（队列已满），返回 ERR_FAILED
 *     diff  > 0  → enq_pos 已被其他生产者推进，重新加载 pos 重试
 */
int32_t mspc_push(mspc_ctx *q, void *data) {
    mspc_cell *cell;
    uint32_t   pos;
    int32_t    diff;
    uint32_t   spins = 0;
    ASSERTAB(NULL != q,    ERRSTR_NULLP);
    ASSERTAB(NULL != data, ERRSTR_INVPARAM);
    pos = ATOMIC_GET(&q->enq.v);
    for (;;) {
        cell = &q->cells[pos & q->mask];
        diff = (int32_t)(ATOMIC_GET(&cell->sequence) - pos);
        if (0 == diff) {
            /* 槽位空闲，尝试原子抢占 enq_pos */
            if (ATOMIC_CAS(&q->enq.v, pos, pos + 1)) {
                break;
            }
            /* CAS 失败说明其他生产者已抢先，重新加载 */
            pos = ATOMIC_GET(&q->enq.v);
        } else if (diff < 0) {
            /* 队列已满 */
            return ERR_FAILED;
        } else {
            /* enq_pos 已过时，重新加载后重试 */
            pos = ATOMIC_GET(&q->enq.v);
        }
        CPU_PAUSE();
        if (++spins >= MSPC_SPIN_MAX) {
            spins = 0;
            THREAD_YIELD();
        }
    }
    /* 已独占该槽位，写入数据并发布（sequence = pos+1 通知消费者） */
    cell->data = data;
    ATOMIC_SET(&cell->sequence, pos + 1);
    return ERR_OK;
}
/*
 * 出队核心逻辑：
 *
 *   signed diff = (int32_t)(sequence - (pos + 1))：
 *     diff == 0  → 槽位已写入数据，可出队，尝试 CAS 抢占 deq_pos
 *     diff  < 0  → 生产者尚未写入（队列为空），返回 NULL
 *     diff  > 0  → deq_pos 已被其他消费者推进，重新加载 pos 重试
 */
void *mspc_pop(mspc_ctx *q) {
    mspc_cell *cell;
    uint32_t   pos;
    int32_t    diff;
    void      *data;
    uint32_t   spins = 0;
    ASSERTAB(NULL != q, ERRSTR_NULLP);
    pos = ATOMIC_GET(&q->deq.v);
    for (;;) {
        cell = &q->cells[pos & q->mask];
        diff = (int32_t)(ATOMIC_GET(&cell->sequence) - (pos + 1));
        if (0 == diff) {
            /* 槽位有数据，尝试原子抢占 deq_pos */
            if (ATOMIC_CAS(&q->deq.v, pos, pos + 1)) {
                break;
            }
            /* CAS 失败说明其他消费者已抢先，重新加载 */
            pos = ATOMIC_GET(&q->deq.v);
        } else if (diff < 0) {
            /* 队列为空 */
            return NULL;
        } else {
            /* deq_pos 已过时，重新加载后重试 */
            pos = ATOMIC_GET(&q->deq.v);
        }
        CPU_PAUSE();
        if (++spins >= MSPC_SPIN_MAX) {
            spins = 0;
            THREAD_YIELD();
        }
    }
    /* 已独占该槽位，读取数据并释放槽位（sequence = pos+capacity 通知生产者下一轮可用） */
    data       = cell->data;
    cell->data = NULL;
    ATOMIC_SET(&cell->sequence, pos + q->capacity);
    return data;
}
/*
 * 返回当前队列元素数量的近似值。
 * 并发场景下 enq_pos 与 deq_pos 分两次读取，结果仅供参考。
 * 无符号减法天然处理 uint32_t 绕回情形。
 */
uint32_t mspc_size(mspc_ctx *q) {
    uint32_t enq = ATOMIC_GET(&q->enq.v);
    uint32_t deq = ATOMIC_GET(&q->deq.v);
    return enq - deq;
}
