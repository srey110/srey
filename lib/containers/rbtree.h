#ifndef RBTREE_H_
#define RBTREE_H_

#include "base/macro.h"

// 红黑树（侵入式）。rbt_node 嵌入元素，树不分配内存，元素地址即句柄。
//
// 典型用法一：宏生成带类型的辅助（比较器是宏，必然内联）
//   typedef struct { uint64_t due; rbt_node node; } tmr;
//   #define TMR_CMP(a, b)  (((a)->due > (b)->due) - ((a)->due < (b)->due))
//   #define TMR_KCMP(k, e) (((k) > (e)->due) - ((k) < (e)->due))
//   RBT_DECL(tmr_tree, tmr, node, TMR_CMP)
//   RBT_DECL_KEY(tmr_tree, tmr, uint64_t, TMR_KCMP)
//   rbt_root_cached q = RBT_ROOT_CACHED_INIT;
//   tmr_tree_add_cached(&t, &q);                        // 允许重复键，相等的排到末尾
//   tmr *first = tmr_tree_entry(rbt_first_cached(&q));  // O(1) 取最小
//   rbt_erase_cached(&t.node, &q);
// 典型用法二：手写下降循环
//   rbt_node **link = &root->rbt_node, *parent = NULL, *cur = *link;
//   while (NULL != cur) { parent = cur; link = 小于 ? &cur->rbt_left : &cur->rbt_right; cur = *link; }
//   rbt_link_node(&e->node, parent, link);
//   rbt_insert_color(&e->node, root);
//
// RBT_DECL(name, T, FIELD, CMP) 生成（name 只作函数前缀，不生成类型）：
//   name##_entry(n) / name##_node(e)：rbt_node * 与 T * 互转，entry 对 NULL 返回 NULL
//   name##_add(e, root) / name##_add_cached(e, root)：允许重复键，相等的排到相等段末尾；
//       add_cached 在 e 成为新最左时返回 e，否则 NULL
//   name##_find_add(e, root) / name##_find_add_cached(e, root)：已有相等元素则返回它且不碰 e，
//       否则插入并返回 NULL（不报告是否成为新最左）
// RBT_DECL_KEY(name, T, K, KCMP) 生成（须在同名 RBT_DECL 之后）：
//   name##_find(k, root)：任一匹配（有重复键时不保证是哪一个）
//   name##_find_first(k, root)：最左匹配
//   name##_next_match(k, e)：e 的中序后继与 k 匹配则返回它，否则 NULL（不向后扫描）
//   name##_lower_bound(k, root) / name##_upper_bound(k, root)：同 std::set，第一个 >= k / > k 的元素，
//       没有返回 NULL；半开区间 [a, b) 就是从 lower_bound(a) 沿 rbt_next 走到 lower_bound(b) 为止
//
// 契约：
//   1. 一个 rbt_node 字段同时只属于一棵树；插入前节点必须不在任何树里（同一节点重复插入会破坏树）；
//      一个元素挂多棵树就放多个 rbt_node 字段、各自 DECL。rbt_link_node 写全三个字段，插入前无需清零。
//   2. rbt_erase 不校验成员关系、也不清节点；要靠 RBT_EMPTY_NODE 判断在不在树里，就在初始化时和
//      erase 之后自己 RBT_CLEAR_NODE。对 RBT_EMPTY_NODE 的节点调 rbt_next / rbt_prev 返回 NULL。
//   3. 元素在树中期间不得移动地址，不得改动参与比较的字段（改键先 erase 再重新插入）。
//   4. CMP(a, b) 三路比较，a、b 为 T const *：可以返回 0（相等可并存），但符号必须有意义，
//      比 HASHMAP_DECL 的 CMPFN 严格。KCMP(k, e) 的 e 为 T const *，必须与 CMP 同向：按中序看 KCMP(k, ·)
//      只能先 >0、再 =0、后 <0；可以比 CMP 粗（如 CMP 比 (分数, id)、KCMP 只比分数），不能换一种排序依据，
//      否则 find 系列会漏找。回绕比较只在树内键都落在半个取值区间内时成立。
//      两者的返回值都会存进 int32_t，写成 (a > b) - (a < b)，别写相减（宽类型相减会截断）。
//   5. 手写 rbt_next 循环时可以先取 next 再 erase 当前节点；rbt_for_each 的步进要读当前节点，
//      循环体内禁止增删替换。后序遍历中只许释放 pos 的内存，禁止增删替换，走完后调用方把 root 重置为空。
//   6. cached 树只能用 *_cached 系列增删替换，否则缓存的最左会过期；只读接口可以直接传 &root->rbt_root。
//   7. rbt_replace_node：新旧节点须等键；新节点无需初始化但不得在任何树中；旧节点替换后不会被清空。
//   8. rbt_node 不得放进 packed 结构（颜色占父地址的最低位）。

