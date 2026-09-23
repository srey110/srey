#ifndef HEAP_H_
#define HEAP_H_

#include "base/macro.h"

// 数组式二叉堆(最小堆)。元素由调用方持有，堆里只存指针，故元素地址稳定、可按引用 O(log n) 删除。
// 元素内需有一个 uint32_t 字段回指自己在堆中的下标，由堆维护，调用方不得改动。
//
// 典型用法：
//   typedef struct { uint32_t hidx; uint64_t due; } my_node;
//   #define MY_LT(a, b) ((a)->due < (b)->due)      // a 优先于 b 时为真
//   HEAP_DECL(due_heap, my_node, hidx, MY_LT)
//   due_heap h; due_heap_init(&h, 0);            // 0 = 延迟分配，首次 insert 才申请；free 后可再 insert（同 ARR/QUE）
//   my_node n; due_heap_insert(&h, &n);
//   due_heap_remove(&h, &n);                       // 按引用 O(log n) 删除
//   my_node *top = due_heap_min(&h);               // 堆顶，空堆返 NULL
//
// 契约：一个元素同时只属于一个堆；remove 不校验成员关系，由调用方保证元素确实在该堆内
//       （同 list_remove）；元素在 insert 与 remove 之间不得移动地址。

#define HEAP_INIT_SIZE 16 // 延迟分配时首次 insert 的容量

// HEAP_DECL(name, T, IDXFIELD, LT)：name 生成的类型名，T 元素类型，
// IDXFIELD 元素内回指下标的字段名，LT(a,b) 比较宏（a、b 均为 T *）
#define HEAP_DECL(name, T, IDXFIELD, LT)                                       \
typedef struct { T **p; uint32_t size; uint32_t maxsize; } name;                \
static inline void name##_init(name *h, uint32_t maxsize) {                     \
    h->size = 0;                                                                \
    h->maxsize = maxsize;                                                       \
    h->p = NULL;                                                                \
    if (0 != maxsize) {                                                         \
        MALLOC(h->p, sizeof(T *) * maxsize);                                    \
    }                                                                           \
}                                                                               \
static inline void name##_free(name *h) {                                       \
    FREE(h->p);                                                                 \
    h->size = 0;                                                                \
    h->maxsize = 0;                                                             \
}                                                                               \
static inline uint32_t name##_size(const name *h) { return h->size; }           \
static inline uint32_t name##_capacity(const name *h) { return h->maxsize; }    \
static inline int32_t name##_empty(const name *h) { return 0 == h->size; }      \
static inline T *name##_min(const name *h) {                                    \
    return (0 == h->size) ? NULL : h->p[0];                                     \
}                                                                               \
/* 上浮:把 pos 处元素往根方向挪到合适位置 */                                    \
static inline void name##_sift_up(name *h, uint32_t pos) {                      \
    T *e = h->p[pos];                                                           \
    uint32_t parent;                                                            \
    while (pos > 0) {                                                           \
        parent = (pos - 1) / 2;                                                 \
        if (!(LT(e, h->p[parent]))) {                                           \
            break;                                                              \
        }                                                                       \
        h->p[pos] = h->p[parent];                                               \
        h->p[pos]->IDXFIELD = pos;                                              \
        pos = parent;                                                           \
    }                                                                           \
    h->p[pos] = e;                                                              \
    e->IDXFIELD = pos;                                                          \
}                                                                               \
/* 下沉:把 pos 处元素往叶方向挪到合适位置 */                                    \
static inline void name##_sift_down(name *h, uint32_t pos) {                    \
    T *e = h->p[pos];                                                           \
    uint32_t child;                                                             \
    for (;;) {                                                                  \
        child = pos * 2 + 1;                                                    \
        if (child >= h->size) {                                                 \
            break;                                                              \
        }                                                                       \
        if (child + 1 < h->size && LT(h->p[child + 1], h->p[child])) {           \
            child++;                                                            \
        }                                                                       \
        if (!(LT(h->p[child], e))) {                                            \
            break;                                                              \
        }                                                                       \
        h->p[pos] = h->p[child];                                                \
        h->p[pos]->IDXFIELD = pos;                                              \
        pos = child;                                                            \
    }                                                                           \
    h->p[pos] = e;                                                              \
    e->IDXFIELD = pos;                                                          \
}                                                                               \
static inline void name##_insert(name *h, T *elem) {                            \
    if (h->size == h->maxsize) {                                                \
        ASSERTAB(h->maxsize <= UINT32_MAX / 2, "heap maxsize overflow.");       \
        h->maxsize = (0 == h->maxsize) ? HEAP_INIT_SIZE : h->maxsize * 2;       \
        REALLOC(h->p, h->p, sizeof(T *) * h->maxsize);                          \
    }                                                                           \
    h->p[h->size] = elem;                                                       \
    elem->IDXFIELD = h->size;                                                   \
    h->size++;                                                                  \
    name##_sift_up(h, h->size - 1);                                             \
}                                                                               \
static inline void name##_remove(name *h, T *elem) {                            \
    uint32_t pos = elem->IDXFIELD;                                              \
    T *last;                                                                    \
    h->size--;                                                                  \
    if (pos == h->size) {                                                       \
        return;                                                                 \
    }                                                                           \
    /* 拿末位填坑,再按它与父的大小关系决定上浮还是下沉 */                       \
    last = h->p[h->size];                                                       \
    h->p[pos] = last;                                                           \
    last->IDXFIELD = pos;                                                       \
    if (pos > 0 && LT(last, h->p[(pos - 1) / 2])) {                              \
        name##_sift_up(h, pos);                                                 \
    } else {                                                                    \
        name##_sift_down(h, pos);                                               \
    }                                                                           \
}                                                                               \
static inline void name##_dequeue(name *h) {                                    \
    if (0 != h->size) {                                                         \
        name##_remove(h, h->p[0]);                                              \
    }                                                                           \
}

#endif//HEAP_H_
