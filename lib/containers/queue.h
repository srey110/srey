#ifndef QUEUE_H_
#define QUEUE_H_

#include "base/macro.h"

// 环形队列(定长元素、倍增扩容、容量恒为 2 的幂)。元素类型编译期固化，
// 故搬运是结构体赋值、回绕是一次与运算。
//
// 典型用法：
//   typedef struct { int a; } my_elem;
//   QUE_DECL(my_que, my_elem)
//   my_que q; my_que_init(&q, 0);            // 0 = 延迟分配，首次 push 才申请
//   my_elem e = { 1 }; my_que_push(&q, &e);
//   my_elem *p = my_que_pop(&q);              // 空队返 NULL
//   my_que_free(&q);
//
// 元素按指针传入(同 ARR_DECL)，取出一律返回指针——指针在下次 push/resize 前有效。

#define QUEUE_INIT_SIZE 32 // 默认初始容量

// 入参写 T const * 而不是 const T *:T 是指针类型时,后者会被解析成指向 const 的指针。
// QUE_DECL(name, T)：name 生成的类型名，T 元素类型
#define QUE_DECL(name, T)                                                      \
typedef struct { uint32_t size; uint32_t maxsize; uint32_t mask; uint32_t offset; T *ptr; } name;\
/* 环形下标回绕。容量恒为 2 的幂，一次与运算即可。maxsize 为 0(延迟分配、尚未装入   \
   元素)时 size 也必为 0，各调用点的 size 守卫会先拦下，不会走到这里拿 mask=0 去算 */\
static inline uint32_t name##_wrap(const name *qu, uint32_t off) {              \
    return off & qu->mask;                                                      \
}                                                                               \
/* 元素数 */                                                                    \
static inline uint32_t name##_size(const name *qu) { return qu->size; }         \
/* 容量(延迟分配态为 0) */                                                      \
static inline uint32_t name##_capacity(const name *qu) { return qu->maxsize; }   \
/* 元素字节数 sizeof(T) */                                                      \
static inline uint32_t name##_elsize(const name *qu) { (void)qu; return (uint32_t)sizeof(T); } \
/* 是否为空 */                                                                  \
static inline int32_t name##_empty(const name *qu) { return 0 == qu->size; }    \
/* 延迟分配态(maxsize 为 0)报未满——那是"还没申请",不是"装不下" */                 \
static inline int32_t name##_full(const name *qu) {                             \
    return 0 != qu->maxsize && qu->size >= qu->maxsize;                         \
}                                                                               \
/* 清空,不释放缓冲 */                                                           \
static inline void name##_clear(name *qu) { qu->size = 0; qu->offset = 0; }     \
/* 容量取不小于 maxsize 的 2 的幂(至少 2);0 为延迟分配,首次 push 才申请 */      \
static inline void name##_init(name *qu, uint32_t maxsize) {                    \
    ASSERTAB(maxsize < UINT32_MAX, "queue maxsize overflow.");                  \
    qu->offset = 0;                                                             \
    qu->size = 0;                                                               \
    if (0 == maxsize) {                                                         \
        /* 延迟分配:供可能永不装入元素的层使用(如 fsqu 的溢出层) */              \
        qu->maxsize = 0;                                                        \
        qu->mask = 0;                                                           \
        qu->ptr = NULL;                                                         \
        return;                                                                 \
    }                                                                           \
    qu->maxsize = pow2_ceil(maxsize < 2 ? 2 : maxsize);                         \
    qu->mask = qu->maxsize - 1;                                                 \
    ASSERTAB(sizeof(T) <= SIZE_MAX / (size_t)qu->maxsize, "byte size overflow.");\
    MALLOC(qu->ptr, sizeof(T) * (size_t)qu->maxsize);                           \
}                                                                               \
/* 释放缓冲并复位,之后可直接再 push */                                          \
static inline void name##_free(name *qu) {                                      \
    FREE(qu->ptr);                                                              \
    /* 长度字段一并复位:只置空 ptr 会留下 size < maxsize 的不一致态,               \
       再 push 不触发 resize 而是直接往 NULL 上算偏移写(同 binary_free) */        \
    qu->offset = 0;                                                             \
    qu->size = 0;                                                               \
    qu->maxsize = 0;                                                            \
    qu->mask = 0;                                                               \
}                                                                               \
/* 调整容量 */                                                                  \
static inline void name##_resize(name *qu, uint32_t maxsize) {                  \
    T *pnew;                                                                    \
    uint32_t first;                                                             \
    ASSERTAB(maxsize < UINT32_MAX, "queue maxsize overflow.");                  \
    maxsize = (0 == maxsize) ? QUEUE_INIT_SIZE : pow2_ceil(maxsize < 2 ? 2 : maxsize);\
    ASSERTAB(maxsize >= qu->size, "max size must big than element count.");     \
    ASSERTAB(sizeof(T) <= SIZE_MAX / (size_t)maxsize, "byte size overflow.");   \
    MALLOC(pnew, sizeof(T) * (size_t)maxsize);                                  \
    /* 旧缓冲按 offset 环形排列，新缓冲从下标 0 起线性放置；环形最多跨两段，       \
       故两次拷贝足够。不逐元素拷:fsqu 的溢出层是在自旋锁内扩容的 */             \
    if (0 != qu->size) {                                                        \
        first = qu->maxsize - qu->offset;                                       \
        if (first > qu->size) {                                                 \
            first = qu->size;                                                   \
        }                                                                       \
        memcpy(pnew, qu->ptr + qu->offset, sizeof(T) * (size_t)first);          \
        if (qu->size > first) {                                                 \
            memcpy(pnew + first, qu->ptr, sizeof(T) * (size_t)(qu->size - first));\
        }                                                                       \
    }                                                                           \
    FREE(qu->ptr);                                                              \
    qu->ptr = pnew;                                                             \
    qu->offset = 0;                                                             \
    qu->maxsize = maxsize;                                                      \
    qu->mask = maxsize - 1;                                                     \
}                                                                               \
/* 从队头数第 pos 个,越界返回 NULL */                                           \
static inline T *name##_at(name *qu, uint32_t pos) {                            \
    if (pos >= qu->size) {                                                      \
        return NULL;                                                            \
    }                                                                           \
    return qu->ptr + name##_wrap(qu, qu->offset + pos);                         \
}                                                                               \
/* 队头元素,空队返回 NULL,不出队 */                                             \
static inline T *name##_peek(name *qu) {                                        \
    return (0 == qu->size) ? NULL : qu->ptr + qu->offset;                       \
}                                                                               \
/* 满了才走的冷路径。NOINLINE 承重别删;sarray 上照搬无效,别套用 */                \
NOINLINE static UNUSED void name##_grow(name *qu) {                            \
    ASSERTAB(qu->maxsize <= UINT32_MAX / 2, "queue maxsize overflow.");        \
    name##_resize(qu, qu->maxsize * 2);                                        \
}                                                                              \
/* 队尾追加,满了自动倍增 */                                                     \
static inline void name##_push(name *qu, T const *elem) {                      \
    if (qu->size == qu->maxsize) {                                              \
        name##_grow(qu);                                                       \
    }                                                                           \
    qu->ptr[name##_wrap(qu, qu->offset + qu->size)] = *elem;                   \
    qu->size++;                                                                 \
}                                                                               \
/* 队尾追加,满则失败且不扩容。有界队列共用这一处判定,不必各写一遍 */              \
static inline int32_t name##_trypush(name *qu, T const *elem) {                \
    if (name##_full(qu)) {                                                      \
        return ERR_FAILED;                                                      \
    }                                                                           \
    name##_push(qu, elem);                                                      \
    return ERR_OK;                                                              \
}                                                                               \
/* 弹出队头,空队返回 NULL */                                                    \
static inline T *name##_pop(name *qu) {                                         \
    T *elem;                                                                    \
    if (0 == qu->size) {                                                        \
        return NULL;                                                            \
    }                                                                           \
    elem = qu->ptr + qu->offset;                                                \
    qu->offset = name##_wrap(qu, qu->offset + 1);                               \
    qu->size--;                                                                 \
    return elem;                                                                \
}                                                                               \
/* 弹出队尾。与 push 配对即后进先出——对象池要的是这个:拿到的是刚归还、cache 最热的 */\
static inline T *name##_pop_back(name *qu) {                                    \
    if (0 == qu->size) {                                                        \
        return NULL;                                                            \
    }                                                                           \
    qu->size--;                                                                 \
    return qu->ptr + name##_wrap(qu, qu->offset + qu->size);                    \
}                                                                               \
/* 一次取走至多 max 条到 out，返回实际取到的条数。同 resize:环形最多跨两段，       \
   两次拷贝就够——调用方是在自旋锁内取的，逐个 pop 会把锁持有时间按元素个数拉长 */ \
