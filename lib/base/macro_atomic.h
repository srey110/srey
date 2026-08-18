#ifndef MACRO_ATOMIC_H_
#define MACRO_ATOMIC_H_

#include "base/os.h"

// 跨平台原子操作。
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
    //内存栅栏（Memory Barrier）。它用于防止编译器和 CPU 在执行多线程代码时对读写指令进行乱序重排，
    //并强制所有使用该级别的操作建立一个全局统一的先后顺序
    #define ATOMIC_THREAD_FENCE_SEQCST() __atomic_thread_fence(__ATOMIC_SEQ_CST)
    //默认用这几个，够强，不会错
    #define ATOMIC_GET(ptr)   __atomic_load_n((ptr), __ATOMIC_ACQUIRE)
    #define ATOMIC_SET(ptr, val) __atomic_exchange_n(ptr, val, __ATOMIC_SEQ_CST)
    #define ATOMIC_ADD(ptr, val) __sync_fetch_and_add(ptr, val)
    #define ATOMIC_CAS(ptr, oldval, newval) __sync_bool_compare_and_swap(ptr, oldval, newval)
    #define ATOMIC64_GET(ptr) __atomic_load_n((ptr), __ATOMIC_ACQUIRE)
    #define ATOMIC64_SET(ptr, val) __atomic_exchange_n(ptr, val, __ATOMIC_SEQ_CST)
    #define ATOMIC64_ADD(ptr, val) __sync_fetch_and_add(ptr, val)
    #define ATOMIC64_CAS(ptr, oldval, newval) __sync_bool_compare_and_swap(ptr, oldval, newval)
    //原子地、不被中断地读取 ptr 指向的变量值，
    //并强制执行顺序一致性 的内存屏障，确保所有线程对该内存操作的顺序达成全局一致
    #define ATOMIC_GET_SEQCST(ptr)   __atomic_load_n((ptr), __ATOMIC_SEQ_CST)
    #define ATOMIC64_GET_SEQCST(ptr) __atomic_load_n((ptr), __ATOMIC_SEQ_CST)
    //原子方式将 val 写入 ptr 指向的内存，
    //确保在此操作之前的所有内存读写（普通变量或原子变量）不会被编译器或 CPU 重排到这个写操作之后
    #define ATOMIC_SET_RELEASE(ptr, val) __atomic_store_n(ptr, val, __ATOMIC_RELEASE)
    #define ATOMIC64_SET_RELEASE(ptr, val) __atomic_store_n(ptr, val, __ATOMIC_RELEASE)
    //宽松顺序 __ATOMIC_RELAXED 内存顺序意味着不提供任何线程间的内存屏障或同步排序约束，仅保证单次读写的原子性（防止撕裂），允许编译器和 CPU 对前后指令进行重排
    //原子操作方式将 val 加到 ptr 指向的内存变量上，并返回旧值。
    #define ATOMIC_ADD_RELAXED(ptr, val) __atomic_fetch_add(ptr, val, __ATOMIC_RELAXED)
    #define ATOMIC64_ADD_RELAXED(ptr, val) __atomic_fetch_add(ptr, val, __ATOMIC_RELAXED)
    //原子方式将 val 写入 ptr 指向的内存地址。
    #define ATOMIC_SET_RELAXED(ptr, val) __atomic_store_n(ptr, val, __ATOMIC_RELAXED)
    #define ATOMIC64_SET_RELAXED(ptr, val) __atomic_store_n(ptr, val, __ATOMIC_RELAXED)
