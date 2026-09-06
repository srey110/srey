#ifndef CHAN_H_
#define CHAN_H_

#include "base/structs.h"
#include "containers/queue.h"
#include "thread/cond.h"

typedef struct chan_ctx chan_ctx;

/// <summary>
/// 模拟go的chan
/// </summary>
/// <param name="capacity">最大容量，非 0 时向上取到偶数(见 queue_init)；0 使用非缓存方式</param>
/// <returns>chan_ctx</returns>
chan_ctx *chan_init(uint32_t capacity);
/// <summary>
/// 释放。buffered chan 须先排空(反复 chan_recv 取尽并自行释放 copy=1 堆数据):
/// chan_free 不析构队列残留,未消费的 copy=1 元素会泄漏。
/// 调用时不得有任何线程还停在 chan_send / chan_recv 里,正确收尾是 chan_close 之后
/// thread_join 掉所有收发线程再 free——光 close 不够,违反的后果不是报错而是直接踩内存。
/// 函数内按 r_waiting / w_waiting 断言了这一条,但只拦得住"忘了 join",拦不住并发的 free
/// </summary>
/// <param name="chan">chan_ctx</param>
void chan_free(chan_ctx *chan);
/// <summary>
/// 关闭
/// </summary>
/// <param name="chan">chan_ctx</param>
void chan_close(chan_ctx *chan);
/// <summary>
/// 是否关闭
/// </summary>
/// <param name="chan">chan_ctx</param>
int32_t chan_is_closed(chan_ctx *chan);
/// <summary>
/// 发送
/// </summary>
/// <param name="chan">chan_ctx</param>
/// <param name="data">数据</param>
/// <param name="lens">data长度</param>
/// <param name="copy">非 0 时内部拷贝一份, data 仍由调用方释放;
/// 0 转移 data 所有权, 但只在成功时转移, 失败时 data 仍归调用方</param>
/// <returns>ERR_OK 成功</returns>
int32_t chan_send(chan_ctx *chan, void *data, size_t lens, int32_t copy);
/// <summary>
/// 接收
/// </summary>
/// <param name="chan">chan_ctx</param>
/// <param name="lens">接收到的数据长度</param>
/// <returns>NULL 无数据或失败。关闭后两种模式口径不同：buffered 会把队列排空再返 NULL；
/// 非缓存是 rendezvous，只有关闭时已有接收者在等，发送方已发布的那条才会交付，
/// 否则发送方放弃(返 ERR_FAILED)，此后 recv 直接返 NULL</returns>
void *chan_recv(chan_ctx *chan, size_t *lens);
/// <summary>
/// 数据数量
/// </summary>
/// <param name="chan">chan_ctx</param>
/// <returns>数据数量</returns>
uint32_t chan_size(chan_ctx *chan);
/// <summary>
/// 是否可以接收
/// </summary>
/// <param name="chan">chan_ctx</param>
/// <returns></returns>
int32_t chan_can_recv(chan_ctx *chan);
/// <summary>
/// 是否可以发送
/// </summary>
/// <param name="chan">chan_ctx</param>
/// <returns></returns>
int32_t chan_can_send(chan_ctx *chan);

#endif//CHAN_H_
