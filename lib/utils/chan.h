#ifndef CHAN_H_
#define CHAN_H_

#include "base/structs.h"
#include "containers/queue.h"
#include "thread/cond.h"

typedef struct chan_ctx chan_ctx;

/// <summary>
/// 模拟go的chan
/// </summary>
/// <param name="capacity">最大容量，0 使用非缓存方式</param>
/// <returns>chan_ctx</returns>
chan_ctx *chan_init(uint32_t capacity);
/// <summary>
/// 释放
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
/// <param name="copy">是否需要拷贝</param>
/// <returns>ERR_OK 成功</returns>
int32_t chan_send(chan_ctx *chan, void *data, size_t lens, int32_t copy);
/// <summary>
/// 接收
/// </summary>
/// <param name="chan">chan_ctx</param>
/// <param name="lens">接收到的数据长度</param>
/// <returns>NULL 无数据或失败</returns>
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
