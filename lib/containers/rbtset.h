#ifndef RBTSET_H_
#define RBTSET_H_

#include "containers/rbtree.h"

// 有序集合：建在 RBT_DECL 上的持有型容器（同 hashset 之于 hashmap），
// 元素按值存进内部节点，节点走 MALLOC/FREE，可选留一个空闲节点池复用（默认关）。
// 模式同 RBT_DECL：RBT_UNIQUE 不允许重复；RBT_MULTI 允许重复，相等元素按插入先后排。
// rbtree.h 的 rbt_foreach / rbt_foreach_safe / rbt_foreach_equal 同样适用。
//
// 典型用法：
//   typedef struct { uint64_t due; uint64_t id; } tmr;
//   #define TMR_KEY(e) ((e)->due)
//   #define U64_LT(a, b) ((a) < (b))
//   RBTSET_DECL(tmr_set, tmr, uint64_t, TMR_KEY, U64_LT, RBT_MULTI)
//   tmr_set s; tmr_set_init(&s, NULL);
//   tmr t = { 100, 1 }; tmr_set_insert(&s, &t, NULL);
//   tmr *hit = tmr_set_find(&s, 100);
//   tmr *old = tmr_set_pop_first(&s);                  // 摘下最小，返回副本
//   tmr_set_free(&s);
//
// 契约：
//   1. K / KEYOF / LESS 口径同 rbtree.h 契约第 4 条，KEYOF 的 e 为 T const *。
//   2. 返回的元素指针在该元素被删除前一直有效（覆盖不换地址），期间不得改动参与比较的字段。
//   3. insert_or_assign 覆盖与 erase / extract / pop_first 交出的副本放在 s->spare，下一次覆盖或删除前有效；
//      它们都不调 elfree，只有 free 会。副本可以再当 item 传回来，唯独不能传给 erase。
//   4. erase 不校验 e 是否属于 s。遍历中删当前元素用 rbt_foreach_safe，其余元素指针不受影响。
//   5. _malloc 失败直接 exit，插入不会失败。非线程安全。
//   6. 开了池，池里的节点 ASan / PageHeap 看不见：删除后继续用旧指针报不出来；池里的内存到 free 或调低上限才归还。

