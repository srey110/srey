#ifndef MACRO_ATOMIC_H_
#define MACRO_ATOMIC_H_

#include "base/os.h"

// 原子操作：GCC/Clang 用编译器内建（一份覆盖所有 OS，含 Windows 上的 MinGW/clang-cl，
// 故 __ldar 等 MSVC 专属内建只在纯 MSVC 分支出现，不会误伤 GCC/Clang-on-Windows）；
// 否则按 OS 回退——MSVC 用 Interlocked，Sun Studio 用 libc atomic + __machine_rw_barrier，
// xlC 用 AIX 原子服务 + __sync()。所需系统头（windows.h / atomic.h+mbarrier.h / sys/atomic_op.h）由 os.h 引入。
// 每分支先定义 ATOMIC_THREAD_FENCE_SEQCST（seq_cst 全屏障），Sun/AIX 的原子原语内联实现直接复用该宏，
// 使各平台的底层屏障（__machine_rw_barrier / __sync）只在宏定义处出现一次。

// atomic_t 仅类型按 OS 不同（AIX 原子服务收 int/long 指针），操作分流见下
#if defined(OS_AIX)
    #ifndef __64BIT__
        #error "32-bit AIX (ILP32) is not supported; compile with -maix64 or -q64"
    #endif
    typedef int32_t atomic_t;  // 32 位原子整数类型（AIX）
    typedef long atomic64_t;   // 64 位原子整数类型（AIX）
#else
    typedef uint32_t atomic_t;   // 32 位原子整数类型
    typedef uint64_t atomic64_t; // 64 位原子整数类型
#endif

#if defined(__GNUC__) || defined(__clang__)
    // seq_cst 全屏障：SB(store-buffer)握手中 store 与其后复合 load(如 fsqu_size)间补 StoreLoad 顺序
    #define ATOMIC_THREAD_FENCE_SEQCST() __atomic_thread_fence(__ATOMIC_SEQ_CST)
    // 原子读取：x86/x64 零开销，ARM/ARM64 上 LDAR/LDAPR 替代 DMB ISH
    #define ATOMIC_GET(ptr)   __atomic_load_n((ptr), __ATOMIC_ACQUIRE)
    #define ATOMIC64_GET(ptr) __atomic_load_n((ptr), __ATOMIC_ACQUIRE)
    // ATOMIC_GET_SEQCST：store-buffer(Dekker)握手专用的顺序一致载入，与前置 ATOMIC_SET(seq_cst) 配对补足
    // StoreLoad 顺序；acquire 载入在 ARMv8.3+ RCpc(LDAPR)下不保证该顺序，seq_cst 载入编为 LDAR(RCsc)才成立
    #define ATOMIC_GET_SEQCST(ptr)   __atomic_load_n((ptr), __ATOMIC_SEQ_CST)
    #define ATOMIC64_GET_SEQCST(ptr) __atomic_load_n((ptr), __ATOMIC_SEQ_CST)
    #define ATOMIC_ADD(ptr, val) __sync_fetch_and_add(ptr, val)
    #define ATOMIC_SET(ptr, val) __atomic_exchange_n(ptr, val, __ATOMIC_SEQ_CST)
    #define ATOMIC_CAS(ptr, oldval, newval) __sync_bool_compare_and_swap(ptr, oldval, newval)
    #define ATOMIC64_ADD(ptr, val) __sync_fetch_and_add(ptr, val)
    #define ATOMIC64_SET(ptr, val) __atomic_exchange_n(ptr, val, __ATOMIC_SEQ_CST)
    #define ATOMIC64_CAS(ptr, oldval, newval) __sync_bool_compare_and_swap(ptr, oldval, newval)
    // 松散序变体：仅适用于单写者字段（无并发写竞争，只需不撕裂 + 迟早可见）；
    // SET 用 store_n（纯写非 RMW）而非 exchange_n，x86 上从 LOCK XCHG 降为裸 MOV；
    // ADD 仍是 RMW，x86 上无论序强弱都得 LOCK XADD，收益主要在 ARM/ARM64（省 DMB）；
    // __atomic_* 内建对超出原生字长的类型自动生成正确代码，32/64 位均安全，无需按位宽拆分
    #define ATOMIC_ADD_RELAXED(ptr, val) __atomic_fetch_add(ptr, val, __ATOMIC_RELAXED)
    #define ATOMIC_SET_RELAXED(ptr, val) __atomic_store_n(ptr, val, __ATOMIC_RELAXED)
    #define ATOMIC64_ADD_RELAXED(ptr, val) __atomic_fetch_add(ptr, val, __ATOMIC_RELAXED)
    #define ATOMIC64_SET_RELAXED(ptr, val) __atomic_store_n(ptr, val, __ATOMIC_RELAXED)
