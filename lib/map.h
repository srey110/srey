#ifndef MAP_H_
#define MAP_H_

#include "macro.h"

/*
* \brief              新建
* \param uielsize     节点长度
* \param hash         hash函数
* \param compare      比较函数
* \param pudata       用户数据
* \return             map_ctx
*/
struct map_ctx *map_new(size_t uielsize, uint64_t(*hash)(void *),
    int32_t(*compare)(void *a, void *b, void *pudata), void *pudata);
/*
* \brief              释放
*/
void map_free(struct map_ctx *pmap);
/*
* \brief              节点数
* \return             节点数
*/
size_t map_size(struct map_ctx *pmap);
/*
* \brief              设置
* \param pitem        节点
*/
void map_set(struct map_ctx *pmap, void *pitem);
/*
* \brief              获取
* \param pkey         key
* \param pitem        节点
* \return             ERR_OK 有数据， 其他无
*/
int32_t map_get(struct map_ctx *pmap, void *pkey, void *pitem);
/*
* \brief              删除
* \param pkey         key
* \param pitem        节点 NULL 不返回被删除的节点
* \return             ERR_OK 有数据， 其他无
*/
int32_t map_remove(struct map_ctx *pmap, void *pkey, void *pitem);
/*
* \brief              清除全部数据
*/
void map_clear(struct map_ctx *pmap);
/*
* \brief              遍历
* \param iter         遍历执行的函数，函数返回非ERR_OK 停止执行
* \param pudata       用户数据
*/
void map_iter(struct map_ctx *pmap, int32_t(*iter)(void *pitem, void *pudata), void *pudata);

#endif//MAP_H_
