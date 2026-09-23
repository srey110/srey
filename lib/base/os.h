#ifndef OS_H_
#define OS_H_

/* 操作系统检测宏，参考 https://sourceforge.net/p/predef/wiki/OperatingSystems/ */
#if defined(_WIN32) || defined(_WIN64)
    #define OS_WIN
    #define OS_NAME "Windows"
#elif defined(linux) || defined(__linux) || defined(__linux__)
    #define OS_LINUX
    #define OS_NAME "Linux"
#elif defined(__APPLE__) && (defined(__GNUC__) || defined(__xlC__) || defined(__xlc__))
    #include <TargetConditionals.h>
    #if defined(TARGET_OS_MAC) && TARGET_OS_MAC
        #define OS_MAC
        #define OS_NAME "Mac OS"
    #elif defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
        #define OS_IOS
        #define OS_NAME "IOS"
    #endif
    #define OS_DARWIN
#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__) 
    #define OS_BSD
    #define OS_FBSD
    #define OS_NAME "FreeBSD"
#elif defined(__NetBSD__)
    #define OS_BSD
    #define OS_NBSD
    #define OS_NAME "NetBSD"
#elif defined(__OpenBSD__)
    #define OS_BSD
    #define OS_OBSD
    #define OS_NAME "OpenBSD"
#elif defined(__DragonFly__)
    #define OS_BSD
    #define OS_DFBSD
    #define OS_NAME "DragonFly"
#elif defined(sun) || defined(__sun)
    #define OS_SUN
    #define OS_NAME "SUN"
#elif defined(hpux) || defined(_hpux)|| defined(__hpux)
    #define OS_HPUX
    #define OS_NAME "HPUX"
#elif defined(_AIX) || defined(__HOS_AIX__)
    #define OS_AIX
    #define OS_NAME "AIX"
    #define READV_EINVAL // 某些 AIX 系统上 readv 无数据时会返回 EINVAL(22) 错误
#else
    #error "Unsupported operating system platform!"
#endif
// io模型
#if defined(OS_WIN)
    #define EV_IOCP
    #define EV_NAME "IOCP"
#elif defined(OS_LINUX)
    #define EV_EPOLL
    #define EV_NAME "EPOLL"
#elif defined(OS_BSD) || defined(OS_DARWIN)
    #define EV_KQUEUE
    #define EV_NAME "KQUEUE"
#elif defined(OS_SUN)
    #define EV_EVPORT
    //#define EV_DEVPOLL
    #if defined(EV_EVPORT)
        #define EV_NAME "EVPORT"
    #else
        #define EV_NAME "DEVPOLL"
    #endif
#elif defined(OS_AIX)
    #define EV_POLLSET
    #define EV_NAME "POLLSET"
#elif defined(OS_HPUX)
    #define EV_DEVPOLL
    #define EV_NAME "DEVPOLL"
#endif
/* 检测 CPU 架构：x64/x86/ARM/ARM64/PPC */
#if defined(OS_WIN)
    #if defined(_M_ARM64) || defined(_M_ARM64EC)
        #define ARCH_ARM64
        #define ARCH_NAME "ARM64"
    #elif defined(_M_ARM)
        #define ARCH_ARM
        #define ARCH_NAME "ARM"
    #elif defined(_M_X64) || defined(_M_AMD64) || (defined(_WIN64) && !defined(_M_IX86))
        #define ARCH_X64
        #define ARCH_NAME "X64"
    #else
        #define ARCH_X86
        #define ARCH_NAME "X86"
    #endif
#else
    #if defined(__i386) || defined(__i386__) || defined(_M_IX86)
        #define ARCH_X86
        #define ARCH_NAME "X86"
    #elif defined(__x86_64) || defined(__x86_64__) || defined(__amd64) || defined(_M_X64)
        #define ARCH_X64
        #define ARCH_NAME "X64"
    #elif defined(__aarch64__) || defined(__ARM64__)
        #define ARCH_ARM64
        #define ARCH_NAME "ARM64"
    #elif defined(__arm__)
        #define ARCH_ARM
        #define ARCH_NAME "ARM"
    #elif defined(__powerpc) || defined(__powerpc__)|| defined(__PPC)|| defined(__PPC__)
        #define ARCH_PPC
        #define ARCH_NAME "PPC"
    #endif
