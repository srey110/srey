#ifndef MPQ_H_
#define MPQ_H_

#include "base/macro.h"

// 无锁多生产者有界队列(Vyukov 序列号算法)。元素类型编译期固化，故搬运是结构体赋值。
// 生产者侧固定多线程 CAS 抢 enq；消费者侧由调用方约定：
//   name##_pop     多消费者安全(内部 CAS 抢 deq)
//   name##_pop_sc  单消费者优化(无 CAS，要求调用方保证仅一个线程调用)
//
// 槽位 sequence 标记它所处的"代"：== pos 空闲、== pos+1 已写入待消费、== pos+cap 已消费可重用。
// 循环用 signed diff = (int32_t)(sequence - pos)：0 可抢占，负数队满，正数是别的生产者
// 已推进 enq 需重载重试。

#define MPQ_DEFAULT_CAP 1024 // 默认容量
// mpq 撞输 CAS 后单次退避最长等这么多个周期,0 = 不退避(重试前一次 CPU_PAUSE 都不做)。
// 1024 是 arm64 上的安全上限,再大低竞争时会掉速
#define MPQ_BACKOFF_CYCLES 1024
// 单次退避的 CPU_PAUSE 次数上限。写成"目标周期数 ÷ 单次 CPU_PAUSE 的周期数"而不是直接给次数:
// CPU_PAUSE 的单价跨平台差三个数量级(arm64 的 yield 近似 nop,兜底平台是系统调用)。
// 单次 CPU_PAUSE 就超过 MPQ_BACKOFF_CYCLES 的平台按 1 次算,那里每次最长等一次 CPU_PAUSE
#define MPQ_BACKOFF_CAP ((MPQ_BACKOFF_CYCLES / CPU_PAUSE_CYCLES) < 1 \
                         ? 1 : (MPQ_BACKOFF_CYCLES / CPU_PAUSE_CYCLES))

// 入参写 T const * 而不是 const T *：T 是指针类型(如 void *)时，后者会被解析成指向 const 的指针，调用方传普通指针就告警。
// MPQ_DECL(name, T)：name 生成的类型名，T 元素类型
#define MPQ_DECL(name, T)                                                      \
/* 槽位按 8 字节对齐,槽间距承重别改 */                                           \
/* _pad 把 data 固定在偏移 8,对齐承重别删。T 按 8 对齐时它不占额外空间;T 对齐不超过 4 \
   时每槽可能多出 8 字节(如 int32_t、32 位平台上的指针) */                         \
typedef struct { atomic_t sequence; uint32_t _pad; T data; } ALIGN8 name##_cell;\
typedef struct {                                                                \
    name##_cell *cells;                                                         \
    uint32_t capacity;                                                          \
    uint32_t mask;                                                              \
    CACHELINE_ALIGN atomic_t enq;   /* 与 deq 各占一条 cache line */             \
    CACHELINE_ALIGN atomic_t deq;                                               \
} name;                                                                         \
/* 容量取不小于 capacity 的 2 的幂(至少 2),0 取 MPQ_DEFAULT_CAP;一次分配,之后不扩容 */ \
static inline void name##_init(name *q, uint32_t capacity) {                    \
    uint32_t i, cap;                                                            \
    name##_cell *cells;                                                         \
    ASSERTAB(NULL != q, ERRSTR_NULLP);                                          \
    q->capacity = (0 == capacity) ? MPQ_DEFAULT_CAP                             \
                                  : pow2_ceil(capacity < 2 ? 2 : capacity);     \
    q->mask = q->capacity - 1;                                                  \
    ASSERTAB(sizeof(name##_cell) <= SIZE_MAX / (size_t)q->capacity, "byte size overflow.");\
    MALLOC(q->cells, sizeof(name##_cell) * (size_t)q->capacity);                \
    ATOMIC_SET_RELAXED(&q->enq, 0);                                                     \
    ATOMIC_SET_RELAXED(&q->deq, 0);                                                     \
    /* 每槽序列号初始化为其下标，表示"可入队"。init 期间队列对别的线程不可见，        \
       普通写即可：发布时的建线程 / 加锁已保证对方看得到 */                        \
    cells = q->cells;                                                           \
    cap = q->capacity;                                                          \
    for (i = 0; i < cap; i++) {                                                 \
        ATOMIC_SET_RELAXED(&cells[i].sequence, (atomic_t)i);                    \
    }                                                                           \
}                                                                               \
/* 释放槽数组;q 为 NULL 时不做事 */                                             \
static inline void name##_free(name *q) {                                       \
    if (NULL == q) {                                                            \
        return;                                                                 \
    }                                                                           \
    FREE(q->cells);                                                             \
    q->capacity = 0;                                                            \
    q->mask = 0;                                                                \
}                                                                               \
/* 容量 */                                                                      \
static inline uint32_t name##_capacity(const name *q) { return q->capacity; }   \
/* 元素字节数 sizeof(T) */                                                      \
static inline uint32_t name##_elsize(const name *q) { (void)q; return (uint32_t)sizeof(T); }\
/* 保守快照:先读 deq 后读 enq,只会把空报成非空,不会把非空报成空。                 \
   已抢槽未发布的也算在内——fsqu 的漏唤醒守卫依赖这个方向 */                      \
static inline uint32_t name##_size(name *q) {                                   \
    uint32_t deq = (uint32_t)ATOMIC_GET(&q->deq);                               \
    uint32_t enq = (uint32_t)ATOMIC_GET(&q->enq);                               \
    uint32_t n = enq - deq;                                                     \
    return (n > q->capacity) ? q->capacity : n;                                 \
}                                                                               \
/* 是否为空,口径同 size */                                                      \
static inline int32_t name##_empty(name *q) {                                   \
    uint32_t deq = (uint32_t)ATOMIC_GET(&q->deq);                               \
    return (uint32_t)ATOMIC_GET(&q->enq) == deq;                                \
}                                                                               \
/* 入队:CAS 抢 enq 独占槽位后写数据并 release 发布。队满立即失败,不等待。          \
   那次 CAS 是全屏障,调用方的丢唤醒握手靠它,不要降级。                            \
   元素按指针传(同 ARR_DECL/QUE_DECL) */                                          \