#elif defined(OS_WIN)
    #define ATOMIC_THREAD_FENCE_SEQCST() MemoryBarrier()
    // InterlockedExchangeAdd：原子加 32 位，返回旧值
    #define ATOMIC_ADD(ptr, val) InterlockedExchangeAdd(ptr, val)
    // InterlockedExchange：原子交换 32 位，返回旧值
    #define ATOMIC_SET(ptr, val) InterlockedExchange(ptr, val)
    // InterlockedCompareExchange：若 *ptr == oldval 则设为 newval，返回旧值；成功时返回值等于 oldval
    #define ATOMIC_CAS(ptr, oldval, newval) (InterlockedCompareExchange(ptr, newval, oldval) == oldval)
    // 64 位原子操作（与 32 位对应）
    #define ATOMIC64_ADD(ptr, val) InterlockedExchangeAdd64(ptr, val)
    #define ATOMIC64_SET(ptr, val) InterlockedExchange64(ptr, val)
    #define ATOMIC64_CAS(ptr, oldval, newval) (InterlockedCompareExchange64(ptr, newval, oldval) == oldval)
    // 原子读取：纯 MSVC（GCC/Clang-on-Windows 已在上一分支走 __atomic），按 ARCH 分档
    #if defined(ARCH_ARM64)
        // 纯 MSVC + Windows ARM64：__ldar32/64 直接发射 LDAR 指令（load-acquire），单指令完成
        #define ATOMIC_GET(ptr)   ((atomic_t)__ldar32((const volatile __int32 *)(ptr)))
        #define ATOMIC64_GET(ptr) ((atomic64_t)__ldar64((const volatile __int64 *)(ptr)))
    #elif defined(ARCH_ARM) || defined(ARCH_X86)
        // 32 位平台：ARM 无 acquire load 指令，x86 的 64 位 volatile load
        // 会编译为两条 mov（撕裂读）。统一退到 RMW 兜底确保原子性
        #define ATOMIC_GET(ptr)   ATOMIC_ADD(ptr, 0)
        #define ATOMIC64_GET(ptr) ATOMIC64_ADD(ptr, 0)
    #else
        // 纯 MSVC + Windows x64/ARM64：默认 /volatile:ms 下 volatile load 隐含 acquire 语义；
        // 64 位对齐 load 天然原子
        #define ATOMIC_GET(ptr)   ((atomic_t)*(const volatile atomic_t *)(ptr))
        #define ATOMIC64_GET(ptr) ((atomic64_t)*(const volatile atomic64_t *)(ptr))
    #endif
    // MSVC 的 ATOMIC_SET(InterlockedExchange) 全屏障 + ARM64 LDAR(RCsc) / x64 TSO 已满足 SB 的 StoreLoad 顺序，别名即可
    #define ATOMIC_GET_SEQCST(ptr)   ATOMIC_GET(ptr)
    #define ATOMIC64_GET_SEQCST(ptr) ATOMIC64_GET(ptr)
    // 松散序变体：仅适用于单写者字段（无并发写竞争，只需不撕裂 + 迟早可见）。
    // ADD 无更便宜的硬件原语，复用现有 Interlocked；32 位对齐 store 天然原子，直接裸写
    #define ATOMIC_ADD_RELAXED(ptr, val) ATOMIC_ADD(ptr, val)
    // volatile 与 ATOMIC_GET 读侧对称：/volatile:ms 下防止 MSVC 把这次写当死代码优化掉
    #define ATOMIC_SET_RELAXED(ptr, val) (*(volatile atomic_t *)(ptr) = (val))
    #define ATOMIC64_ADD_RELAXED(ptr, val) ATOMIC64_ADD(ptr, val)
    #if defined(ARCH_ARM) || defined(ARCH_X86)
        // 32 位平台：64 位 plain store 会编译成两条 mov（撕裂写），退到现有 Interlocked64
        #define ATOMIC64_SET_RELAXED(ptr, val) ATOMIC64_SET(ptr, val)
    #else
        // x64/ARM64：64 位对齐 store 天然原子
        #define ATOMIC64_SET_RELAXED(ptr, val) (*(volatile atomic64_t *)(ptr) = (val))
    #endif
