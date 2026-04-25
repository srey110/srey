#ifndef HEAP_H_
#define HEAP_H_

#include "base/macro.h"

// 堆节点，嵌入用户结构体使用（侵入式链接）
typedef struct heap_node {
    struct heap_node *parent; // 父节点
    struct heap_node *left;   // 左子节点
    struct heap_node *right;  // 右子节点
}heap_node;
// 堆比较函数类型：lhs 优先级高于 rhs 时返回非零（决定最小堆/最大堆）
typedef int(*_heap_compare)(const heap_node *lhs, const heap_node *rhs);
// 二叉堆上下文（数组式完全二叉树，通过节点指针链接）
typedef struct heap_ctx {
    int32_t    nelts;    // 当前元素数量
    heap_node *root;     // 根节点指针
    // 比较函数为 less_than 时为最小堆（根最小），为 larger_than 时为最大堆（根最大）
    _heap_compare _compare; // 节点优先级比较函数
}heap_ctx;

/// <summary>
/// 初始化堆
/// </summary>
/// <param name="heap">heap_ctx 指针</param>
/// <param name="_compare">比较函数，lhs 应排在 rhs 前返回非零</param>
void heap_init(heap_ctx *heap, _heap_compare _compare);
/// <summary>
/// 向堆中插入节点
/// </summary>
/// <param name="heap">heap_ctx 指针</param>
/// <param name="node">要插入的节点</param>
void heap_insert(heap_ctx *heap, heap_node *node);
/// <summary>
/// 从堆中移除指定节点
/// </summary>
/// <param name="heap">heap_ctx 指针</param>
/// <param name="node">要移除的节点</param>
void heap_remove(heap_ctx *heap, heap_node *node);
/// <summary>
/// 移除堆顶节点（根节点）
/// </summary>
/// <param name="heap">heap_ctx 指针</param>
void heap_dequeue(heap_ctx *heap);

#endif//HEAP_H_