#define RBT_RED ((uintptr_t)0) // rbt_parent_color 的 bit0：0 红 1 黑
#define RBT_BLACK ((uintptr_t)1)

#define RBT_ROOT_INIT { NULL } // 只能用于定义时初始化，运行期重置用 rbt_root_init
#define RBT_ROOT_CACHED_INIT { { NULL }, NULL } // 同上，运行期重置用 rbt_root_cached_init
#define RBT_EMPTY_ROOT(root) (NULL == (root)->rbt_node)
// 父指针指向自己表示节点不在任何树里；node 会多次求值，须传无副作用的表达式
#define RBT_EMPTY_NODE(node) ((node)->rbt_parent_color == (uintptr_t)(const void *)(node))
#define RBT_CLEAR_NODE(node) ((node)->rbt_parent_color = (uintptr_t)(void *)(node))

// 遍历与 key 匹配的所有元素；it 为 T *，由调用方先声明；key 会多次求值；循环体内禁止增删替换
#define rbt_for_each(it, key, root, name) \
    for ((it) = name##_find_first((key), (root)); NULL != (it); (it) = name##_next_match((key), (it)))
// 后序遍历（孩子先于父），循环体内可释放 pos；pos、n 为 T *，由调用方先声明；循环中禁止增删替换
#define rbt_postorder_for_each_entry_safe(pos, n, root, name)                                   \
    for ((pos) = name##_entry(rbt_first_postorder(root)),                                       \
         (n) = (NULL != (pos)) ? name##_entry(rbt_next_postorder(name##_node(pos))) : NULL;     \
         NULL != (pos);                                                                         \
         (pos) = (n), (n) = (NULL != (pos)) ? name##_entry(rbt_next_postorder(name##_node(pos))) : NULL)

// RBT_DECL(name, T, FIELD, CMP)：name 生成函数的前缀，T 元素类型，FIELD 元素内 rbt_node 字段名，
// CMP(a, b) 三路比较宏（契约见文件头第 4 条）
#define RBT_DECL(name, T, FIELD, CMP)                                              \
/* FIELD 不是首字段时 UPCAST(NULL) 不是 NULL，须先判 */                            \
static inline T *name##_entry(rbt_node *n) {                                      \
    return (NULL == n) ? NULL : UPCAST(n, T, FIELD);                               \
}                                                                                 \
static inline rbt_node *name##_node(T *e) {                                       \
    return &e->FIELD;                                                              \
}                                                                                 \
/* 相等时往右走，新元素排到相等段末尾 */                                           \
static inline void name##_add(T *e, rbt_root *root) {                             \
    rbt_node **link = &root->rbt_node;                                             \
    rbt_node *parent = NULL;                                                       \
    rbt_node *cur = *link;                                                         \
    while (NULL != cur) {                                                          \
        parent = cur;                                                              \
        if (CMP(e, UPCAST(cur, T, FIELD)) < 0) {                                   \
            link = &cur->rbt_left;                                                 \
        } else {                                                                   \
            link = &cur->rbt_right;                                                \
        }                                                                          \
        cur = *link;                                                               \
    }                                                                              \
    rbt_link_node(&e->FIELD, parent, link);                                        \
    rbt_insert_color(&e->FIELD, root);                                             \
}                                                                                 \
/* 一路往左才是新最左 */                                                           \
static inline T *name##_add_cached(T *e, rbt_root_cached *root) {                 \
    rbt_node **link = &root->rbt_root.rbt_node;                                    \
    rbt_node *parent = NULL;                                                       \
    rbt_node *cur = *link;                                                         \
    int32_t leftmost = 1;                                                          \
    while (NULL != cur) {                                                          \
        parent = cur;                                                              \
        if (CMP(e, UPCAST(cur, T, FIELD)) < 0) {                                   \
            link = &cur->rbt_left;                                                 \
        } else {                                                                   \
            link = &cur->rbt_right;                                                \
            leftmost = 0;                                                          \
        }                                                                          \
        cur = *link;                                                               \
    }                                                                              \
    rbt_link_node(&e->FIELD, parent, link);                                        \
    rbt_insert_color_cached(&e->FIELD, root, leftmost);                            \
    return leftmost ? e : NULL;                                                    \
}                                                                                 \
static inline T *name##_find_add(T *e, rbt_root *root) {                          \
    rbt_node **link = &root->rbt_node;                                             \
    rbt_node *parent = NULL;                                                       \
    rbt_node *cur = *link;                                                         \
    int32_t c;                                                                     \
    while (NULL != cur) {                                                          \
        parent = cur;                                                              \
        c = CMP(e, UPCAST(cur, T, FIELD));                                         \
        if (c < 0) {                                                               \
            link = &cur->rbt_left;                                                 \
        } else if (c > 0) {                                                        \
            link = &cur->rbt_right;                                                \
        } else {                                                                   \
            return UPCAST(cur, T, FIELD);                                          \
        }                                                                          \
        cur = *link;                                                               \
    }                                                                              \
    rbt_link_node(&e->FIELD, parent, link);                                        \
    rbt_insert_color(&e->FIELD, root);                                             \
    return NULL;                                                                   \
}                                                                                 \
static inline T *name##_find_add_cached(T *e, rbt_root_cached *root) {            \
    rbt_node **link = &root->rbt_root.rbt_node;                                    \
    rbt_node *parent = NULL;                                                       \
    rbt_node *cur = *link;                                                         \
    int32_t leftmost = 1;                                                          \
    int32_t c;                                                                     \
    while (NULL != cur) {                                                          \
        parent = cur;                                                              \
        c = CMP(e, UPCAST(cur, T, FIELD));                                         \
        if (c < 0) {                                                               \
            link = &cur->rbt_left;                                                 \
        } else if (c > 0) {                                                        \
            link = &cur->rbt_right;                                                \
            leftmost = 0;                                                          \
        } else {                                                                   \
            return UPCAST(cur, T, FIELD);                                          \
        }                                                                          \
        cur = *link;                                                               \
    }                                                                              \
    rbt_link_node(&e->FIELD, parent, link);                                        \
    rbt_insert_color_cached(&e->FIELD, root, leftmost);                            \
    return NULL;                                                                   \
}

