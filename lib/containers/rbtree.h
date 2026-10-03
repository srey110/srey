#ifndef RBTREE_H_
#define RBTREE_H_

#include "base/base.h"

// 红黑树（侵入式）。rbt_node 嵌入元素，树不分配内存，元素地址即句柄。
// 上层是 RBT_DECL 生成的带类型树，一般只用它；
// 下层是 rbt_* 节点级接口（红黑树算法本身），只在要手写下降循环时才直接用。
//
// 典型用法：
//   typedef struct { uint64_t due; rbt_node node; } tmr;
//   #define TMR_KEY(e) ((e)->due)
//   #define U64_LT(a, b) ((a) < (b))
//   RBT_DECL(tmr_tree, tmr, node, uint64_t, TMR_KEY, U64_LT, RBT_MULTI)
//   tmr_tree q; tmr_tree_init(&q);
//   tmr_tree_insert(&q, &t);                              // MULTI：相等的排到相等段末尾
//   tmr *hit = tmr_tree_find(&q, 100);                     // 相等段里最早插入的那个
//   tmr *first = tmr_tree_pop_first(&q);                   // 摘下最小（找最小是 O(1)）
//   rbt_foreach(&q, tmr_tree, e) { ... }                   // 中序遍历，e 为 tmr *
//   tmr_tree_clear(&q, NULL, NULL);
// 手写下降循环（底层接口）：
//   rbt_node **link = &root->rbt_node, *parent = NULL, *cur = *link;
//   while (NULL != cur) { parent = cur; link = 小于 ? &cur->rbt_left : &cur->rbt_right; cur = *link; }
//   rbt_link_node(&e->node, parent, link);
//   rbt_insert_color(root, &e->node);
//
// 契约：
//   1. 一个 rbt_node 字段同时只属于一棵树；插入前节点必须不在任何树里（同一节点重复插入会破坏树）；
//      一个元素挂多棵树就放多个 rbt_node 字段、各自 DECL。rbt_link_node 写全三个字段。
//   2. 节点的 rbt_parent_color 为 0 即不在任何树里（在树的节点此字段恒非 0：只有根的父为空，而根恒黑），
//      所以元素整体清零就是"不在树里"。name##_erase / extract / pop_first / replace / clear 摘下的节点都会清零；
//      底层 rbt_erase 系列不清，用它们时自己 RBT_CLEAR_NODE。对不在树里的节点调 rbt_next / rbt_prev 返回 NULL。
//   3. 元素在树中期间不得移动地址，不得改动参与比较的字段（改键先 erase 再重新插入）。
//   4. LESS(a, b) 是键上的严格弱序：a 排在 b 前面才为真；相等由 !LESS(a, b) && !LESS(b, a) 推出。
//      KEYOF(e) 的 e 为 T *，返回 K。回绕比较只在树内键都落在半个取值区间内时成立。
//   5. rbt_foreach / rbt_foreach_equal 循环体内禁止增删；要删当前元素用 rbt_foreach_safe（删别的仍然禁止）。
//      手写 rbt_next 循环时可以先取 next 再删当前节点。底层后序遍历（rbt_first_postorder / rbt_next_postorder）
//      中只许释放已访问过的节点，禁止增删替换，走完后调用方把根重置为空。
//   6. 别绕过 name##_* 直接对 name 的 root 调底层增删，否则 size 与最左缓存都会过期；只读接口不受限。
//      底层单独用时，cached 树只能用 *_cached 系列增删替换。
//   7. rbt_replace_node：新旧节点须等键；新节点无需初始化但不得在任何树中；旧节点替换后不会被清空。
//   8. rbt_node 不得放进 packed 结构（颜色占父地址的最低位）。

#define RBT_RED ((uintptr_t)0) // rbt_parent_color 的 bit0：0 红 1 黑
#define RBT_BLACK ((uintptr_t)1)
#define RBT_UNIQUE 0 // 不允许重复键（std::set）
#define RBT_MULTI 1 // 允许重复键，相等元素按插入先后排（std::multiset）

#define RBT_ROOT_INIT { NULL } // 只能用于定义时初始化，运行期重置用 rbt_root_init
#define RBT_ROOT_CACHED_INIT { { NULL }, NULL } // 同上，运行期重置用 rbt_root_cached_init
#define RBT_EMPTY_ROOT(root) (NULL == (root)->rbt_node)
// 节点在不在树里（契约第 2 条）；node 会多次求值，须传无副作用的表达式
#define RBT_EMPTY_NODE(node) (0 == (node)->rbt_parent_color)
#define RBT_CLEAR_NODE(node) ((node)->rbt_parent_color = 0)

