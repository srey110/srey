#ifndef BUFFER_H_
#define BUFFER_H_

#include "base/structs.h"

//非连续内存读写
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
#define MAX_EXPAND_NIOV          4

typedef struct buffer_ctx {
    volatile int32_t freeze_read;
    volatile int32_t freeze_write;
    struct bufnode_ctx *head;
    struct bufnode_ctx *tail;
    struct bufnode_ctx **tail_with_data;
    size_t total_lens;//数据总长度
}buffer_ctx;
/// <summary>
/// 非连续内存初始化
/// </summary>
/// <param name="ctx">buffer_ctx</param>
void buffer_init(buffer_ctx *ctx);
/// <summary>
/// 非连续内存释放
/// </summary>
/// <param name="ctx">buffer_ctx</param>
void buffer_free(buffer_ctx *ctx);
/// <summary>
/// 获取数据长度
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <returns>长度</returns>
size_t buffer_size(buffer_ctx *ctx);
/// <summary>
/// 将外部数据data链接到buffer,减少一次拷贝,方便后续的读取。
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="data">数据</param>
/// <param name="lens">长度</param>
/// <param name="free_cb">data释放函数</param>
void buffer_external(buffer_ctx *ctx, void *data, const size_t lens, free_cb _free);
/// <summary>
/// 写入数据
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="data">数据</param>
/// <param name="lens">长度</param>
/// <returns>ERR_OK 成功</returns>
int32_t buffer_append(buffer_ctx *ctx, void *data, const size_t lens);
/// <summary>
/// 写入数据
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="fmt">格式化</param>
/// <param name="...">变参</param>
/// <returns>ERR_OK 成功</returns>
int32_t buffer_appendv(buffer_ctx *ctx, const char *fmt, ...);
/// <summary>
/// 读取数据
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="start">开始位置</param>
/// <param name="out">数据</param>
/// <param name="lens">读取长度</param>
/// <returns>实际读取到的长度</returns>
size_t buffer_copyout(buffer_ctx *ctx, const size_t start, void *out, size_t lens);
/// <summary>
/// 删除数据
/// </summary>
/// <param name="lens">长度</param>
/// <returns>实际删除的长度</returns>
size_t buffer_drain(buffer_ctx *ctx, size_t lens);
/// <summary>
/// 读取，并删除数据
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="out">数据</param>
/// <param name="lens">长度</param>
/// <returns>实际长度</returns>
size_t buffer_remove(buffer_ctx *ctx, void *out, size_t lens);
/// <summary>
/// 查找
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="ncs">0 区分大小写</param>
/// <param name="start">开始搜索位置</param>
/// <param name="end">结束搜索位置, 0 直到数据结束</param>
/// <param name="what">要搜索的数据</param>
/// <param name="wlens">搜索的数据长度</param>
/// <returns>ERR_FAILED 失败 其他数据所在开始位置</returns>
int32_t buffer_search(buffer_ctx *ctx, const int32_t ncs,
    const size_t start, size_t end, char *what, size_t wlens);
/// <summary>
/// 获取指定位置值
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="pos">位置</param>
/// <returns>char</returns>
char buffer_at(buffer_ctx *ctx, size_t pos);
/// <summary>
/// 获取指定容量的内存,用于写入
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="lens">要获取的大小</param>
/// <param name="iov">IOV数组</param>
/// <param name="cnt">IOV数组长度</param>
/// <returns>IOV数量</returns>
uint32_t buffer_expand(buffer_ctx *ctx, const size_t lens, IOV_TYPE *iov, const uint32_t cnt);
/// <summary>
/// buffer_expand 提交写入
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="lens">数据长度</param>
/// <param name="iov">IOV数组</param>
/// <param name="cnt">IOV数组长度</param>
void buffer_commit_expand(buffer_ctx *ctx, size_t lens ,IOV_TYPE *iov, const uint32_t cnt);
/// <summary>
/// 获取指定长度的数据
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="atmost">长度</param>
/// <param name="iov">IOV数组</param>
/// <param name="cnt">IOV数组长度</param>
/// <returns>IOV数量</returns>
uint32_t buffer_get(buffer_ctx *ctx, size_t atmost, IOV_TYPE *iov, const uint32_t cnt);
/// <summary>
/// buffer_get 数据删除
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="size">长度</param>
void buffer_commit_get(buffer_ctx *ctx, size_t size);
/// <summary>
/// 从socket中读入数据
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="fd">socket句柄</param>
/// <param name="nread">读取到的长度</param>
/// <param name="_readv">读取函数</param>
/// <param name="arg">参数</param>
/// <returns>ERR_OK 成功</returns>
int32_t buffer_from_sock(buffer_ctx *ctx, SOCKET fd, size_t *nread,
    int32_t(*_readv)(SOCKET, IOV_TYPE *, uint32_t, void *, size_t *), void *arg);

#endif//BUFFER_H_
