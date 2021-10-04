#ifndef OS_H_
#define OS_H_

/*check system*/
#if defined (_WIN32)
#define OS_WIN
#elif defined (__linux__)
#define OS_LINUX
#elif defined (_AIX)
#define OS_AIX
#elif defined(__sun) || defined(sun)
#define OS_SUN
##elif defined(__DragonFly__)      || \
       defined(__FreeBSD__)        || \
       defined(__FreeBSD_kernel__) || \
       defined(__OpenBSD__)        || \
       defined(__NetBSD__)
#define OS_BSD
#elif defined _hpux
#define OS_HPUX
#else
#pragma error "unknown os system!"
#endif

/*check version x64 x86*/
#ifdef OS_WIN
#ifdef _WIN64
#define OS_X64
#else
#define OS_X86
#endif
#else
#ifdef __GNUC__
#if __x86_64__ || __ppc64__ || __x86_64 || __amd64__  || __amd64
#define OS_X64
#else
#define OS_X86
#endif
#else
#pragma error "unknown compile!"
#endif
#endif

#endif//OS_H_