#endif
// tools/deps.py 落在 bin/ 的第三方库名带变体后缀「[d]_<x86|x64|arm64>」,各变体共存。
// Windows 靠 #pragma comment(lib) 链,名字得在源码里拼;POSIX 那边由 mk.sh 拼 -l
#if defined(OS_WIN)
    #if defined(ARCH_ARM64)
        #define DEPS_ARCH_SUFFIX "arm64"
    #elif defined(ARCH_X64)
        #define DEPS_ARCH_SUFFIX "x64"
    #else
        #define DEPS_ARCH_SUFFIX "x86"
    #endif
    #ifdef _DEBUG
        #define DEPS_LIB_SUFFIX "d_" DEPS_ARCH_SUFFIX
    #else
        #define DEPS_LIB_SUFFIX "_" DEPS_ARCH_SUFFIX
    #endif
#endif
// 触发模式。epoll 两种都支持,默认取水平触发:边缘触发下每个读事件都要多付一次必然
// EAGAIN 的 recv,水平触发让 buffer_from_sock 读到没填满即停,省掉那一次。
// 改回 1 即切边缘触发,下面的 TRIGGER_LT 会自动让开,两者恒互斥
#if defined(EV_EPOLL) && !defined(TRIGGER_ET)
    #define TRIGGER_ET          0 // epoll 是否使用边缘触发模式
#endif
// 水平触发:没读干净事件会重复报,收数据不必读到 EAGAIN 才停。IOCP 是完成通知,不在此列
#if defined(EV_KQUEUE) || defined(EV_EVPORT) || defined(EV_POLLSET) || defined(EV_DEVPOLL) \
    || (defined(EV_EPOLL) && 0 == TRIGGER_ET)
    #define TRIGGER_LT          1 // kqueue/evport/pollset/devpoll 恒是;epoll 看 TRIGGER_ET
#endif
// CPU cache line 大小,用于消除并发结构 false sharing
// 主流 x86_64 / ARM64 = 64;Apple Silicon / IBM POWER = 128;IBM z = 256;老 ARMv6 及以下 = 32
#if defined(__APPLE__) && defined(__aarch64__)
    #define CACHELINE_SIZE  128
#elif defined(__powerpc64__) || defined(__ppc64__) || defined(_ARCH_PPC64)
    #define CACHELINE_SIZE  128
#elif defined(__s390x__) || defined(__zarch__)
    #define CACHELINE_SIZE  256
#elif defined(__arm__) && (__ARM_ARCH < 7)
    #define CACHELINE_SIZE  32
#else
    #define CACHELINE_SIZE  64
#endif
// 五个编译器属性宏,按编译器分派而不是按 OS(理由同 macro_atomic.h)。
// CACHELINE_ALIGN:与 CACHELINE_SIZE 配对,消除 false sharing;落 #else 空实现只影响并发写入快慢,不影响正确性
// ALIGN8:按 8 字节对齐,给"序列号 + 定长元素"这类槽位用;落 #else 只影响寻址是否规整,不影响正确性
// FORCE_INLINE:强制内联,只给实测有效的热路径小函数用;必须与 static 配对,否则链接失败
// NOINLINE:禁止内联。给"热函数里那条几乎不走的慢路径"用——慢路径可内联时会把热函数的
//   内联成本顶过编译器阈值,于是热函数整个进不了内联,反而更慢。同样只在实测有效时加
// UNUSED:允许这个 static 函数没人调。只给"必须去掉 inline"的头文件函数用:inline 与
//   noinline 同时写 gcc 报 -Wattributes,去掉 inline 又会报 -Wunused-function,两头堵。
//   写法固定为 NOINLINE static UNUSED;其余头文件函数照旧 static inline,别拿它代替 inline
#if defined(__GNUC__) || defined(__clang__)
    #define CACHELINE_ALIGN __attribute__((aligned(CACHELINE_SIZE)))
    #define ALIGN8 __attribute__((aligned(8)))
    #define FORCE_INLINE inline __attribute__((always_inline))
    #define NOINLINE __attribute__((noinline))
    #define UNUSED __attribute__((unused))
#elif defined(OS_WIN)
    #define CACHELINE_ALIGN __declspec(align(CACHELINE_SIZE))
    #define ALIGN8 __declspec(align(8))
    #define FORCE_INLINE __forceinline
    #define NOINLINE __declspec(noinline)
    #define UNUSED
