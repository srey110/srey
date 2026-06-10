#ifndef HASH_RING_H_
#define HASH_RING_H_

#include "base/macro.h"

typedef struct hash_ring_node {
    uint32_t nreplicas;//节点数
    void *name;//节点名
    size_t lens;//name 长度
} hash_ring_node;
typedef struct hash_ring_ctx {
    uint32_t nnodes;            //真实节点数量
    uint32_t nitems;            //虚拟节点（副本）总数
    struct hash_ring_list *nodes; //真实节点链表
    struct hash_ring_item **items;//按 digest 排序的虚拟节点数组
} hash_ring_ctx;

/// <summary>
/// 初始化
/// </summary>
/// <param name="ring">hash_ring_ctx</param>
void hash_ring_init(hash_ring_ctx *ring);
/// <summary>
/// 释放
/// </summary>
/// <param name="ring">hash_ring_ctx</param>
void hash_ring_free(hash_ring_ctx *ring);
/// <summary>
/// 添加节点并排序
/// </summary>
/// <param name="ring">hash_ring_ctx</param>
/// <param name="name">节点名</param>
/// <param name="lens">name长度</param>
/// <param name="nreplicas">节点数</param>
/// <returns>ERR_OK 成功 </returns>
int32_t hash_ring_add(hash_ring_ctx *ring, void *name, size_t lens, uint32_t nreplicas);
/// <summary>
/// 添加节点，不排序(调用hash_ring_sort)，在批量添加时排除排序
/// </summary>
/// <param name="ring">hash_ring_ctx</param>
/// <param name="name">节点名</param>
/// <param name="lens">name长度</param>
/// <param name="nreplicas">节点数</param>
/// <returns>ERR_OK 成功 </returns>
int32_t hash_ring_add_nosort(hash_ring_ctx *ring, void *name, size_t lens, uint32_t nreplicas);
/// <summary>
/// 排序hash ring，配合hash_ring_add_nosort在末尾统一排一次
/// </summary>
/// <param name="ring">hash_ring_ctx</param>
void hash_ring_sort(hash_ring_ctx *ring);
/// <summary>
/// 移除已经添加的节点
/// </summary>
/// <param name="ring">hash_ring_ctx</param>
/// <param name="name">节点名</param>
/// <param name="lens">name长度</param>
void hash_ring_remove(hash_ring_ctx *ring, void *name, size_t lens);
/// <summary>
/// 查找key对应的节点
/// </summary>
/// <param name="ring">hash_ring_ctx</param>
/// <param name="key">key名</param>
/// <param name="lens">key长度</param>
/// <returns>hash_ring_node</returns>
hash_ring_node *hash_ring_find(hash_ring_ctx *ring, void *key, size_t lens);
/// <summary>
/// 打印
/// </summary>
/// <param name="ring">hash_ring_ctx</param>
void hash_ring_print(hash_ring_ctx *ring);

#endif//HASH_RING_H_