static inline int32_t name##_trypush(name *q, T const *data) {                  \
    name##_cell *cell;                                                          \
    uint32_t pos;                                                               \
    uint32_t bo = 0;                                                            \
    int32_t diff;                                                               \
    pos = (uint32_t)ATOMIC_GET(&q->enq);                                        \
    for (;;) {                                                                  \
        cell = &q->cells[pos & q->mask];                                        \
        diff = (int32_t)((uint32_t)ATOMIC_GET(&cell->sequence) - pos);          \
        if (0 == diff) {                                                        \
            if (ATOMIC_CAS(&q->enq, (atomic_t)pos, (atomic_t)(pos + 1))) {      \
                break;                                                          \
            }                                                                   \
            pos = (uint32_t)ATOMIC_GET(&q->enq);                                \
        } else if (diff < 0) {                                                  \
            return ERR_FAILED;                                                  \
        } else {                                                                \
            pos = (uint32_t)ATOMIC_GET(&q->enq);                                \
        }                                                                       \
        _mpq_backoff(&bo);                                                      \
    }                                                                           \
    memcpy(&cell->data, data, sizeof(T));                                       \
    ATOMIC_SET_RELEASE(&cell->sequence, (atomic_t)(pos + 1));                   \
    return ERR_OK;                                                              \
}                                                                               \
/* 出队(多消费者安全)。返回 1 表示"已抢槽未发布",调用方据此区分真空与在途 */       \
static inline int32_t name##_pop(name *q, T *out) {                             \
    name##_cell *cell;                                                          \
    uint32_t pos;                                                               \
    uint32_t bo = 0;                                                            \
    int32_t diff;                                                               \
    ASSERTAB(NULL != out, ERRSTR_NULLP);                                        \
    pos = (uint32_t)ATOMIC_GET(&q->deq);                                        \
    for (;;) {                                                                  \
        cell = &q->cells[pos & q->mask];                                        \
        diff = (int32_t)((uint32_t)ATOMIC_GET(&cell->sequence) - (pos + 1));    \
        if (0 == diff) {                                                        \
            if (ATOMIC_CAS(&q->deq, (atomic_t)pos, (atomic_t)(pos + 1))) {      \
                break;                                                          \
            }                                                                   \
            pos = (uint32_t)ATOMIC_GET(&q->deq);                                \
        } else if (diff < 0) {                                                  \
            return ((uint32_t)ATOMIC_GET(&q->enq) != pos) ? 1 : ERR_FAILED;     \
        } else {                                                                \
            pos = (uint32_t)ATOMIC_GET(&q->deq);                                \
        }                                                                       \
        _mpq_backoff(&bo);                                                      \
    }                                                                           \
    *out = cell->data;                                                          \
    ATOMIC_SET_RELEASE(&cell->sequence, (atomic_t)(pos + q->capacity));         \
    return ERR_OK;                                                              \
}                                                                               \
/* 出队(单消费者):独占 deq 无需 CAS。顺序不可颠倒——先推进 deq,再释放槽位。          \
   读 deq 别换成 ATOMIC_GET_RELAXED */                                           \
