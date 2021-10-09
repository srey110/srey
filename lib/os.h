#ifndef OS_H_
#define OS_H_

/*check system*/
#if defined(_WIN32) || defined(_WIN64)
    #define OS_WIN
#elif defined(linux) || defined(__linux) || defined(__linux__)
    #define OS_LINUX
#elif defined(__APPLE__) && (defined(__GNUC__) || defined(__xlC__) || defined(__xlc__))
    #include <TargetConditionals.h>
    #if defined(TARGET_OS_MAC) && TARGET_OS_MAC
        #define OS_MAC
    #elif defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
        #define OS_IOS
    #endif
    #define OS_DARWIN
#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || defined(__NetBSD__) || defined(__OpenBSD__)
    #define OS_BSD
#elif defined(sun) || defined(__sun) || defined(__sun__)
    #define OS_SOLARIS
#elif defined _hpux
    #define OS_HPUX
#elif defined _AIX
    #define OS_AIX
#else
    #error "Unsupported operating system platform!"
#endif
/*check version x64 x86*/
#ifdef OS_WIN
    #ifdef _WIN64
        #define ARCH_X64
    #else
        #define ARCH_X86
    #endif
#else
    #if defined(__i386) || defined(__i386__) || defined(_M_IX86)
        #define ARCH_X86
    #elif defined(__x86_64) || defined(__x86_64__) || defined(__amd64) || defined(_M_X64)
        #define ARCH_X64
    #elif defined(__arm__)
        #define ARCH_ARM
    #elif defined(__aarch64__) || defined(__ARM64__)
        #define ARCH_ARM64
    #else
        #define ARCH_UNKNOWN
        #warning "Unknown hardware architecture!"
    #endif
#endif

#include <string>
#include <list>
#include <vector>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>
#include <time.h>
#include <fcntl.h>
#include <wchar.h>
#include <math.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef OS_WIN
    #include <winsock2.h>
    #include <ws2ipdef.h>
    #include <ws2tcpip.h>
    #include <TlHelp32.h>
    #include <io.h>
    #include <tchar.h>
    #include <direct.h>
    #include <process.h>
    #include <ObjBase.h>
    #include <Windows.h>
    #include <MSTcpIP.h>
#else
    #include <unistd.h>
    #include <signal.h>
    #include <netinet/in.h>
    #include <errno.h>
    #include <dirent.h>
    #include <libgen.h>    
    #include <dlfcn.h>
    #include <locale.h>
    #include <syslog.h>
    #include <sys/statvfs.h>
    #include <sys/msg.h>
    #include <sys/sem.h>
    #include <sys/ipc.h>
    #include <sys/errno.h>
    #include <netinet/tcp.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <semaphore.h>
    #include <sys/socketvar.h>
    #include <sys/resource.h>
    #include <sys/mman.h>
    #include <sys/time.h>
    #include <sys/wait.h>
    #include <sys/ioctl.h>
    #include <net/if.h>
    #include <sys/syscall.h>
    #include <net/if_arp.h>
    #ifdef OS_AIX
        #include <sys/systemcfg.h>
    #endif
    #ifdef OS_SOLARIS
        #include <atomic.h>
        #include <sys/filio.h>
    #endif
#endif // OS_WIN

#endif//OS_H_
