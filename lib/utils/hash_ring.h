#ifndef HASH_RING_H_
#define HASH_RING_H_

#include "crypt/digest.h"

typedef struct hash_ring_node {
    uint32_t nreplicas;//�ڵ���
    void *name;//�ڵ���
    size_t lens;//name ����
} hash_ring_node;
typedef struct hash_ring_ctx {
    uint32_t nnodes;
    uint32_t nitems;
    struct hash_ring_list *nodes;
    struct hash_ring_item **items;
    digest_ctx md5;
} hash_ring_ctx;

/// <summary>
/// ��ʼ��
/// </summary>
/// <param name="ring">hash_ring_ctx</param>
void hash_ring_init(hash_ring_ctx *ring);
/// <summary>
/// �ͷ�
/// </summary>
/// <param name="ring">hash_ring_ctx</param>
void hash_ring_free(hash_ring_ctx *ring);
/// <summary>
/// ���ӽڵ㲢����
/// </summary>
/// <param name="ring">hash_ring_ctx</param>
/// <param name="name">�ڵ���</param>
/// <param name="lens">name����</param>
/// <param name="nreplicas">�ڵ���</param>
/// <returns>ERR_OK �ɹ� </returns>
int32_t hash_ring_add(hash_ring_ctx *ring, void *name, size_t lens, uint32_t nreplicas);
/// <summary>
/// ���ӽڵ㣬����������hash_ring_sort�������ڳ�������ʱ�ų�����
/// </summary>
/// <param name="ring">hash_ring_ctx</param>
/// <param name="name">�ڵ���</param>
/// <param name="lens">name����</param>
/// <param name="nreplicas">�ڵ���</param>
/// <returns>ERR_OK �ɹ� </returns>
int32_t hash_ring_add_nosort(hash_ring_ctx *ring, void *name, size_t lens, uint32_t nreplicas);
/// <summary>
/// ����hash ring��������hash_ring_add_nosort����ĩ����һ������
/// </summary>
/// <param name="ring">hash_ring_ctx</param>
void hash_ring_sort(hash_ring_ctx *ring);
/// <summary>
/// �Ƴ��Ѿ����ӵĽڵ�
/// </summary>
/// <param name="ring">hash_ring_ctx</param>
/// <param name="name">�ڵ���</param>
/// <param name="lens">name����</param>
void hash_ring_remove(hash_ring_ctx *ring, void *name, size_t lens);
/// <summary>
/// ����key��Ӧ�Ľڵ�
/// </summary>
/// <param name="ring">hash_ring_ctx</param>
/// <param name="key">�ڵ���</param>
/// <param name="lens">key����</param>
/// <returns>hash_ring_node</returns>
hash_ring_node *hash_ring_find(hash_ring_ctx *ring, void *key, size_t lens);
/// <summary>
/// ��ӡ
/// </summary>
/// <param name="ring">hash_ring_ctx</param>
void hash_ring_print(hash_ring_ctx *ring);

#endif//HASH_RING_H_