// 入参写 T const * 而不是 const T *：理由同 hashmap.h。
// RBTSET_DECL(name, T, K, KEYOF, LESS, MODE)：name 生成的类型名，T 元素类型，K 键类型，KEYOF(e) 从元素取键，
// LESS(a, b) 键的小于比较，MODE 取 RBT_UNIQUE / RBT_MULTI
#define RBTSET_DECL(name, T, K, KEYOF, LESS, MODE)                                                   \
typedef struct name##_node {                                                                         \
    rbt_node rb;                                                                                     \
    T item;                                                                                          \
} name##_node;                                                                                       \
/* 节点取键，供 RBT_DECL */                                                                          \
static inline K name##_nkey(const name##_node *n) {                                                  \
    return KEYOF(&n->item);                                                                          \
}                                                                                                    \
RBT_DECL(name##_rb, name##_node, rb, K, name##_nkey, LESS, MODE)                                     \
typedef struct name {                                                                                \
    name##_rb tree;             /* 节点树，元素数也在里面 */                                         \
    void (*elfree)(void *item); /* free 时逐个调用，可为 NULL */                                     \
    rbt_node *freelist;         /* 空闲节点池，借节点的 rbt_right 串成链 */                          \
    size_t nfree;               /* 池里的节点数 */                                                   \
    size_t maxfree;             /* 池上限，0 为不留（默认） */                                       \
    T spare;                    /* 覆盖与删除交出的旧值副本 */                                       \
} name;                                                                                              \
/* 节点转元素指针，NULL 进 NULL 出 */                                                                \
static inline T *name##_item(name##_node *nd) {                                                      \
    return (NULL == nd) ? NULL : &nd->item;                                                          \
}                                                                                                    \
/* 元素指针转节点 */                                                                                 \
static inline name##_node *name##_node_of(T *e) {                                                    \
    return UPCAST(e, name##_node, item);                                                             \
}                                                                                                    \
/* 池非空就取链头（刚放回的，还在缓存里），空了才分配 */                                             \
static inline name##_node *name##_alloc(name *s) {                                                   \
    rbt_node *n = s->freelist;                                                                       \
    name##_node *nd;                                                                                 \
    if (NULL != n) {                                                                                 \
        s->freelist = n->rbt_right;                                                                  \
        s->nfree--;                                                                                  \
        return UPCAST(n, name##_node, rb);                                                           \
    }                                                                                                \
    MALLOC(nd, sizeof(name##_node));                                                                 \
    return nd;                                                                                       \
}                                                                                                    \
/* 池没满就留着复用，满了直接释放 */                                                                 \
static inline void name##_release(name *s, name##_node *nd) {                                        \
    if (s->nfree < s->maxfree) {                                                                     \
        nd->rb.rbt_right = s->freelist;                                                              \
        s->freelist = &nd->rb;                                                                       \
        s->nfree++;                                                                                  \
        return;                                                                                      \
    }                                                                                                \
    FREE(nd);                                                                                        \
}                                                                                                    \
/* 池里多于 keep 个的节点当场释放 */                                                                 \
static inline void name##_pool_shrink(name *s, size_t keep) {                                        \
    rbt_node *n;                                                                                     \
    name##_node *nd;                                                                                 \
    while (s->nfree > keep) {                                                                        \
        n = s->freelist;                                                                             \
        s->freelist = n->rbt_right;                                                                  \
        s->nfree--;                                                                                  \
        nd = UPCAST(n, name##_node, rb);                                                             \
        FREE(nd);                                                                                    \
    }                                                                                                \
}                                                                                                    \
/* elfree 在 free 时对每个元素调用，可为 NULL；节点池默认关 */                                       \
static inline void name##_init(name *s, void (*elfree)(void *item)) {                                \
    name##_rb_init(&s->tree);                                                                        \
    s->elfree = elfree;                                                                              \
    s->freelist = NULL;                                                                              \
    s->nfree = 0;                                                                                    \
    s->maxfree = 0;                                                                                  \
}                                                                                                    \
/* 逐个调 elfree 再释放节点（连池里的一起），之后可直接再用，池上限保持不变 */                       \
static inline void name##_free(name *s) {                                                            \
    rbt_node *n = rbt_first_postorder(&s->tree.root.rbt_root);                                       \
    rbt_node *next;                                                                                  \
    name##_node *nd;                                                                                 \
    while (NULL != n) {                                                                              \
        next = rbt_next_postorder(n);                                                                \
        nd = UPCAST(n, name##_node, rb);                                                             \
        if (NULL != s->elfree) {                                                                     \
            s->elfree(&nd->item);                                                                    \
        }                                                                                            \
        FREE(nd);                                                                                    \
        n = next;                                                                                    \
    }                                                                                                \
    name##_rb_init(&s->tree);                                                                        \
    name##_pool_shrink(s, 0);                                                                        \
}                                                                                                    \
/* 删除后最多留 maxfree 个节点给后面的插入复用；0 即不留（默认）；调低时当场释放多出来的 */          \
static inline void name##_set_pool(name *s, size_t maxfree) {                                        \
    s->maxfree = maxfree;                                                                            \
    name##_pool_shrink(s, maxfree);                                                                  \
}                                                                                                    \
/* 元素数 */                                                                                         \
static inline size_t name##_size(const name *s) {                                                    \
    return name##_rb_size(&s->tree);                                                                 \
}                                                                                                    \
/* 是否为空 */                                                                                       \
static inline int32_t name##_empty(const name *s) {                                                  \
    return name##_rb_empty(&s->tree);                                                                \
}                                                                                                    \
/* 最小元素，O(1)；空集合返回 NULL */                                                                \
static inline T *name##_first(const name *s) {                                                       \
    return name##_item(name##_rb_first(&s->tree));                                                   \
}                                                                                                    \
/* 最大元素，空集合返回 NULL */                                                                      \
static inline T *name##_last(const name *s) {                                                        \
    return name##_item(name##_rb_last(&s->tree));                                                    \
}                                                                                                    \
/* 中序后继，到头返回 NULL */                                                                        \
static inline T *name##_next(T *e) {                                                                 \
    return name##_item(name##_rb_next(name##_node_of(e)));                                           \
}                                                                                                    \
/* 中序前驱，到头返回 NULL */                                                                        \
static inline T *name##_prev(T *e) {                                                                 \
    return name##_item(name##_rb_prev(name##_node_of(e)));                                           \
}                                                                                                    \
/* 同 std::set：第一个不小于 k 的元素，没有返回 NULL */                                              \
static inline T *name##_lower_bound(const name *s, K k) {                                            \
    return name##_item(name##_rb_lower_bound(&s->tree, k));                                          \
}                                                                                                    \
/* 同 std::set：第一个大于 k 的元素，没有返回 NULL */                                                \
static inline T *name##_upper_bound(const name *s, K k) {                                            \
    return name##_item(name##_rb_upper_bound(&s->tree, k));                                          \
}                                                                                                    \
/* 与 k 相等的元素，MULTI 下是最早插入的那个；没有返回 NULL */                                       \
static inline T *name##_find(const name *s, K k) {                                                   \
    return name##_item(name##_rb_find(&s->tree, k));                                                 \
}                                                                                                    \
/* 是否存在与 k 相等的元素 */                                                                        \
static inline int32_t name##_contains(const name *s, K k) {                                          \
    return name##_rb_contains(&s->tree, k);                                                          \
}                                                                                                    \
/* e 的中序后继与 k 相等则返回它，否则 NULL（rbt_foreach_equal 用） */                               \
static inline T *name##_next_equal(T *e, K k) {                                                      \
    return name##_item(name##_rb_next_equal(name##_node_of(e), k));                                  \
}                                                                                                    \
/* 与 k 相等的元素个数 */                                                                            \
static inline size_t name##_count(const name *s, K k) {                                              \
    return name##_rb_count(&s->tree, k);                                                             \
}                                                                                                    \
/* 同 std::set::insert：UNIQUE 下已有相等元素就返回它、不覆盖，否则插入并返回新元素；MULTI 总是插入。 \
   found 非 NULL 时写入是否原已存在 */                                                               \
