#ifndef OS_UNIX_H_
#define OS_UNIX_H_

// 非 Windows 平台的系统头与差异宏，只由 os.h 末尾引入，别处不要直接 include

#ifndef OS_WIN

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
    #if !defined(CC_GNU)
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

// 主机字节序：常量联合体读首字节，编译期即可折成常量。IS_LITTLE 是表达式不是预处理常量，不能写进 #if
static const union {
    int32_t dummy;
    int8_t little;// 小端机器上为 1
} _host_endian = { 1 };
#define IS_LITTLE (0 != _host_endian.little) // 1 小端，0 大端
#define DLL_EXNAME "so" // 动态库扩展名
#define PATH_SEPARATOR '/' // 路径分隔符
#define PATH_SEPARATORSTR "/" // 路径分隔符字符串
#define SOCKET int // socket 类型
#define INVALID_SOCK -1 // 无效 socket 句柄

#if defined(PATH_MAX)
    #define PATH_LENS PATH_MAX
#elif defined(MAXPATHLEN)
    #define PATH_LENS MAXPATHLEN
#else
    #define PATH_LENS 260 // 路径最大长度（默认值）
#endif//PATH_MAX

#if EAGAIN == EWOULDBLOCK
    #define IS_EAGAIN(e) (EAGAIN == (e)) // 判断是否为 EAGAIN 错误
#else
    #define IS_EAGAIN(e) (EAGAIN == (e) || EWOULDBLOCK == (e)) // 判断是否为 EAGAIN 或 EWOULDBLOCK 错误
#endif//EAGAIN == EWOULDBLOCK
#define ERR_RW_RETRIABLE(e)      ((e) == EINTR || IS_EAGAIN(e)) // 判断读写错误是否可重试
#define ERR_CONNECT_RETRIABLE(e) ((e) == EINTR || (e) == EINPROGRESS) // 判断连接错误是否可重试

#define GETPID   getpid // 获取当前进程 ID
#define STRTOK   strtok_r // 线程安全的字符串分割
#define SNPRINTF snprintf // 格式化输出到缓冲区
#define SWPRINTF swprintf // 宽字符格式化输出
#define FSTAT    stat // 获取文件状态
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
#define TIMEB timeb // 时间结构体类型
#define FTIME ftime // 获取当前时间（毫秒精度）
#define ACCESS access // 检查文件访问权限
#define MKDIR(path) mkdir(path, S_IRWXU) // 创建目录（仅属主 rwx；目录必须含 x 位才能 traverse 进入，否则后续在目录内 fopen 会因路径解析 EACCES 失败）
// 线程安全的本地时间转换；返回 0 成功、非 0 失败（与 Windows 侧 localtime_s 同约定）。
// ts 超出可表示范围时 localtime_r 返 NULL 且不保证写 dt，调用方必须判返回值再用 dt
#define LOCALTIME(ts, dt) (NULL == localtime_r((ts), (dt)) ? -1 : 0)
#define GMTIME(ts, dt) (NULL == gmtime_r((ts), (dt)) ? -1 : 0)
#define SOCK_CLOSE  close // 关闭 socket
#define SET_CLOEXEC(fd) (void)fcntl((fd), F_SETFD, FD_CLOEXEC) // 标记 fd 为 exec 时关闭(防子进程继承)
#define ERRNO errno // 获取当前 errno 错误码
#define ERRORSTR(errcode) strerror(errcode) // 将错误码转换为字符串
#define THREAD_YIELD() sched_yield() // OS 级线程让出，用于自旋超限后的兜底退避
/* 自旋等待 CPU 暂停提示，降低功耗并减少流水线压力。
   CPU_PAUSE_CYCLES 是它大致值多少个周期，供按"要等多久"来用它的地方换算次数——
   各平台单价差三个数量级。没实测过的平台一律往贵了估：估贵了只是少等一点，
   估便宜了会等过头，在低竞争下真掉速 */
#if defined(ARCH_X86) || defined(ARCH_X64)
    #define CPU_PAUSE() __asm__ volatile("pause" ::: "memory") // x86/x64 平台：使用 pause 指令
    #define CPU_PAUSE_CYCLES 140 // Skylake 起约 140,更早的与 Zen 都更便宜,按上面的规则取最贵的
#elif defined(ARCH_ARM64)
    #define CPU_PAUSE() __asm__ volatile("yield" ::: "memory") // ARM64：使用 yield 指令
    #define CPU_PAUSE_CYCLES 1 // 近似 nop
#elif defined(ARCH_ARM)
    #if defined(__ARM_ARCH) && __ARM_ARCH >= 7
        #define CPU_PAUSE() __asm__ volatile("yield" ::: "memory") // ARMv7+：使用 yield 指令
        #define CPU_PAUSE_CYCLES 1 // 同 ARM64，近似 nop
    #else
        #define CPU_PAUSE() sched_yield() // ARMv4/5/6：让出 CPU
        #define CPU_PAUSE_CYCLES 4096 // 系统调用
    #endif
#elif defined(ARCH_PPC)
    #define CPU_PAUSE() __asm__ volatile("or 27,27,27" ::: "memory") // PPC 平台：低优先级提示
    #define CPU_PAUSE_CYCLES 4096 // 没实测过，按最贵算
#else
    #define CPU_PAUSE() sched_yield() // 其他平台：主动让出 CPU
    #define CPU_PAUSE_CYCLES 4096 // 系统调用
#endif
// 线程局部存储
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
    #define THREAD_LOCAL _Thread_local
#else
    #define THREAD_LOCAL __thread
#endif

#endif//OS_WIN
#endif//OS_UNIX_H_
