#ifndef SPSC_H_
#define SPSC_H_

#include "base/macro.h"

// 无锁单生产者单消费者有界队列。元素类型编译期固化，故搬运是结构体赋值而非运行期 memcpy。
// 生产者与消费者各自缓存对端的下标，只有缓存显示"满/空"时才真去读对方那条 cache line——
// 不缓存的话每次 push/pop 都要拉一次对端缓存行，是这类队列最主要的一致性开销。
// 两个 cache 各由一侧独占读写，故是普通变量、不需要原子。
//
// 典型用法：
//   typedef struct { int a; } my_elem;
//   SPSC_DECL(my_q, my_elem)
//   my_q q; my_q_init(&q, 1024);
//   my_elem e = { 1 };
//   if (ERR_OK == my_q_trypush(&q, &e)) { ... }  // 满则 ERR_FAILED
//   my_elem out;
//   if (ERR_OK == my_q_pop(&q, &out)) { ... }    // 空则 ERR_FAILED
//   my_q_free(&q);
//
// 契约：只允许一个生产者线程调 trypush、一个消费者线程调 pop；size/empty 两侧都可调，
//       但拿到的是保守快照（只会把空报成非空，不会把非空报成空）。

#define SPSC_DEFAULT_CAP 1024 // 默认容量

// 入参写 T const * 而不是 const T *:T 是指针类型时,后者会被解析成指向 const 的指针。
// SPSC_DECL(name, T)：name 生成的类型名，T 元素类型
#define SPSC_DECL(name, T)                                                     \
typedef struct {                                                                \
    T *cell;                                                                    \
    uint32_t capacity;                                                          \
    uint32_t mask;                                                              \
    CACHELINE_ALIGN atomic_t enq;      /* 生产者独占写 */                        \
    uint32_t deq_cache;                /* 生产者私有:消费者下标的缓存 */          \
    CACHELINE_ALIGN atomic_t deq;      /* 消费者独占写 */                        \
    uint32_t enq_cache;                /* 消费者私有:生产者下标的缓存 */          \
} name;                                                                         \
/* 容量取不小于 capacity 的 2 的幂(至少 2),0 取 SPSC_DEFAULT_CAP;一次分配,之后不扩容 */ \
static inline void name##_init(name *q, uint32_t capacity) {                    \
    ASSERTAB(NULL != q, ERRSTR_NULLP);                                          \
    q->capacity = (0 == capacity) ? SPSC_DEFAULT_CAP                            \
                                  : pow2_ceil(capacity < 2 ? 2 : capacity);     \
    q->mask = q->capacity - 1;                                                  \
    ASSERTAB(sizeof(T) <= SIZE_MAX / (size_t)q->capacity, "byte size overflow.");\
    MALLOC(q->cell, sizeof(T) * (size_t)q->capacity);                           \
    ATOMIC_SET_RELAXED(&q->enq, 0);                                                     \
    ATOMIC_SET_RELAXED(&q->deq, 0);                                                     \
    q->deq_cache = 0;                                                           \
    q->enq_cache = 0;                                                           \
}                                                                               \
/* 释放缓冲;q 为 NULL 时不做事 */                                               \
static inline void name##_free(name *q) {                                       \
    if (NULL == q) {                                                            \
        return;                                                                 \
    }                                                                           \
    FREE(q->cell);                                                              \
    q->capacity = 0;                                                            \
    q->mask = 0;                                                                \
}                                                                               \
/* 容量 */                                                                      \
static inline uint32_t name##_capacity(const name *q) { return q->capacity; }   \
/* 元素字节数 sizeof(T) */                                                      \
static inline uint32_t name##_elsize(const name *q) { (void)q; return (uint32_t)sizeof(T); }\
/* 保守快照:先读 deq 后读 enq,只会把空报成非空,不会把非空报成空 */                \
static inline uint32_t name##_size(name *q) {                                   \
    uint32_t deq = (uint32_t)ATOMIC_GET(&q->deq);                               \
    return (uint32_t)ATOMIC_GET(&q->enq) - deq;                                 \
}                                                                               \
/* 是否为空,口径同 size */                                                      \
static inline int32_t name##_empty(name *q) {                                   \
    uint32_t deq = (uint32_t)ATOMIC_GET(&q->deq);                               \
    return (uint32_t)ATOMIC_GET(&q->enq) == deq;                                \
}                                                                               \
/* 入队(单生产者):独占 enq 无需 CAS。先看本地缓存的 deq,缓存说满了才真读对方。       \
   写数据后用 release 发布——不许上面那次赋值下沉,消费者 acquire 到新 enq 就一定看得到 */\
static inline int32_t name##_trypush(name *q, T const *data) {                 \
    uint32_t enq = (uint32_t)ATOMIC_GET_RELAXED(&q->enq);                               \
    if (enq - q->deq_cache >= q->capacity) {                                    \
        q->deq_cache = (uint32_t)ATOMIC_GET(&q->deq);                           \
        if (enq - q->deq_cache >= q->capacity) {                                \
            return ERR_FAILED;                                                  \
        }                                                                       \
    }                                                                           \
    q->cell[enq & q->mask] = *data;                                            \
    ATOMIC_SET_RELEASE(&q->enq, enq + 1);                                       \
    return ERR_OK;                                                              \
}                                                                               \
/* 出队(单消费者):独占 deq 无需 CAS。缓存说空了才真读对方。                        \
   ATOMIC_GET(enq) 是 acquire,取数据能看到生产者发布前写入的内容;                  \
   推进 deq 用 release,不许上面那次读下沉到它之后,否则槽可能已被覆盖 */            \
static inline int32_t name##_pop(name *q, T *out) {                             \
    uint32_t deq;                                                               \
    ASSERTAB(NULL != out, ERRSTR_NULLP);                                        \
    deq = (uint32_t)ATOMIC_GET_RELAXED(&q->deq);                                        \
    if (deq == q->enq_cache) {                                                  \
        q->enq_cache = (uint32_t)ATOMIC_GET(&q->enq);                           \
        if (deq == q->enq_cache) {                                              \
            return ERR_FAILED;                                                  \
        }                                                                       \
    }                                                                           \
    *out = q->cell[deq & q->mask];                                              \
    ATOMIC_SET_RELEASE(&q->deq, deq + 1);                                       \
    return ERR_OK;                                                              \
}

#endif//SPSC_H_