// 以下三个遍历宏对 RBT_DECL 与 RBTSET_DECL 生成的类型都适用；e、tmp 为元素指针，由调用方先声明
// 中序遍历全部元素
#define rbt_foreach(t, name, e) \
    for ((e) = name##_first(t); NULL != (e); (e) = name##_next(e))
// 同上，循环体内可以删掉当前元素 e
#define rbt_foreach_safe(t, name, e, tmp)                                                        \
    for ((e) = name##_first(t), (tmp) = (NULL != (e)) ? name##_next(e) : NULL;                  \
         NULL != (e);                                                                           \
         (e) = (tmp), (tmp) = (NULL != (e)) ? name##_next(e) : NULL)
// 按插入先后遍历与 k 相等的元素；k 会多次求值
#define rbt_foreach_equal(t, name, k, e) \
    for ((e) = name##_find((t), (k)); NULL != (e); (e) = name##_next_equal((e), (k)))

// 入参写 T const * 而不是 const T *：理由同 hashmap.h。
// RBT_DECL(name, T, FIELD, K, KEYOF, LESS, MODE)：name 生成的类型名，T 元素类型，FIELD 元素内 rbt_node 字段名，
// K 键类型，KEYOF(e) 从元素取键，LESS(a, b) 键的小于比较（契约第 4 条），MODE 取 RBT_UNIQUE / RBT_MULTI
#define RBT_DECL(name, T, FIELD, K, KEYOF, LESS, MODE)                                               \
typedef struct name {                                                                                \
    rbt_root_cached root;       /* 带最左缓存，first 为 O(1) */                                      \
    size_t size;                /* 元素数 */                                                         \
} name;                                                                                              \
/* 节点转元素指针，NULL 进 NULL 出；FIELD 不是首字段时 UPCAST(NULL) 不是 NULL，须先判 */             \
static inline T *name##_entry(rbt_node *n) {                                                         \
    return (NULL == n) ? NULL : UPCAST(n, T, FIELD);                                                 \
}                                                                                                    \
/* 初始化为空树，也用于运行期重置 */                                                                 \
static inline void name##_init(name *t) {                                                            \
    rbt_root_cached_init(&t->root);                                                                  \
    t->size = 0;                                                                                     \
}                                                                                                    \
/* 元素数 */                                                                                         \
static inline size_t name##_size(const name *t) {                                                    \
    return t->size;                                                                                  \
}                                                                                                    \
/* 是否为空 */                                                                                       \
static inline int32_t name##_empty(const name *t) {                                                  \
    return 0 == t->size;                                                                             \
}                                                                                                    \
/* e 是否在某棵树里（契约第 2 条） */                                                                \
static inline int32_t name##_linked(T const *e) {                                                    \
    return !RBT_EMPTY_NODE(&e->FIELD);                                                               \
}                                                                                                    \
/* 最小元素，O(1)；空树返回 NULL */                                                                  \
static inline T *name##_first(const name *t) {                                                       \
    return name##_entry(rbt_first_cached(&t->root));                                                 \
}                                                                                                    \
/* 最大元素，空树返回 NULL */                                                                        \
static inline T *name##_last(const name *t) {                                                        \
    return name##_entry(rbt_last(&t->root.rbt_root));                                                \
}                                                                                                    \
/* 中序后继，到头返回 NULL */                                                                        \
static inline T *name##_next(T *e) {                                                                 \
    return name##_entry(rbt_next(&e->FIELD));                                                        \
}                                                                                                    \
/* 中序前驱，到头返回 NULL */                                                                        \
static inline T *name##_prev(T *e) {                                                                 \
    return name##_entry(rbt_prev(&e->FIELD));                                                        \
}                                                                                                    \
/* 同 std::set：第一个不小于 k 的元素，有重复键时是相等段里最早插入的；没有返回 NULL */              \
static inline T *name##_lower_bound(const name *t, K k) {                                            \
    rbt_node *node = t->root.rbt_root.rbt_node;                                                      \
    T *res = NULL;                                                                                   \
    T *e;                                                                                            \
    while (NULL != node) {                                                                           \
        e = UPCAST(node, T, FIELD);                                                                  \
        if (!(LESS(KEYOF(e), k))) {                                                                  \
            res = e;                                                                                 \
            node = node->rbt_left;                                                                   \
        } else {                                                                                     \
            node = node->rbt_right;                                                                  \
        }                                                                                            \
    }                                                                                                \
    return res;                                                                                      \
}                                                                                                    \
/* 同 std::set：第一个大于 k 的元素，没有返回 NULL */                                                \
static inline T *name##_upper_bound(const name *t, K k) {                                            \
    rbt_node *node = t->root.rbt_root.rbt_node;                                                      \
    T *res = NULL;                                                                                   \
    T *e;                                                                                            \
    while (NULL != node) {                                                                           \
        e = UPCAST(node, T, FIELD);                                                                  \
        if (LESS(k, KEYOF(e))) {                                                                     \
            res = e;                                                                                 \
            node = node->rbt_left;                                                                   \
        } else {                                                                                     \
            node = node->rbt_right;                                                                  \
        }                                                                                            \
    }                                                                                                \
    return res;                                                                                      \
}                                                                                                    \
/* 与 k 相等的元素，MULTI 下是最早插入的那个；没有返回 NULL */                                       \
static inline T *name##_find(const name *t, K k) {                                                   \
    T *e = name##_lower_bound(t, k);                                                                 \
    return (NULL != e && !(LESS(k, KEYOF(e)))) ? e : NULL;                                           \
}                                                                                                    \
/* 是否存在与 k 相等的元素 */                                                                        \
static inline int32_t name##_contains(const name *t, K k) {                                          \
    return NULL != name##_find(t, k);                                                                \
}                                                                                                    \
/* e 的中序后继与 k 相等则返回它，否则 NULL；e 须与 k 相等（rbt_foreach_equal 用） */                \
static inline T *name##_next_equal(T *e, K k) {                                                      \
    T *n = name##_entry(rbt_next(&e->FIELD));                                                        \
    return (NULL != n && !(LESS(k, KEYOF(n)))) ? n : NULL;                                           \
}                                                                                                    \
/* 与 k 相等的元素个数 */                                                                            \
static inline size_t name##_count(const name *t, K k) {                                              \
    T *e;                                                                                            \
    size_t n = 0;                                                                                    \
    rbt_foreach_equal(t, name, k, e) {                                                               \
        n++;                                                                                         \
    }                                                                                                \
    return n;                                                                                        \
}                                                                                                    \
/* 找插入位置：UNIQUE 下已有相等元素则返回它，pos 不写；否则返回 NULL，pos 记下插入点（MULTI 排到相等段末尾）。 \
   pos 只给紧接着的 insert_commit 用，两者之间不得增删 */                                            \