static inline int32_t name##_pop_sc(name *q, T *out) {                          \
    name##_cell *cell;                                                          \
    uint32_t pos;                                                               \
    int32_t diff;                                                               \
    ASSERTAB(NULL != out, ERRSTR_NULLP);                                        \
    pos = (uint32_t)ATOMIC_GET(&q->deq);                                        \
    cell = &q->cells[pos & q->mask];                                            \
    diff = (int32_t)((uint32_t)ATOMIC_GET(&cell->sequence) - (pos + 1));        \
    if (0 != diff) {                                                            \
        if (diff < 0 && (uint32_t)ATOMIC_GET(&q->enq) != pos) {                 \
            return 1;                                                           \
        }                                                                       \
        return ERR_FAILED;                                                      \
    }                                                                           \
    *out = cell->data;                                                          \
    ATOMIC_SET_RELEASE(&q->deq, (atomic_t)(pos + 1));                           \
    ATOMIC_SET_RELEASE(&cell->sequence, (atomic_t)(pos + q->capacity));         \
    return ERR_OK;                                                              \
}                                                                               \
/* 批量出队(单消费者):连着取到停不下来为止,deq 只推一次,返回取到的条数。          \
   与 pop_sc 的顺序差别:整批拷完才统一放槽,deq 留到最后。单消费者下中间没人来读    \
   deq,外部看到的 deq 只会偏小,即只多报不少报;放槽前这些槽对生产者仍算占用,       \
   近满时生产者会早一批降级。                                                    \
   rtn 只有两态:ERR_OK 取满了 max,ERR_FAILED 没取满,不分真空与在途;             \
   需要区分时自己补一次 name##_pop_sc */                                          \
static inline uint32_t name##_pop_sc_batch(name *q, T *out, uint32_t max, int32_t *rtn) {\
    name##_cell *cells = q->cells;                                              \
    name##_cell *cell;                                                          \
    uint32_t mask = q->mask;                                                    \
    uint32_t cap = q->capacity;                                                 \
    uint32_t pos, k, i;                                                         \
    ASSERTAB(NULL != out, ERRSTR_NULLP);                                        \
    pos = (uint32_t)ATOMIC_GET(&q->deq);                                        \
    for (k = 0; k < max; k++) {                                                 \
        cell = &cells[(pos + k) & mask];                                        \
        if (0 != (int32_t)((uint32_t)ATOMIC_GET(&cell->sequence)                \
                           - (pos + k + 1))) {                                  \
            break;                                                              \
        }                                                                       \
        out[k] = cell->data;                                                    \
    }                                                                           \
    for (i = 0; i < k; i++) {                                                   \
        ATOMIC_SET_RELEASE(&cells[(pos + i) & mask].sequence,                   \
                           (atomic_t)(pos + i + cap));                          \
    }                                                                           \
    SET_PTR(rtn, (k == max) ? ERR_OK : ERR_FAILED);                             \
    if (0 != k) {                                                               \
        ATOMIC_SET_RELEASE(&q->deq, (atomic_t)(pos + k));                       \
    }                                                                           \
    return k;                                                                   \
}

// 撞输 CAS 后的退避,连着撞才越等越久。第一次重试不等——那时计数器刚被别人推走,
// 立刻重读拿到的就是新值。不退避会让多个生产者全速互抽同一条 cache line
static inline void _mpq_backoff(uint32_t *state) {
    if (0 == MPQ_BACKOFF_CYCLES) {
        return;
    }
    uint32_t i;
    uint32_t n = *state;
    if (0 == n) {
        *state = 1;
        return;
    }
    for (i = 0; i < n; i++) {
        CPU_PAUSE();
    }
    *state = ((n << 1) < MPQ_BACKOFF_CAP) ? (n << 1) : MPQ_BACKOFF_CAP;
}

#endif//MPQ_H_
