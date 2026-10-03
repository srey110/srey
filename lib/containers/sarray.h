#ifndef SARRAY_H_
#define SARRAY_H_

#include "base/base.h"

// 动态数组(连续存储、倍增扩容)。元素类型编译期固化，故搬运是结构体赋值而非运行期 memcpy。
//
// 典型用法：
//   typedef struct { int a; } my_elem;
//   ARR_DECL(my_arr, my_elem)
//   my_arr arr; my_arr_init(&arr, 0);        // 0 = 延迟分配，首次 push_back 才申请
//   my_elem e = { 1 }; my_arr_push_back(&arr, &e);
//   my_elem *p = my_arr_at(&arr, -1);         // 负下标从尾部反向索引
//   my_arr_free(&arr);
//
// 元素一律按指针传入，取出一律返回指针；pos 为 int32_t，负数从尾部反向索引。

#define ARRAY_INIT_SIZE 32 // 默认初始容量

// 入参写 T const * 而不是 const T *:T 是指针类型时,后者会被解析成指向 const 的指针。
// ARR_DECL(name, T)：name 生成的类型名，T 元素类型
#define ARR_DECL(name, T)                                                      \
typedef struct { uint32_t size; uint32_t maxsize; T *ptr; } name;               \
/* 调整容量,0 取 ARRAY_INIT_SIZE;不得小于当前元素数 */                          \
static inline void name##_resize(name *arr, uint32_t maxsize) {                 \
    ASSERTAB(maxsize < UINT32_MAX, "array maxsize overflow.");                  \
    maxsize = (0 == maxsize) ? ARRAY_INIT_SIZE : (uint32_t)ROUND_UP(maxsize, 2);\
    ASSERTAB(maxsize >= arr->size, "max size must big than element count.");    \
    ASSERTAB(sizeof(T) <= SIZE_MAX / (size_t)maxsize, "byte size overflow.");   \
    REALLOC(arr->ptr, arr->ptr, sizeof(T) * (size_t)maxsize);                   \
    arr->maxsize = maxsize;                                                     \
}                                                                               \
/* 0 为延迟分配,首次 push_back / add 才申请 */                                  \
static inline void name##_init(name *arr, uint32_t maxsize) {                   \
    arr->size = 0;                                                              \
    if (0 == maxsize) {                                                         \
        /* 延迟分配:不预付内存,首次 push_back/add 命中 size==maxsize 才申请 */   \
        arr->maxsize = 0;                                                       \
        arr->ptr = NULL;                                                        \
        return;                                                                 \
    }                                                                           \
    ASSERTAB(maxsize < UINT32_MAX, "array maxsize overflow.");                  \
    arr->maxsize = (uint32_t)ROUND_UP(maxsize, 2);                              \
    ASSERTAB(sizeof(T) <= SIZE_MAX / (size_t)arr->maxsize, "byte size overflow.");\
    MALLOC(arr->ptr, sizeof(T) * (size_t)arr->maxsize);                         \
}                                                                               \
/* 释放缓冲并复位,之后可直接再 push_back */                                     \
static inline void name##_free(name *arr) {                                     \
    FREE(arr->ptr);                                                             \
    /* 长度字段留旧值的话,free 后再 push_back 会绕过扩容分支往 NULL 上写 */      \
    arr->size = 0;                                                              \
    arr->maxsize = 0;                                                           \
}                                                                               \
/* 满了就倍增,push_back / add 内部调用 */                                       \
static inline void name##_grow_if_full(name *arr) {                             \
    if (arr->size == arr->maxsize) {                                            \
        ASSERTAB(arr->maxsize <= UINT32_MAX / 2, "array maxsize overflow.");    \
        name##_resize(arr, arr->maxsize * 2);                                   \
    }                                                                           \
}                                                                               \
/* 负下标归一(-1 即末元素)并校验范围。inclusive 非 0 时允许等于 size ——              \
   那是 add 的插入位,其余入口一律要求 < size。                                  \
   归一后仍为负的 pos 转成无符号必 >= 2^31、不小于 lim,故一次无符号比较同时挡住 \
   负数与越界;前提同 (int32_t)arr->size:size 不超过 INT32_MAX */                \
