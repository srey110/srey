#ifndef HASHSET_H_
#define HASHSET_H_

#include "containers/hashmap.h"

// 类型化 hashset:HASHMAP_DECL 的薄包装(键即元素本身,不另存 value)。
// 语义与 name##_hm 的对应函数相同(对应关系见各函数前的注释),只是 contains 把指针压成 0/1。
//
// 典型用法:
//   HASHSET_DECL(id_set, uint64_t, id_hash, id_cmp)
//   id_set *s = id_set_new(0, NULL);
//
// 生成的 name##_hm_* 也可直接用(需要 get / get_set / probe / 指定 hash 的变体时)。

// HASHSET_DECL(name, T, HASHFN, CMPFN):参数含义同 HASHMAP_DECL
#define HASHSET_DECL(name, T, HASHFN, CMPFN)                                   \
HASHMAP_DECL(name##_hm, T, HASHFN, CMPFN)                                       \
typedef name##_hm name;                                                         \
/* 同 name##_hm_new */                                                          \
static inline name *name##_new(size_t cap, void (*elfree)(void *item)) {       \
    return name##_hm_new(cap, elfree);                                         \
}                                                                              \
/* 同 name##_hm_free */                                                         \
static inline void name##_free(name *s) {                                       \
    name##_hm_free(s);                                                          \
}                                                                               \
/* 同 name##_hm_clear */                                                        \
static inline void name##_clear(name *s, int32_t update_cap) {                  \
    name##_hm_clear(s, update_cap);                                             \
}                                                                               \
/* 同 name##_hm_size */                                                         \
static inline uint32_t name##_size(const name *s) {                            \
    return name##_hm_size(s);                                                  \
}                                                                               \
/* 同 name##_hm_elsize */                                                       \
static inline uint32_t name##_elsize(const name *s) {                           \
    return name##_hm_elsize(s);                                                 \
}                                                                               \
/* 同 name##_hm_oom */                                                          \
static inline int32_t name##_oom(const name *s) {                               \
    return name##_hm_oom(s);                                                    \
}                                                                               \
/* 即 name##_hm_set:已存在则原位覆盖并返回旧元素的副本,否则返回 NULL */         \
static inline T *name##_add(name *s, T const *item) {                           \
    return name##_hm_set(s, item);                                              \
}                                                                               \
/* 即 name##_hm_get,把指针压成 0/1 */                                           \
static inline int32_t name##_contains(const name *s, T const *item) {           \
    return NULL != name##_hm_get(s, item);                                      \
}                                                                               \
/* 即 name##_hm_delete:返回被删元素的副本,找不到返回 NULL */                    \
static inline T *name##_remove(name *s, T const *item) {                        \
    return name##_hm_delete(s, item);                                           \
}                                                                               \
/* 同 name##_hm_scan */                                                         \
static inline int32_t name##_scan(name *s,                                      \
        int32_t (*iter)(T const *item, void *udata), void *udata) {             \
    return name##_hm_scan(s, iter, udata);                                      \
}                                                                               \
/* 同 name##_hm_iter */                                                         \
static inline int32_t name##_iter(name *s, size_t *i, T **item) {               \
    return name##_hm_iter(s, i, item);                                          \
}

#endif//HASHSET_H_
