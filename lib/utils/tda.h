#ifndef TDA_H_
#define TDA_H_

#include "base/macro.h"

// 翻倍告警状态(threshold-doubling alarm):
// 业务侧每次观察到当前值(队列长度/字节累计等)时调 tda_check 检测是否跨越新阈值,
// 跨越则阈值翻倍(2x)直到覆盖当前值, 仅"新跨越"返回 1; 当前值降回 init 以下时自动复位 threshold.
typedef struct tda_ctx {
    size_t overload_init;       // 首阈值; 0 表示禁用告警(tda_check 始终 return 0)
    size_t overload_threshold;  // 当前告警阈值, 每次跨越后翻倍, 当前值 < init 时复位为 init
} tda_ctx;

/// <summary>
/// 初始化翻倍告警状态, threshold 与 init 同步置为 overload_init.
/// </summary>
/// <param name="ctx">tda_ctx, 由调用方持有</param>
/// <param name="overload_init">首阈值; 0 表示禁用告警</param>
void tda_init(tda_ctx *ctx, size_t overload_init);
/// <summary>
/// 传入当前观察值, 检测是否跨越新阈值;
/// overload 小于 init 时复位 threshold 并 return 0;
/// overload 大于 threshold 时 while 翻倍至覆盖, return 1(告警侧据此打 LOG);
/// 否则 return 0(已翻倍后未到下一阈值, 不重复告警).
/// </summary>
/// <param name="ctx">tda_ctx</param>
/// <param name="overload">当前观察值(队列长度 / 字节累计等)</param>
/// <returns>1 表示本次跨越新阈值, 0 表示未跨越或禁用</returns>
int32_t tda_check(tda_ctx *ctx, size_t overload);

#endif
