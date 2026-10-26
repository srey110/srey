#ifndef RADIX_H_
#define RADIX_H_

#include "base/base.h"
#include "containers/sarray.h"

// 类型化压缩基数树
//
// 典型用法：
//   RADIX_DECL(seg_map, uint32_t)
//   seg_map m;
//   seg_map_init(&m, NULL);
//   uint32_t v = 42;
//   seg_map_set(&m, "user", 4, &v, NULL);
//   uint32_t *p = seg_map_get(&m, "user", 4);
//   seg_map_iter it;
//   seg_map_iter_init(&it, &m);
//   for (seg_map_iter_first(&it); seg_map_iter_valid(&it); seg_map_iter_next(&it)) { ... }
//   seg_map_iter_free(&it);
//   seg_map_free(&m);
//
// 契约：
//   1. key 二进制安全，可含 '\0'，不以 '\0' 结尾；set 时复制。
//   2. 空 key 合法，挂在根节点上。
//   3. 字典序按无符号字节比较，短 key 排在它的长前缀之前。
//   4. set / del 的旧值只写副本到 old，不调用 elfree；只有 clear / free 会对每个
//      有值节点调 elfree。与 HASHMAP_DECL 的 elfree 契约一致。
//   5. get / iter_val 返回节点内值指针，下一次增删 / clear / free 后失效。
//   6. 迭代器使用前必须 iter_init，用完必须 iter_free；遍历期间增删由 version
//      检出并使迭代器失效。iter_del 是唯一允许边遍历边删当前项的入口，删完游标已在
//      下一项，别再调 iter_next（否则跳过一项）。
//   7. 回调遍历的 cb 返回 0 即停止；回调内禁止增删。walk 返回 ERR_OK 走完，
//      ERR_FAILED 表示被回调停下或遍历期间表被改。
//   8. 不做线程同步；注册期写完再并发读由调用方保证。
//   9. 单条 key 须短于 UINT32_MAX 字节（节点前缀长度用 uint32_t 存），set 入口用 ASSERTAB 拒绝。
//
// 实现约束：
//   - 子节点数组、迭代器路径栈、key 缓冲、删除路径栈都用 sarray.h 的 ARR_DECL，
//     不手写扩容/搬移。
//   - 只保留 radix 特有的 LCP、分裂、合并；其余分配、释放、排序插入、栈增长走现有容器。

#define RADIX_MAX_CHILD 256

