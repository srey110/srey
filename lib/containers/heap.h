#ifndef HEAP_H_
#define HEAP_H_

#include "base/macro.h"

typedef struct heap_node {
    struct heap_node *parent;
    struct heap_node *left;
    struct heap_node *right;
}heap_node;
typedef int(*_heap_compare)(const heap_node *lhs, const heap_node *rhs);
typedef struct heap_ctx {
    int32_t nelts;
    heap_node *root;
    // if compare is less_than, root is min of heap
    // if compare is larger_than, root is max of heap
    _heap_compare _compare;
}heap_ctx;

void heap_init(heap_ctx *heap, _heap_compare _compare);
void heap_insert(heap_ctx *heap, heap_node *node);
void heap_remove(heap_ctx *heap, heap_node *node);
void heap_dequeue(heap_ctx *heap);

#endif//HEAP_H_