// RBT_DECL_KEY(name, T, K, KCMP)：name、T 同 RBT_DECL，K 键类型，KCMP(k, e) 三路比较宏（契约见文件头第 4 条）
#define RBT_DECL_KEY(name, T, K, KCMP)                                             \
static inline T *name##_find(K k, const rbt_root *root) {                         \
    rbt_node *node = root->rbt_node;                                               \
    T *e;                                                                          \
    int32_t c;                                                                     \
    while (NULL != node) {                                                         \
        e = name##_entry(node);                                                    \
        c = KCMP(k, e);                                                            \
        if (c < 0) {                                                               \
            node = node->rbt_left;                                                 \
        } else if (c > 0) {                                                        \
            node = node->rbt_right;                                                \
        } else {                                                                   \
            return e;                                                              \
        }                                                                          \
    }                                                                              \
    return NULL;                                                                   \
}                                                                                 \
/* 命中后继续往左找，停在相等段最左 */                                             \
static inline T *name##_find_first(K k, const rbt_root *root) {                   \
    rbt_node *node = root->rbt_node;                                               \
    T *match = NULL;                                                               \
    T *e;                                                                          \
    int32_t c;                                                                     \
    while (NULL != node) {                                                         \
        e = name##_entry(node);                                                    \
        c = KCMP(k, e);                                                            \
        if (c <= 0) {                                                              \
            if (0 == c) {                                                          \
                match = e;                                                         \
            }                                                                      \
            node = node->rbt_left;                                                 \
        } else {                                                                   \
            node = node->rbt_right;                                                \
        }                                                                          \
    }                                                                              \
    return match;                                                                  \
}                                                                                 \
/* 同 std::set::lower_bound：第一个不小于 k 的元素，有重复键时停在相等段首 */      \
static inline T *name##_lower_bound(K k, const rbt_root *root) {                  \
    rbt_node *node = root->rbt_node;                                               \
    T *res = NULL;                                                                 \
    T *e;                                                                          \
    while (NULL != node) {                                                         \
        e = name##_entry(node);                                                    \
        if (KCMP(k, e) <= 0) {/* e 不小于 k：记为候选，往左找更靠前的 */            \
            res = e;                                                               \
            node = node->rbt_left;                                                 \
        } else {                                                                   \
            node = node->rbt_right;                                                \
        }                                                                          \
    }                                                                              \
    return res;                                                                    \
}                                                                                 \
/* 同 std::set::upper_bound：第一个大于 k 的元素，有重复键时越过整个相等段 */      \
static inline T *name##_upper_bound(K k, const rbt_root *root) {                  \
    rbt_node *node = root->rbt_node;                                               \
    T *res = NULL;                                                                 \
    T *e;                                                                          \
    while (NULL != node) {                                                         \
        e = name##_entry(node);                                                    \
        if (KCMP(k, e) < 0) {/* e 大于 k：记为候选，往左找更靠前的 */               \
            res = e;                                                               \
            node = node->rbt_left;                                                 \
        } else {                                                                   \
            node = node->rbt_right;                                                \
        }                                                                          \
    }                                                                              \
    return res;                                                                    \
}                                                                                 \
static inline T *name##_next_match(K k, T *e) {                                   \
    T *next = name##_entry(rbt_next(name##_node(e)));                              \
    return (NULL != next && 0 == KCMP(k, next)) ? next : NULL;                     \
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
/// <param name="node">刚由 rbt_link_node 挂上的节点</param>
/// <param name="root">所在树</param>
static inline void rbt_insert_color(rbt_node *node, rbt_root *root) {
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
/// <param name="node">树中节点</param>
/// <param name="root">所在树；cached 树须改用 rbt_erase_cached</param>
static inline void rbt_erase(rbt_node *node, rbt_root *root) {
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
/// <param name="victim">树中节点，替换后字段保持原样、不会被清空</param>
/// <param name="rep">替换节点，无需初始化，但不得在任何树中</param>
/// <param name="root">所在树；cached 树须改用 rbt_replace_node_cached</param>
static inline void rbt_replace_node(rbt_node *victim, rbt_node *rep, rbt_root *root) {
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
/// <param name="node">刚由 rbt_link_node 挂上的节点</param>
/// <param name="root">所在树</param>
/// <param name="leftmost">下降时一路往左（node 成为新最左）为非 0</param>
static inline void rbt_insert_color_cached(rbt_node *node, rbt_root_cached *root, int32_t leftmost) {
    if (leftmost) {
        root->rbt_leftmost = node;
    }
    rbt_insert_color(node, &root->rbt_root);
}
/// <summary>
/// 同 rbt_erase，另维护最左缓存
/// </summary>
/// <param name="node">树中节点</param>
/// <param name="root">所在树</param>
/// <returns>node 是最左时返回新的最左节点（删空为 NULL）；node 不是最左时返回 NULL，缓存不变</returns>
static inline rbt_node *rbt_erase_cached(rbt_node *node, rbt_root_cached *root) {
    rbt_node *leftmost = NULL;
    if (root->rbt_leftmost == node) {
        leftmost = rbt_next(node);// 必须在摘除之前取后继
        root->rbt_leftmost = leftmost;
    }
    rbt_erase(node, &root->rbt_root);
    return leftmost;
}
/// <summary>
/// 同 rbt_replace_node，另维护最左缓存
/// </summary>
/// <param name="victim">树中节点</param>
/// <param name="rep">替换节点，约束同 rbt_replace_node</param>
/// <param name="root">所在树</param>
static inline void rbt_replace_node_cached(rbt_node *victim, rbt_node *rep, rbt_root_cached *root) {
    if (root->rbt_leftmost == victim) {
        root->rbt_leftmost = rep;
    }
    rbt_replace_node(victim, rep, &root->rbt_root);
}

#endif//RBTREE_H_