// RADIX_DECL(name, T)：name 生成的类型名，T 元素类型。
// 入参写 T const * 而不是 const T *：T 是指针类型时，后者会被解析成指向 const 的指针。
#define RADIX_DECL(name, T)                                                   \
typedef struct name name;                                                      \
typedef struct name##_node name##_node;                                        \
typedef struct name##_iter name##_iter;                                        \
ARR_DECL(name##_kids, name##_node *)                                           \
ARR_DECL(name##_node_stack, name##_node *)                                     \
ARR_DECL(name##_bytebuf, uint8_t)                                              \
ARR_DECL(name##_link_stack, name##_node **)                                    \
struct name##_node {                                                           \
    uint32_t prefix_len;                                                       \
    uint8_t has_value;                                                         \
    uint8_t edge;                                                              \
    uint8_t pad[2];                                                            \
    uint8_t *prefix;                                                           \
    name##_kids child;                                                         \
    T value;                                                                   \
};                                                                             \
struct name##_iter {                                                           \
    uint32_t version;                                                          \
    const name *r;                                                             \
    name##_node *cur;                                                          \
    size_t key_len;                                                            \
    name##_node_stack stack;                                                   \
    name##_bytebuf key;                                                        \
};                                                                             \
struct name {                                                                  \
    uint32_t version;                                                          \
    void (*elfree)(void *item);                                                \
    name##_node *root;                                                         \
    size_t size;                                                               \
    size_t node_count;                                                         \
    size_t max_key_len;                                                        \
};                                                                             \
typedef int32_t (*name##_walk_cb)(const uint8_t *key, size_t klen, T *val, void *ud); \
/* key 指向迭代器内部缓冲，下一次迭代操作后失效 */                             \
static inline const uint8_t *name##_iter_key(name##_iter *it, size_t *klen);   \
/* 子节点数；布局藏在 ARR 里，外部不直接碰 size */                             \
static inline size_t name##_child_count(const name##_node *n) {                \
    return n->child.size;                                                      \
}                                                                              \
/* 按下标取子节点；调用方保证 i < child_count */                               \
static inline name##_node *name##_child_at(const name##_node *n, size_t i) {   \
    return n->child.ptr[i];                                                    \
}                                                                              \
/* 新建节点：复制前缀、记录 edge，node_count++ */                              \
static inline name##_node *name##_node_new(name *r, const uint8_t *prefix,     \
                                           size_t plen, uint8_t edge) {        \
    name##_node *n;                                                            \
    MALLOC(n, sizeof(*n));                                                     \
    ZERO(n, sizeof(*n));                                                       \
    name##_kids_init(&n->child, 0);                                            \
    n->prefix_len = (uint32_t)plen;                                            \
    n->edge = edge;                                                            \
    if (plen > 0) {                                                            \
        MALLOC(n->prefix, plen);                                               \
        memcpy(n->prefix, prefix, plen);                                       \
    }                                                                          \
    r->node_count++;                                                           \
    return n;                                                                  \
}                                                                              \
/* 只释放节点自身；子节点由调用方先摘或先处理 */                               \
static inline void name##_node_free(name##_node *n) {                          \
    name##_kids_free(&n->child);                                               \
    FREE(n->prefix);                                                           \
    FREE(n);                                                                   \
}                                                                              \
/* 二分找 edge；未找到时 idx 是插入位 */                                       \
static inline name##_node **name##_child_slot(name##_node *n, uint8_t c,       \
                                               size_t *idx) {                  \
    size_t lo = 0;                                                             \
    size_t hi = name##_child_count(n);                                         \
    size_t mid;                                                                \
    while (lo < hi) {                                                          \
        mid = lo + ((hi - lo) >> 1);                                           \
        if (name##_child_at(n, mid)->edge < c) {                               \
            lo = mid + 1;                                                      \
        } else {                                                               \
            hi = mid;                                                          \
        }                                                                      \
    }                                                                          \
    *idx = lo;                                                                 \
    if (lo < name##_child_count(n) && name##_child_at(n, lo)->edge == c) {     \
        return &n->child.ptr[lo];                                              \
    }                                                                          \
    return NULL;                                                               \
}                                                                              \
/* 有序插入；首次分配按 2 个槽起（resize 向上取偶），不预占 32 槽 */           \
static inline void name##_child_add(name##_node *n, size_t idx,                \
                                    name##_node *child) {                      \
    ASSERTAB(name##_child_count(n) < RADIX_MAX_CHILD, "radix child overflow."); \
    if (NULL == n->child.ptr) {                                                \
        name##_kids_resize(&n->child, 1);                                      \
    }                                                                          \
    name##_kids_add(&n->child, &child, (int32_t)idx);                          \
}                                                                              \
/* 删除后空了就释放数组，避免节点长期占空缓冲 */                               \
static inline void name##_child_remove(name##_node *n, size_t idx) {           \
    name##_kids_del(&n->child, (int32_t)idx);                                  \
    if (0 == name##_child_count(n)) {                                          \
        name##_kids_free(&n->child);                                           \
    }                                                                          \
}                                                                              \
/* 回溯用：按子节点指针找兄弟下标 */                                           \
static inline size_t name##_child_index(const name##_node *n,                  \
                                        const name##_node *child) {            \
    for (size_t i = 0; i < name##_child_count(n); i++) {                       \
        if (name##_child_at(n, i) == child) {                                  \
            return i;                                                          \
        }                                                                      \
    }                                                                          \
    ASSERTAB(0, "radix child not found.");                                     \
    return 0;                                                                  \
}                                                                              \
/* 在 common 处分叉：后半段连同值/子节点下移，n 只留前半段 */                  \
static inline void name##_split(name *r, name##_node *n, size_t common) {      \
    ASSERTAB(common < n->prefix_len, "bad radix split.");                      \
    name##_node *c = name##_node_new(r, n->prefix + common + 1,                \
                                     n->prefix_len - common - 1, n->prefix[common]); \
    c->has_value = n->has_value;                                               \
    c->value = n->value;                                                       \
    c->child = n->child;                                                       \
    /* n 的旧子节点数组整体转给 c，n 重新挂一个子节点 */                       \
    name##_kids_init(&n->child, 0);                                            \
    name##_child_add(n, 0, c);                                                 \
    n->has_value = 0;                                                          \
    /* n 缩短到 common，旧前缀拷到新缓冲 */                                    \
    uint8_t *old = n->prefix;                                                  \
    n->prefix_len = (uint32_t)common;                                          \
    if (common > 0) {                                                          \
        MALLOC(n->prefix, common);                                             \
        memcpy(n->prefix, old, common);                                        \
    } else {                                                                   \
        n->prefix = NULL;                                                      \
    }                                                                          \
    FREE(old);                                                                 \
}                                                                              \
/* 无值单子节点合并：child 的 edge/前缀接回 n，接管其值/子节点 */              \
static inline void name##_merge(name *r, name##_node *n) {                     \
    ASSERTAB(0 == n->has_value && 1 == name##_child_count(n),                  \
             "bad radix merge.");                                              \
    name##_node *c = name##_child_at(n, 0);                                    \
    /* 先拼新前缀，再释放 n 的旧前缀和旧子节点数组 */                          \
    size_t nlen = (size_t)n->prefix_len + 1 + c->prefix_len;                   \
    uint8_t *np;                                                               \
    MALLOC(np, nlen);                                                          \
    if (n->prefix_len > 0) {                                                   \
        memcpy(np, n->prefix, n->prefix_len);                                  \
    }                                                                          \
    np[n->prefix_len] = c->edge;                                               \
    if (c->prefix_len > 0) {                                                   \
        memcpy(np + n->prefix_len + 1, c->prefix, c->prefix_len);              \
    }                                                                          \
    FREE(n->prefix);                                                           \
    name##_kids_free(&n->child);                                               \
    n->prefix = np;                                                            \
    n->prefix_len = (uint32_t)nlen;                                            \
    n->has_value = c->has_value;                                               \
    n->value = c->value;                                                       \
    n->child = c->child;                                                       \
    /* c 的前缀已拼进 np，子节点数组整体转给 n */                              \
    name##_kids_init(&c->child, 0);                                            \
    FREE(c->prefix);                                                           \
    FREE(c);                                                                   \
    r->node_count--;                                                           \
}                                                                              \
/* 零初始化；elfree 只在 clear/free 时调用 */                                  \
static inline void name##_init(name *r, void (*elfree)(void *item)) {          \
    ZERO(r, sizeof(*r));                                                       \
    r->elfree = elfree;                                                        \
}                                                                              \
/* set / try_set / get_or_insert 共用的一次下降：找到或建出 key 的节点。       \
   新建时写入 *val、*created 置 1；已存在时不动值、*created 置 0。返回节点内的值槽 */ \
