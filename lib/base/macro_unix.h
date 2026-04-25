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
#define STRNCPY  strncpy        // 固定长度字符串复制
#define FSTAT    stat           // 获取文件状态
#define USLEEP(us) usleep(us)   // 微秒级睡眠
#define MSLEEP(ms) usleep(ms * 1000) // 毫秒级睡眠
/* 自旋等待 CPU 暂停提示，降低功耗并减少流水线压力 */
#if defined(ARCH_X86) || defined(ARCH_X64)
    #define CPU_PAUSE() __asm__ volatile("pause" ::: "memory")     // x86/x64 平台：使用 pause 指令
#elif defined(ARCH_ARM) || defined(ARCH_ARM64)
    #define CPU_PAUSE() __asm__ volatile("yield" ::: "memory")     // ARM 平台：使用 yield 指令
#elif defined(ARCH_PPC)
    #define CPU_PAUSE() __asm__ volatile("or 27,27,27" ::: "memory") // PPC 平台：低优先级提示
#else
    #define CPU_PAUSE() sched_yield()   // 其他平台：主动让出 CPU
#endif
#define TIMEB  timeb            // 时间结构体类型
#define FTIME  ftime            // 获取当前时间（毫秒精度）
#define ACCESS access           // 检查文件访问权限
#define MKDIR(path) mkdir(path, S_IRUSR|S_IWUSR) // 创建目录（仅属主读写）
#define SOCK_CLOSE  close       // 关闭 socket
#define ERRNO       errno       // 获取当前 errno 错误码
#define ERRORSTR(errcode) strerror(errcode) // 将错误码转换为字符串

#if defined(OS_SUN)
    typedef uint32_t atomic_t;   // 32 位原子整数类型（Solaris）
    typedef uint64_t atomic64_t; // 64 位原子整数类型（Solaris）
    // 原子加，返回旧值（Solaris atomic_add_32_nv 返回新值，减去 val 得旧值）
    static inline atomic_t _fetchandadd(atomic_t *ptr, atomic_t val) {
        return atomic_add_32_nv((volatile atomic_t *)ptr, val) - val;
    }
    static inline atomic64_t _fetchandadd64(atomic64_t *ptr, atomic64_t val) {
        return atomic_add_64_nv((volatile atomic64_t *)ptr, val) - val;
    }
    // 原子加 32 位，返回旧值
    #define ATOMIC_ADD(ptr, val) _fetchandadd(ptr, val)
    // 原子交换 32 位，返回旧值
    #define ATOMIC_SET(ptr, val) atomic_swap_32((volatile atomic_t *)ptr, val)
    // 原子比较并交换 32 位，成功返回非零
    #define ATOMIC_CAS(ptr, oldval, newval) (atomic_cas_32((volatile atomic_t *)ptr, oldval, newval) == oldval)
    #define ATOMIC64_ADD(ptr, val) _fetchandadd64(ptr, val)
    #define ATOMIC64_SET(ptr, val) atomic_swap_64((volatile atomic64_t *)ptr, val)
    #define ATOMIC64_CAS(ptr, oldval, newval) (atomic_cas_64((volatile atomic64_t *)ptr, oldval, newval) == oldval)
#elif defined(OS_AIX)
    typedef int32_t atomic_t;  // 32 位原子整数类型（AIX）
    typedef long atomic64_t;   // 64 位原子整数类型（AIX）
    // 原子交换，返回旧值（通过 CAS 循环实现）
    static inline atomic_t _fetchandset(atomic_t *ptr, atomic_t value) {
        atomic_t oldval;
        do {
            oldval = *ptr;
        } while (!compare_and_swap(ptr, &oldval, value));
        return oldval;
    };
    static inline atomic_t _fetchandset64(atomic64_t *ptr, atomic64_t value) {
        atomic64_t oldval;
        do {
            oldval = *ptr;
        } while (!compare_and_swaplp(ptr, &oldval, value));
        return oldval;
    };
    // 原子比较并交换 32 位（AIX 封装）
    static inline int32_t _aix_cas(atomic_t *ptr, atomic_t oldval, atomic_t newval) {
        return compare_and_swap(ptr, &oldval, newval);
    };
    static inline int32_t _aix_cas64(atomic64_t *ptr, atomic64_t oldval, atomic64_t newval) {
        return compare_and_swaplp(ptr, &oldval, newval);
    };
    // 原子加 32 位，返回旧值
    #define ATOMIC_ADD(ptr, val) fetch_and_add(ptr, val)
    #define ATOMIC_SET(ptr, val) _fetchandset(ptr, val)
    #define ATOMIC_CAS(ptr, oldval, newval) _aix_cas(ptr, oldval, newval)
    #define ATOMIC64_ADD(ptr, val) fetch_and_addlp(ptr, val)
    #define ATOMIC64_SET(ptr, val) _fetchandset64(ptr, val)
    #define ATOMIC64_CAS(ptr, oldval, newval) _aix_cas64(ptr, oldval, newval)
#else
    typedef uint32_t atomic_t;   // 32 位原子整数类型（GCC 内建）
    typedef uint64_t atomic64_t; // 64 位原子整数类型（GCC 内建）
    // 原子交换，返回旧值（通过 CAS 循环实现）
    static inline atomic_t _fetchandset(atomic_t *ptr, atomic_t value) {
        atomic_t oldval;
        do {
            oldval = *ptr;
        } while (!__sync_bool_compare_and_swap(ptr, oldval, value));
        return oldval;
    };
    static inline atomic64_t _fetchandset64(atomic64_t *ptr, atomic64_t value) {
        atomic64_t oldval;
        do {
            oldval = *ptr;
        } while (!__sync_bool_compare_and_swap(ptr, oldval, value));
        return oldval;
    };
    // __sync_fetch_and_add：原子加，返回旧值
    #define ATOMIC_ADD(ptr, val) __sync_fetch_and_add(ptr, val)
    #define ATOMIC_SET(ptr, val) _fetchandset(ptr, val)
    // __sync_bool_compare_and_swap：CAS，成功返回 true
    #define ATOMIC_CAS(ptr, oldval, newval) __sync_bool_compare_and_swap(ptr, oldval, newval)
    #define ATOMIC64_ADD(ptr, val) __sync_fetch_and_add(ptr, val)
    #define ATOMIC64_SET(ptr, val) _fetchandset64(ptr, val)
    #define ATOMIC64_CAS(ptr, oldval, newval) __sync_bool_compare_and_swap(ptr, oldval, newval)
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
