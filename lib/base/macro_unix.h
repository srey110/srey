#ifndef MACRO_UNIX_H_
#define MACRO_UNIX_H_

#include "base/os.h"

#ifndef OS_WIN

#define DLL_EXNAME "so"         // 动态库扩展名
#define PATH_SEPARATOR '/'      // 路径分隔符
#define PATH_SEPARATORSTR "/"   // 路径分隔符字符串
#if defined(PATH_MAX)
    #define PATH_LENS PATH_MAX
#elif defined(MAXPATHLEN)
    #define PATH_LENS MAXPATHLEN
#else
    #define PATH_LENS 260       // 路径最大长度（默认值）
#endif
#define SOCKET int      // socket 类型
#define INVALID_SOCK -1 // 无效 socket 句柄

#if EAGAIN == EWOULDBLOCK
    #define IS_EAGAIN(e) (EAGAIN == (e))                        // 判断是否为 EAGAIN 错误
#else
    #define IS_EAGAIN(e) (EAGAIN == (e) || EWOULDBLOCK == (e))  // 判断是否为 EAGAIN 或 EWOULDBLOCK 错误
#endif
#define ERR_RW_RETRIABLE(e)      ((e) == EINTR || IS_EAGAIN(e))         // 判断读写错误是否可重试
#define ERR_CONNECT_RETRIABLE(e) ((e) == EINTR || (e) == EINPROGRESS)   // 判断连接错误是否可重试
#define GETPID   getpid         // 获取当前进程 ID
#define STRICMP  strcasecmp     // 不区分大小写字符串比较
#define STRNCMP  strncasecmp    // 不区分大小写的前 n 字节字符串比较
#define STRTOK   strtok_r       // 线程安全的字符串分割
#define SNPRINTF snprintf       // 格式化输出到缓冲区
#define SWPRINTF swprintf       // 宽字符格式化输出
#define FSTAT    stat           // 获取文件状态
// 微秒级睡眠
#define USLEEP(us)\
    do {\
        uint64_t _slus = (uint64_t)(us);\
        struct timespec _slts;\
        _slts.tv_sec = (time_t)(_slus / 1000000);\
        _slts.tv_nsec = (long)(_slus % 1000000) * 1000L;\
        nanosleep(&_slts, NULL);\
    } while (0)
// 毫秒级睡眠
#define MSLEEP(ms) USLEEP((uint64_t)(ms) * 1000)
#define THREAD_YIELD() sched_yield() // OS 级线程让出，用于自旋超限后的兜底退避
/* 自旋等待 CPU 暂停提示，降低功耗并减少流水线压力 */
#if defined(ARCH_X86) || defined(ARCH_X64)
    #define CPU_PAUSE() __asm__ volatile("pause" ::: "memory")     // x86/x64 平台：使用 pause 指令
#elif defined(ARCH_ARM64)
    #define CPU_PAUSE() __asm__ volatile("yield" ::: "memory")     // ARM64：使用 yield 指令
#elif defined(ARCH_ARM)
    #if defined(__ARM_ARCH) && __ARM_ARCH >= 7
        #define CPU_PAUSE() __asm__ volatile("yield" ::: "memory") // ARMv7+：使用 yield 指令
    #else
        #define CPU_PAUSE() sched_yield()                          // ARMv4/5/6：让出 CPU
    #endif
#elif defined(ARCH_PPC)
    #define CPU_PAUSE() __asm__ volatile("or 27,27,27" ::: "memory") // PPC 平台：低优先级提示
#else
    #define CPU_PAUSE() sched_yield()   // 其他平台：主动让出 CPU
#endif
// 线程局部存储
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
    #define THREAD_LOCAL _Thread_local
#else
    #define THREAD_LOCAL __thread
#endif
#define TIMEB  timeb            // 时间结构体类型
#define FTIME  ftime            // 获取当前时间（毫秒精度）
#define ACCESS access           // 检查文件访问权限
#define MKDIR(path) mkdir(path, S_IRWXU) // 创建目录（仅属主 rwx；目录必须含 x 位才能 traverse 进入，否则后续在目录内 fopen 会因路径解析 EACCES 失败）
#define LOCALTIME(ts, dt) localtime_r((ts), (dt)) // 线程安全的本地时间转换
#define SOCK_CLOSE  close       // 关闭 socket
#define ERRNO       errno       // 获取当前 errno 错误码
#define ERRORSTR(errcode) strerror(errcode) // 将错误码转换为字符串

// typedef 按 OS（仅类型不同）；ATOMIC_* 在下方按编译器统一分流
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
// ATOMIC_* 按编译器分流：GCC/Clang 用内建（一份覆盖所有 OS）；
// 否则按 OS 回退——Sun Studio 用 libc atomic + __machine_rw_barrier，xlC 用 AIX 原子服务 + __sync()
#if defined(__GNUC__) || defined(__clang__)
    #define ATOMIC_ADD(ptr, val) __sync_fetch_and_add(ptr, val)
    #define ATOMIC_SET(ptr, val) __atomic_exchange_n(ptr, val, __ATOMIC_SEQ_CST)
    #define ATOMIC_CAS(ptr, oldval, newval) __sync_bool_compare_and_swap(ptr, oldval, newval)
    #define ATOMIC64_ADD(ptr, val) __sync_fetch_and_add(ptr, val)
    #define ATOMIC64_SET(ptr, val) __atomic_exchange_n(ptr, val, __ATOMIC_SEQ_CST)
    #define ATOMIC64_CAS(ptr, oldval, newval) __sync_bool_compare_and_swap(ptr, oldval, newval)