static inline T *name##_slot(name *r, const void *key, size_t klen,            \
                             T const *val, int32_t *created) {                 \
    ASSERTAB(NULL != val, "radix set value is NULL.");                         \
    ASSERTAB(klen < UINT32_MAX, "radix key too long.");                        \
    const uint8_t *p = (const uint8_t *)key;                                   \
    size_t left = klen;                                                        \
    name##_node **link = &r->root;                                             \
    name##_node *hit = NULL;                                                   \
    if (NULL == *link) {                                                       \
        /* 空树：整条 key 就是根前缀 */                                        \
        hit = name##_node_new(r, p, left, 0);                                  \
        *link = hit;                                                           \
    }                                                                          \
    name##_node *n;                                                            \
    name##_node **slot;                                                        \
    size_t max, common, idx;                                                   \
    while (NULL == hit) {                                                      \
        n = *link;                                                             \
        max = n->prefix_len < left ? n->prefix_len : left;                     \
        common = _radix_lcp(n->prefix, p, max);                                \
        if (common < n->prefix_len) {                                          \
            /* 前缀分叉：n 缩短后，p/left 只前进已匹配部分 */                  \
            name##_split(r, n, common);                                        \
        }                                                                      \
        if (common > 0) {                                                      \
            p += common;                                                       \
            left -= common;                                                    \
        }                                                                      \
        if (0 == left) {                                                       \
            if (n->has_value) {                                                \
                *created = 0;                                                  \
                return &n->value;                                              \
            }                                                                  \
            hit = n;                                                           \
            break;                                                             \
        }                                                                      \
        slot = name##_child_slot(n, p[0], &idx);                               \
        if (NULL == slot) {                                                    \
            /* 没有对应 edge：新建叶子，剩余 key 放 child->prefix */           \
            hit = name##_node_new(r, p + 1, left - 1, p[0]);                   \
            name##_child_add(n, idx, hit);                                     \
            break;                                                             \
        }                                                                      \
        link = slot;                                                           \
        p++;                                                                   \
        left--;                                                                \
    }                                                                          \
    hit->has_value = 1;                                                        \
    hit->value = *val;                                                         \
    r->size++;                                                                 \
    r->version++;                                                              \
    if (klen > r->max_key_len) {                                               \
        r->max_key_len = klen;                                                 \
    }                                                                          \
    *created = 1;                                                              \
    return &hit->value;                                                        \
}                                                                              \
/* 插入或替换；1=新插入，0=替换；旧值副本写 old，不调 elfree */                \
static inline int32_t name##_set(name *r, const void *key, size_t klen,        \
                                 T const *val, T *old) {                       \
    int32_t created;                                                           \
    T *slot = name##_slot(r, key, klen, val, &created);                        \
    if (0 == created) {                                                        \
        if (NULL != old) {                                                     \
            *old = *slot;                                                      \
        }                                                                      \
        *slot = *val;                                                          \
        r->version++;                                                          \
    }                                                                          \
    return created;                                                            \
}                                                                              \
/* 逐节点比前缀；child->prefix 不含父边，下降时单独消费 edge */                \
static inline T *name##_get(const name *r, const void *key, size_t klen) {     \
    const uint8_t *p = (const uint8_t *)key;                                   \
    size_t left = klen;                                                        \
    name##_node *n = r->root;                                                  \
    size_t idx;                                                                \
    name##_node **slot;                                                        \
    while (NULL != n) {                                                        \
        if (left < n->prefix_len) {                                            \
            return NULL;                                                       \
        }                                                                      \
        if (n->prefix_len > 0                                                  \
            && 0 != memcmp(n->prefix, p, n->prefix_len)) {                     \
            return NULL;                                                       \
        }                                                                      \
        if (n->prefix_len > 0) {                                               \
            p += n->prefix_len;                                                \
            left -= n->prefix_len;                                             \
        }                                                                      \
        if (0 == left) {                                                       \
            return n->has_value ? &n->value : NULL;                            \
        }                                                                      \
        slot = name##_child_slot(n, p[0], &idx);                               \
        if (NULL == slot) {                                                    \
            return NULL;                                                       \
        }                                                                      \
        n = *slot;                                                             \
        p++;                                                                   \
        left--;                                                                \
    }                                                                          \
    return NULL;                                                               \
}                                                                              \
/* 语义同 get != NULL */                                                       \
static inline int32_t name##_contains(const name *r, const void *key,          \
                                      size_t klen) {                           \
    return NULL != name##_get(r, key, klen);                                   \
}                                                                              \
/* 只插入；已存在不覆盖，返回 0 */                                             \
static inline int32_t name##_try_set(name *r, const void *key, size_t klen,    \
                                     T const *val) {                           \
    int32_t created;                                                           \
    (void)name##_slot(r, key, klen, val, &created);                            \
    return created;                                                            \
}                                                                              \
/* 已存在直接返回；否则插入 def（NULL 用零值） */                              \
static inline T *name##_get_or_insert(name *r, const void *key, size_t klen,   \
                                      T const *def, int32_t *created) {        \
    T tmp;                                                                     \
    if (NULL != def) {                                                         \
        tmp = *def;                                                            \
    } else {                                                                   \
        ZERO(&tmp, sizeof(tmp));                                               \
    }                                                                          \
    int32_t was_new;                                                           \
    T *slot = name##_slot(r, key, klen, &tmp, &was_new);                       \
    if (NULL != created) {                                                     \
        *created = was_new;                                                    \
    }                                                                          \
    return slot;                                                               \
}                                                                              \
/* 删 key 并清理空节点；1=删到，0=不存在；旧值副本写 old */                    \
static inline int32_t name##_del(name *r, const void *key, size_t klen,        \
                                 T *old) {                                     \
    name##_node *n = r->root;                                                  \
    if (NULL == n) {                                                           \
        return 0;                                                              \
    }                                                                          \
    const uint8_t *p = (const uint8_t *)key;                                   \
    size_t left = klen;                                                        \
    name##_node **root_link = &r->root;                                        \
    /* 路径栈记指向各层节点的槽（第 i 层节点就是 *links[i]），删除/合并时回退用 */ \
    name##_link_stack links;                                                   \
    name##_link_stack_init(&links, 16);                                        \
    name##_link_stack_push_back(&links, &root_link);                           \
    size_t idx;                                                                \
    name##_node **slot;                                                        \
    name##_node *parent;                                                       \
    for (;;) {                                                                 \
        if (left < n->prefix_len) {                                            \
            goto not_found;                                                    \
        }                                                                      \
        if (n->prefix_len > 0                                                  \
            && 0 != memcmp(n->prefix, p, n->prefix_len)) {                     \
            goto not_found;                                                    \
        }                                                                      \
        if (n->prefix_len > 0) {                                               \
            p += n->prefix_len;                                                \
            left -= n->prefix_len;                                             \
        }                                                                      \
        if (0 == left) {                                                       \
            break;                                                             \
        }                                                                      \
        slot = name##_child_slot(n, p[0], &idx);                               \
        if (NULL == slot) {                                                    \
            goto not_found;                                                    \
        }                                                                      \
        n = *slot;                                                             \
        name##_link_stack_push_back(&links, &slot);                            \
        p++;                                                                   \
        left--;                                                                \
    }                                                                          \
    if (!n->has_value) {                                                       \
        goto not_found;                                                        \
    }                                                                          \
    if (NULL != old) {                                                         \
        *old = n->value;                                                       \
    }                                                                          \
    n->has_value = 0;                                                          \
    /* 清值后自底向上：空了删，无值单子合并，其余停 */                         \
    r->size--;                                                                 \
    r->version++;                                                              \
    for (;;) {                                                                 \
        if (n->has_value || name##_child_count(n) >= 2) {                      \
            break;                                                             \
        }                                                                      \
        if (1 == name##_child_count(n)) {                                      \
            do {                                                               \
                name##_merge(r, n);                                            \
            } while (!n->has_value && 1 == name##_child_count(n));             \
            break;                                                             \
        }                                                                      \
        if (1 == links.size) {                                                 \
            name##_node_free(n);                                               \
            r->node_count--;                                                   \
            r->root = NULL;                                                    \
            break;                                                             \
        }                                                                      \
        /* 从父节点摘掉当前节点；父节点可能因此可删/可合并 */                  \
        parent = *links.ptr[links.size - 2];                                   \
        slot = links.ptr[links.size - 1];                                      \
        idx = (size_t)(slot - parent->child.ptr);                              \
        name##_node_free(n);                                                   \
        r->node_count--;                                                       \
        name##_child_remove(parent, idx);                                      \
        links.size--;                                                          \
        n = parent;                                                            \
    }                                                                          \
    name##_link_stack_free(&links);                                            \
    return 1;                                                                  \
not_found:                                                                     \
    name##_link_stack_free(&links);                                            \
    return 0;                                                                  \
}                                                                              \
/* 沿树下降，记录最后一个“是查询前缀”的有值节点。keybuf 非 NULL 时写走过的路径， \
   放不下就整体返回 0（即便已有更短的匹配）；keycap 不小于 klen 时不会放不下 */ \
static inline int32_t name##_prefix_match(const name *r, const void *key,      \
                                          size_t klen, uint8_t *keybuf,        \
                                          size_t keycap, size_t *match_len,    \
                                          T **val) {                           \
    if (NULL == match_len || NULL == val) {                                    \
        return 0;                                                              \
    }                                                                          \
    *match_len = 0;                                                            \
    *val = NULL;                                                               \
    const uint8_t *p = (const uint8_t *)key;                                   \
    size_t left = klen;                                                        \
    size_t out_len = 0;                                                        \
    name##_node *n = r->root;                                                  \
    name##_node *best = NULL;                                                  \
    size_t best_len = 0;                                                       \
    size_t max, common, idx;                                                   \
    name##_node **slot;                                                        \
    while (NULL != n) {                                                        \
        max = n->prefix_len < left ? n->prefix_len : left;                     \
        common = _radix_lcp(n->prefix, p, max);                                \
        if (common < n->prefix_len) {                                          \
            break;                                                             \
        }                                                                      \
        if (NULL != keybuf) {                                                  \
            if (out_len + n->prefix_len > keycap) {                            \
                return 0;                                                      \
            }                                                                  \
            if (n->prefix_len > 0) {                                           \
                memcpy(keybuf + out_len, n->prefix, n->prefix_len);            \
            }                                                                  \
        }                                                                      \
        out_len += n->prefix_len;                                              \
        if (n->prefix_len > 0) {                                               \
            p += n->prefix_len;                                                \
            left -= n->prefix_len;                                             \
        }                                                                      \
        if (n->has_value) {                                                    \
            best = n;                                                          \
            /* 只记录最后一个；更长的匹配会覆盖它 */                           \
            best_len = out_len;                                                \
        }                                                                      \
        if (0 == left) {                                                       \
            break;                                                             \
        }                                                                      \
        slot = name##_child_slot(n, p[0], &idx);                               \
        if (NULL == slot) {                                                    \
            break;                                                             \
        }                                                                      \
        if (NULL != keybuf) {                                                  \
            /* 写入本层 edge，再下降到子节点 */                                \
            if (out_len + 1 > keycap) {                                        \
                return 0;                                                      \
            }                                                                  \
            keybuf[out_len] = p[0];                                            \
        }                                                                      \
        out_len++;                                                             \
        p++;                                                                   \
        left--;                                                                \
        n = *slot;                                                             \
    }                                                                          \
    if (NULL == best) {                                                        \
        return 0;                                                              \
    }                                                                          \
    *match_len = best_len;                                                     \
    *val = &best->value;                                                       \
    return 1;                                                                  \
}                                                                              \
/* 状态字段直接读出 */                                                         \
static inline size_t name##_size(const name *r) {                              \
    return r->size;                                                            \
}                                                                              \
/* size == 0 */                                                                \
static inline int32_t name##_empty(const name *r) {                            \
    return 0 == r->size;                                                       \
}                                                                              \
/* 节点总数，含根 */                                                           \
static inline size_t name##_node_count(const name *r) {                        \
    return r->node_count;                                                      \
}                                                                              \
/* 插入过的最长 key 长度；删除不回落，clear 归零 */                            \
static inline size_t name##_max_key_len(const name *r) {                       \
    return r->max_key_len;                                                     \
}                                                                              \
/* 显式栈后序遍历：先收集子节点，再释放本节点，不递归 */                       \
static inline void name##_clear(name *r) {                                     \
    if (NULL != r->root) {                                                     \
        name##_node_stack st;                                                  \
        name##_node_stack_init(&st, 16);                                       \
        name##_node_stack_push_back(&st, &r->root);                            \
        name##_node *n;                                                        \
        name##_node *c;                                                        \
        size_t i;                                                              \
        while (st.size > 0) {                                                  \
            n = st.ptr[--st.size];                                             \
            for (i = 0; i < name##_child_count(n); i++) {                      \
                c = name##_child_at(n, i);                                     \
                name##_node_stack_push_back(&st, &c);                          \
            }                                                                  \
            if (n->has_value && NULL != r->elfree) {                           \
                r->elfree(&n->value);                                          \
            }                                                                  \
            name##_node_free(n);                                               \
        }                                                                      \
        name##_node_stack_free(&st);                                           \
    }                                                                          \
    r->root = NULL;                                                            \
    r->size = 0;                                                               \
    r->node_count = 0;                                                         \
    r->max_key_len = 0;                                                        \
    r->version++;                                                              \
}                                                                              \
/* clear 后整体清零，之后可直接再 init */                                      \
static inline void name##_free(name *r) {                                      \
    name##_clear(r);                                                           \
    ZERO(r, sizeof(*r));                                                       \
}                                                                              \
/* 近似统计：节点结构 + 前缀 + 子节点数组容量 */                               \
static inline size_t name##_mem(const name *r) {                               \
    size_t total = sizeof(*r);                                                 \
    if (NULL == r->root) {                                                     \
        return total;                                                          \
    }                                                                          \
    /* 显式栈代替递归 */                                                       \
    name##_node_stack st;                                                      \
    name##_node_stack_init(&st, 16);                                           \
    name##_node_stack_push_back(&st, &r->root);                                \
    name##_node *n;                                                            \
    name##_node *c;                                                            \
    size_t i;                                                                  \
    while (st.size > 0) {                                                      \
        n = st.ptr[--st.size];                                                 \
        total += sizeof(*n) + n->prefix_len                                    \
                 + (size_t)n->child.maxsize * sizeof(name##_node *);           \
        for (i = 0; i < name##_child_count(n); i++) {                          \
            c = name##_child_at(n, i);                                         \
            name##_node_stack_push_back(&st, &c);                              \
        }                                                                      \
    }                                                                          \
    name##_node_stack_free(&st);                                               \
    return total;                                                              \
}                                                                              \
/* 校验 edge 有序、无空叶子、size/node_count 一致 */                           \
static inline int32_t name##_check(const name *r) {                            \
    if (NULL == r->root) {                                                     \
        return (0 == r->size && 0 == r->node_count) ? ERR_OK : ERR_FAILED;     \
    }                                                                          \
    size_t values = 0;                                                         \
    size_t nodes = 0;                                                          \
    name##_node_stack st;                                                      \
    name##_node_stack_init(&st, 16);                                           \
    name##_node_stack_push_back(&st, &r->root);                                \
    name##_node *n;                                                            \
    name##_node *c;                                                            \
    size_t i;                                                                  \
    while (st.size > 0) {                                                      \
        n = st.ptr[--st.size];                                                 \
        nodes++;                                                               \
        if (name##_child_count(n) > RADIX_MAX_CHILD) {                         \
            goto bad;                                                          \
        }                                                                      \
        if (n->prefix_len > 0 && NULL == n->prefix) {                          \
            goto bad;                                                          \
        }                                                                      \
        if (!n->has_value && 0 == name##_child_count(n)) {                     \
            /* 无值叶子是坏节点：split 留下的无值节点必有子，delete 会合并 */  \
            goto bad;                                                          \
        }                                                                      \
        for (i = 0; i < name##_child_count(n); i++) {                          \
            c = name##_child_at(n, i);                                         \
            if (NULL == c) {                                                   \
                goto bad;                                                      \
            }                                                                  \
            if (i > 0 && name##_child_at(n, i - 1)->edge >= c->edge) {         \
                goto bad;                                                      \
            }                                                                  \
        }                                                                      \
        if (n->has_value) {                                                    \
            values++;                                                          \
        }                                                                      \
        for (i = 0; i < name##_child_count(n); i++) {                          \
            c = name##_child_at(n, i);                                         \
            name##_node_stack_push_back(&st, &c);                              \
        }                                                                      \
    }                                                                          \
    name##_node_stack_free(&st);                                               \
    if (values != r->size || nodes != r->node_count) {                         \
        return ERR_FAILED;                                                     \
    }                                                                          \
    return ERR_OK;                                                             \
