#ifndef LOAD_TREND_H_
#define LOAD_TREND_H_

#include "base/macro.h"

// 基于"采样值变化趋势"的负载判定：
// - 记录上次采样值，当前采样 < 上次 * busy_num/busy_den 时判定为忙
// - 不持锁，调用方负责采样的串行性（per-watcher / per-task 单线程语义）
// - 不内置任何动作，模块只做"判定"
typedef struct load_trend_ctx {
    size_t prev;
}load_trend_ctx;

/// <summary>
/// 初始化趋势检测器，首次采样默认判定为不忙
/// </summary>
/// <param name="trend">趋势上下文</param>
static inline void load_trend_init(load_trend_ctx *trend) {
    trend->prev = 0;
}
/// <summary>
/// 采样当前值并基于趋势判断负载是否繁忙。
/// 典型阈值 (4, 5) 表示采样值跌幅超过 20% 视为繁忙。
/// </summary>
/// <param name="trend">趋势上下文</param>
/// <param name="cur">当前采样值</param>
/// <param name="busy_num">繁忙阈值分子</param>
/// <param name="busy_den">繁忙阈值分母</param>
/// <returns>非 0 繁忙；0 不忙</returns>
static inline int32_t load_trend_busy(load_trend_ctx *trend, size_t cur,
                                      uint32_t busy_num, uint32_t busy_den) {
    size_t prev = trend->prev;
    trend->prev = cur;
    if (0 == prev) {
        return 0;
    }
    if ((uint64_t)cur * busy_den < (uint64_t)prev * busy_num) {
        return 1;
    }
    return 0;
}

#endif//LOAD_TREND_H_