#elif defined(OS_SUN)
    // Sun Studio：atomic_ops(3C) 不含屏障，前后补 __machine_rw_barrier(<mbarrier.h>) 凑齐 seq_cst
    static inline atomic_t _fetchandadd(atomic_t *ptr, atomic_t val) {
        return atomic_add_32_nv((volatile atomic_t *)ptr, val) - val;
    }
    static inline atomic64_t _fetchandadd64(atomic64_t *ptr, atomic64_t val) {
        return atomic_add_64_nv((volatile atomic64_t *)ptr, val) - val;
    }
    static inline atomic_t _sun_swap(atomic_t *ptr, atomic_t val) {
        __machine_rw_barrier();
        atomic_t old = atomic_swap_32((volatile atomic_t *)ptr, val);
        __machine_rw_barrier();
        return old;
    }
    static inline atomic64_t _sun_swap64(atomic64_t *ptr, atomic64_t val) {
        __machine_rw_barrier();
        atomic64_t old = atomic_swap_64((volatile atomic64_t *)ptr, val);
        __machine_rw_barrier();
        return old;
    }
    static inline int32_t _sun_cas(atomic_t *ptr, atomic_t oldval, atomic_t newval) {
        __machine_rw_barrier();
        int32_t ok = (atomic_cas_32((volatile atomic_t *)ptr, oldval, newval) == oldval);
        __machine_rw_barrier();
        return ok;
    }
    static inline int32_t _sun_cas64(atomic64_t *ptr, atomic64_t oldval, atomic64_t newval) {
        __machine_rw_barrier();
        int32_t ok = (atomic_cas_64((volatile atomic64_t *)ptr, oldval, newval) == oldval);
        __machine_rw_barrier();
        return ok;
    }
    #define ATOMIC_ADD(ptr, val) _fetchandadd(ptr, val)
    #define ATOMIC_SET(ptr, val) _sun_swap((atomic_t *)(ptr), val)
    #define ATOMIC_CAS(ptr, oldval, newval) _sun_cas((atomic_t *)(ptr), oldval, newval)
    #define ATOMIC64_ADD(ptr, val) _fetchandadd64(ptr, val)
    #define ATOMIC64_SET(ptr, val) _sun_swap64((atomic64_t *)(ptr), val)
    #define ATOMIC64_CAS(ptr, oldval, newval) _sun_cas64((atomic64_t *)(ptr), oldval, newval)
#elif defined(OS_AIX)
    // xlC：AIX 原子服务不含内存序，用 __sync()(PowerPC sync 全屏障，含 StoreLoad) 前后夹住凑齐 seq_cst。
    // AIX 无原子交换服务，ATOMIC_SET 由 compare_and_swap 循环构造
    static inline atomic_t _aix_swap(atomic_t *ptr, atomic_t val) {
        atomic_t old;
        __sync();
        do {
            old = *(volatile atomic_t *)ptr;
        } while (0 == compare_and_swap(ptr, &old, val));
        __sync();
        return old;
    }
    static inline atomic64_t _aix_swap64(atomic64_t *ptr, atomic64_t val) {
        atomic64_t old;
        __sync();
        do {
            old = *(volatile atomic64_t *)ptr;
        } while (0 == compare_and_swaplp(ptr, &old, val));
        __sync();
        return old;
    }
    static inline int32_t _aix_cas(atomic_t *ptr, atomic_t oldval, atomic_t newval) {
        __sync();
        int32_t ok = compare_and_swap(ptr, &oldval, newval);
        __sync();
        return ok;
    }
    static inline int32_t _aix_cas64(atomic64_t *ptr, atomic64_t oldval, atomic64_t newval) {
        __sync();
        int32_t ok = compare_and_swaplp(ptr, &oldval, newval);
        __sync();
        return ok;
    }
    #define ATOMIC_ADD(ptr, val) fetch_and_add(ptr, val)
    #define ATOMIC_SET(ptr, val) _aix_swap(ptr, val)
    #define ATOMIC_CAS(ptr, oldval, newval) _aix_cas(ptr, oldval, newval)
    #define ATOMIC64_ADD(ptr, val) fetch_and_addlp(ptr, val)
    #define ATOMIC64_SET(ptr, val) _aix_swap64(ptr, val)
    #define ATOMIC64_CAS(ptr, oldval, newval) _aix_cas64(ptr, oldval, newval)
#else
    #error "atomic ops: unsupported compiler (need GCC/Clang, Sun Studio on Solaris, or xlC on AIX)"
#endif
/// <summary>
/// 不区分大小写的内存比较（Unix 平台实现）
/// </summary>
/// <param name="ptr1">第一块内存指针</param>
/// <param name="ptr2">第二块内存指针</param>
/// <param name="lens">比较字节数</param>
/// <returns>0 表示相等，正数表示 ptr1 大，负数表示 ptr1 小</returns>
int32_t _memicmp(const void *ptr1, const void *ptr2, size_t lens);

#endif//OS_WIN
#endif//MACRO_UNIX_H_
