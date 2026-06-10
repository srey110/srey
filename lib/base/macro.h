#ifndef MACRO_H_
#define MACRO_H_

#include "base/config.h"
#include "base/err.h"
#include "base/macro_unix.h"
#include "base/macro_win.h"
#include "base/memory.h"

#define ONEK                 1024    // 1K 字节
#define TIME_LENS            128     // 时间字符串缓冲区长度
#define HOST_LENS            256     // 主机名缓冲区长度
#define IP_LENS              64      // IP 地址字符串缓冲区长度
#define PORT_LENS            8       // 端口号字符串缓冲区长度
#define UUID_LENS            16      // UUID 字节长度
#define INVALID_FD           -1      // 无效文件描述符
#define FLOAT_PRECISION      1e-6    // 浮点数比较精度

#define FLAG_CRLF           "\r\n"  // HTTP/文本协议行结束符
#define CRLF_SIZE           2       // CRLF 字节数

#define ABS(n) ((n) > 0 ? (n) : -(n)) // 取绝对值
#define FLOAT_EQZERO(f) (ABS(f) < FLOAT_PRECISION) // 判断浮点数是否等于零
#define ARRAY_SIZE(a) (sizeof(a) / sizeof(*(a))) // 获取静态数组元素个数
#define EMPTYSTR(str) ((NULL == (str)) || ('\0' == *(const char *)(str))) // 判断字符串是否为空
#define EMPTYPTR(ptr, lens) ((NULL == (ptr)) || ((lens) <= 0))
#define ROUND_UP(s, n) (((s) + (n) - 1) & (~((n) - 1))) //s向上 取n(n 为2的倍数)的整数倍

#define CONCAT2(a, b) a b // 拼接两个字符串字面量
#define CONCAT3(a, b, c) a b c // 拼接三个字符串字面量
#define __FILENAME__(file) (strrchr(file, PATH_SEPARATOR) ? strrchr(file, PATH_SEPARATOR) + 1 : file) // 从路径中提取文件名
#define PRINT(fmt, ...) printf(CONCAT3("[%s %s %d] ", fmt, "\n"),  __FILENAME__(__FILE__), __FUNCTION__, __LINE__, ##__VA_ARGS__) // 带位置信息的标准输出

#ifndef offsetof
    #define offsetof(type, field) ((size_t)(&((type *)0)->field)) // 获取结构体字段偏移量
#endif
#define UPCAST(ptr, type, field) ((type *)(((char*)(ptr)) - offsetof(type, field))) // 通过成员指针还原外层结构体指针

//动态变量名
#define __ANONYMOUS(type, name, line)  type  name##line
#define _ANONYMOUS(type, line)  __ANONYMOUS(type, _anonymous, line)
#define ANONYMOUS(type)  _ANONYMOUS(type, __LINE__)

#define BIT_SET(status, flag)    ((status) |= (flag))   // 设置位标志
#define BIT_CHECK(status, flag)  ((status) & (flag))    // 检查位标志是否已设置
#define BIT_REMOVE(status, flag) ((status) &= ~(flag))  // 清除位标志
#define BIT_GETN(x, n)           (((x) >> (n)) & 1u)   // 获取第 n 位的值
// 将 x 的第 n 位设为 val 的最低位；x、n 被多次求值，须传入无副作用表达式（ __typeof__ 不兼容 MSVC）
#define BIT_SETN(x, n, val) ((x) = (((x) & ~((uint64_t)1 << (n))) | (((uint64_t)(val) & 1) << (n))))

#define ZERO(name, len) memset(name, 0, len)                               // 将内存区域清零
#define MALLOC(ptr, size) *(void**)&(ptr) = _malloc(size)                  // 分配内存并赋值给指针
#define CALLOC(ptr, count, size) *(void**)&(ptr) = _calloc(count, size)    // 分配并清零内存
#define REALLOC(ptr, oldptr, size) *(void**)&(ptr) = _realloc(oldptr, size) // 重新分配内存

// 原子读取
#if defined(__GNUC__) || defined(__clang__)
    // x86/x64 零开销，ARM/ARM64 上 LDAR/LDAPR 替代 DMB ISH
    #define ATOMIC_GET(ptr)   __atomic_load_n((ptr), __ATOMIC_ACQUIRE)
    #define ATOMIC64_GET(ptr) __atomic_load_n((ptr), __ATOMIC_ACQUIRE)
