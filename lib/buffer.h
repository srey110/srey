#ifndef BUFFER_H_
#define BUFFER_H_

#include "mutex.h"

#if defined(OS_WIN)
#define IOV_TYPE WSABUF
#define IOV_PTR_FIELD buf
#define IOV_LEN_FIELD len
#define IOV_LEN_TYPE ULONG
#else
//struct iovec {
//    void *iov_base;
//    size_t iov_len;
//};
#define IOV_TYPE struct iovec
#define IOV_PTR_FIELD iov_base
#define IOV_LEN_FIELD iov_len
#define IOV_LEN_TYPE size_t
#endif
typedef struct buffer_ctx
{
    struct buffernode_ctx *head;
    struct buffernode_ctx *tail;
    struct buffernode_ctx **tail_with_data;
    int32_t freeze_head;
    int32_t freeze_tail;
    size_t total_len;//数据总长度
    mutex_ctx mutex;
}buffer_ctx;

/*
* \brief          初始化
*/
void buffer_init(struct buffer_ctx *pctx);
/*
* \brief          释放
*/
void buffer_free(struct buffer_ctx *pctx);
/*
* \brief          添加数据
* \param pdata    数据
* \param uilen    数据长度
* \return         ERR_OK 成功
* \return         ERR_FAILED 失败
*/
int32_t buffer_append(struct buffer_ctx *pctx, void *pdata, const size_t uilen);
/*
* \brief          添加数据
* \param pfmt     格式
* \param ...      变参
* \return         ERR_OK 成功
* \return         ERR_FAILED 失败
*/
int32_t buffer_appendv(struct buffer_ctx *pctx, const char *pfmt, ...);
/*
* \brief          拷贝数据，不删除
* \param pout     接收数据指针
* \param uilen    拷贝多少
* \return         ERR_FAILED 失败
* \return         实际拷贝数
*/
int32_t buffer_copyout(struct buffer_ctx *pctx, void *pout, size_t uilen);
/*
* \brief          删除数据
* \param uilen    删除多少
* \return         ERR_FAILED 失败
* \return         实际删除数
*/
int32_t buffer_drain(struct buffer_ctx *pctx, size_t uilen);
/*
* \brief          拷贝并删除数据
* \param pout     接收数据指针
* \param uilen    拷贝多少
* \return         ERR_FAILED 失败
* \return         实际拷贝删除长度
*/
int32_t buffer_remove(struct buffer_ctx *pctx, void *pout, size_t uilen);
/*
* \brief          搜索，按字节比较
* \param uistart  开始搜索的位置
* \param pwhat    要搜索的数据
* \param uiwlens  pwhat长度
* \return         ERR_FAILED 未找到 或者 头部被锁定
* \return         第一次出现的位置
*/
int32_t buffer_search(struct buffer_ctx *pctx, size_t uistart, void *pwhat, size_t uiwlens);
//锁
static inline void buffer_lock(struct buffer_ctx *pctx)
{
    mutex_lock(&pctx->mutex);
};
static inline void buffer_unlock(struct buffer_ctx *pctx)
{
    mutex_unlock(&pctx->mutex);
};
/*
* \brief          数据长度
* \return         数据长度
*/
static inline size_t buffer_size(struct buffer_ctx *pctx)
{
    buffer_lock(pctx);
    size_t uisize = pctx->total_len;
    buffer_unlock(pctx);

    return uisize;
}
/*
* \brief          扩展一个节点，连续内存
* \param uilen    扩展多少
* \return         扩展了的节点
*/
struct buffernode_ctx *_buffer_expand_single(struct buffer_ctx *pctx, const size_t uilens);
/*
* \brief          扩展节点，非连续内存,填充数据到 piov后使用_buffer_commit_iov提交本次操作
* \param uilen    扩展多少
* \param piov     扩展后可用于存储的iov数组 长度为uicount个
* \param uicount  最多使用多少个节点
* \return         实际扩展的节点数，小于等于uicount
*/
size_t _buffer_expand_iov(struct buffer_ctx *pctx, const size_t uilens, 
    IOV_TYPE *piov, const size_t uicount);
/*
* \brief          提交填充了数据的iov，该iov由_buffer_expand_iov扩展。
* \param piov     iov数组
* \param uicount  piov个数
* \return         添加了多少数据
*/
size_t _buffer_commit_iov(struct buffer_ctx *pctx, IOV_TYPE *piov, const size_t uicount);
/*
* \brief             返回buffer中的数据并装填进piov
* \param uihowmuch   需要装填的数据长度
* \param piov        iov数组
* \param uiovsize    iov数组长度
* \param prealsize   实际装填的数据长度
* \return            有数据的piov个数
*/
size_t _buffer_get_iov(struct buffer_ctx *pctx, size_t uihowmuch, 
    IOV_TYPE *piov, size_t uiovsize, size_t *prealsize);

#endif//BUFFER_H_
