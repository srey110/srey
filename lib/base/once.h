#ifndef ONCE_H_
#define ONCE_H_

#include "base/os.h"
#include "base/macro_atomic.h"

#define SPIN_YIELD_CNT  64 // spin_backoff 等对方释放时自旋这么多次仍等不到就 THREAD_YIELD 让出 CPU。
                           // 是次数不是周期预算,别按 CPU_PAUSE_CYCLES 去折算
// 一次性初始化：多个线程同时到达时只有一个做初始化，其余等它做完再往下走。
// 用法：if (once_begin(&flag)) { 初始化…; once_end(&flag); }，之后两条路径都能看到初始化的结果。
// 抢到的那个必须 once_end，中途不能返回(失败就 abort)，否则别人永远等下去
typedef atomic_t once_ctx;// 0 没做，1 正在做，2 做完；静态零初始化即可

/// <summary>
/// 自旋等待时的退避：先 CPU_PAUSE 空转，累计到 SPIN_YIELD_CNT 次仍等不到就 THREAD_YIELD
/// 让出 CPU。对端被抢占时继续空转是纯烧核，让出才给得了它跑完的机会
/// </summary>
/// <param name="spins">自旋计数，调用方持有并初始化为 0；让出时函数自己归零。一次等待从头到尾
///   共用一个计数——分段各起一个会把让出前的预算按段数放大；等到了(或不再等了)之后调用方要清零，
///   不清则下一次等待带着上回用剩的预算开始，可能第一次就让出 CPU</param>
static inline void spin_backoff(uint32_t *spins) {
    if (++(*spins) < SPIN_YIELD_CNT) {
        CPU_PAUSE();
        return;
    }
    *spins = 0;
    THREAD_YIELD();
}
/// <summary>
/// 抢一次性初始化的权。别人正在做时等它做完才返回
/// </summary>
/// <param name="once">once_ctx</param>
/// <returns>非 0 表示抢到了，调用方做完初始化后必须调 once_end；0 表示已经做完，初始化的结果可直接用</returns>
static inline int32_t once_begin(once_ctx *once) {
    uint32_t spins = 0;
    if (2 == ATOMIC_GET(once)) {
        return 0;
    }
    if (ATOMIC_CAS(once, 0, 1)) {
        return 1;
    }
    while (2 != ATOMIC_GET(once)) {
        spin_backoff(&spins);
    }
    return 0;
}
/// <summary>
/// 初始化做完，放行等待者。初始化里的写入在此之前完成，等待者返回后都看得见
/// </summary>
/// <param name="once">once_ctx</param>
static inline void once_end(once_ctx *once) {
    ATOMIC_SET_RELEASE(once, 2);
}
/// <summary>
/// 回到"没做"，下一次 once_begin 又能抢到。给"释放后允许再初始化"的场合用，调用时不能有人正在 once_begin
/// </summary>
/// <param name="once">once_ctx</param>
static inline void once_reset(once_ctx *once) {
    ATOMIC_SET_RELEASE(once, 0);
}

#endif//ONCE_H_