bad:                                                                           \
    name##_node_stack_free(&st);                                               \
    LOG_WARN("radix check failed.");                                           \
    return ERR_FAILED;                                                         \
}                                                                              \
/* 迭代器零初始化；路径栈和 key 缓冲按需增长 */                                \
static inline void name##_iter_init(name##_iter *it, const name *r) {          \
    ZERO(it, sizeof(*it));                                                     \
    name##_node_stack_init(&it->stack, 16);                                    \
    name##_bytebuf_init(&it->key, 0);                                          \
    it->r = r;                                                                 \
    it->version = r->version;                                                  \
}                                                                              \
/* 释放迭代器缓冲并清零 */                                                     \
static inline void name##_iter_free(name##_iter *it) {                         \
    name##_node_stack_free(&it->stack);                                        \
    name##_bytebuf_free(&it->key);                                             \
    ZERO(it, sizeof(*it));                                                     \
}                                                                              \
/* 压路径节点；ARR 负责扩容 */                                                 \
static inline void name##_iter_push(name##_iter *it, name##_node *n) {         \
    name##_node_stack_push_back(&it->stack, &n);                               \
}                                                                              \
/* 只清游标和路径，缓冲留着复用 */                                             \
static inline void name##_iter_invalidate(name##_iter *it) {                   \
    it->cur = NULL;                                                            \
    name##_node_stack_clear(&it->stack);                                       \
    it->key_len = 0;                                                           \
}                                                                              \
/* next/prev 前查 version；表改过就失效并告警 */                               \
static inline int32_t name##_iter_check(name##_iter *it) {                     \
    if (NULL == it->r) {                                                       \
        return 0;                                                              \
    }                                                                          \
    if (it->version != it->r->version) {                                       \
        _radix_warn_modified();                                                \
        name##_iter_invalidate(it);                                            \
        it->version = it->r->version;                                          \
        return 0;                                                              \
    }                                                                          \
    return 1;                                                                  \
}                                                                              \
/* 按路径栈重建 key：根前缀 + 每层 edge + 本层前缀 */                          \
static inline void name##_iter_build_key(name##_iter *it) {                    \
    size_t total = 0;                                                          \
    size_t i;                                                                  \
    for (i = 0; i < it->stack.size; i++) {                                     \
        if (i > 0) {                                                           \
            total++;                                                           \
        }                                                                      \
        total += it->stack.ptr[i]->prefix_len;                                 \
    }                                                                          \
    ASSERTAB(total < UINT32_MAX, "radix key too long.");                       \
    /* resize 每次都重分配，容量够就不调；空 key 也要留 1 字节，iter_key 才不返 NULL */ \
    if (0 == it->key.maxsize || total > it->key.maxsize) {                     \
        name##_bytebuf_resize(&it->key, (uint32_t)(total ? total : 1));        \
    }                                                                          \
    size_t off = 0;                                                            \
    name##_node *n;                                                            \
    for (i = 0; i < it->stack.size; i++) {                                     \
        n = it->stack.ptr[i];                                                  \
        if (i > 0) {                                                           \
            it->key.ptr[off++] = n->edge;                                      \
        }                                                                      \
        if (n->prefix_len > 0) {                                               \
            memcpy(it->key.ptr + off, n->prefix, n->prefix_len);               \
            off += n->prefix_len;                                              \
        }                                                                      \
    }                                                                          \
    it->key_len = off;                                                         \
}                                                                              \
/* 向下找第一个有值节点；节点自身有值就是它 */                                 \
static inline int32_t name##_iter_first_from(name##_iter *it) {                \
    name##_node *n;                                                            \
    for (;;) {                                                                 \
        n = it->stack.ptr[it->stack.size - 1];                                 \
        if (n->has_value) {                                                    \
            it->cur = n;                                                       \
            name##_iter_build_key(it);                                         \
            return 1;                                                          \
        }                                                                      \
        if (0 == name##_child_count(n)) {                                      \
            name##_iter_invalidate(it);                                        \
            return 0;                                                          \
        }                                                                      \
        name##_iter_push(it, name##_child_at(n, 0));                           \
    }                                                                          \
}                                                                              \
/* 一直走最后一个子节点：节点自己的值排在子节点之前，最大的 key 在最右的叶子上（叶子必有值） */ \
static inline int32_t name##_iter_last_from(name##_iter *it) {                 \
    name##_node *n;                                                            \
    for (;;) {                                                                 \
        n = it->stack.ptr[it->stack.size - 1];                                 \
        if (0 == name##_child_count(n)) {                                      \
            if (n->has_value) {                                                \
                it->cur = n;                                                   \
                name##_iter_build_key(it);                                     \
                return 1;                                                      \
            }                                                                  \
            name##_iter_invalidate(it);                                        \
            return 0;                                                          \
        }                                                                      \
        name##_iter_push(it, name##_child_at(n, name##_child_count(n) - 1));   \
    }                                                                          \
}                                                                              \
/* 栈顶子树走完：回溯到最近一个还有下一个兄弟的祖先，取那个兄弟子树的第一个值 */ \
static inline int32_t name##_iter_up_next(name##_iter *it) {                   \
    name##_node *child;                                                        \
    name##_node *parent;                                                       \
    size_t idx;                                                                \
    while (it->stack.size > 1) {                                               \
        child = it->stack.ptr[it->stack.size - 1];                             \
        parent = it->stack.ptr[it->stack.size - 2];                            \
        idx = name##_child_index(parent, child);                               \
        if (idx + 1 < name##_child_count(parent)) {                            \
            it->stack.ptr[it->stack.size - 1] = name##_child_at(parent, idx + 1); \
            return name##_iter_first_from(it);                                 \
        }                                                                      \
        it->stack.size--;                                                      \
    }                                                                          \
    name##_iter_invalidate(it);                                                \
    return 0;                                                                  \
}                                                                              \
/* 清路径后从根开始 */                                                         \
static inline int32_t name##_iter_first(name##_iter *it) {                     \
    if (NULL == it->r || NULL == it->r->root) {                                \
        name##_iter_invalidate(it);                                            \
        return 0;                                                              \
    }                                                                          \
    it->version = it->r->version;                                              \
    name##_node_stack_clear(&it->stack);                                       \
    it->cur = NULL;                                                            \
    name##_iter_push(it, it->r->root);                                         \
    return name##_iter_first_from(it);                                         \
}                                                                              \
/* 清路径后从根开始，取最后一个 */                                             \
static inline int32_t name##_iter_last(name##_iter *it) {                      \
    if (NULL == it->r || NULL == it->r->root) {                                \
        name##_iter_invalidate(it);                                            \
        return 0;                                                              \
    }                                                                          \
    it->version = it->r->version;                                              \
    name##_node_stack_clear(&it->stack);                                       \
    it->cur = NULL;                                                            \
    name##_iter_push(it, it->r->root);                                         \
    return name##_iter_last_from(it);                                          \
}                                                                              \
/* 有子节点走第一个；否则回溯到下一个兄弟 */                                   \
static inline int32_t name##_iter_next(name##_iter *it) {                      \
    if (!name##_iter_check(it) || NULL == it->cur) {                           \
        return 0;                                                              \
    }                                                                          \
    name##_node *n = it->stack.ptr[it->stack.size - 1];                        \
    if (name##_child_count(n) > 0) {                                           \
        name##_iter_push(it, name##_child_at(n, 0));                           \
        return name##_iter_first_from(it);                                     \
    }                                                                          \
    return name##_iter_up_next(it);                                            \
}                                                                              \
/* 先找前一个兄弟的最后一个值；没有则父节点有值就取父 */                       \
static inline int32_t name##_iter_prev(name##_iter *it) {                      \
    if (!name##_iter_check(it) || NULL == it->cur) {                           \
        return 0;                                                              \
    }                                                                          \
    name##_node *child;                                                        \
    name##_node *parent;                                                       \
    size_t idx;                                                                \
    while (it->stack.size > 1) {                                               \
        child = it->stack.ptr[it->stack.size - 1];                             \
        parent = it->stack.ptr[it->stack.size - 2];                            \
        idx = name##_child_index(parent, child);                               \
        if (idx > 0) {                                                         \
            it->stack.ptr[it->stack.size - 1] = name##_child_at(parent, idx - 1); \
            return name##_iter_last_from(it);                                  \
        }                                                                      \
        it->stack.size--;                                                      \
        parent = it->stack.ptr[it->stack.size - 1];                            \
        if (parent->has_value) {                                               \
            it->cur = parent;                                                  \
            name##_iter_build_key(it);                                         \
            return 1;                                                          \
        }                                                                      \
    }                                                                          \
    name##_iter_invalidate(it);                                                \
    return 0;                                                                  \
}                                                                              \
/* lower_bound：前缀相等继续下降；分叉时按字节大小决定取子树还是回溯 */        \
static inline int32_t name##_iter_seek(name##_iter *it, const void *key,       \
                                       size_t klen) {                          \
    if (NULL == it->r || NULL == it->r->root) {                                \
        name##_iter_invalidate(it);                                            \
        return 0;                                                              \
    }                                                                          \
    it->version = it->r->version;                                              \
    name##_node_stack_clear(&it->stack);                                       \
    it->cur = NULL;                                                            \
    name##_iter_push(it, it->r->root);                                         \
    const uint8_t *p = (const uint8_t *)key;                                   \
    size_t left = klen;                                                        \
    name##_node *n;                                                            \
    name##_node **slot;                                                        \
    size_t max, common, idx;                                                   \
    for (;;) {                                                                 \
        n = it->stack.ptr[it->stack.size - 1];                                 \
        max = n->prefix_len < left ? n->prefix_len : left;                     \
        common = _radix_lcp(n->prefix, p, max);                                \
        if (common < n->prefix_len) {                                          \
            /* 当前节点前缀比目标大：本子树第一个值就是下界 */                 \
            if (common == left || n->prefix[common] > p[common]) {             \
                return name##_iter_first_from(it);                             \
            }                                                                  \
        } else {                                                               \
            if (n->prefix_len > 0) {                                           \
                p += n->prefix_len;                                            \
                left -= n->prefix_len;                                         \
            }                                                                  \
            if (0 == left) {                                                   \
                if (n->has_value) {                                            \
                    it->cur = n;                                               \
                    name##_iter_build_key(it);                                 \
                    return 1;                                                  \
                }                                                              \
                return name##_iter_first_from(it);                             \
            }                                                                  \
            slot = name##_child_slot(n, p[0], &idx);                           \
            if (NULL != slot) {                                                \
                name##_iter_push(it, *slot);                                   \
                p++;                                                           \
                left--;                                                        \
                continue;                                                      \
            }                                                                  \
            if (idx < name##_child_count(n)) {                                 \
                /* 没有同 edge 子节点，但下一个兄弟更大：取它子树最小值 */     \
                name##_iter_push(it, name##_child_at(n, idx));                 \
                return name##_iter_first_from(it);                             \
            }                                                                  \
        }                                                                      \
        /* 本子树全都小于目标：取后面第一个兄弟子树的最小值 */                 \
        return name##_iter_up_next(it);                                        \
    }                                                                          \
}                                                                              \
/* seek 到 prefix 后检查首项是否真以 prefix 开头 */                            \
static inline int32_t name##_iter_seek_prefix(name##_iter *it,                 \
                                              const void *prefix, size_t plen) { \
    if (!name##_iter_seek(it, prefix, plen)) {                                 \
        return 0;                                                              \
    }                                                                          \
    size_t klen = 0;                                                           \
    const uint8_t *k = name##_iter_key(it, &klen);                             \
    if (NULL == k || klen < plen                                               \
        || (plen > 0 && 0 != memcmp(k, prefix, plen))) {                       \
        name##_iter_invalidate(it);                                            \
        return 0;                                                              \
    }                                                                          \
    return 1;                                                                  \
}                                                                              \
/* 游标有值且表没改过 */                                                       \
static inline int32_t name##_iter_valid(const name##_iter *it) {               \
    if (NULL == it->r || NULL == it->cur) {                                    \
        return 0;                                                              \
    }                                                                          \
    if (it->version != it->r->version) {                                       \
        _radix_warn_modified();                                                \
        return 0;                                                              \
    }                                                                          \
    return 1;                                                                  \
}                                                                              \
static inline const uint8_t *name##_iter_key(name##_iter *it, size_t *klen) {  \
    if (!name##_iter_valid(it)) {                                              \
        if (NULL != klen) {                                                    \
            *klen = 0;                                                         \
        }                                                                      \
        return NULL;                                                           \
    }                                                                          \
    if (NULL != klen) {                                                        \
        *klen = it->key_len;                                                   \
    }                                                                          \
    return it->key.ptr;                                                        \
}                                                                              \
/* 值指针；下一次增删后失效 */                                                 \
static inline T *name##_iter_val(name##_iter *it) {                            \
    if (!name##_iter_valid(it)) {                                              \
        return NULL;                                                           \
    }                                                                          \
    return &it->cur->value;                                                    \
}                                                                              \
/* 保存当前 key，删完重新 seek 到下一个 */                                     \
static inline int32_t name##_iter_del(name *r, name##_iter *it, T *old) {      \
    if (it->r != r || !name##_iter_valid(it)) {                                \
        return 0;                                                              \
    }                                                                          \
    size_t klen = it->key_len;                                                 \
    uint8_t *tmp;                                                              \
    MALLOC(tmp, klen ? klen : 1);                                              \
    if (klen > 0) {                                                            \
        memcpy(tmp, it->key.ptr, klen);                                        \
    }                                                                          \
    int32_t rtn = name##_del(r, tmp, klen, old);                               \
    it->version = r->version;                                                  \
    name##_node_stack_clear(&it->stack);                                       \
    it->cur = NULL;                                                            \
    if (0 != rtn) {                                                            \
        (void)name##_iter_seek(it, tmp, klen);                                 \
    }                                                                          \
    FREE(tmp);                                                                 \
    return rtn;                                                                \
}                                                                              \
/* 顺序遍历；回调返回 0 停止；改表返回 ERR_FAILED */                           \
static inline int32_t name##_walk(const name *r, name##_walk_cb cb, void *ud) { \
    name##_iter it;                                                            \
    uint32_t ver = r->version;                                                 \
    int32_t rtn = ERR_OK;                                                      \
    name##_iter_init(&it, r);                                                  \
    size_t klen;                                                               \
    const uint8_t *k;                                                          \
    for (name##_iter_first(&it); name##_iter_valid(&it);                       \
         name##_iter_next(&it)) {                                              \
        k = name##_iter_key(&it, &klen);                                       \
        if (0 == cb(k, klen, name##_iter_val(&it), ud)) {                      \
            rtn = ERR_FAILED;                                                  \
            break;                                                             \
        }                                                                      \
    }                                                                          \
    if (r->version != ver) {                                                   \
        rtn = ERR_FAILED;                                                      \
    }                                                                          \
    name##_iter_free(&it);                                                     \
    return rtn;                                                                \
}                                                                              \
/* [lo, hi)；lo/hi 为 NULL 表示该端不限 */                                     \
static inline int32_t name##_walk_range(const name *r,                         \
                                        const void *lo, size_t lolen,          \
                                        const void *hi, size_t hilen,          \
                                        name##_walk_cb cb, void *ud) {         \
    name##_iter it;                                                            \
    uint32_t ver = r->version;                                                 \
    int32_t rtn = ERR_OK;                                                      \
    name##_iter_init(&it, r);                                                  \
    /* lo 为空从头，否则 lower_bound；hi 由循环内比较截断 */                   \
    int32_t ok = (NULL == lo) ? name##_iter_first(&it)                         \
                              : name##_iter_seek(&it, lo, lolen);              \
    size_t klen;                                                               \
    const uint8_t *k;                                                          \
    while (ok && name##_iter_valid(&it)) {                                     \
        k = name##_iter_key(&it, &klen);                                       \
        if (NULL != hi && _radix_cmp(k, klen,                                  \
                                     (const uint8_t *)hi, hilen) >= 0) {       \
            break;                                                             \
        }                                                                      \
        if (0 == cb(k, klen, name##_iter_val(&it), ud)) {                      \
            rtn = ERR_FAILED;                                                  \
            break;                                                             \
        }                                                                      \
        ok = name##_iter_next(&it);                                            \
    }                                                                          \
    if (r->version != ver) {                                                   \
        rtn = ERR_FAILED;                                                      \
    }                                                                          \
    name##_iter_free(&it);                                                     \
    return rtn;                                                                \
}                                                                              \
/* 从第一个 prefix 项起，遇到不再带 prefix 就停 */                             \
static inline int32_t name##_walk_prefix(const name *r, const void *prefix,    \
                                         size_t plen, name##_walk_cb cb,       \
                                         void *ud) {                           \
    name##_iter it;                                                            \
    uint32_t ver = r->version;                                                 \
    int32_t rtn = ERR_OK;                                                      \
    name##_iter_init(&it, r);                                                  \
    int32_t ok = name##_iter_seek_prefix(&it, prefix, plen);                   \
    size_t klen;                                                               \
    const uint8_t *k;                                                          \
    while (ok && name##_iter_valid(&it)) {                                     \
        k = name##_iter_key(&it, &klen);                                       \
        if (klen < plen                                                        \
            || (plen > 0 && 0 != memcmp(k, prefix, plen))) {                   \
            break;                                                             \
        }                                                                      \
        if (0 == cb(k, klen, name##_iter_val(&it), ud)) {                      \
            rtn = ERR_FAILED;                                                  \
            break;                                                             \
        }                                                                      \
        ok = name##_iter_next(&it);                                            \
    }                                                                          \
    if (r->version != ver) {                                                   \
        rtn = ERR_FAILED;                                                      \
    }                                                                          \
    name##_iter_free(&it);                                                     \
    return rtn;                                                                \
}

// RADIX_DECL 生成的函数用到的工具；宏在使用处才展开，放在宏后面即可
/* 三路比较：按无符号字节比，公共前缀相同时短的小 */
static inline int32_t _radix_cmp(const uint8_t *a, size_t alen,
                                 const uint8_t *b, size_t blen) {
    size_t n = alen < blen ? alen : blen;
    if (n > 0) {
        int32_t c = memcmp(a, b, n);
        if (0 != c) {
            return c < 0 ? -1 : 1;
        }
    }
    if (alen < blen) {
        return -1;
    }
    if (alen > blen) {
        return 1;
    }
    return 0;
}
/* 最长公共前缀：radix 压缩节点的核心。 */
static inline size_t _radix_lcp(const uint8_t *a, const uint8_t *b, size_t n) {
    size_t i = 0;
    while (i < n && a[i] == b[i]) {
        i++;
    }
    return i;
}
static inline void _radix_warn_modified(void) {
    LOG_WARN("radix modified during iteration, iterator invalidated.");
}

#endif // RADIX_H_
