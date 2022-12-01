#ifndef MACRO_H_
#define MACRO_H_

#include "config.h"
#include "errcode.h"
#include "macro_unix.h"
#include "macro_win.h"
#include "memory.h"

#define ONEK                 1024
#define TIME_LENS            64
#define IP_LENS              64
#define PORT_LENS            8
#define SOCKKPA_DELAY        10
#define SOCKKPA_INTVL        1
#define INVALID_FD           -1
#define TIMER_ACCURACY       (1000 * 1000)

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define EMPTYSTR(str) ((NULL == str) || (0 == strlen(str)))

//s向上 取n(n 为2的倍数)的整数倍
#define ROUND_UP(s, n) (((s) + (n) - 1) & (~((n) - 1)))

#define CONCAT2(a, b) a b
#define CONCAT3(a, b, c) a b c
#define __FILENAME__(file) (strrchr(file, PATH_SEPARATOR) ? strrchr(file, PATH_SEPARATOR) + 1 : file)
#define PRINT(fmt, ...) printf(CONCAT3("[%s %s %d] ", fmt, "\n"),  __FILENAME__(__FILE__), __FUNCTION__, __LINE__, ##__VA_ARGS__)

#if PRINT_DEBUG
#define PRINTD(fmt, ...) PRINT(fmt, ##__VA_ARGS__)
#else
#define PRINTD(fmt, ...)
#endif

#if MEMORY_CHECK
#define MEMCHECK()  atexit(_memcheck)
#else
#define MEMCHECK()
#endif

#ifndef offsetof
#define offsetof(type, field) ((size_t)(&((type *)0)->field))
#endif
#define UPCAST(ptr, type, field) ((type *)(((char*)(ptr)) - offsetof(type, field)))

//动态变量名
#define __ANONYMOUS(type, name, line)  type  name##line
#define _ANONYMOUS(type, line)  __ANONYMOUS(type, _anonymous, line)
#define ANONYMOUS(type)  _ANONYMOUS(type, __LINE__) 

#define ATOMIC_GET(ptr) ATOMIC_ADD(ptr, 0)
#define ATOMIC64_GET(ptr) ATOMIC64_ADD(ptr, 0)

#define ZERO(name, len) memset(name, 0, len)
#define MALLOC(ptr, size)\
do\
{\
    *(void**)&(ptr) = _malloc(size);\
    PRINTD("malloc(%p, size=%zu)", ptr, size);\
}while (0)

#define CALLOC(ptr, count, size)\
do\
{\
    *(void**)&(ptr) = _calloc(count, size);\
    PRINTD("calloc(%p, count=%zu, size=%zu)", ptr, count, size);\
}while (0)

#define REALLOC(ptr, oldptr, size)\
do\
{\
    *(void**)&(ptr) = _realloc(oldptr, size);\
    PRINTD("realloc(%p, old=%p, size=%zu)", ptr, oldptr, size);\
}while (0)

#define FREE(ptr)\
do\
{\
    if (NULL != ptr)\
    {\
        _free(ptr); \
        PRINTD("free(%p)", ptr);\
        ptr = NULL; \
    }\
}while (0)

#define ASSERTAB(exp, errstr)\
do\
{\
    if (!(exp))\
    {\
        PRINT("%s", errstr);\
        abort();\
    }\
} while (0);

#define CLOSE_SOCK(fd)\
do\
{\
    if (INVALID_SOCK != (fd) && 0 != (fd))\
    {\
        SOCK_CLOSE(fd);\
        (fd) = INVALID_SOCK;\
    }\
}while(0)

#endif//MACRO_H_