static inline T *name##_insert_check(name *t, K k, rbt_insert_pos *pos) {                            \
    rbt_node **link = &t->root.rbt_root.rbt_node;                                                    \
    rbt_node *parent = NULL;                                                                         \
    rbt_node *cur = *link;                                                                           \
    rbt_node *cand = NULL;                                                                           \
    int32_t leftmost = 1;                                                                            \
    while (NULL != cur) {                                                                            \
        parent = cur;                                                                                \
        if (LESS(k, KEYOF(UPCAST(cur, T, FIELD)))) {                                                 \
            link = &cur->rbt_left;                                                                   \
        } else {                                                                                     \
            cand = cur;                                                                              \
            link = &cur->rbt_right;                                                                  \
            leftmost = 0;                                                                            \
        }                                                                                            \
        cur = *link;                                                                                 \
    }                                                                                                \
    if (RBT_UNIQUE == (MODE) && NULL != cand && !(LESS(KEYOF(UPCAST(cand, T, FIELD)), k))) {         \
        return UPCAST(cand, T, FIELD);                                                               \
    }                                                                                                \
    pos->parent = parent;                                                                            \
    pos->link = link;                                                                                \
    pos->leftmost = leftmost;                                                                        \
    return NULL;                                                                                     \
}                                                                                                    \
/* 把 e 挂到 insert_check 给出的位置；e 的键须与 check 时的 k 相等，e 不得在任何树中 */              \
static inline void name##_insert_commit(name *t, T *e, const rbt_insert_pos *pos) {                  \
    rbt_link_node(&e->FIELD, pos->parent, pos->link);                                                \
    rbt_insert_color_cached(&t->root, &e->FIELD, pos->leftmost);                                     \
    t->size++;                                                                                       \
}                                                                                                    \
/* UNIQUE：已有相等元素则返回它、e 不入树，否则插入并返回 NULL；MULTI：总是插入到相等段末尾，返回 NULL */ \
static inline T *name##_insert(name *t, T *e) {                                                      \
    rbt_insert_pos pos;                                                                              \
    T *old = name##_insert_check(t, KEYOF(e), &pos);                                                 \
    if (NULL == old) {                                                                               \
        name##_insert_commit(t, e, &pos);                                                            \
    }                                                                                                \
    return old;                                                                                      \
}                                                                                                    \
/* 删除 e 并把节点清零；不校验 e 是否在 t 中 */                                                      \
static inline void name##_erase(name *t, T *e) {                                                     \
    rbt_erase_cached(&t->root, &e->FIELD);                                                           \
    RBT_CLEAR_NODE(&e->FIELD);                                                                       \
    t->size--;                                                                                       \
}                                                                                                    \
/* 摘下与 k 相等的元素（MULTI 下是最早插入的那个）并返回，节点清零；没有返回 NULL */                 \
static inline T *name##_extract(name *t, K k) {                                                      \
    T *e = name##_find(t, k);                                                                        \
    if (NULL != e) {                                                                                 \
        name##_erase(t, e);                                                                          \
    }                                                                                                \
    return e;                                                                                        \
}                                                                                                    \
/* 摘下最小元素并返回，节点清零；空树返回 NULL */                                                    \
static inline T *name##_pop_first(name *t) {                                                         \
    T *e = name##_first(t);                                                                          \
    if (NULL != e) {                                                                                 \
        name##_erase(t, e);                                                                          \
    }                                                                                                \
    return e;                                                                                        \
}                                                                                                    \
/* 用 e 原位顶替 old：二者须等键，e 不得在任何树中；old 的节点随后清零 */                            \
static inline void name##_replace(name *t, T *old, T *e) {                                           \
    rbt_replace_node_cached(&t->root, &old->FIELD, &e->FIELD);                                       \
    RBT_CLEAR_NODE(&old->FIELD);                                                                     \
}                                                                                                    \
/* 清空：后序逐个把节点清零后调 fn(e, ud)（fn 可为 NULL，里面可以释放元素），不做旋转 */             \
static inline void name##_clear(name *t, void (*fn)(T *e, void *ud), void *ud) {                     \
    rbt_node *n = rbt_first_postorder(&t->root.rbt_root);                                            \
    rbt_node *next;                                                                                  \
    T *e;                                                                                            \
    while (NULL != n) {                                                                              \
        next = rbt_next_postorder(n);                                                                \
        e = UPCAST(n, T, FIELD);                                                                     \
        RBT_CLEAR_NODE(n);                                                                           \
        if (NULL != fn) {                                                                            \
            fn(e, ud);                                                                               \
        }                                                                                            \
        n = next;                                                                                    \
    }                                                                                                \
    name##_init(t);                                                                                  \
}

