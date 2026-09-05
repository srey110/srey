#include "containers/heap.h"

void heap_init(heap_ctx *heap, _heap_compare _compare) {
    ZERO(heap, sizeof(heap_ctx));
    heap->_compare = _compare;
}
// 交换父子节点位置，维护堆的结构性质
static inline void _heap_swap(heap_ctx *heap, heap_node *parent, heap_node *child) {
    ASSERTAB(child->parent == parent
             && (parent->left == child || parent->right == child), ERRSTR_INVPARAM);
    heap_node *pparent = parent->parent;
    heap_node *lchild = child->left;
    heap_node *rchild = child->right;
    heap_node *sibling = NULL;
    if (NULL == pparent) {
        heap->root = child;
    } else if (pparent->left == parent) {
        pparent->left = child;
    } else if (pparent->right == parent) {
        pparent->right = child;
    }
    if (lchild) {
        lchild->parent = parent;
    }
    if (rchild) {
        rchild->parent = parent;
    }
    child->parent = pparent;
    if (parent->left == child) {
        sibling = parent->right;
        child->left = parent;
        child->right = sibling;
    } else {
        sibling = parent->left;
        child->left = sibling;
        child->right = parent;
    }
    if (sibling) {
        sibling->parent = child;
    }
    parent->parent = child;
    parent->left = lchild;
    parent->right = rchild;
}
// 定位第 nelts 个节点(完全二叉树按层编号,1 起)的父节点,返回后 *path 最低位表示该节点
// 是父的左(0)还是右(1)子。nelts 由调用方按各自时机传:insert 传自增后的新槽位,
// remove 传自减前的现末尾节点。返回 NULL 只出现在 insert 的 nelts == 1(空堆首次插入),
// heap_insert 正靠它建根,那条判空不能删;remove 那侧 nelts >= 1 即 root 非空
static heap_node *_heap_last_parent(heap_ctx *heap, uint32_t nelts, uint32_t *path) {
    uint32_t d = 0;
    *path = 0;
    for (uint32_t n = nelts; n >= 2; ++d, n >>= 1) {
        *path = (*path << 1) | (n & 1);
    }
    heap_node *parent = heap->root;
    while (d > 1) {
        parent = (*path & 1) ? parent->right : parent->left;
        --d;
        *path >>= 1;
    }
    return parent;
}
void heap_insert(heap_ctx *heap, heap_node *node) {
    uint32_t path;
    ++heap->nelts;
    heap_node *parent = _heap_last_parent(heap, heap->nelts, &path);
    // 插入节点
    node->parent = parent;
    node->left = node->right = NULL;
    if (NULL == parent) {
        heap->root = node;
    } else if (path & 1) {
        parent->right = node;
    } else {
        parent->left = node;
    }
    // 上浮调整
    while (node->parent
           && heap->_compare(node, node->parent)) {
        _heap_swap(heap, node->parent, node);
    }
}
// 用节点 r 替换节点 s 在堆中的位置（内部使用，r 可为 NULL 表示直接删除）
static inline void _heap_replace(heap_ctx *heap, heap_node *s, heap_node *r) {
    if (NULL == s->parent) {
        heap->root = r;
    } else if (s->parent->left == s) {
        s->parent->left = r;
    } else if (s->parent->right == s) {
        s->parent->right = r;
    }
    if (s->left) {
        s->left->parent = r;
    }
    if (s->right) {
        s->right->parent = r;
    }
    if (r) {
        r->parent = s->parent;
        r->left = s->left;
        r->right = s->right;
    }
}
void heap_remove(heap_ctx *heap, heap_node *node) {
    if (0 == heap->nelts) {
        return;
    }
    uint32_t path;
    heap_node *parent = _heap_last_parent(heap, heap->nelts, &path);
    --heap->nelts;
    // 用末尾节点替换待删除节点
    heap_node *last = NULL;
    if (path & 1) {
        last = parent->right;
        parent->right = NULL;
    } else {
        last = parent->left;
        parent->left = NULL;
    }
    if (NULL == last) {
        if (heap->root == node) {
            heap->root = NULL;
        }
        node->parent = node->left = node->right = NULL;
        return;
    }
    _heap_replace(heap, node, last);
    node->parent = node->left = node->right = NULL;
    heap_node *v = last;
    heap_node *est = NULL;
    // 下沉调整
    while (1) {
        est = v;
        if (v->left) {
            est = heap->_compare(est, v->left) ? est : v->left;
        }
        if (v->right) {
            est = heap->_compare(est, v->right) ? est : v->right;
        }
        if (est == v) {
            break;
        }
        _heap_swap(heap, v, est);
    }
    // 上浮调整
    while (v->parent
           && heap->_compare(v, v->parent)) {
        _heap_swap(heap, v->parent, v);
    }
}
void heap_dequeue(heap_ctx *heap) {
    heap_remove(heap, heap->root);
}