#elif defined(OS_SUN)
    // Sun Studio：atomic_ops(3C) 不含屏障，前后补 __machine_rw_barrier(<mbarrier.h>) 凑齐 seq_cst
    #define ATOMIC_THREAD_FENCE_SEQCST() __machine_rw_barrier()
    static inline atomic_t _fetchandadd(atomic_t *ptr, atomic_t val) {
        return atomic_add_32_nv((volatile atomic_t *)ptr, val) - val;
    }
    static inline atomic64_t _fetchandadd64(atomic64_t *ptr, atomic64_t val) {
        return atomic_add_64_nv((volatile atomic64_t *)ptr, val) - val;
    }
    static inline atomic_t _sun_swap(atomic_t *ptr, atomic_t val) {
        ATOMIC_THREAD_FENCE_SEQCST();
        atomic_t old = atomic_swap_32((volatile atomic_t *)ptr, val);
        ATOMIC_THREAD_FENCE_SEQCST();
        return old;
    }
    static inline atomic64_t _sun_swap64(atomic64_t *ptr, atomic64_t val) {
        ATOMIC_THREAD_FENCE_SEQCST();
        atomic64_t old = atomic_swap_64((volatile atomic64_t *)ptr, val);
        ATOMIC_THREAD_FENCE_SEQCST();
        return old;
    }
    static inline int32_t _sun_cas(atomic_t *ptr, atomic_t oldval, atomic_t newval) {
        ATOMIC_THREAD_FENCE_SEQCST();
        int32_t ok = (atomic_cas_32((volatile atomic_t *)ptr, oldval, newval) == oldval);
        ATOMIC_THREAD_FENCE_SEQCST();
        return ok;
    }
    static inline int32_t _sun_cas64(atomic64_t *ptr, atomic64_t oldval, atomic64_t newval) {
        ATOMIC_THREAD_FENCE_SEQCST();
        int32_t ok = (atomic_cas_64((volatile atomic64_t *)ptr, oldval, newval) == oldval);
        ATOMIC_THREAD_FENCE_SEQCST();
        return ok;
    }
    #define ATOMIC_ADD(ptr, val) _fetchandadd(ptr, val)
    #define ATOMIC_SET(ptr, val) _sun_swap((atomic_t *)(ptr), val)
    #define ATOMIC_CAS(ptr, oldval, newval) _sun_cas((atomic_t *)(ptr), oldval, newval)
    #define ATOMIC64_ADD(ptr, val) _fetchandadd64(ptr, val)
    #define ATOMIC64_SET(ptr, val) _sun_swap64((atomic64_t *)(ptr), val)
    #define ATOMIC64_CAS(ptr, oldval, newval) _sun_cas64((atomic64_t *)(ptr), oldval, newval)
    // 原子读取：无 __atomic_* 内建，用 RMW 兜底（过强但正确）
    #define ATOMIC_GET(ptr)   ATOMIC_ADD(ptr, 0)
    #define ATOMIC64_GET(ptr) ATOMIC64_ADD(ptr, 0)
    #define ATOMIC_GET_SEQCST(ptr)   ATOMIC_GET(ptr)
    #define ATOMIC64_GET_SEQCST(ptr) ATOMIC64_GET(ptr)
    // 不手写 relaxed 原语，直接复用现有 seq_cst 版本（正确但不加速）
    #define ATOMIC_ADD_RELAXED(ptr, val) ATOMIC_ADD(ptr, val)
    #define ATOMIC_SET_RELAXED(ptr, val) ATOMIC_SET(ptr, val)
    #define ATOMIC64_ADD_RELAXED(ptr, val) ATOMIC64_ADD(ptr, val)
    #define ATOMIC64_SET_RELAXED(ptr, val) ATOMIC64_SET(ptr, val)
