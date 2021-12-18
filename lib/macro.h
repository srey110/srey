#ifndef MACRO_H_
#define MACRO_H_

#include "os.h"
#include "errcode.h"

#define MALLOC malloc
#define CALLOC calloc
#define REALLOC realloc
#define FREE free
#define ZERO(name, len) memset(name, 0, len)
#define SAFE_FREE(v_para)\
do\
{\
    if (NULL != v_para)\
    {\
        FREE(v_para);\
        v_para = NULL;\
    }\
}while(0)

#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
//s向上 取n(n 为2的倍数)的整数倍
#define ROUND_UP(s, n) (((s) + (n) - 1) & (~((n) - 1)))

#if defined(OS_WIN)
    #define PATH_SEPARATOR '\\'
    #define PATH_SEPARATORSTR "\\"
    #define PATH_LENS MAX_PATH
    #define INVALID_SOCK INVALID_SOCKET
#else
    #define PATH_SEPARATOR '/'
    #define PATH_SEPARATORSTR "/"
    #define SIGNAL_EXIT SIGRTMIN + 10
    #define SOCKET int
    #define INVALID_SOCK -1
    #if defined(PATH_MAX)
        #define PATH_LENS PATH_MAX
    #elif defined(MAXPATHLEN)
        #define PATH_LENS MAXPATHLEN
    #else
        #define PATH_LENS 256
    #endif
#endif
#define NAME_LENS 32

#define CONCAT2(a, b) a b
#define CONCAT3(a, b, c) a b c
#define __FILENAME__(file) (strrchr(file, PATH_SEPARATOR) ? strrchr(file, PATH_SEPARATOR) + 1 : file)
#define PRINTF(fmt, ...) printf(CONCAT3("[%s %s %d] ", fmt, "\n"),  __FILENAME__(__FILE__), __FUNCTION__, __LINE__, ##__VA_ARGS__)

#ifndef offsetof
#define offsetof(type, field) ((size_t)(&((type *)0)->field))
#endif
#define UPCAST(ptr, type, field) ((type *)(((char*)(ptr)) - offsetof(type, field)))

//typeid(name).name() 变量类型
//变量名称字符串
#define VARNAME(name) #name

//动态变量名
#define __ANONYMOUS(type, name, line)  type  name##line
#define _ANONYMOUS(type, line)  __ANONYMOUS(type, _anonymous, line)
#define ANONYMOUS(type)  _ANONYMOUS(type, __LINE__) 

#define ASSERTAB(exp, errstr)\
do\
{\
    if (!(exp))\
    {\
        PRINTF("%s", errstr);\
        abort();\
    }\
} while (0);

#define ONEK 1024
#define TIME_LENS 64
#define IP_LENS   64
#define PORT_LENS 8
#define SOCKKPA_DELAY 60
#define SOCKKPA_INTVL 1
#define SOCKK_BACKLOG 128

#define MSEC    1000 //毫秒
#define NANOSEC 1000000000//纳秒

#if defined(OS_WIN)
    #define IS_EAGAIN(e) (WSAEWOULDBLOCK == (e) || EAGAIN == (e))
    #define STRCMP _stricmp
    #define STRNCMP _strnicmp
    #define STRTOK strtok_s
    #define SNPRINTF _snprintf
    #define SWPRINTF swprintf
    #define STRNCPY strncpy_s
    #define ITOA _itoa
    #define STAT _stat
    #define USLEEP(us)\
    do\
    {\
        LARGE_INTEGER stft;\
        stft.QuadPart = -(10 * (__int64)us);\
        HANDLE htimer = CreateWaitableTimer(NULL, TRUE, NULL);\
        SetWaitableTimer(htimer, &stft, 0, NULL, NULL, 0);\
        (void)WaitForSingleObject(htimer, INFINITE);\
        CloseHandle(htimer);\
    } while (0)
    #define MSLEEP(ms) Sleep(ms)
    #define TIMEB _timeb
    #define FTIME _ftime
    #define ACCESS _access
    #define MKDIR _mkdir    
    #define SHUT_RD   SD_RECEIVE
    #define SHUT_WR   SD_SEND
    #define SHUT_RDWR SD_BOTH
    #define SOCK_CLOSE closesocket    
    #define ERRNO GetLastError()
    static inline const char *_fmterror(DWORD error)
    {
        char *perror = NULL;
        if (0 == FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL,
            error,
            MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
            (LPTSTR)&perror,
            0,
            NULL))
        {
            return "FormatMessageA error.";
        }

        char *ppos = strrchr(perror, '\r');
        if (NULL != ppos)
        {
            ppos[0] = '\0';
        }
        static char errstr[512];
        size_t ilens = strlen(perror);
        ilens = ilens >= sizeof(errstr) ? sizeof(errstr) - 1 : ilens;
        memcpy(errstr, perror, ilens);        
        errstr[ilens] = '\0';
        LocalFree(perror);

        return errstr;
    };
    #define ERRORSTR(errcode) _fmterror(errcode)
