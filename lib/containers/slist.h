#ifndef SLIST_H_
#define SLIST_H_

#include "base/macro.h"

// 侵入式双向链表（intrusive，显式 head/tail/size，NULL 结尾，非线程安全）
// 典型用法：
//   typedef struct { list_node node; int val; } my_node;
//   list_ctx l; list_init(&l);
//   my_node n; n.val = 42; list_push_tail(&l, &n.node);
//   list_remove(&l, &n.node);                 // 任意节点 O(1)，无需搜索/前驱
//   list_foreach(&l, it) { my_node *e = UPCAST(it, my_node, node); /* ... */ }
// 契约：一个节点同时只属于一个链表；remove/pop/insert 不校验成员关系，由调用方保证
//       （同 heap_remove）；insert_before/insert_after 的 pos 须为表中有效节点（空表用 push_*）。
// 链表节点，嵌入用户结构体使用（侵入式）
typedef struct list_node {
    struct list_node *next; // 后继，队尾为 NULL
    struct list_node *prev; // 前驱，队头为 NULL
}list_node;
// 双向链表上下文（显式 head/tail + 计数）
typedef struct list_ctx {
    uint32_t size; // 当前节点数
    list_node *head; // 队头，空表为 NULL
    list_node *tail; // 队尾，空表为 NULL
}list_ctx;
// 遍历游标（函数式迭代器，预存 next，返回后删当前节点安全）
typedef struct list_iter {
    list_node *next; // 下次 list_iter_next 返回的节点
}list_iter;

/// <summary>
/// 初始化链表为空
/// </summary>
/// <param name="lst">list_ctx 指针</param>
void list_init(list_ctx *lst);
/// <summary>
/// 头插：node 成为新队头
/// </summary>
/// <param name="lst">list_ctx 指针</param>
/// <param name="node">待插入节点（不属于任何链表）</param>
void list_push_head(list_ctx *lst, list_node *node);
/// <summary>
/// 尾插：node 成为新队尾（FIFO 入队）
/// </summary>
/// <param name="lst">list_ctx 指针</param>
/// <param name="node">待插入节点（不属于任何链表）</param>
void list_push_tail(list_ctx *lst, list_node *node);
/// <summary>
/// 在 pos 之前插入 node；pos 为队头时 node 成为新队头
/// </summary>
/// <param name="lst">list_ctx 指针</param>
/// <param name="pos">表中有效节点（非空表）</param>
/// <param name="node">待插入节点（不属于任何链表）</param>
void list_insert_before(list_ctx *lst, list_node *pos, list_node *node);
/// <summary>
/// 在 pos 之后插入 node；pos 为队尾时 node 成为新队尾
/// </summary>
/// <param name="lst">list_ctx 指针</param>
/// <param name="pos">表中有效节点（非空表）</param>
/// <param name="node">待插入节点（不属于任何链表）</param>
void list_insert_after(list_ctx *lst, list_node *pos, list_node *node);
/// <summary>
/// 摘除并返回队头
/// </summary>
/// <param name="lst">list_ctx 指针</param>
/// <returns>队头节点；空表返回 NULL</returns>
list_node *list_pop_head(list_ctx *lst);
/// <summary>
/// 摘除并返回队尾
/// </summary>
/// <param name="lst">list_ctx 指针</param>
/// <returns>队尾节点；空表返回 NULL</returns>
list_node *list_pop_tail(list_ctx *lst);
/// <summary>
/// 摘除任意节点 O(1)；解链后置 node->next/prev = NULL。node 须在 lst 中（不校验）
/// </summary>
/// <param name="lst">list_ctx 指针</param>
/// <param name="node">待摘除节点</param>
void list_remove(list_ctx *lst, list_node *node);
/// <summary>
/// 将 src 整条链接到 dst 队尾，src 清空为空表 O(1)；dst 与 src 须为不同链表
/// </summary>
/// <param name="dst">目标链表</param>
/// <param name="src">源链表（调用后为空）</param>
void list_splice_tail(list_ctx *dst, list_ctx *src);
/// <summary>
/// 链表是否为空
/// </summary>
/// <param name="lst">list_ctx 指针</param>
/// <returns>空返回非 0，否则 0</returns>
int32_t list_empty(const list_ctx *lst);
/// <summary>
/// 当前节点数
/// </summary>
/// <param name="lst">list_ctx 指针</param>
/// <returns>节点数</returns>
uint32_t list_size(const list_ctx *lst);
/// <summary>
/// 初始化遍历游标，首个 list_iter_next 返回队头
/// </summary>
/// <param name="it">list_iter 指针</param>
/// <param name="lst">list_ctx 指针</param>
void list_iter_init(list_iter *it, const list_ctx *lst);
/// <summary>
/// 返回当前节点并预推进游标；返回后可对该节点调 list_remove（安全）
/// </summary>
/// <param name="it">list_iter 指针</param>
/// <returns>当前节点；遍历结束返回 NULL</returns>
list_node *list_iter_next(list_iter *it);

// 简单遍历：it 依次取每个 list_node*，body 内不可删 it（要删用 list_foreach_safe）
#define list_foreach(lst, it) \
    for (list_node *it = (lst)->head; NULL != it; it = it->next)
// 安全遍历：先存 next，body 内仅可 list_remove(lst, it) 删当前节点（删 it->next 等其它节点会使 tmp 悬空）
#define list_foreach_safe(lst, it, tmp) \
    for (list_node *it = (lst)->head, *tmp = (NULL != it) ? it->next : NULL; \
         NULL != it; it = tmp, tmp = (NULL != it) ? it->next : NULL)

#endif//SLIST_H_