#elif defined(OS_AIX)
    // xlC：AIX 原子服务不含内存序，用 __sync()(PowerPC sync 全屏障，含 StoreLoad) 前后夹住凑齐 seq_cst。
    // AIX 无原子交换服务，ATOMIC_SET 由 compare_and_swap 循环构造
    #define ATOMIC_THREAD_FENCE_SEQCST() __sync()
    static inline atomic_t _aix_swap(atomic_t *ptr, atomic_t val) {
        atomic_t old;
        ATOMIC_THREAD_FENCE_SEQCST();
        do {
            old = *(volatile atomic_t *)ptr;
        } while (0 == compare_and_swap(ptr, &old, val));
        ATOMIC_THREAD_FENCE_SEQCST();
        return old;
    }
    static inline atomic64_t _aix_swap64(atomic64_t *ptr, atomic64_t val) {
        atomic64_t old;
        ATOMIC_THREAD_FENCE_SEQCST();
        do {
            old = *(volatile atomic64_t *)ptr;
        } while (0 == compare_and_swaplp(ptr, &old, val));
        ATOMIC_THREAD_FENCE_SEQCST();
        return old;
    }
    static inline int32_t _aix_cas(atomic_t *ptr, atomic_t oldval, atomic_t newval) {
        ATOMIC_THREAD_FENCE_SEQCST();
        int32_t ok = compare_and_swap(ptr, &oldval, newval);
        ATOMIC_THREAD_FENCE_SEQCST();
        return ok;
    }
    static inline int32_t _aix_cas64(atomic64_t *ptr, atomic64_t oldval, atomic64_t newval) {
        ATOMIC_THREAD_FENCE_SEQCST();
        int32_t ok = compare_and_swaplp(ptr, &oldval, newval);
        ATOMIC_THREAD_FENCE_SEQCST();
        return ok;
    }
    #define ATOMIC_ADD(ptr, val) fetch_and_add(ptr, val)
    #define ATOMIC_SET(ptr, val) _aix_swap(ptr, val)
    #define ATOMIC_CAS(ptr, oldval, newval) _aix_cas(ptr, oldval, newval)
    #define ATOMIC64_ADD(ptr, val) fetch_and_addlp(ptr, val)
    #define ATOMIC64_SET(ptr, val) _aix_swap64(ptr, val)
    #define ATOMIC64_CAS(ptr, oldval, newval) _aix_cas64(ptr, oldval, newval)
    // 原子读取：无 __atomic_* 内建，用 RMW 兜底（过强但正确）
    #define ATOMIC_GET(ptr)   ATOMIC_ADD(ptr, 0)
    #define ATOMIC64_GET(ptr) ATOMIC64_ADD(ptr, 0)
    #define ATOMIC_GET_SEQCST(ptr)   ATOMIC_GET(ptr)
    #define ATOMIC64_GET_SEQCST(ptr) ATOMIC64_GET(ptr)
    // 不手写 relaxed 原语，直接复用现有 seq_cst 版本（正确但不加速）
    #define ATOMIC_ADD_RELAXED(ptr, val) ATOMIC_ADD(ptr, val)
    #define ATOMIC_SET_RELAXED(ptr, val) ATOMIC_SET(ptr, val)
    #define ATOMIC64_ADD_RELAXED(ptr, val) ATOMIC64_ADD(ptr, val)
    #define ATOMIC64_SET_RELAXED(ptr, val) ATOMIC64_SET(ptr, val)
#else
    #error "atomic ops: unsupported compiler (need GCC/Clang, MSVC, Sun Studio on Solaris, or xlC on AIX)"
#endif

#endif//MACRO_ATOMIC_H_