#elif defined(OS_WIN) && defined(ARCH_ARM64)
    // 纯 MSVC + Windows ARM64：__ldar32/64 直接发射 LDAR 指令（load-acquire），单指令完成
    #define ATOMIC_GET(ptr)   ((atomic_t)__ldar32((const volatile __int32 *)(ptr)))
    #define ATOMIC64_GET(ptr) ((atomic64_t)__ldar64((const volatile __int64 *)(ptr)))
#elif defined(OS_WIN) && (defined(ARCH_ARM) || defined(ARCH_X86))
    // 32 位平台：ARM 无 acquire load 指令，x86 的 64 位 volatile load
    // 会编译为两条 mov（撕裂读）。统一退到 RMW 兜底确保原子性
    #define ATOMIC_GET(ptr)   ATOMIC_ADD(ptr, 0)
    #define ATOMIC64_GET(ptr) ATOMIC64_ADD(ptr, 0)
#elif defined(OS_WIN)
    // 纯 MSVC + Windows x64/ARM64：默认 /volatile:ms 下 volatile load 隐含 acquire 语义；
    // 64 位对齐 load 天然原子
    #define ATOMIC_GET(ptr)   ((atomic_t)*(const volatile atomic_t *)(ptr))
    #define ATOMIC64_GET(ptr) ((atomic64_t)*(const volatile atomic64_t *)(ptr))
#else
    // AIX/HPUX/Sun 等无 __atomic_* 内建且非 MSVC：保留 RMW 兜底（过强但正确）
    #define ATOMIC_GET(ptr)   ATOMIC_ADD(ptr, 0)
    #define ATOMIC64_GET(ptr) ATOMIC64_ADD(ptr, 0)
#endif

// 释放内存并将指针置为 NULL，避免悬空指针
#define FREE(ptr)\
    do {\
        if (NULL != ptr) {\
            _free(ptr); \
            ptr = NULL; \
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
            (*ptr) = (val);\
        }\
    } while(0)
// 断言宏：条件不满足则打印并终止程序
#define ASSERTAB(exp, errstr)\
    do {\
        if (!(exp)) {\
            if (!EMPTYSTR(errstr)) {\
                fprintf(stderr, "[ABORT][%s %s %d] %s\n", __FILENAME__(__FILE__), __FUNCTION__, __LINE__, errstr);\
                fflush(stderr);\
            }\
            abort();\
        }\
    } while(0)

//日志级别
typedef enum LOG_LEVEL {
    LOGLV_FATAL = 0, // 致命错误，程序无法继续
    LOGLV_ERROR,     // 错误
    LOGLV_WARN,      // 警告
    LOGLV_INFO,      // 信息
    LOGLV_DEBUG,     // 调试
}LOG_LEVEL;
/// <summary>
/// 底层日志输出函数
/// </summary>
/// <param name="lv">日志级别，参见 LOG_LEVEL</param>
/// <param name="fmt">格式化字符串</param>
void slog(int32_t lv, const char *fmt, ...);
#define LOG(lv, fmt, ...) slog(lv, CONCAT2("[%s %s %d] ", fmt), __FILENAME__(__FILE__), __FUNCTION__, __LINE__, ##__VA_ARGS__) // 带文件/函数/行号的日志宏
#define LOG_FATAL(fmt, ...) LOG(LOGLV_FATAL, fmt, ##__VA_ARGS__) // 致命错误日志
#define LOG_ERROR(fmt, ...) LOG(LOGLV_ERROR, fmt, ##__VA_ARGS__) // 错误日志
#define LOG_WARN(fmt, ...)  LOG(LOGLV_WARN,  fmt, ##__VA_ARGS__) // 警告日志
#define LOG_INFO(fmt, ...)  LOG(LOGLV_INFO,  fmt, ##__VA_ARGS__) // 信息日志
#define LOG_DEBUG(fmt, ...) LOG(LOGLV_DEBUG, fmt, ##__VA_ARGS__) // 调试日志

#endif//MACRO_H_
