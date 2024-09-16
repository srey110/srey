#include "containers/heap.h"

void heap_init(heap_ctx *heap, _heap_compare _compare) {
    ZERO(heap, sizeof(heap_ctx));
    heap->_compare = _compare;
}
static void _heap_swap(heap_ctx *heap, heap_node *parent, heap_node *child) {
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
void heap_insert(heap_ctx *heap, heap_node *node) {
    // 0: left, 1: right
    int32_t path = 0;
    int32_t n, d;
    ++heap->nelts;
    // traverse from bottom to up, get path of last node
    for (d = 0, n = heap->nelts; n >= 2; ++d, n >>= 1) {
        path = (path << 1) | (n & 1);
    }
    // get last->parent by path
    heap_node *parent = heap->root;
    while (d > 1) {
        parent = (path & 1) ? parent->right : parent->left;
        --d;
        path >>= 1;
    }
    // insert node
    node->parent = parent;
    if (NULL == parent) {
        heap->root = node;
    } else if (path & 1) {
        parent->right = node;
    } else {
        parent->left = node;
    }
    // sift up
    if (heap->_compare) {
        while (node->parent 
            && heap->_compare(node, node->parent)) {
            _heap_swap(heap, node->parent, node);
        }
    }
}
static void _heap_replace(heap_ctx *heap, heap_node *s, heap_node *r) {
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
    // 0: left, 1: right
    int32_t path = 0;
    int32_t n, d;
    // traverse from bottom to up, get path of last node
    for (d = 0, n = heap->nelts; n >= 2; ++d, n >>= 1) {
        path = (path << 1) | (n & 1);
    }
    --heap->nelts;
    // get last->parent by path
    heap_node *parent = heap->root;
    while (d > 1) {
        parent = (path & 1) ? parent->right : parent->left;
        --d;
        path >>= 1;
    }
    // replace node with last
    heap_node *last = NULL;
    if (NULL == parent) {
        return;
    } else if (path & 1) {
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
        return;
    }
    _heap_replace(heap, node, last);
    node->parent = node->left = node->right = NULL;
    if (!heap->_compare) {
        return;
    }
    heap_node *v = last;
    heap_node *est = NULL;
    // sift down
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
    // sift up
    while (v->parent 
        && heap->_compare(v, v->parent)) {
        _heap_swap(heap, v->parent, v);
    }
}
void heap_dequeue(heap_ctx *heap) {
    heap_remove(heap, heap->root);
}