static inline uint32_t name##_pop_batch(name *qu, T *out, uint32_t max) {        \
    uint32_t n = (max < qu->size) ? max : qu->size;                             \
    uint32_t first;                                                             \
    if (0 == n) {                                                               \
        return 0;                                                               \
    }                                                                           \
    first = qu->maxsize - qu->offset;                                           \
    if (first > n) {                                                            \
        first = n;                                                              \
    }                                                                           \
    memcpy(out, qu->ptr + qu->offset, sizeof(T) * (size_t)first);               \
    if (n > first) {                                                            \
        memcpy(out + first, qu->ptr, sizeof(T) * (size_t)(n - first));          \
    }                                                                           \
    qu->offset = name##_wrap(qu, qu->offset + n);                               \
    qu->size -= n;                                                              \
    return n;                                                                   \
}                                                                               \
/* 删除指定位置元素(保持顺序,后续元素整体前移) */                                 \
static inline void name##_del_at(name *qu, uint32_t pos) {                      \
    uint32_t src, dst, seg, rest;                                               \
    if (pos >= qu->size) {                                                      \
        return;                                                                 \
    }                                                                           \
    if (0 == pos) {                                                             \
        qu->offset = name##_wrap(qu, qu->offset + 1);                           \
        qu->size--;                                                             \
        return;                                                                 \
    }                                                                           \
    dst = name##_wrap(qu, qu->offset + pos);                                    \
    src = name##_wrap(qu, qu->offset + pos + 1);                                \
    rest = qu->size - pos - 1;                                                  \
    while (0 != rest) {                                                         \
        /* 一次搬到缓冲末尾或搬完为止,跨回绕时再绕回头部继续 */                   \
        seg = qu->maxsize - (src > dst ? src : dst);                            \
        if (seg > rest) {                                                       \
            seg = rest;                                                         \
        }                                                                       \
        memmove(qu->ptr + dst, qu->ptr + src, sizeof(T) * (size_t)seg);         \
        dst = name##_wrap(qu, dst + seg);                                       \
        src = name##_wrap(qu, src + seg);                                       \
        rest -= seg;                                                            \
    }                                                                           \
    qu->size--;                                                                 \
}

#endif//QUEUE_H_
