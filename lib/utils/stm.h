#ifndef STM_H_
#define STM_H_

#include "base/macro.h"
#include "thread/rwlock.h"

// 共享只读快照 (software transactional memory):

// 单次快照, 可被 N 个 reader 引用; ref 归 0 时 FREE(data) + FREE(自身)
typedef struct stm_data {
    atomic_t ref;   // 引用计数: writer 持 1 + 每个 stm_grab_data 的 reader +1
    size_t sz;      // 数据字节数
    void *data;     // MALLOC 持有, ref 归 0 时 FREE
} stm_data;
// writer 长期持有的对象, 跨多次 update 复用; ctx 内存由 stm 内部管理
typedef struct stm_ctx {
    atomic_t ref;       // 引用计数: writer 持 1 + 每个 grab 的 reader +1
    stm_data *data;     // 当前快照; wrlock 下原子换; writer 释放后置 NULL
    rwlock_ctx lock;    // 保护 data 指针的换出
} stm_ctx;

/// <summary>
/// 创建 stm_ctx, 持有 data 的首份快照. data 必须由 MALLOC 宏分配 (或 copy=1 内部 MALLOC 拷贝).
/// </summary>
/// <param name="data">初始数据指针</param>
/// <param name="sz">数据字节数</param>
/// <param name="copy">1=内部 MALLOC 拷贝, 调用方仍持有 data; 0=转移所有权, stm 内部 FREE</param>
/// <returns>stm_ctx 堆指针; MALLOC 失败 ASSERTAB</returns>
stm_ctx *stm_new(void *data, size_t sz, int32_t copy);
/// <summary>
/// 释放 stm_ctx. 若有 reader 持有, ctx->data 置 NULL, ctx 保留; reader 全部释放后才真正 FREE ctx.
/// </summary>
/// <param name="ctx">stm_ctx</param>
void stm_free(stm_ctx *ctx);
/// <summary>
/// writer 替换当前快照. wrlock 下原子换 ctx->data, 旧 stm_data ref-- 归 0 时 FREE.
/// 已持有旧 stm_data 的 reader 继续可读直到自己 stm_ungrab_data.
/// </summary>
/// <param name="ctx">stm_ctx</param>
/// <param name="data">新数据指针</param>
/// <param name="sz">新数据字节数</param>
/// <param name="copy">1=内部拷贝; 0=转移所有权</param>
void stm_update(stm_ctx *ctx, void *data, size_t sz, int32_t copy);
/// <summary>
/// reader 增加对 stm_ctx 的引用, 用于把 ctx 传给另一个 task 之前先 grab 一次.
/// 配对 stm_ungrab 使用.
/// </summary>
/// <param name="ctx">stm_ctx</param>
/// <returns>ctx 本身 (链式)</returns>
stm_ctx *stm_grab(stm_ctx *ctx);
/// <summary>
/// reader 释放对 stm_ctx 的引用.
/// 最后一个释放者 (且 writer 已释放) 真正 FREE ctx.
/// </summary>
/// <param name="ctx">stm_ctx</param>
void stm_ungrab(stm_ctx *ctx);
/// <summary>
/// reader 获取当前快照并 inc data.ref.
/// writer 已释放且 ctx->data=NULL 时返回 NULL.
/// 配对 stm_ungrab_data 使用.
/// 调用方比对返回值与上次保存的指针即可判断 "是否更新" (同指针=未更新).
/// </summary>
/// <param name="ctx">stm_ctx</param>
/// <returns>stm_data 指针; writer 已释放时 NULL</returns>
stm_data *stm_grab_data(stm_ctx *ctx);
/// <summary>
/// 释放 stm_data 引用, ref 归 0 时 FREE(data->data) + FREE(data).
/// 允许 data=NULL.
/// </summary>
/// <param name="data">stm_data</param>
void stm_ungrab_data(stm_data *data);

#endif//STM_H_