#else
    #if EAGAIN == EWOULDBLOCK
        #define IS_EAGAIN(e) (EAGAIN == (e))
    #else
        #define IS_EAGAIN(e) (EAGAIN == (e) || EWOULDBLOCK == (e))
    #endif
    #define ERR_RW_RETRIABLE(e)	((e) == EINTR || IS_EAGAIN(e))
    #define ERR_CONNECT_RETRIABLE(e) ((e) == EINTR || (e) == EINPROGRESS)
    #define ERR_ACCEPT_RETRIABLE(e)	((e) == EINTR || IS_EAGAIN(e) || (e) == ECONNABORTED)
    #define ERR_CONNECT_REFUSED(e) ((e) == ECONNREFUSED)
    #define STRCMP strcasecmp
    #define STRNCMP strncasecmp
    #define STRTOK strtok_r
    #define SNPRINTF snprintf
    #define SWPRINTF swprintf
    #define STRNCPY strncpy
    #define ITOA itoa
    #define STAT stat
    #define USLEEP(us) usleep(us)
    #define MSLEEP(ms) usleep(ms * 1000)
    #define TIMEB timeb
    #define FTIME ftime
    #define ACCESS access
    #define MKDIR(path) mkdir(path, S_IRUSR|S_IWUSR)
    #define SOCK_CLOSE close
    #define ERRNO errno
    #define ERRORSTR(errcode) strerror(errcode)
#endif

#if defined(OS_WIN)
    typedef uint32_t atomic_t;
    typedef uint64_t atomic64_t;
    //LONG InterlockedExchangeAdd(LONG volatile *Addend,LONG Value)  返回旧值
    #define ATOMIC_ADD(ptr, val) InterlockedExchangeAdd(ptr, val)
    //LONG InterlockedExchange(LONG volatile *Target,LONG Value); 返回旧值
    #define ATOMIC_SET(ptr, val) InterlockedExchange(ptr, val)
    //比较*ptr与oldval的值，如果两者相等，则将newval更新到*ptr并返回操作之前*ptr的值 成功 返回值等于oldval
    //LONG InterlockedCompareExchange(LONG volatile *Destination, LONG ExChange, LONG Comperand);
    #define ATOMIC_CAS(ptr, oldval, newval) (InterlockedCompareExchange(ptr, newval, oldval) == oldval)
    #define ATOMIC64_ADD(ptr, val) InterlockedExchangeAdd64(ptr, val)
    #define ATOMIC64_SET(ptr, val) InterlockedExchange64(ptr, val)
    #define ATOMIC64_CAS(ptr, oldval, newval) (InterlockedCompareExchange64(ptr, newval, oldval) == oldval)
    #define ATOMICPTR_CAS(ptr, oldval, newval) (InterlockedCompareExchangePointer(ptr, newval, oldval) == oldval)
#elif defined(OS_SUN)
    typedef uint32_t atomic_t;
    typedef uint64_t atomic64_t;
    //uint32_t atomic_add_32_nv(volatile uint32_t *target, int32_t delta); return the new value of target.
    #define ATOMIC_ADD(ptr, val) atomic_add_32_nv(ptr, val)
    //uint32_t atomic_swap_32(volatile uint32_t *target, uint32_t newval);  return the old of *target.
    #define ATOMIC_SET(ptr, val) atomic_swap_32(ptr, val)
    //uint32_t atomic_cas_32(volatile uint32_t *target, uint32_t cmp, uint32_t newval);
    #define ATOMIC_CAS(ptr, oldval, newval) (atomic_cas_32(ptr, oldval, newval) == oldval)
    #define ATOMIC64_ADD(ptr, val) atomic_add_64_nv(ptr, val)
    #define ATOMIC64_SET(ptr, val) atomic_swap_64(ptr, val)
    #define ATOMIC64_CAS(ptr, oldval, newval) (atomic_cas_64(ptr, oldval, newval) == oldval)
    #define ATOMICPTR_CAS(ptr, oldval, newval) (atomic_cas_ptr(ptr, oldval, newval) == oldval)
#else
    typedef uint32_t atomic_t;
    typedef uint64_t atomic64_t;
    static inline atomic_t _fetchandset(volatile atomic_t *ptr, atomic_t value)
    {
        atomic_t oldvar;
        do
        {
            oldvar = *ptr;
        } while (!__sync_bool_compare_and_swap(ptr, oldvar, value));
        return oldvar;
    };
    static inline atomic64_t _fetchandset64(volatile atomic64_t *ptr, atomic64_t value)
    {
        atomic64_t oldvar;
        do
        {
            oldvar = *ptr;
        } while (!__sync_bool_compare_and_swap(ptr, oldvar, value));
        return oldvar;
    };
    //type __sync_fetch_and_add (type *ptr, type value, ...)//返回旧值
    #define ATOMIC_ADD(ptr, val) __sync_fetch_and_add(ptr, val)
    #define ATOMIC_SET(ptr, val) _fetchandset(ptr, val)
    //bool __sync_bool_compare_and_swap (type *ptr, type oldval type newval); 
    #define ATOMIC_CAS(ptr, oldval, newval) __sync_bool_compare_and_swap(ptr, oldval, newval)
    #define ATOMIC64_ADD(ptr, val) __sync_fetch_and_add(ptr, val)
    #define ATOMIC64_SET(ptr, val) _fetchandset64(ptr, val) 
    #define ATOMIC64_CAS(ptr, oldval, newval) __sync_bool_compare_and_swap(ptr, oldval, newval)
    #define ATOMICPTR_CAS(ptr, oldval, newval) __sync_bool_compare_and_swap(ptr, oldval, newval)
#endif
#define ATOMIC_GET(ptr) ATOMIC_ADD(ptr, 0)
#define ATOMIC64_GET(ptr) ATOMIC64_ADD(ptr, 0)

#define SAFE_CLOSE_SOCK(fd)\
do\
{\
    if (INVALID_SOCK != (fd))\
    {\
        SOCK_CLOSE(fd);\
        (fd) = INVALID_SOCK;\
    }\
}while(0)

#endif//MACRO_H_