// 树节点，嵌入元素使用；字段由树维护，调用方不得改动
typedef struct rbt_node {
    uintptr_t rbt_parent_color; // 父节点地址 | bit0 颜色（见 RBT_RED / RBT_BLACK）
    struct rbt_node *rbt_right; // 右孩子
    struct rbt_node *rbt_left;  // 左孩子
}rbt_node;
// 树根
typedef struct rbt_root {
    rbt_node *rbt_node;         // 根节点，空树为 NULL
}rbt_root;
// 缓存最左节点的树根，O(1) 取最小
typedef struct rbt_root_cached {
    rbt_root rbt_root;          // 普通树根，只读接口可直接传它
    rbt_node *rbt_leftmost;     // 最左节点，空树为 NULL
}rbt_root_cached;
// insert_check 给出的插入点，只给紧接着的 insert_commit 用
typedef struct rbt_insert_pos {
    rbt_node *parent;           // 挂到它下面，空树为 NULL
    rbt_node **link;            // parent 里要写的那个孩子指针（空树时为根指针）
    int32_t leftmost;           // 下降时一路往左（成为新最左）为 1
}rbt_insert_pos;

// 父指针与颜色压在一个字段里（pc）：父地址去掉低两位即得，指针与整数互转一律经 void *
static inline rbt_node *_rbt_pc_parent(uintptr_t pc) {
    return (rbt_node *)(void *)(pc & ~(uintptr_t)3);
}
static inline int32_t _rbt_pc_is_black(uintptr_t pc) {
    return (int32_t)(pc & RBT_BLACK);
}
/// <summary>
/// 取父节点
/// </summary>
/// <param name="node">树中节点</param>
/// <returns>父节点；根节点返回 NULL</returns>
static inline rbt_node *rbt_parent(const rbt_node *node) {
    return _rbt_pc_parent(node->rbt_parent_color);
}
static inline int32_t _rbt_is_black(const rbt_node *node) {
    return _rbt_pc_is_black(node->rbt_parent_color);
}
static inline int32_t _rbt_is_red(const rbt_node *node) {
    return !_rbt_is_black(node);
}
// 只对红节点用：红色 bit0 为 0，字段值就是父地址
static inline rbt_node *_rbt_red_parent(const rbt_node *red) {
    return (rbt_node *)(void *)red->rbt_parent_color;
}
static inline void _rbt_set_black(rbt_node *node) {
    node->rbt_parent_color |= RBT_BLACK;
}
// 换父节点，保留颜色
static inline void _rbt_set_parent(rbt_node *node, rbt_node *parent) {
    node->rbt_parent_color = (node->rbt_parent_color & RBT_BLACK) + (uintptr_t)(void *)parent;
}
// 父节点与颜色一起覆盖
static inline void _rbt_set_parent_color(rbt_node *node, rbt_node *parent, uintptr_t color) {
    node->rbt_parent_color = (uintptr_t)(void *)parent + color;
}
// 把 parent 指向 old 的那根指针改指 rep；parent 为 NULL 时改根
static inline void _rbt_change_child(rbt_node *old, rbt_node *rep, rbt_node *parent, rbt_root *root) {
    if (NULL != parent) {
        if (parent->rbt_left == old) {
            parent->rbt_left = rep;
        } else {
            parent->rbt_right = rep;
        }
    } else {
        root->rbt_node = rep;
    }
}
// 旋转收尾：rep 接过 old 的父节点与颜色，old 挂到 rep 下并染成 color
static inline void _rbt_rotate_set_parents(rbt_node *old, rbt_node *rep, rbt_root *root, uintptr_t color) {
    rbt_node *parent = rbt_parent(old);
    rep->rbt_parent_color = old->rbt_parent_color;
    _rbt_set_parent_color(old, rep, color);
    _rbt_change_child(old, rep, parent, root);
}
/// <summary>
/// 运行期把树重置为空（不碰原有节点）
/// </summary>
/// <param name="root">rbt_root 指针</param>
static inline void rbt_root_init(rbt_root *root) {
    root->rbt_node = NULL;
}
/// <summary>
/// 同 rbt_root_init，另清最左缓存
/// </summary>
/// <param name="root">rbt_root_cached 指针</param>
static inline void rbt_root_cached_init(rbt_root_cached *root) {
    root->rbt_root.rbt_node = NULL;
    root->rbt_leftmost = NULL;
}
/// <summary>
/// 把 node 作为红色叶子挂到 parent 下 link 所指的空位，之后须调 rbt_insert_color
/// </summary>
/// <param name="node">待插入节点，写全三个字段，无需预先清零</param>
/// <param name="parent">空位的父节点，空树为 NULL</param>
/// <param name="link">下降循环停下的空位：&parent->rbt_left、&parent->rbt_right 或 &root->rbt_node</param>
static inline void rbt_link_node(rbt_node *node, rbt_node *parent, rbt_node **link) {
    node->rbt_parent_color = (uintptr_t)(void *)parent;// 红为 0，直接写父地址就是红色
    node->rbt_left = NULL;
    node->rbt_right = NULL;
    *link = node;
}
/// <summary>
/// 插入修色：rbt_link_node 挂上的红节点破坏了红黑性质，逐层向上修复
/// </summary>
/// <param name="root">所在树</param>
/// <param name="node">刚由 rbt_link_node 挂上的节点</param>
static inline void rbt_insert_color(rbt_root *root, rbt_node *node) {
    rbt_node *parent = _rbt_red_parent(node);
    rbt_node *gparent;
    rbt_node *tmp;
    for (;;) {
        if (NULL == parent) {
            _rbt_set_parent_color(node, NULL, RBT_BLACK);// 升到了根：根染黑
            break;
        }
        if (_rbt_is_black(parent)) {// 父黑：没有连续的红，结束
            break;
        }
        gparent = _rbt_red_parent(parent);// 父红就不是根，祖父一定存在且为黑
        tmp = gparent->rbt_right;
        if (parent != tmp) {// parent 在祖父左侧，tmp 是叔叔
            if (NULL != tmp && _rbt_is_red(tmp)) {
                // 叔叔红：父叔染黑、祖父染红，黑高不变，问题上移到祖父继续
                _rbt_set_parent_color(tmp, gparent, RBT_BLACK);
                _rbt_set_parent_color(parent, gparent, RBT_BLACK);
                node = gparent;
                parent = rbt_parent(node);
                _rbt_set_parent_color(node, parent, RBT_RED);
                continue;
            }
            tmp = parent->rbt_right;
            if (node == tmp) {
                // 叔叔黑且 node 在内侧：在 parent 左旋转成外侧，node 与 parent 互换角色
                tmp = node->rbt_left;
                parent->rbt_right = tmp;
                node->rbt_left = parent;
                if (NULL != tmp) {
                    _rbt_set_parent_color(tmp, parent, RBT_BLACK);
                }
                _rbt_set_parent_color(parent, node, RBT_RED);
                parent = node;
                tmp = node->rbt_right;
            }
            // 叔叔黑且 node 在外侧：在祖父右旋，parent 顶上去染黑、祖父染红，结束
            gparent->rbt_left = tmp;
            parent->rbt_right = gparent;
            if (NULL != tmp) {
                _rbt_set_parent_color(tmp, gparent, RBT_BLACK);
            }
            _rbt_rotate_set_parents(gparent, parent, root, RBT_RED);
            break;
        }
        // 以下是镜像：parent 在祖父右侧，左右对调，其余同上
        tmp = gparent->rbt_left;
        if (NULL != tmp && _rbt_is_red(tmp)) {
            _rbt_set_parent_color(tmp, gparent, RBT_BLACK);
            _rbt_set_parent_color(parent, gparent, RBT_BLACK);
            node = gparent;
            parent = rbt_parent(node);
            _rbt_set_parent_color(node, parent, RBT_RED);
            continue;
        }
        tmp = parent->rbt_left;
        if (node == tmp) {
            tmp = node->rbt_right;
            parent->rbt_left = tmp;
            node->rbt_right = parent;
            if (NULL != tmp) {
                _rbt_set_parent_color(tmp, parent, RBT_BLACK);
            }
            _rbt_set_parent_color(parent, node, RBT_RED);
            parent = node;
            tmp = node->rbt_left;
        }
        gparent->rbt_right = tmp;
        parent->rbt_left = gparent;
        if (NULL != tmp) {
            _rbt_set_parent_color(tmp, gparent, RBT_BLACK);
        }
        _rbt_rotate_set_parents(gparent, parent, root, RBT_RED);
        break;
    }
}
// 把 node 摘出树：至多一个孩子时直接摘，两个孩子时由后继整节点顶位（不搬元素、不拷键）。
// 返回删除修色的起点，不需要修色返回 NULL
static inline rbt_node *_rbt_erase_node(rbt_node *node, rbt_root *root) {
    rbt_node *child = node->rbt_right;
    rbt_node *tmp = node->rbt_left;
    rbt_node *parent;
    rbt_node *rebalance;
    rbt_node *successor;
    rbt_node *child2;
    uintptr_t pc;
    if (NULL == tmp) {
        // 没有左孩子：右孩子（可以为空）顶到 node 的位置
        pc = node->rbt_parent_color;
        parent = _rbt_pc_parent(pc);
        _rbt_change_child(node, child, parent, root);
        if (NULL != child) {
            child->rbt_parent_color = pc;// 独子必红、node 必黑：独子接过 node 的父与颜色，黑高不变
            rebalance = NULL;
        } else {
            rebalance = _rbt_pc_is_black(pc) ? parent : NULL;// 摘掉黑叶子才少一个黑
        }
    } else if (NULL == child) {
        // 只有左孩子：同上，左孩子接过 node 的父与颜色
        pc = node->rbt_parent_color;
        tmp->rbt_parent_color = pc;
        parent = _rbt_pc_parent(pc);
        _rbt_change_child(node, tmp, parent, root);
        rebalance = NULL;
    } else {
        // 两个孩子：后继（右子树最左）整节点顶到 node 的位置，元素本身不搬
        successor = child;
        tmp = child->rbt_left;
        if (NULL == tmp) {
            // 后继就是右孩子：原地上提，它的右子树跟着走
            parent = successor;
            child2 = successor->rbt_right;
        } else {
            // 后继在更深处：先摘下它（它的右子树补到原位），再把 node 的右子树挂到它右边
            do {
                parent = successor;
                successor = tmp;
                tmp = tmp->rbt_left;
            } while (NULL != tmp);
            child2 = successor->rbt_right;
            parent->rbt_left = child2;
            successor->rbt_right = child;
            _rbt_set_parent(child, successor);
        }
        // node 的左子树交给后继，后继接替 node 在父节点下的位置
        tmp = node->rbt_left;
        successor->rbt_left = tmp;
        _rbt_set_parent(tmp, successor);
        pc = node->rbt_parent_color;
        tmp = _rbt_pc_parent(pc);
        _rbt_change_child(node, successor, tmp, root);
        // 后继原位置由 child2 补上：child2 必红，染黑即补回黑高；没有 child2 时后继原为黑才要修色
        if (NULL != child2) {
            _rbt_set_parent_color(child2, parent, RBT_BLACK);
            rebalance = NULL;
        } else {
            rebalance = _rbt_is_black(successor) ? parent : NULL;
        }
        successor->rbt_parent_color = pc;// 接过 node 的父与颜色，必须在读完后继原颜色之后
    }
    return rebalance;
}
// 删除修色：经过 parent 某一侧的路径比别处少一个黑节点（首轮缺黑的位置为 NULL）
static inline void _rbt_erase_color(rbt_node *parent, rbt_root *root) {
    rbt_node *node = NULL;
    rbt_node *sibling;
    rbt_node *tmp1;
    rbt_node *tmp2;
    for (;;) {
        // 每轮进来时：node 为黑或 NULL，经过它的路径比 parent 另一侧少一个黑；兄弟一定存在
        sibling = parent->rbt_right;
        if (node != sibling) {// 缺黑在 parent 左侧
            if (_rbt_is_red(sibling)) {
                // 兄弟红：在 parent 左旋并互换颜色，转成兄弟黑的情形
                tmp1 = sibling->rbt_left;
                parent->rbt_right = tmp1;
                sibling->rbt_left = parent;
                _rbt_set_parent_color(tmp1, parent, RBT_BLACK);
                _rbt_rotate_set_parents(parent, sibling, root, RBT_RED);
                sibling = tmp1;
            }
            tmp1 = sibling->rbt_right;
            if (NULL == tmp1 || _rbt_is_black(tmp1)) {
                tmp2 = sibling->rbt_left;
                if (NULL == tmp2 || _rbt_is_black(tmp2)) {
                    // 兄弟与两个侄子都黑：兄弟染红，两侧一起少一个黑；
                    // parent 红就染黑补回来结束，否则缺黑上移到 parent，到根为止
                    _rbt_set_parent_color(sibling, parent, RBT_RED);
                    if (_rbt_is_red(parent)) {
                        _rbt_set_black(parent);
                    } else {
                        node = parent;
                        parent = rbt_parent(node);
                        if (NULL != parent) {
                            continue;
                        }
                    }
                    break;
                }
                // 远侄子黑、近侄子红：在兄弟右旋，转成远侄子红的情形
                tmp1 = tmp2->rbt_right;
                sibling->rbt_left = tmp1;
                tmp2->rbt_right = sibling;
                parent->rbt_right = tmp2;
                if (NULL != tmp1) {
                    _rbt_set_parent_color(tmp1, sibling, RBT_BLACK);
                }
                tmp1 = sibling;// 旋转后原兄弟成了远侄子
                sibling = tmp2;
            }
            // 远侄子红：在 parent 左旋，兄弟接过 parent 的颜色，parent 与远侄子染黑，结束
            tmp2 = sibling->rbt_left;
            parent->rbt_right = tmp2;
            sibling->rbt_left = parent;
            _rbt_set_parent_color(tmp1, sibling, RBT_BLACK);
            if (NULL != tmp2) {
                _rbt_set_parent(tmp2, parent);
            }
            _rbt_rotate_set_parents(parent, sibling, root, RBT_BLACK);
            break;
        }
        // 以下是镜像：缺黑在 parent 右侧，左右对调，其余同上
        sibling = parent->rbt_left;
        if (_rbt_is_red(sibling)) {
            tmp1 = sibling->rbt_right;
            parent->rbt_left = tmp1;
            sibling->rbt_right = parent;
            _rbt_set_parent_color(tmp1, parent, RBT_BLACK);
            _rbt_rotate_set_parents(parent, sibling, root, RBT_RED);
            sibling = tmp1;
        }
        tmp1 = sibling->rbt_left;
        if (NULL == tmp1 || _rbt_is_black(tmp1)) {
            tmp2 = sibling->rbt_right;
            if (NULL == tmp2 || _rbt_is_black(tmp2)) {
                _rbt_set_parent_color(sibling, parent, RBT_RED);
                if (_rbt_is_red(parent)) {
                    _rbt_set_black(parent);
                } else {
                    node = parent;
                    parent = rbt_parent(node);
                    if (NULL != parent) {
                        continue;
                    }
                }
                break;
            }
            tmp1 = tmp2->rbt_left;
            sibling->rbt_right = tmp1;
            tmp2->rbt_left = sibling;
            parent->rbt_left = tmp2;
            if (NULL != tmp1) {
                _rbt_set_parent_color(tmp1, sibling, RBT_BLACK);
            }
            tmp1 = sibling;
            sibling = tmp2;
        }
        tmp2 = sibling->rbt_right;
        parent->rbt_left = tmp2;
        sibling->rbt_right = parent;
        _rbt_set_parent_color(tmp1, sibling, RBT_BLACK);
        if (NULL != tmp2) {
            _rbt_set_parent(tmp2, parent);
        }
        _rbt_rotate_set_parents(parent, sibling, root, RBT_BLACK);
        break;
    }
}
/// <summary>
/// 从树中删除 node 并修色；不校验 node 是否在树中，也不清 node 的字段
/// </summary>
/// <param name="root">所在树；cached 树须改用 rbt_erase_cached</param>
/// <param name="node">树中节点</param>
static inline void rbt_erase(rbt_root *root, rbt_node *node) {
    rbt_node *rebalance = _rbt_erase_node(node, root);
    if (NULL != rebalance) {
        _rbt_erase_color(rebalance, root);
    }
}
/// <summary>
/// 中序第一个（最小）节点
/// </summary>
/// <param name="root">树根</param>
/// <returns>最小节点；空树返回 NULL</returns>
static inline rbt_node *rbt_first(const rbt_root *root) {
    rbt_node *node = root->rbt_node;
    if (NULL == node) {
        return NULL;
    }
    while (NULL != node->rbt_left) {
        node = node->rbt_left;
    }
    return node;
}
/// <summary>
/// 中序最后一个（最大）节点
/// </summary>
/// <param name="root">树根</param>
/// <returns>最大节点；空树返回 NULL</returns>
static inline rbt_node *rbt_last(const rbt_root *root) {
    rbt_node *node = root->rbt_node;
    if (NULL == node) {
        return NULL;
    }
    while (NULL != node->rbt_right) {
        node = node->rbt_right;
    }
    return node;
}
/// <summary>
/// 中序后继
/// </summary>
/// <param name="node">树中节点</param>
/// <returns>后继；node 是最大节点或 RBT_EMPTY_NODE 时返回 NULL</returns>
static inline rbt_node *rbt_next(const rbt_node *node) {
    rbt_node *parent;
    if (RBT_EMPTY_NODE(node)) {
        return NULL;
    }
    if (NULL != node->rbt_right) {// 有右子树：后继是右子树的最左节点
        node = node->rbt_right;
        while (NULL != node->rbt_left) {
            node = node->rbt_left;
        }
        return (rbt_node *)node;
    }
    // 没有右子树：往上走，直到自己落在某个祖先的左子树里，那个祖先就是后继
    while (NULL != (parent = rbt_parent(node)) && node == parent->rbt_right) {
        node = parent;
    }
    return parent;
}
/// <summary>
/// 中序前驱
/// </summary>
/// <param name="node">树中节点</param>
/// <returns>前驱；node 是最小节点或 RBT_EMPTY_NODE 时返回 NULL</returns>
static inline rbt_node *rbt_prev(const rbt_node *node) {
    rbt_node *parent;
    if (RBT_EMPTY_NODE(node)) {
        return NULL;
    }
    if (NULL != node->rbt_left) {// 与 rbt_next 左右对调
        node = node->rbt_left;
        while (NULL != node->rbt_right) {
            node = node->rbt_right;
        }
        return (rbt_node *)node;
    }
    while (NULL != (parent = rbt_parent(node)) && node == parent->rbt_left) {
        node = parent;
    }
    return parent;
}
/// <summary>
/// 用 rep 原位替换 victim，不修色、不改变树形；调用方保证二者等键
/// </summary>
/// <param name="root">所在树；cached 树须改用 rbt_replace_node_cached</param>
/// <param name="victim">树中节点，替换后字段保持原样、不会被清空</param>
/// <param name="rep">替换节点，无需初始化，但不得在任何树中</param>
static inline void rbt_replace_node(rbt_root *root, rbt_node *victim, rbt_node *rep) {
    rbt_node *parent = rbt_parent(victim);
    *rep = *victim;// 指针与颜色整个拷过去，再让孩子与父节点改指 rep
    if (NULL != victim->rbt_left) {
        _rbt_set_parent(victim->rbt_left, rep);
    }
    if (NULL != victim->rbt_right) {
        _rbt_set_parent(victim->rbt_right, rep);
    }
    _rbt_change_child(victim, rep, parent, root);
}
// 从 node 起优先往左、没有左孩子才往右，一直走到叶子
static inline rbt_node *_rbt_left_deepest_node(const rbt_node *node) {
    for (;;) {
        if (NULL != node->rbt_left) {
            node = node->rbt_left;
        } else if (NULL != node->rbt_right) {
            node = node->rbt_right;
        } else {
            return (rbt_node *)node;
        }
    }
}
/// <summary>
/// 后序第一个节点（孩子先于父）
/// </summary>
/// <param name="root">树根</param>
/// <returns>后序第一个节点；空树返回 NULL</returns>
static inline rbt_node *rbt_first_postorder(const rbt_root *root) {
    if (NULL == root->rbt_node) {
        return NULL;
    }
    return _rbt_left_deepest_node(root->rbt_node);
}
/// <summary>
/// 后序下一个节点；前面访问过的节点可以已被释放（见文件头契约 5）
/// </summary>
/// <param name="node">当前节点，可为 NULL</param>
/// <returns>下一个节点；node 为根或 NULL 时返回 NULL</returns>
static inline rbt_node *rbt_next_postorder(const rbt_node *node) {
    rbt_node *parent;
    if (NULL == node) {
        return NULL;
    }
    parent = rbt_parent(node);
    // 自己是左孩子且有右兄弟：下一个是右兄弟子树的后序第一个；否则父节点的孩子都走完了，轮到父节点
    if (NULL != parent && node == parent->rbt_left && NULL != parent->rbt_right) {
        return _rbt_left_deepest_node(parent->rbt_right);
    }
    return parent;
}
/// <summary>
/// 缓存的最左（最小）节点，O(1)
/// </summary>
/// <param name="root">rbt_root_cached 指针</param>
/// <returns>最小节点；空树返回 NULL</returns>
static inline rbt_node *rbt_first_cached(const rbt_root_cached *root) {
    return root->rbt_leftmost;
}
/// <summary>
/// 同 rbt_insert_color，另维护最左缓存
/// </summary>
/// <param name="root">所在树</param>
/// <param name="node">刚由 rbt_link_node 挂上的节点</param>
/// <param name="leftmost">下降时一路往左（node 成为新最左）为非 0</param>
static inline void rbt_insert_color_cached(rbt_root_cached *root, rbt_node *node, int32_t leftmost) {
    if (leftmost) {
        root->rbt_leftmost = node;
    }
    rbt_insert_color(&root->rbt_root, node);
}
/// <summary>
/// 同 rbt_erase，另维护最左缓存
/// </summary>
/// <param name="root">所在树</param>
/// <param name="node">树中节点</param>
/// <returns>node 是最左时返回新的最左节点（删空为 NULL）；node 不是最左时返回 NULL，缓存不变</returns>
static inline rbt_node *rbt_erase_cached(rbt_root_cached *root, rbt_node *node) {
    rbt_node *leftmost = NULL;
    if (root->rbt_leftmost == node) {
        leftmost = rbt_next(node);// 必须在摘除之前取后继
        root->rbt_leftmost = leftmost;
    }
    rbt_erase(&root->rbt_root, node);
    return leftmost;
}
/// <summary>
/// 同 rbt_replace_node，另维护最左缓存
/// </summary>
/// <param name="root">所在树</param>
/// <param name="victim">树中节点</param>
/// <param name="rep">替换节点，约束同 rbt_replace_node</param>
static inline void rbt_replace_node_cached(rbt_root_cached *root, rbt_node *victim, rbt_node *rep) {
    if (root->rbt_leftmost == victim) {
        root->rbt_leftmost = rep;
    }
    rbt_replace_node(&root->rbt_root, victim, rep);
}

#endif//RBTREE_H_