#else
    #define CACHELINE_ALIGN
    #define ALIGN8
    #define FORCE_INLINE inline
    #define NOINLINE
    #define UNUSED
#endif
// accept4 / pipe2 能力：无标准 feature-test 宏，按 OS 推导（新增支持平台在此一处维护）
#if defined(OS_LINUX) || defined(OS_BSD)
    #define HAVE_ACCEPT4
    #define HAVE_PIPE2
#endif
// backtrace / <execinfo.h> 能力：AIX 没有（新增不支持的平台在此一处维护）
#ifndef OS_AIX
    #define HAVE_BACKTRACE
#endif
// 是否启用了 AddressSanitizer：gcc 看 __SANITIZE_ADDRESS__，clang 看 __has_feature
#if defined(__SANITIZE_ADDRESS__)
    #define ENABLED_ASAN       1
#elif defined(__has_feature)
    #if __has_feature(address_sanitizer)
        #define ENABLED_ASAN   1
    #else
        #define ENABLED_ASAN   0
    #endif
#else
    #define ENABLED_ASAN       0
#endif
// 是否启用了 ThreadSanitizer。比 ASan 多一路 BUILD_TSAN：__SANITIZE_THREAD__ 要 GCC 7 才有，
// 更老的 GCC 与部分商用编译器两个谓词都没有，探测不到就会带着 -fsanitize=thread 却不通知
// minicoro 切换协程栈，报一片假 race。故 mk.sh 的 tsan 分支额外传 -DBUILD_TSAN=1 兜底
#if defined(BUILD_TSAN) || defined(__SANITIZE_THREAD__)
    #define ENABLED_TSAN       1
#elif defined(__has_feature)
    #if __has_feature(thread_sanitizer)
        #define ENABLED_TSAN   1
    #else
        #define ENABLED_TSAN   0
    #endif
#else
    #define ENABLED_TSAN       0
#endif

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <assert.h>
#include <time.h>
#include <fcntl.h>
#include <wchar.h>
#include <math.h>
#include <float.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <ctype.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <inttypes.h>
#if defined(OS_WIN)
    #include <winsock2.h>
    #include <ws2ipdef.h>
    #include <ws2tcpip.h>
    #include <TlHelp32.h>
    #include <io.h>
    #include <tchar.h>
    #include <direct.h>
    #include <process.h>
    #include <ObjBase.h>
    #include <minwindef.h>
    #include <guiddef.h>
    #include <Windows.h>
    #include <MSTcpIP.h>
    #include <mswsock.h>
    #include <sys/timeb.h>
    #pragma warning(push)
    #pragma warning(disable: 4091)
    #include <DbgHelp.h>
    #pragma warning(pop)
    #include <bcrypt.h>
#else
    #include <unistd.h>
    #include <signal.h>    
    #include <dirent.h>
    #include <libgen.h>
    #include <dlfcn.h>
    #include <locale.h>
    #include <netdb.h>
    #include <semaphore.h>
    #include <pthread.h>
    #include <sys/socket.h>
    #include <sys/mman.h>
    #include <sys/time.h>
    #include <sys/wait.h>
    #include <sys/ioctl.h>
    #include <sys/poll.h>
    #include <sched.h>
    #ifdef HAVE_BACKTRACE
        #include <execinfo.h>
        #include <sys/syscall.h>
    #endif
    #include <sys/resource.h>
    #include <sys/uio.h>
    #include <net/if.h>    
    #include <net/if_arp.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>    
    #if defined(OS_LINUX) 
        #include <sys/epoll.h>
    #elif defined(OS_DARWIN)
        #include <mach/mach_time.h>
        #include <sys/event.h>
        #include <os/lock.h>
    #elif defined(OS_SUN)
        #include <port.h>
        #include <atomic.h>
        #if !defined(__GNUC__) && !defined(__clang__)
            #include <mbarrier.h>
        #endif
        #include <sys/filio.h>
        #include <sys/devpoll.h>
    #elif defined (OS_BSD)
        #include <sys/sysctl.h>
        #include <sys/event.h>
    #elif defined (OS_AIX)
        #include <procinfo.h>
        #include <sys/atomic_op.h>
        #include <sys/pollset.h>
    #elif defined (OS_HPUX)
        #include <sys/param.h>
        #include <sys/pstat.h>
        #include <dl.h>
        #include <sys/devpoll.h>
    #endif
#endif // OS_WIN

#endif//OS_H_
