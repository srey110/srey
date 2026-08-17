#include "containers/mpq.h"
#include "utils/utils.h"

#define MPQ_DEFAULT_CAP  1024

//按 pos 取槽位地址：基址 + (pos & mask) * stride
static inline mpq_cell *_mpq_cell_at(mpq_ctx *q, uint32_t pos) {
    return (mpq_cell *)(q->cells + (size_t)(pos & q->mask) * q->stride);
}
void mpq_init(mpq_ctx *q, size_t elsize, uint32_t capacity) {
    ASSERTAB(NULL != q, ERRSTR_NULLP);
    ASSERTAB(elsize > 0 && elsize <= (size_t)UINT32_MAX - sizeof(atomic_t) - 7, ERRSTR_INVPARAM);
    capacity = (0 == capacity) ? MPQ_DEFAULT_CAP : pow2_ceil(capacity);
    ASSERTAB(capacity >= 2, ERRSTR_INVPARAM);
    q->capacity = capacity;
    q->mask = capacity - 1;
    q->elsize = (uint32_t)elsize;
    //每槽位 = 序列号 + elsize 数据，向上对齐到 8 字节，保证各槽位 sequence 对齐
    q->stride = (uint32_t)ROUND_UP(sizeof(atomic_t) + elsize, 8);
    q->enq.v = 0;
    q->deq.v = 0;
    ASSERTAB((size_t)capacity <= SIZE_MAX / q->stride, "byte size overflow.");
    MALLOC(q->cells, (size_t)q->stride * capacity);
    uint32_t i;
    //初始化每个槽位的序列号为其下标，表示"可入队"状态
    for (i = 0; i < capacity; i++) {
        _mpq_cell_at(q, i)->sequence = i;
    }
}
void mpq_free(mpq_ctx *q) {
    if (NULL == q) {
        return;
    }
    FREE(q->cells);
}
/*
 * 入队核心（Vyukov 多生产者序列号算法）：槽位 sequence 标记它所处的"代"——
 * == pos 空闲、== pos+1 已写入待消费、== pos+cap 已消费可重用。
 * 循环用 signed diff = (int32_t)(sequence - pos)：0 可抢占，负数队满，
 * 正数是别的生产者已推进 enq_pos 需重载重试。队满立即 ERR_FAILED，拷 elsize 字节
 */
int32_t mpq_trypush(mpq_ctx *q, const void *data) {
    if (NULL == q || NULL == data) {
        return ERR_FAILED;
    }
    mpq_cell *cell;
    uint32_t pos;
    int32_t diff;
    pos = ATOMIC_GET(&q->enq.v);
    for (;;) {
        cell = _mpq_cell_at(q, pos);
        diff = (int32_t)(ATOMIC_GET(&cell->sequence) - pos);
        if (0 == diff) {
            //槽位空闲，尝试原子抢占 enq_pos
            if (ATOMIC_CAS(&q->enq.v, pos, pos + 1)) {
                break;
            }
            //CAS 失败说明其他生产者已抢先，重新加载
            pos = ATOMIC_GET(&q->enq.v);
        } else if (diff < 0) {
            //队列已满，立即返回
            return ERR_FAILED;
        } else {
            //enq_pos 已过时，重新加载后重试
            pos = ATOMIC_GET(&q->enq.v);
        }
        //CAS 竞争或 pos 过期时短暂让出总线再重试，不 yield（由调用方决策）
        CPU_PAUSE();
    }
    /* 已独占该槽位，写入数据并发布（sequence = pos+1 通知消费者）。
     * memcpy 为普通写，但其后的 ATOMIC_SET_RELEASE 保证它不会越到 store 之后，
     * 与消费者侧 ATOMIC_GET（acquire）构成 synchronizes-with 关系，
     * 保证本次写对消费者可见，ARM 弱序架构下同样成立。
     * 此处只需发布语义, 不需要 StoreLoad：调用方(log.c / loader.c)的 SB 握手
     * 靠的是 enq.v 那个 ATOMIC_CAS 的全屏障, 与本 store 无关, 故不必用 seq_cst 交换。*/
    memcpy(cell->data, data, q->elsize);
    ATOMIC_SET_RELEASE(&cell->sequence, pos + 1);
    return ERR_OK;
}
/*
 * 出队（多消费者安全）：signed diff = (int32_t)(sequence - (pos + 1))——
 * 0 表示已写入可出队(CAS 抢 deq_pos)，负数是队空，正数是别的消费者已推进 deq_pos 需重载重试
 */