static inline uint32_t name##_norm_pos(const name *arr, int32_t pos, int32_t inclusive) {\
    uint32_t lim;                                                               \
    if (pos < 0) {                                                              \
        pos += (int32_t)arr->size;                                              \
    }                                                                           \
    lim = (0 != inclusive) ? arr->size + 1 : arr->size;                         \
    ASSERTAB((uint32_t)pos < lim, "array pos out of range.");                   \
    return (uint32_t)pos;                                                       \
}                                                                               \
/* 元素数 */                                                                    \
static inline uint32_t name##_size(const name *arr) { return arr->size; }       \
/* 容量(延迟分配态为 0) */                                                      \
static inline uint32_t name##_capacity(const name *arr) { return arr->maxsize; } \
/* 元素字节数 sizeof(T) */                                                      \
static inline uint32_t name##_elsize(const name *arr) { (void)arr; return (uint32_t)sizeof(T); } \
/* 是否为空 */                                                                  \
static inline int32_t name##_empty(const name *arr) { return 0 == arr->size; }  \
/* 清空,不释放缓冲 */                                                           \
static inline void name##_clear(name *arr) { arr->size = 0; }                   \
/* 取 pos 处元素,越界 ASSERTAB 中止 */                                          \
static inline T *name##_at(name *arr, int32_t pos) {                            \
    return arr->ptr + name##_norm_pos(arr, pos, 0);                             \
}                                                                               \
/* 首元素,空数组返回 NULL */                                                    \
static inline T *name##_front(name *arr) {                                      \
    return (0 == arr->size) ? NULL : arr->ptr;                                  \
}                                                                               \
/* 末元素,空数组返回 NULL */                                                    \
static inline T *name##_back(name *arr) {                                       \
    return (0 == arr->size) ? NULL : arr->ptr + (arr->size - 1);                \
}                                                                               \
/* 尾部追加,满了自动倍增 */                                                     \
static inline void name##_push_back(name *arr, T const *elem) {                \
    name##_grow_if_full(arr);                                                   \
    arr->ptr[arr->size] = *elem;                                               \
    arr->size++;                                                                \
}                                                                               \
/* 弹出末元素,空数组返回 NULL */                                                \
static inline T *name##_pop_back(name *arr) {                                   \
    if (0 == arr->size) {                                                       \
        return NULL;                                                            \
    }                                                                           \
    arr->size--;                                                                \
    return arr->ptr + arr->size;                                                \
}                                                                               \
/* 插到 pos 处,后面的依次后移;pos 可以等于 size(即追加),再往外 ASSERTAB 中止 */ \
static inline void name##_add(name *arr, T const *elem, int32_t pos) {         \
    uint32_t p = name##_norm_pos(arr, pos, 1);/* 插入位允许等于 size */          \
    name##_grow_if_full(arr);                                                   \
    if (p < arr->size) {                                                        \
        memmove(arr->ptr + p + 1, arr->ptr + p, sizeof(T) * (size_t)(arr->size - p));\
    }                                                                           \
    arr->ptr[p] = *elem;                                                       \
    arr->size++;                                                                \
}                                                                               \
/* 删 pos 处,后面的依次前移;越界 ASSERTAB 中止 */                               \
static inline void name##_del(name *arr, int32_t pos) {                         \
    uint32_t p = name##_norm_pos(arr, pos, 0);                                  \
    arr->size--;                                                                \
    if (p < arr->size) {                                                        \
        memmove(arr->ptr + p, arr->ptr + p + 1, sizeof(T) * (size_t)(arr->size - p));\
    }                                                                           \
}                                                                               \
/* 用末元素顶替被删位置,不保持顺序 */                                            \
static inline void name##_del_nomove(name *arr, int32_t pos) {                  \
    uint32_t p = name##_norm_pos(arr, pos, 0);                                  \
    arr->size--;                                                                \
    if (p < arr->size) {                                                        \
        arr->ptr[p] = arr->ptr[arr->size];                                      \
    }                                                                           \
}                                                                               \
/* 交换两个元素;越界 ASSERTAB 中止 */                                           \
static inline void name##_swap(name *arr, int32_t pos1, int32_t pos2) {         \
    uint32_t p1 = name##_norm_pos(arr, pos1, 0);                                \
    uint32_t p2 = name##_norm_pos(arr, pos2, 0);                                \
    T tmp;                                                                      \
    if (p1 == p2) {                                                             \
        return;                                                                 \
    }                                                                           \
    tmp = arr->ptr[p1];                                                         \
    arr->ptr[p1] = arr->ptr[p2];                                                \
    arr->ptr[p2] = tmp;                                                         \
}

#endif//SARRAY_H_
