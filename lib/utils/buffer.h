#ifndef BUFFER_H_
#define BUFFER_H_

#include "base/structs.h"

//分散内存读写
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
    volatile int32_t freeze_read;  //读冻结标志：非零时禁止并发读操作
    volatile int32_t freeze_write; //写冻结标志：非零时禁止并发写操作
    struct bufnode_ctx *head;      //节点链表头
    struct bufnode_ctx *tail;      //节点链表尾
    struct bufnode_ctx **tail_with_data; //指向最后一个有数据节点的指针的指针
    size_t total_lens;             //缓存中当前数据总长度
    struct bufnode_ctx *hint_node;    //搜索游标：上次命中的节点
    size_t             hint_base_off; //游标节点之前所有节点的累计字节偏移
    uint32_t           pinned_n;      //buffer_get 本次锁定的节点数，buffer_commit_get 按它精确解锁
}buffer_ctx;
/// <summary>
/// 分散内存初始化
/// </summary>
/// <param name="ctx">buffer_ctx</param>
void buffer_init(buffer_ctx *ctx);
/// <summary>
/// 分散内存释放。调用后 ctx 回到 buffer_init 后的空状态，可安全重复调用；
/// 复位是为了重复释放不变成 double free、释放后读不到陈旧节点与字节数，
/// 不是"清空并复用"的入口——需要保留已分配节点的清空请用 buffer_drain。
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
/// 现有节点链上还能直接写入的字节数（不新建节点）。返回值即"传给 buffer_expand 不会
/// 触发新节点分配"的最大 lens，故 cnt 须与随后 buffer_expand 的 cnt 一致；
/// 返回 0 表示链上已无空闲空间，任何 expand 都会新建节点。纯查询，不改动任何状态
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="cnt">随后 buffer_expand 将使用的 IOV 数组长度</param>
/// <returns>可直接写入的字节数</returns>
size_t buffer_space(buffer_ctx *ctx, const uint32_t cnt);
/// <summary>
/// 将外部缓存data添加到buffer,不做一次拷贝,供零拷贝的读取用。
/// lens 为 0 时不挂节点, 直接以 ext_free 归还 data —— 零长节点既不携带数据也不提供可写空间,
/// 挂上链只会让写入端的 iov 记录与提交校验错位, 并让按 off 游走的读取定位提前终止
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="data">数据</param>
/// <param name="lens">长度, 0 表示无数据, data 即刻由 ext_free 归还</param>
/// <param name="ext_free">data释放函数</param>
void buffer_external(buffer_ctx *ctx, void *data, const size_t lens, free_cb ext_free);
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
/// <param name="...">参数</param>
/// <returns>ERR_OK 成功</returns>
int32_t buffer_appendv(buffer_ctx *ctx, const char *fmt, ...);
/// <summary>
/// 读取数据
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="start">起始位置</param>
/// <param name="out">输出</param>
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
/// 读取并删除数据
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="out">输出</param>
/// <param name="lens">长度</param>
/// <returns>实际长度</returns>
size_t buffer_remove(buffer_ctx *ctx, void *out, size_t lens);
/// <summary>
/// 搜索
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="ncs">0 区分大小写</param>
/// <param name="start">起始搜索位置</param>
/// <param name="end">搜索结束位置, 0 直到数据结束</param>
/// <param name="what">要搜索的数据，NULL 直接返回 ERR_FAILED</param>
/// <param name="wlens">搜索数据长度，0 直接返回 ERR_FAILED（不按空串恒匹配处理）</param>
/// <returns>ERR_FAILED 失败 否则返回搜索的起始位置</returns>
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
/// 获取指定长度的内存,供外部写入。返回后进入写暂存态，在 buffer_commit_expand 之前
/// 不得再调用本模块任何写接口，也不可与 buffer_get 的读暂存态交叠，均以断言拦截
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
/// 获取指定长度的数据，供 writev 等分散读零拷贝取用。
/// 返回非 0 即进入读暂存态：iov 直接指向节点内部数据区，且这些节点被标记为锁定，
/// 在 buffer_commit_get 之前不得再调用本模块任何读写接口 —— 写入可能触发对齐 memmove
/// 或节点迁移，使已暂存的 iov 指向被改写或已释放的内存；读写两族接口均以断言拦截，
/// 两种暂存态(本函数与 buffer_expand)也互斥，不可交叠。
/// 返回 0 表示无数据可取且未进入暂存态，此时不可调用 buffer_commit_get
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="atmost">最多取出的字节数</param>
/// <param name="iov">IOV数组</param>
/// <param name="cnt">IOV数组长度</param>
/// <returns>IOV数量，0 表示无数据且未进入暂存态</returns>
uint32_t buffer_get(buffer_ctx *ctx, size_t atmost, IOV_TYPE *iov, const uint32_t cnt);
/// <summary>
/// 解除 buffer_get 的读暂存态并删除已消费的数据；只能在 buffer_get 返回非 0 后调用一次
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="lens">要删除的长度，0 表示只解除暂存不删除数据</param>
void buffer_commit_get(buffer_ctx *ctx, size_t lens);
/// <summary>
/// 从socket中读数据。按边缘触发契约循环读到无数据为止；
/// 上一轮未读满时认为 socket 已空，下一轮只用 buffer_space 报出的现成空间去做确认性
/// readv，不为这次大概率空转的读再分配节点（读满则说明还有数据，仍按 MAX_RECV_SIZE 取）
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="fd">socket描述符</param>
/// <param name="nread">读取到的长度</param>
/// <param name="_readv">读取函数</param>
/// <param name="arg">参数</param>
/// <returns>ERR_OK 成功</returns>
int32_t buffer_from_sock(buffer_ctx *ctx, SOCKET fd, size_t *nread,
    int32_t(*_readv)(SOCKET, IOV_TYPE *, uint32_t, void *, size_t *), void *arg);

#endif//BUFFER_H_
