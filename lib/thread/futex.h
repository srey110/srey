#ifndef FUTEX_H_
#define FUTEX_H_

#include "base/base.h"

// 按地址等待：等一个 32 位整数离开某个值，改值的一方再按地址叫醒。"比较仍相等才睡"由内核一步完成，
// 所以只要先改值再叫就丢不了唤醒，不用锁。叫醒只按地址找等待者，那块内存已释放也不出错，最多让别处多一次虚假唤醒。
// 内核只保证同一个字上"比较→睡"与"改值→叫"不错过；睡前还要复查别的变量(先登记自己在等、再看对方)时，
// 两边的先后(足序读写或全屏障)由调用方保证，参照 thread/waiter.h 的 waiter_prepare 与 waiter_state_get。
// 只在 config.h 的 WITH_FUTEX 为 1 时可用：Linux futex、macOS os_sync(14.4 起)、FreeBSD umtx、
// Windows WaitOnAddress(8 起，链接 Synchronization.lib，见 srey/main.c)
#if WITH_FUTEX

#if !defined(OS_LINUX) && !defined(OS_FBSD) && !defined(OS_WIN) && !(defined(OS_DARWIN) && defined(HAVE_OS_SYNC))
    #error "WITH_FUTEX: unsupported platform"
#endif
#if defined(OS_LINUX)
    // 超时结构要与系统调用号配对：SYS_futex 收 {long, long}(32 位上就是旧的 32 位时间)，32 位 libc 的 struct timespec
    // 可能已是 64 位时间，不能直接传；只有 SYS_futex_time64 的平台(如 riscv32)才收 64 位的 struct timespec
    #if defined(SYS_futex)
        #define FUTEX_SYSNO SYS_futex
        typedef struct futex_ts {
            long tv_sec;
            long tv_nsec;
        }futex_ts;
    #else
        #define FUTEX_SYSNO SYS_futex_time64
        typedef struct timespec futex_ts;
    #endif
#endif

/// <summary>
/// *addr 仍等于 val 就睡，直到被 futex_wake / futex_wake_all 叫醒或超时；不等于 val 立即返回。
/// 可能虚假唤醒，返回后调用方重读 *addr 自己判断；超时与否只是提示，叫醒与超时同时到时各平台报法不一
/// </summary>
/// <param name="addr">等待字，4 字节对齐</param>
/// <param name="val">认为"还没变"的那个值</param>
/// <param name="ms">最多等多少毫秒，0 表示一直等；Windows 上 UINT32_MAX 即 INFINITE，也是一直等</param>
/// <returns>1 超时；ERR_OK 其它：被叫醒、值已不等于 val、虚假唤醒、被信号打断，以及参数错等系统调用失败</returns>
static inline int32_t futex_wait(atomic_t *addr, uint32_t val, uint32_t ms) {
#if defined(OS_LINUX)
    futex_ts ts;
    if (0 == ms) {
        (void)syscall(FUTEX_SYSNO, addr, FUTEX_WAIT_PRIVATE, val, NULL, NULL, 0);
        return ERR_OK;
    }
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000;
    if (-1 == syscall(FUTEX_SYSNO, addr, FUTEX_WAIT_PRIVATE, val, &ts, NULL, 0)
        && ETIMEDOUT == ERRNO) {
        return 1;
    }
    return ERR_OK;
#elif defined(OS_DARWIN)
    if (0 == ms) {
        (void)os_sync_wait_on_address((void *)addr, val, sizeof(atomic_t), OS_SYNC_WAIT_ON_ADDRESS_NONE);
        return ERR_OK;
    }
    if (-1 == os_sync_wait_on_address_with_timeout((void *)addr, val, sizeof(atomic_t), OS_SYNC_WAIT_ON_ADDRESS_NONE,
                                                   OS_CLOCK_MACH_ABSOLUTE_TIME, (uint64_t)ms * 1000000)
        && ETIMEDOUT == ERRNO) {
        return 1;
    }
    return ERR_OK;
#elif defined(OS_FBSD)
    struct timespec ts;
    if (0 == ms) {
        (void)_umtx_op((void *)addr, UMTX_OP_WAIT_UINT_PRIVATE, (u_long)val, NULL, NULL);
        return ERR_OK;
    }
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000;
    // 带时长时 uaddr 传时长结构的大小，uaddr2 传相对时长
    if (-1 == _umtx_op((void *)addr, UMTX_OP_WAIT_UINT_PRIVATE, (u_long)val, (void *)(uintptr_t)sizeof(ts), &ts)
        && ETIMEDOUT == ERRNO) {
        return 1;
    }
    return ERR_OK;
#else
    if (!WaitOnAddress((volatile VOID *)addr, &val, sizeof(val), 0 == ms ? INFINITE : ms)
        && ERROR_TIMEOUT == ERRNO) {
        return 1;
    }
    return ERR_OK;
#endif
}
/// <summary>
/// 叫醒一个等在 addr 上的线程，没人等时什么都不做。必须先改 *addr 再调：
/// 先叫后改的话，对方可能在两步之间比较仍相等、睡下去就没人叫了
/// </summary>
/// <param name="addr">等待字</param>
static inline void futex_wake(atomic_t *addr) {
#if defined(OS_LINUX)
    (void)syscall(FUTEX_SYSNO, addr, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
#elif defined(OS_DARWIN)
    (void)os_sync_wake_by_address_any((void *)addr, sizeof(atomic_t), OS_SYNC_WAKE_BY_ADDRESS_NONE);
#elif defined(OS_FBSD)
    (void)_umtx_op((void *)addr, UMTX_OP_WAKE_PRIVATE, 1, NULL, NULL);
#else
    WakeByAddressSingle((PVOID)addr);
#endif
}
/// <summary>
/// 同 futex_wake，但叫醒所有等在 addr 上的线程
/// </summary>
/// <param name="addr">等待字</param>
static inline void futex_wake_all(atomic_t *addr) {
#if defined(OS_LINUX)
    (void)syscall(FUTEX_SYSNO, addr, FUTEX_WAKE_PRIVATE, INT32_MAX, NULL, NULL, 0);
#elif defined(OS_DARWIN)
    (void)os_sync_wake_by_address_all((void *)addr, sizeof(atomic_t), OS_SYNC_WAKE_BY_ADDRESS_NONE);
#elif defined(OS_FBSD)
    (void)_umtx_op((void *)addr, UMTX_OP_WAKE_PRIVATE, INT32_MAX, NULL, NULL);
#else
    WakeByAddressAll((PVOID)addr);
#endif
}

#endif//WITH_FUTEX
#endif//FUTEX_H_
