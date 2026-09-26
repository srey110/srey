#ifndef MACRO_ATOMIC_H_
#define MACRO_ATOMIC_H_

#include "base/os.h"

// 跨平台原子操作。四套后端：GCC/Clang 用编译器内建（一份覆盖所有 OS，含 Windows 上的
// MinGW/clang-cl），MSVC 用 Interlocked，Sun Studio 用 libc atomic，xlC 用 AIX 原子服务。
// 后两家的原语本身不带内存序，靠前后各夹一道 ATOMIC_THREAD_FENCE_SEQCST 凑出来。
// 该用哪个：
//   ATOMIC_GET / ATOMIC_SET / ATOMIC_ADD / ATOMIC_CAS  默认用这四个，够强，不会错
//   ATOMIC_SET_RELEASE   发布：写完一段数据再置标志位，对方 ATOMIC_GET 到标志就能看到数据。
//                        比 ATOMIC_SET 便宜，但它不挡后面的读上浮，握手场景别用
//   ATOMIC_*_RELAXED     只有一个线程写、读方也不在乎它与别的读写谁先谁后时用，最省
//   ATOMIC_GET_SEQCST    握手：双方"我先置标志，再看对方"这类互相试探的场景（log.c、loader.c
//   ATOMIC_THREAD_FENCE_SEQCST   的丢唤醒防护）。两边都得用足序版本，否则会同时看漏
//   ATOMIC_SET_SEQCST    握手里"置标志"那一步，配 ATOMIC_GET_SEQCST 用。比 ATOMIC_SET 便宜，但不返回旧值
// 内存序参数（GCC/Clang 内建里的 __ATOMIC_*）：
//   __ATOMIC_ACQUIRE     用在读上：读到对方用 RELEASE 写进去的值，就一定能看到对方在那次写之前写的数据
//   __ATOMIC_RELEASE     用在写上：配对方的 ACQUIRE 读，保证这次写之前的数据不会晚于它被对方看到
//   __ATOMIC_SEQ_CST     最严：所有线程看到的读写先后都一致，握手场景要它
//   __ATOMIC_RELAXED     只保证这个变量本身读写不撕裂，不管它和别的读写谁先谁后
// 带 64 的是 64 位版本，语义与 32 位一致
#if defined(OS_AIX)
    #ifndef __64BIT__
        #error "32-bit AIX (ILP32) is not supported; compile with -maix64 or -q64"
    #endif
    typedef int32_t atomic_t; // 32 位原子整数类型（AIX）
    typedef long atomic64_t; // 64 位原子整数类型（AIX）
#else
    typedef uint32_t atomic_t; // 32 位原子整数类型
    typedef uint64_t atomic64_t; // 64 位原子整数类型
#endif