int32_t mpq_pop(mpq_ctx *q, void *out) {
    if (NULL == q || NULL == out) {
        return ERR_FAILED;
    }
    mpq_cell *cell;
    uint32_t pos;
    int32_t diff;
    pos = ATOMIC_GET(&q->deq.v);
    for (;;) {
        cell = _mpq_cell_at(q, pos);
        diff = (int32_t)(ATOMIC_GET(&cell->sequence) - (pos + 1));
        if (0 == diff) {
            //槽位有数据，尝试原子抢占 deq_pos
            if (ATOMIC_CAS(&q->deq.v, pos, pos + 1)) {
                break;
            }
            //CAS 失败说明其他消费者已抢先，重新加载
            pos = ATOMIC_GET(&q->deq.v);
        } else if (diff < 0) {
            //槽位未发布。这里手上的 pos 就是刚用来索引 cell 的 deq，直接拿它跟 enq 比：
            //相等说明没人抢过这个槽位，队列真空；不等说明已被抢占、只是还没发布
            return (ATOMIC_GET(&q->enq.v) != pos) ? 1 : ERR_FAILED;
        } else {
            //deq_pos 已过时，重新加载后重试
            pos = ATOMIC_GET(&q->deq.v);
        }
        //CAS 竞争或 pos 过期时短暂让出总线再重试，不 yield（由调用方决策）
        CPU_PAUSE();
    }
    /* 已独占该槽位，读取数据并释放槽位（sequence = pos+capacity 通知生产者下一轮可用）。
     * ATOMIC_GET(sequence) 是 acquire，保证此后 memcpy 能观察到生产者
     * 在发布 sequence = pos+1 之前写入的值，无需对 cell->data 本身加原子操作。
     * 释放槽位用 release store：它同时挡住上面那次 memcpy 读被下沉，
     * 否则生产者拿到空槽后可能在我们读完之前就把数据覆盖掉。*/
    memcpy(out, cell->data, q->elsize);
    ATOMIC_SET_RELEASE(&cell->sequence, pos + q->capacity);
    return ERR_OK;
}
/*
 * 出队（单消费者）：独占 deq.v 无需 CAS，只校验 sequence 是否就绪——
 * diff == 0 可出队，负数是队空，正数不会出现(pos 不被他人推进)。
 * 出队后推进 deq.v 并把 sequence 置为 pos+capacity，通知生产者该槽可重用
 */
int32_t mpq_pop_sc(mpq_ctx *q, void *out) {
    if (NULL == q || NULL == out) {
        return ERR_FAILED;
    }
    //消费者独占 deq.v，本地一次 load 即可（无并发推进者）
    uint32_t pos = ATOMIC_GET(&q->deq.v);
    mpq_cell *cell = _mpq_cell_at(q, pos);
    int32_t diff = (int32_t)(ATOMIC_GET(&cell->sequence) - (pos + 1));
    if (0 != diff) {
        //diff < 0：槽位未发布，再拿 pos 跟 enq 比分清真空与已抢占未发布（同 mpq_pop）；
        //diff > 0：单消费者约束被违反（不应出现）
        if (diff < 0
            && ATOMIC_GET(&q->enq.v) != pos) {
            return 1;
        }
        return ERR_FAILED;
    }
    /* 槽位就绪，拷出数据后推进 deq.v，再释放槽位（sequence = pos+capacity 通知生产者下一轮可用）。
     * ATOMIC_GET(sequence) 是 acquire，保证此后 memcpy 能观察到生产者
     * 在发布 sequence = pos+1 之前写入的值，无需对 cell->data 本身加原子操作。
     * 这两步顺序不可颠倒：先释放槽位的话，两步之间生产者可抢占该槽把 enq.v 推到
     * deq+capacity+1，mpq_size 随即触发 ">capacity 返 0" 的钳位，把满队列报成空。
     * 生产者从不读 deq.v，先推进无副作用（mpq_pop 同样是先 CAS deq 再释放槽位）。
     * 两处都用 release store：release 不允许先前的读写下沉，故 memcpy 读、deq.v 写、
     * 槽位释放三者的先后关系全部保持；本函数的调用方在其后自带 fence(log.c / loader.c
     * 的 SB 握手)，不依赖这里提供 StoreLoad。*/
    memcpy(out, cell->data, q->elsize);
    ATOMIC_SET_RELEASE(&q->deq.v, pos + 1);
    ATOMIC_SET_RELEASE(&cell->sequence, pos + q->capacity);
    return ERR_OK;
}
/*
 * 元素数近似值，并发下不精确。必须先读 deq 再读 enq：两者单调递增且恒有 enq >= deq，
 * 这个顺序下下溢不可能。越界钳到 capacity 而非 0——调用方靠它判"还有没有活要干"，
 * 高估最多多醒一次，低估就是漏唤醒
 */
uint32_t mpq_size(mpq_ctx *q) {
    uint32_t deq = ATOMIC_GET(&q->deq.v);
    uint32_t enq = ATOMIC_GET(&q->enq.v);
    uint32_t size = enq - deq;
    return size > q->capacity ? q->capacity : size;
}
