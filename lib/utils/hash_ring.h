#ifndef HASH_RING_H_
#define HASH_RING_H_

#include "crypt/digest.h"

typedef struct hash_ring_node {
    uint32_t nreplicas;//节点数
    void *name;//节点名
    size_t lens;//name 长度
} hash_ring_node;
typedef struct hash_ring_ctx {
    struct hash_ring_list *nodes;
    uint32_t nnodes;
    struct hash_ring_item **items;
    uint32_t nitems;
    digest_ctx md5;
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
/// 添加节点
/// </summary>
/// <param name="ring">hash_ring_ctx</param>
/// <param name="name">节点名</param>
/// <param name="lens">name长度</param>
/// <param name="nreplicas">节点数</param>
/// <returns>ERR_OK 成功 </returns>
int32_t hash_ring_add_node(hash_ring_ctx *ring, void *name, size_t lens, uint32_t nreplicas);
/// <summary>
/// 获取已经添加的节点
/// </summary>
/// <param name="ring">hash_ring_ctx</param>
/// <param name="name">节点名</param>
/// <param name="lens">name长度</param>
/// <returns>hash_ring_node</returns>
hash_ring_node *hash_ring_get_node(hash_ring_ctx *ring, void *name, size_t lens);
/// <summary>
/// 移除已经添加的节点
/// </summary>
/// <param name="ring">hash_ring_ctx</param>
/// <param name="name">节点名</param>
/// <param name="lens">name长度</param>
void hash_ring_remove_node(hash_ring_ctx *ring, void *name, size_t lens);
/// <summary>
/// 查找key对应的节点
/// </summary>
/// <param name="ring">hash_ring_ctx</param>
/// <param name="key">节点名</param>
/// <param name="lens">key长度</param>
/// <returns>hash_ring_node</returns>
hash_ring_node *hash_ring_find_node(hash_ring_ctx *ring, void *key, size_t lens);
/// <summary>
/// 打印
/// </summary>
/// <param name="ring">hash_ring_ctx</param>
void hash_ring_print(hash_ring_ctx *ring);

#endif//HASH_RING_H_