#if defined(__GNUC__) || defined(__clang__)
    // 防止编译器和 CPU 对前后内存读写进行重排，并保证全局的顺序一致性
    #define ATOMIC_THREAD_FENCE_SEQCST() __atomic_thread_fence(__ATOMIC_SEQ_CST)
    #define ATOMIC_GET(ptr)   __atomic_load_n((ptr), __ATOMIC_ACQUIRE)
    #define ATOMIC_SET(ptr, val) __atomic_exchange_n(ptr, val, __ATOMIC_SEQ_CST)
    #define ATOMIC_ADD(ptr, val) __sync_fetch_and_add(ptr, val)
    #define ATOMIC_CAS(ptr, oldval, newval) __sync_bool_compare_and_swap(ptr, oldval, newval)
    #define ATOMIC64_GET(ptr) __atomic_load_n((ptr), __ATOMIC_ACQUIRE)
    #define ATOMIC64_SET(ptr, val) __atomic_exchange_n(ptr, val, __ATOMIC_SEQ_CST)
    #define ATOMIC64_ADD(ptr, val) __sync_fetch_and_add(ptr, val)
    #define ATOMIC64_CAS(ptr, oldval, newval) __sync_bool_compare_and_swap(ptr, oldval, newval)
    // ARMv8.3+ 上普通 acquire 读可能编成 LDAPR, 挡不住握手要的那种顺序, 得是 seq_cst 才编成 LDAR
    #define ATOMIC_GET_SEQCST(ptr)   __atomic_load_n((ptr), __ATOMIC_SEQ_CST)
    #define ATOMIC64_GET_SEQCST(ptr) __atomic_load_n((ptr), __ATOMIC_SEQ_CST)
    #define ATOMIC_SET_SEQCST(ptr, val) __atomic_store_n(ptr, val, __ATOMIC_SEQ_CST)
    #define ATOMIC64_SET_SEQCST(ptr, val) __atomic_store_n(ptr, val, __ATOMIC_SEQ_CST)
    #define ATOMIC_SET_RELEASE(ptr, val) __atomic_store_n(ptr, val, __ATOMIC_RELEASE)
    #define ATOMIC64_SET_RELEASE(ptr, val) __atomic_store_n(ptr, val, __ATOMIC_RELEASE)
    // 宽松内存顺序
    #define ATOMIC_ADD_RELAXED(ptr, val) __atomic_fetch_add(ptr, val, __ATOMIC_RELAXED)
    #define ATOMIC64_ADD_RELAXED(ptr, val) __atomic_fetch_add(ptr, val, __ATOMIC_RELAXED)
    #define ATOMIC_SET_RELAXED(ptr, val) __atomic_store_n(ptr, val, __ATOMIC_RELAXED)
    #define ATOMIC64_SET_RELAXED(ptr, val) __atomic_store_n(ptr, val, __ATOMIC_RELAXED)
    #define ATOMIC_GET_RELAXED(ptr)   __atomic_load_n((ptr), __ATOMIC_RELAXED)
    #define ATOMIC64_GET_RELAXED(ptr) __atomic_load_n((ptr), __ATOMIC_RELAXED)
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
        // STLR 之后的 LDAR 不会先于它执行，配上面的 __ldar32/64 就是足序握手
        #define ATOMIC_SET_SEQCST(ptr, val) __stlr32((unsigned __int32 volatile *)(ptr), (unsigned __int32)(val))
        #define ATOMIC64_SET_SEQCST(ptr, val) __stlr64((unsigned __int64 volatile *)(ptr), (unsigned __int64)(val))
    #else
        // x86/x64 的足序写本来就是一条 XCHG；32 位 ARM 没有对应的单条指令
        #define ATOMIC_SET_SEQCST(ptr, val) ATOMIC_SET(ptr, val)
        #define ATOMIC64_SET_SEQCST(ptr, val) ATOMIC64_SET(ptr, val)
    #endif
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
    #if defined(ARCH_ARM64)
        // _nf 是不带屏障的版本，只保原子性
        #define ATOMIC_ADD_RELAXED(ptr, val) _InterlockedExchangeAdd_nf((long volatile *)(ptr), (long)(val))
        #define ATOMIC64_ADD_RELAXED(ptr, val) _InterlockedExchangeAdd64_nf((__int64 volatile *)(ptr), (__int64)(val))
    #else
        #define ATOMIC_ADD_RELAXED(ptr, val) ATOMIC_ADD(ptr, val)
        #define ATOMIC64_ADD_RELAXED(ptr, val) ATOMIC64_ADD(ptr, val)
    #endif
    #define ATOMIC_SET_RELAXED(ptr, val) (*(volatile atomic_t *)(ptr) = (val))
    #define ATOMIC_GET_RELAXED(ptr) (*(volatile atomic_t *)(ptr))
    #if defined(ARCH_ARM) || defined(ARCH_X86)
        // 32 位读写 64 位都会拆成两条 mov 而撕裂，退回 Interlocked64
        #define ATOMIC64_SET_RELAXED(ptr, val) ATOMIC64_SET(ptr, val)
        #define ATOMIC64_GET_RELAXED(ptr) ATOMIC64_GET(ptr)
    #else
        #define ATOMIC64_SET_RELAXED(ptr, val) (*(volatile atomic64_t *)(ptr) = (val))
        #define ATOMIC64_GET_RELAXED(ptr) (*(volatile atomic64_t *)(ptr))
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
#else
    #error "atomic ops: unsupported compiler (need GCC/Clang, MSVC, Sun Studio on Solaris, or xlC on AIX)"
#endif
// 弱序别名兜底: 没有更弱版本可用的后端(Sun / AIX)一律退化到全屏障版, 语义只会更强不会更弱。
// 十个别名按后端整组给出, 所以一个 #ifndef 守住全组即可; GCC/Clang 与 MSVC 自己定义齐了不会进来
#ifndef ATOMIC_SET_RELEASE
    #define ATOMIC_SET_SEQCST(ptr, val) ATOMIC_SET(ptr, val)
    #define ATOMIC64_SET_SEQCST(ptr, val) ATOMIC64_SET(ptr, val)
    #define ATOMIC_SET_RELEASE(ptr, val) ATOMIC_SET(ptr, val)
    #define ATOMIC64_SET_RELEASE(ptr, val) ATOMIC64_SET(ptr, val)
    #define ATOMIC_ADD_RELAXED(ptr, val) ATOMIC_ADD(ptr, val)
    #define ATOMIC64_ADD_RELAXED(ptr, val) ATOMIC64_ADD(ptr, val)
    #define ATOMIC_SET_RELAXED(ptr, val) ATOMIC_SET(ptr, val)
    #define ATOMIC64_SET_RELAXED(ptr, val) ATOMIC64_SET(ptr, val)
    #define ATOMIC_GET_RELAXED(ptr) ATOMIC_GET(ptr)
    #define ATOMIC64_GET_RELAXED(ptr) ATOMIC64_GET(ptr)
#endif

#endif//MACRO_ATOMIC_H_