static inline T *name##_insert(name *s, T const *item, int32_t *found) {                             \
    rbt_insert_pos pos;                                                                              \
    name##_node *nd = name##_rb_insert_check(&s->tree, KEYOF(item), &pos);                           \
    if (NULL != nd) {                                                                                \
        SET_PTR(found, 1);                                                                           \
        return &nd->item;                                                                            \
    }                                                                                                \
    nd = name##_alloc(s);                                                                            \
    nd->item = *item;                                                                                \
    name##_rb_insert_commit(&s->tree, nd, &pos);                                                     \
    SET_PTR(found, 0);                                                                               \
    return &nd->item;                                                                                \
}                                                                                                    \
/* 同 std::map::insert_or_assign：UNIQUE 下已有相等元素就原位覆盖、返回旧值副本，否则插入并返回 NULL； \
   MULTI 总是插入，返回 NULL。item 可以就是 &s->spare */                                             \
static inline T *name##_insert_or_assign(name *s, T const *item) {                                   \
    rbt_insert_pos pos;                                                                              \
    T old;                                                                                           \
    name##_node *nd = name##_rb_insert_check(&s->tree, KEYOF(item), &pos);                           \
    if (NULL == nd) {                                                                                \
        nd = name##_alloc(s);                                                                        \
        nd->item = *item;                                                                            \
        name##_rb_insert_commit(&s->tree, nd, &pos);                                                 \
        return NULL;                                                                                 \
    }                                                                                                \
    old = nd->item;                                                                                  \
    nd->item = *item;                                                                                \
    s->spare = old;                                                                                  \
    return &s->spare;                                                                                \
}                                                                                                    \
/* 删 e 这一个元素，返回副本；e 不能是 &s->spare */                                                  \
static inline T *name##_erase(name *s, T *e) {                                                       \
    name##_node *nd = name##_node_of(e);                                                             \
    name##_rb_erase(&s->tree, nd);                                                                   \
    s->spare = nd->item;                                                                             \
    name##_release(s, nd);                                                                           \
    return &s->spare;                                                                                \
}                                                                                                    \
/* 摘下与 k 相等的元素（MULTI 下是最早插入的那个），返回副本；没有返回 NULL */                       \
static inline T *name##_extract(name *s, K k) {                                                      \
    name##_node *nd = name##_rb_find(&s->tree, k);                                                   \
    return (NULL == nd) ? NULL : name##_erase(s, &nd->item);                                         \
}                                                                                                    \
/* 摘下最小元素，返回副本；空集合返回 NULL */                                                        \
static inline T *name##_pop_first(name *s) {                                                         \
    name##_node *nd = name##_rb_first(&s->tree);                                                     \
    return (NULL == nd) ? NULL : name##_erase(s, &nd->item);                                         \
}

#endif//RBTSET_H_
