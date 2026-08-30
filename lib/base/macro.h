#ifndef MACRO_H_
#define MACRO_H_

#include "base/base.h"
#include "base/config.h"
#include "base/macro_atomic.h"
#include "base/macro_unix.h"
#include "base/macro_win.h"
#include "base/memory.h"

#define ONEK                 1024 // 1K 字节
#define TIME_LENS            128 // 时间字符串缓冲区长度
#define HOST_LENS            256 // 主机名缓冲区长度
#define IP_LENS              64 // IP 地址字符串缓冲区长度
#define UUID_LENS            16 // UUID 字节长度
#define INVALID_FD           -1 // 无效文件描述符

#define FLAG_CRLF           "\r\n" // HTTP/文本协议行结束符
#define CRLF_SIZE           2 // CRLF 字节数

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(*(a))) // 获取静态数组元素个数
#define EMPTYSTR(str) ((NULL == (str)) || ('\0' == *(const char *)(str))) // 判断字符串是否为空
#define EMPTYPTR(ptr, lens) ((NULL == (ptr)) || (0 == (lens)))
// s 向上取整。掩码 ~(n-1) 只在 n 是 2 的幂时才等价于"取 n 的整数倍",
// n 取别的值（比如 6）结果不会是 n 的倍数；但任何 n 下结果都 >= s（被清掉的低位至多 n-1）
#define ROUND_UP(s, n) (((s) + (n) - 1) & (~((n) - 1)))
#define LOG_PREFIX_FMT "[%s %s %d] " // 日志/PRINT 行首:文件 函数 行号
#define CONCAT2(a, b) a b // 拼接两个字符串字面量
#define CONCAT3(a, b, c) a b c // 拼接三个字符串字面量
#define __FILENAME__(file) _filename(file)
#define PRINT(fmt, ...) printf(CONCAT3(LOG_PREFIX_FMT, fmt, "\n"),  __FILENAME__(__FILE__), __FUNCTION__, __LINE__, ##__VA_ARGS__) // 带位置信息的标准输出

#ifndef offsetof
    #define offsetof(type, field) ((size_t)(&((type *)0)->field)) // 获取结构体字段偏移量
#endif
#define UPCAST(ptr, type, field) ((type *)(((char*)(ptr)) - offsetof(type, field))) // 通过成员指针还原外层结构体指针

#define BIT_SET(status, flag)    ((status) |= (flag)) // 设置位标志
#define BIT_CHECK(status, flag)  ((status) & (flag)) // 检查位标志是否已设置
#define BIT_REMOVE(status, flag) ((status) &= ~(flag)) // 清除位标志
#define BIT_GETN(x, n)           (((x) >> (n)) & 1u) // 获取第 n 位的值
// 将 x 的第 n 位设为 val 的最低位；x、n 被多次求值，须传入无副作用表达式（ __typeof__ 不兼容 MSVC）
#define BIT_SETN(x, n, val) ((x) = (((x) & ~((uint64_t)1 << (n))) | (((uint64_t)(val) & 1) << (n))))

#define ZERO(name, len) memset(name, 0, len) // 将内存区域清零
#define MALLOC(ptr, size) *(void**)&(ptr) = _malloc(size) // 分配内存并赋值给指针
#define CALLOC(ptr, count, size) *(void**)&(ptr) = _calloc(count, size) // 分配并清零内存
#define REALLOC(ptr, oldptr, size) *(void**)&(ptr) = _realloc(oldptr, size) // 重新分配内存

// 释放内存并将指针置为 NULL，避免悬空指针
#define FREE(ptr)\
    do {\
        if (NULL != ptr) {\
            _free(ptr); \
            ptr = NULL; \
        }\
    } while(0)
#define SECURE_FREE(ptr, lens)\
    do {\
        secure_zero(ptr, lens);\
        FREE(ptr);\
    } while(0)
// copy=0（本层已接管 data 所有权）时释放 data；copy!=0（复制语义）不释放。ev_send/task_* 接管失败或无接管的兜底
#define CHECK_COPY_FREE(data, copy)\
    do {\
        if (!(copy)) {\
            FREE(data);\
        }\
    } while(0)
// 关闭 socket 并将句柄置为无效值
#define CLOSE_SOCK(fd)\
    do {\
        if (INVALID_SOCK != fd) {\
            SOCK_CLOSE(fd);\
            fd = INVALID_SOCK;\
        }\
    } while(0)
// 安全地对指针解引用赋值（ptr 为 NULL 时不操作）
#define SET_PTR(ptr, val)\
    do {\
        if (NULL != (ptr)) {\
            (*(ptr)) = (val);\
        }\
    } while(0)
// 断言宏：条件不满足则打印并终止程序
#define ASSERTAB(exp, errstr)\
    do {\
        if (!(exp)) {\
            const char *_abstr = (errstr);\
            if (!EMPTYSTR(_abstr)) {\
                fprintf(stderr, "[ABORT][%s %s %d] %s\n", __FILENAME__(__FILE__), __FUNCTION__, __LINE__, _abstr);\
                fflush(stderr);\
            }\
            abort();\
        }\
    } while(0)
// 断言"返回错误码而非设 errno"的调用：pthread 系与 WSAStartup 都属此类，
// 失败信息须取自返回值，取 ERRNO 会打印无关的 strerror(errno)
#define ASSERTAB_CODE(exp)\
    do {\
        int32_t _acode = (exp);\
        ASSERTAB(ERR_OK == _acode, ERRORSTR(_acode));\
    } while(0)

//日志级别
typedef enum log_level {
    LOGLV_FATAL = 0, // 致命错误，程序无法继续
    LOGLV_ERROR,     // 错误
    LOGLV_WARN,      // 警告
    LOGLV_INFO,      // 信息
    LOGLV_DEBUG,     // 调试
}log_level;
#define LOG(lv, fmt, ...) slog(lv, CONCAT2(LOG_PREFIX_FMT, fmt), __FILENAME__(__FILE__), __FUNCTION__, __LINE__, ##__VA_ARGS__) // 带文件/函数/行号的日志宏
#define LOG_FATAL(fmt, ...) LOG(LOGLV_FATAL, fmt, ##__VA_ARGS__) // 致命错误日志
#define LOG_ERROR(fmt, ...) LOG(LOGLV_ERROR, fmt, ##__VA_ARGS__) // 错误日志
#define LOG_WARN(fmt, ...)  LOG(LOGLV_WARN,  fmt, ##__VA_ARGS__) // 警告日志
#define LOG_INFO(fmt, ...)  LOG(LOGLV_INFO,  fmt, ##__VA_ARGS__) // 信息日志
#define LOG_DEBUG(fmt, ...) LOG(LOGLV_DEBUG, fmt, ##__VA_ARGS__) // 调试日志

#endif//MACRO_H_
