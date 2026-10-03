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

// buffer_from_sock 的 _readv 回调除成功/失败外可回的额外码：这次读成功，且回调侧确知自己
// 那层缓冲(如 SSL)已空、无须再问。收到后按成功处理并停止本轮抽取，不会透传给调用方
#define BUFFER_READV_DRAINED 3

typedef struct buffer_ctx {
    volatile int32_t freeze_read;  //读暂存态：buffer_get 到 buffer_commit_get 之间置位，期间禁调读接口
    volatile int32_t freeze_write; //写暂存态：buffer_expand 到 buffer_commit_expand 之间置位，期间禁调写接口
    struct bufnode_ctx *head;      //节点链表头
    struct bufnode_ctx *tail;      //节点链表尾
    struct bufnode_ctx **tail_with_data; //指向最后一个有数据节点的指针的指针
    size_t total_lens;             //缓存中当前数据总长度
    struct bufnode_ctx *hint_node;    //搜索游标：上次命中的节点
    size_t             hint_base_off; //游标节点之前所有节点的累计字节偏移
    uint32_t           pinned_n;      //buffer_get 本次锁定的节点数，buffer_commit_get 按它精确解锁
}buffer_ctx;

/// <summary>
/// 释放本线程留存的备用节点。任何调用过 buffer_* 的线程在退出前都要调一次，
/// 否则那一个节点会以泄漏的形式留到进程结束。可安全重复调用。
/// 经 thread_creat / thread_creat_hooks 创建的线程由 main 注册的 thread_global_hooks 全局 exit 统一调，
/// 主线程等不经它们创建的线程须自己调
/// </summary>
void buffer_thread_cleanup(void);
/// <summary>
/// 分散内存初始化
/// </summary>
/// <param name="ctx">buffer_ctx</param>
void buffer_init(buffer_ctx *ctx);
/// <summary>
/// 分散内存释放。调用后 ctx 回到 buffer_init 后的空状态，可安全重复调用，也可接着用。
/// 只想丢数据用 buffer_drain：它不还空节点（读到 FIN 那次 expand 出来的节点就是空的），
/// 对象要回池或长时间闲置时用本函数把节点全还掉
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
/// 现有节点链上还能直接写入的字节数（不新建节点）；cnt 须与随后 buffer_expand 的 cnt 一致。
/// 纯查询，不改动任何状态
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="cnt">随后 buffer_expand 将使用的 IOV 数组长度</param>
/// <returns>可直接写入的字节数：传给 buffer_expand 的 lens 不超过它就不会新建节点
/// （expand 还可能把尾节点的残包前移腾出更多，所以这是下界）；
/// 0 表示链上已无空闲空间</returns>
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
/// <param name="ctx">buffer_ctx</param>
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
/// <param name="end">搜索结束位置，闭区间且要求整个 what 落在 [start, end] 内：
///   末字节压在 end 上算命中，起点压在 end 上(what 超过 1 字节时)不算。0 表示直到数据结束</param>
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
/// 获取至少 lens 字节的可写空间,供外部写入。新建节点按 ROUND_UP 分配,挤出来的余量
/// 一并登记进 iov,故实际交出的总量可能大于 lens。返回后进入写暂存态，在 buffer_commit_expand 之前
/// 不得再调用本模块任何写接口，也不可与 buffer_get 的读暂存态交叠，均以断言拦截
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="lens">要获取的最小大小</param>
/// <param name="iov">IOV数组</param>
/// <param name="cnt">IOV数组长度</param>
/// <param name="iovlens">出参，回传这批 iov 的字节总和（即实际交出的总量，见 summary）。
/// 必须非 NULL，函数内裸解引用</param>
/// <returns>IOV数量</returns>
uint32_t buffer_expand(buffer_ctx *ctx, const size_t lens, IOV_TYPE *iov,
                       const uint32_t cnt, size_t *iovlens);
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
/// 取到数据即进入读暂存态：iov 直接指向节点内部数据区，在 buffer_commit_get 之前
/// 不得再调用本模块任何读写接口（写入可能搬动节点让 iov 悬空，断言拦截），
/// 且与 buffer_expand 的暂存态互斥
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="atmost">最多取出的字节数</param>
/// <param name="iov">IOV数组</param>
/// <param name="cnt">IOV数组长度</param>
/// <returns>IOV数量，0 表示无数据且未进入暂存态（此时不可调用 buffer_commit_get）</returns>
uint32_t buffer_get(buffer_ctx *ctx, size_t atmost, IOV_TYPE *iov, const uint32_t cnt);
/// <summary>
/// 解除 buffer_get 的读暂存态并删除已消费的数据；只能在 buffer_get 返回非 0 后调用一次
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="lens">要删除的长度，0 表示只解除暂存不删除数据</param>
void buffer_commit_get(buffer_ctx *ctx, size_t lens);
/// <summary>
/// 从socket中读数据，循环到读不出为止。未读满即认定 socket 已空直接停，剩余数据由下次
/// 可读事件领走；只有 epoll 边缘触发（TRIGGER_ET=1）例外，它必须读到 EAGAIN，那一轮只用
/// buffer_space 报出的现成空间去读，不为它再分配节点。
/// 本函数自己的早退只对裸 socket 生效；带缓冲的 _readv（arg 非 NULL）由回调侧自行判断，
/// 判定已空时回 BUFFER_READV_DRAINED
/// </summary>
/// <param name="ctx">buffer_ctx</param>
/// <param name="fd">socket描述符</param>
/// <param name="nread">读取到的长度</param>
/// <param name="_readv">读取函数。可回 BUFFER_READV_DRAINED 表示自己那层缓冲已空，本函数
/// 据此停止抽取并把返回值归一成 ERR_OK</param>
/// <param name="arg">透传给 _readv 的参数。非 NULL 表示这不是裸 socket 读（调用方在 _readv 里
/// 另有一层缓冲，如 SSL），本函数自己的两档早退会因此不生效——它们只对"未读数据留在
/// 内核 socket buffer 里、下一次可读事件还会来"成立，看不见那层缓冲</param>
/// <returns>ERR_OK 成功；其余原样透传 _readv 最后一次的返回码，调用方按自己的约定解读。
/// BUFFER_READV_DRAINED 不会透传出来，它在内部已被归一成 ERR_OK</returns>
int32_t buffer_from_sock(buffer_ctx *ctx, SOCKET fd, size_t *nread,
    int32_t(*_readv)(SOCKET, IOV_TYPE *, uint32_t, void *, size_t *), void *arg);

#endif//BUFFER_H_