#elif defined(OS_WIN)
    #define ATOMIC_THREAD_FENCE_SEQCST() MemoryBarrier()
    #if defined(ARCH_ARM64)
        // __ldar32/64 就是一条 load-acquire 指令
        #define ATOMIC_GET(ptr)   ((atomic_t)__ldar32((const volatile __int32 *)(ptr)))
        #define ATOMIC64_GET(ptr) ((atomic64_t)__ldar64((const volatile __int64 *)(ptr)))
    #elif defined(ARCH_ARM) || defined(ARCH_X86)
        // 32 位：ARM 没有 acquire load 指令，x86 读 64 位会拆成两条 mov 而撕裂，都退回 RMW
        #define ATOMIC_GET(ptr)   ATOMIC_ADD(ptr, 0)
        #define ATOMIC64_GET(ptr) ATOMIC64_ADD(ptr, 0)
    #else
        // x64：默认 /volatile:ms 下 volatile 读自带 acquire，64 位对齐读天然原子
        #define ATOMIC_GET(ptr)   ((atomic_t)*(const volatile atomic_t *)(ptr))
        #define ATOMIC64_GET(ptr) ((atomic64_t)*(const volatile atomic64_t *)(ptr))
    #endif
    #define ATOMIC_SET(ptr, val) InterlockedExchange(ptr, val)
    #define ATOMIC_ADD(ptr, val) InterlockedExchangeAdd(ptr, val)
    #define ATOMIC_CAS(ptr, oldval, newval) (InterlockedCompareExchange(ptr, newval, oldval) == oldval)
    #define ATOMIC64_SET(ptr, val) InterlockedExchange64(ptr, val)
    #define ATOMIC64_ADD(ptr, val) InterlockedExchangeAdd64(ptr, val)
    #define ATOMIC64_CAS(ptr, oldval, newval) (InterlockedCompareExchange64(ptr, newval, oldval) == oldval)
    #define ATOMIC_GET_SEQCST(ptr)   ATOMIC_GET(ptr)
    #define ATOMIC64_GET_SEQCST(ptr) ATOMIC64_GET(ptr)
    #if defined(ARCH_ARM64)
        // __stlr32/64 就是一条 store-release 指令，与 __ldar32/64 配对
        #define ATOMIC_SET_RELEASE(ptr, val) __stlr32((unsigned __int32 volatile *)(ptr), (unsigned __int32)(val))
        #define ATOMIC64_SET_RELEASE(ptr, val) __stlr64((unsigned __int64 volatile *)(ptr), (unsigned __int64)(val))
    #elif defined(ARCH_ARM)
        // 32 位 ARM 默认 /volatile:iso，volatile 写不带任何顺序，只能退回 Interlocked
        #define ATOMIC_SET_RELEASE(ptr, val) ATOMIC_SET(ptr, val)
        #define ATOMIC64_SET_RELEASE(ptr, val) ATOMIC64_SET(ptr, val)
    #elif defined(ARCH_X86)
        // 32 位 x86：volatile 写自带 release，但写 64 位会拆成两条 mov 而撕裂，64 位那条退回 Interlocked64
        #define ATOMIC_SET_RELEASE(ptr, val) (*(volatile atomic_t *)(ptr) = (val))
        #define ATOMIC64_SET_RELEASE(ptr, val) ATOMIC64_SET(ptr, val)
    #else
        // x64：/volatile:ms 下 volatile 写自带 release，从 LOCK XCHG 变成一条 MOV
        #define ATOMIC_SET_RELEASE(ptr, val) (*(volatile atomic_t *)(ptr) = (val))
        #define ATOMIC64_SET_RELEASE(ptr, val) (*(volatile atomic64_t *)(ptr) = (val))
    #endif
    #define ATOMIC_ADD_RELAXED(ptr, val) ATOMIC_ADD(ptr, val)
    #define ATOMIC64_ADD_RELAXED(ptr, val) ATOMIC64_ADD(ptr, val)
    #define ATOMIC_SET_RELAXED(ptr, val) (*(volatile atomic_t *)(ptr) = (val))
    #if defined(ARCH_ARM) || defined(ARCH_X86)
        // 32 位写 64 位会拆成两条 mov 而撕裂，退回 Interlocked64
        #define ATOMIC64_SET_RELAXED(ptr, val) ATOMIC64_SET(ptr, val)
    #else
        #define ATOMIC64_SET_RELAXED(ptr, val) (*(volatile atomic64_t *)(ptr) = (val))
    #endif
