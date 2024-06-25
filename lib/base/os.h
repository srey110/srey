#ifndef OS_H_
#define OS_H_

/*check system https://sourceforge.net/p/predef/wiki/OperatingSystems/ */
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
    #define READV_EINVAL//readv 有些系统没有数据会返回EINVAL(22)错误
#else
    #error "Unsupported operating system platform!"
#endif

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

/*check version x64 x86*/
#if defined(OS_WIN)
    #if defined(_WIN64)
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
    #elif defined(__arm__)
        #define ARCH_ARM
        #define ARCH_NAME "ARM"
    #elif defined(__aarch64__) || defined(__ARM64__)
        #define ARCH_ARM64
        #define ARCH_NAME "ARM64"
    #elif defined(__powerpc) || defined(__powerpc__)|| defined(__PPC)|| defined(__PPC__)
        #define ARCH_PPC
        #define ARCH_NAME "PPC"
    #endif
#endif

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
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
#else
    #include <unistd.h>
    #include <signal.h>    
    #include <errno.h>
    #include <dirent.h>
    #include <libgen.h>
    #include <dlfcn.h>
    #include <locale.h>
    #include <netdb.h>
    #include <semaphore.h>
    #include <pthread.h>
    #include <sys/socket.h>    
    #include <sys/socketvar.h>
    #include <sys/mman.h>
    #include <sys/time.h>
    #include <sys/wait.h>
    #include <sys/ioctl.h>
    #include <sys/poll.h>
#ifndef OS_AIX
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
