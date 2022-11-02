#ifndef MACRO_WIN_H_
#define MACRO_WIN_H_

#include "os.h"

#ifdef OS_WIN

#define PATH_SEPARATOR '\\'
#define PATH_SEPARATORSTR "\\"
#define PATH_LENS MAX_PATH
#define INVALID_SOCK INVALID_SOCKET
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

#endif
#endif//MACRO_WIN_H_