#elif defined(OS_SUN)
    // Sun Studio：atomic_ops(3C) 的原语不带内存序，前后各夹一道屏障凑成 seq_cst
    #define ATOMIC_THREAD_FENCE_SEQCST() __machine_rw_barrier()
    static inline atomic_t _fetchandadd(atomic_t *ptr, atomic_t val) {
        ATOMIC_THREAD_FENCE_SEQCST();
        atomic_t old = atomic_add_32_nv((volatile atomic_t *)ptr, val) - val;
        ATOMIC_THREAD_FENCE_SEQCST();
        return old;
    }
    static inline atomic64_t _fetchandadd64(atomic64_t *ptr, atomic64_t val) {
        ATOMIC_THREAD_FENCE_SEQCST();
        atomic64_t old = atomic_add_64_nv((volatile atomic64_t *)ptr, val) - val;
        ATOMIC_THREAD_FENCE_SEQCST();
        return old;
    }
    // 读：对齐的单条 ld 本身就是原子的，读完补一道屏障就成 acquire。
    // 不用 atomic_add_32_nv(ptr,0)——那是个 cas 循环，为一次只读把缓存行抢成独占再写回
    static inline atomic_t _sun_load(atomic_t *ptr) {
        atomic_t v = *(volatile atomic_t *)ptr;
        ATOMIC_THREAD_FENCE_SEQCST();
        return v;
    }
    static inline atomic64_t _sun_load64(atomic64_t *ptr) {
    #if defined(_LP64) || defined(__LP64__)
        atomic64_t v = *(volatile atomic64_t *)ptr;// ldx / movq 单指令
    #else
        // 32 位 ABI 下读 64 位会被拆成两条 ld 而撕裂（SPARC V8 无 ldx、i386 无单指令 64 位读），
        // 退回 cas 循环换原子性。SPARC 不在 os.h 的 ARCH_* 里，所以这里按 ABI 宽度判
        atomic64_t v = atomic_add_64_nv((volatile atomic64_t *)ptr, 0);
    #endif
        ATOMIC_THREAD_FENCE_SEQCST();
        return v;
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
    #define ATOMIC_GET(ptr)   _sun_load((atomic_t *)(ptr))
    #define ATOMIC_SET(ptr, val) _sun_swap((atomic_t *)(ptr), val)
    #define ATOMIC_ADD(ptr, val) _fetchandadd((atomic_t *)(ptr), val)
    #define ATOMIC_CAS(ptr, oldval, newval) _sun_cas((atomic_t *)(ptr), oldval, newval)
    #define ATOMIC64_GET(ptr) _sun_load64((atomic64_t *)(ptr))
    #define ATOMIC64_SET(ptr, val) _sun_swap64((atomic64_t *)(ptr), val)
    #define ATOMIC64_ADD(ptr, val) _fetchandadd64((atomic64_t *)(ptr), val)
    #define ATOMIC64_CAS(ptr, oldval, newval) _sun_cas64((atomic64_t *)(ptr), oldval, newval)
    #define ATOMIC_GET_SEQCST(ptr)   _fetchandadd((atomic_t *)(ptr), 0)
    #define ATOMIC64_GET_SEQCST(ptr) _fetchandadd64((atomic64_t *)(ptr), 0)
    #define ATOMIC_SET_RELEASE(ptr, val) ATOMIC_SET(ptr, val)
    #define ATOMIC64_SET_RELEASE(ptr, val) ATOMIC64_SET(ptr, val)
    #define ATOMIC_ADD_RELAXED(ptr, val) ATOMIC_ADD(ptr, val)
    #define ATOMIC64_ADD_RELAXED(ptr, val) ATOMIC64_ADD(ptr, val)
    #define ATOMIC_SET_RELAXED(ptr, val) ATOMIC_SET(ptr, val)
    #define ATOMIC64_SET_RELAXED(ptr, val) ATOMIC64_SET(ptr, val)
#elif defined(OS_AIX)
    // xlC：AIX 原子服务同样不带内存序，用 __sync()（PowerPC 全屏障）前后夹住凑成 seq_cst。
    // AIX 没有原子交换服务，ATOMIC_SET 只能用 compare_and_swap 循环拼
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
    static inline atomic_t _aix_fetchandadd(atomic_t *ptr, atomic_t val) {
        ATOMIC_THREAD_FENCE_SEQCST();
        atomic_t old = fetch_and_add(ptr, val);
        ATOMIC_THREAD_FENCE_SEQCST();
        return old;
    }
    static inline atomic64_t _aix_fetchandadd64(atomic64_t *ptr, atomic64_t val) {
        ATOMIC_THREAD_FENCE_SEQCST();
        atomic64_t old = fetch_and_addlp(ptr, val);
        ATOMIC_THREAD_FENCE_SEQCST();
        return old;
    }
    // 读：POWER 上对齐的单条 lwz/ld 本身就是原子的，读完补一道屏障就成 acquire。
    // 不用 fetch_and_add(ptr,0)——那是个 lwarx/stwcx. 循环，为一次只读把缓存行抢成独占再写回
    static inline atomic_t _aix_load(atomic_t *ptr) {
        atomic_t v = *(volatile atomic_t *)ptr;
        ATOMIC_THREAD_FENCE_SEQCST();
        return v;
    }
    static inline atomic64_t _aix_load64(atomic64_t *ptr) {
        atomic64_t v = *(volatile atomic64_t *)ptr;
        ATOMIC_THREAD_FENCE_SEQCST();
        return v;
    }
    #define ATOMIC_GET(ptr)   _aix_load((atomic_t *)(ptr))
    #define ATOMIC_SET(ptr, val) _aix_swap(ptr, val)
    #define ATOMIC_ADD(ptr, val) _aix_fetchandadd((atomic_t *)(ptr), val)
    #define ATOMIC_CAS(ptr, oldval, newval) _aix_cas(ptr, oldval, newval)
    #define ATOMIC64_GET(ptr) _aix_load64((atomic64_t *)(ptr))
    #define ATOMIC64_SET(ptr, val) _aix_swap64(ptr, val)
    #define ATOMIC64_ADD(ptr, val) _aix_fetchandadd64((atomic64_t *)(ptr), val)
    #define ATOMIC64_CAS(ptr, oldval, newval) _aix_cas64(ptr, oldval, newval)
    #define ATOMIC_GET_SEQCST(ptr)   _aix_fetchandadd((atomic_t *)(ptr), 0)
    #define ATOMIC64_GET_SEQCST(ptr) _aix_fetchandadd64((atomic64_t *)(ptr), 0)
    #define ATOMIC_SET_RELEASE(ptr, val) ATOMIC_SET(ptr, val)
    #define ATOMIC64_SET_RELEASE(ptr, val) ATOMIC64_SET(ptr, val)
    #define ATOMIC_ADD_RELAXED(ptr, val) ATOMIC_ADD(ptr, val)
    #define ATOMIC64_ADD_RELAXED(ptr, val) ATOMIC64_ADD(ptr, val)
    #define ATOMIC_SET_RELAXED(ptr, val) ATOMIC_SET(ptr, val)
    #define ATOMIC64_SET_RELAXED(ptr, val) ATOMIC64_SET(ptr, val)
#else
    #error "atomic ops: unsupported compiler (need GCC/Clang, MSVC, Sun Studio on Solaris, or xlC on AIX)"
#endif

#endif//MACRO_ATOMIC_H_
