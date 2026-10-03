#ifndef MACRO_LOG_H_
#define MACRO_LOG_H_

#include "base/os.h"
#include "base/err.h"
#include "base/macro_util.h"
#include "base/bytes.h"

// 日志与断言宏。__FILENAME__ 回落时要用 bytes.h 的 _filename，所以本文件排在 bytes.h 之上

#define LOG_PREFIX_FMT "[%s %s %d] " // 日志/PRINT 行首:文件 函数 行号
// 当前源文件名：编译器给得出 __FILE_NAME__(clang 9+ / gcc 12+)就编译期定死，否则运行时从 __FILE__ 里切。
// 要切的是任意路径(不是当前源文件)时直接调 _filename
#ifdef __FILE_NAME__
    #define __FILENAME__ __FILE_NAME__
#endif
#ifndef __FILENAME__
    #define __FILENAME__ _filename(__FILE__)
#endif
// LOG / PRINT 的行首，格式同 LOG_PREFIX_FMT。有 __FILE_NAME__ 时文件名与行号编译期拼进格式串
// (函数名在 C 里不是字面量，仍走 %s)；否则三项都运行期填
#ifdef __FILE_NAME__
    #define LOG_PREFIX(fmt) "[" __FILE_NAME__ " %s " TOSTR(__LINE__) "] " fmt
    #define LOG_PREFIX_ARGS __FUNCTION__
#else
    #define LOG_PREFIX(fmt) LOG_PREFIX_FMT fmt
    #define LOG_PREFIX_ARGS __FILENAME__, __FUNCTION__, __LINE__
#endif
#define PRINT(fmt, ...) printf(LOG_PREFIX(fmt) "\n", LOG_PREFIX_ARGS, ##__VA_ARGS__) // 带位置信息的标准输出

// 断言宏：条件不满足则打印并终止程序
#define ASSERTAB(exp, errstr)\
    do {\
        if (!(exp)) {\
            const char *_abstr = (errstr);\
            if (EMPTYSTR(_abstr)) {\
                _abstr = "assertion failed";\
            }\
            fprintf(stderr, "[ABORT][%s %s %d] %s\n", __FILENAME__, __FUNCTION__, __LINE__, _abstr);\
            fflush(stderr);\
            log_abort(__FILENAME__, __FUNCTION__, __LINE__, _abstr);\
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
#define LOG(lv, fmt, ...) slog(lv, LOG_PREFIX(fmt), LOG_PREFIX_ARGS, ##__VA_ARGS__) // 带文件/函数/行号的日志宏
#define LOG_FATAL(fmt, ...) LOG(LOGLV_FATAL, fmt, ##__VA_ARGS__) // 致命错误日志
#define LOG_ERROR(fmt, ...) LOG(LOGLV_ERROR, fmt, ##__VA_ARGS__) // 错误日志
#define LOG_WARN(fmt, ...)  LOG(LOGLV_WARN,  fmt, ##__VA_ARGS__) // 警告日志
#define LOG_INFO(fmt, ...)  LOG(LOGLV_INFO,  fmt, ##__VA_ARGS__) // 信息日志
#define LOG_DEBUG(fmt, ...) LOG(LOGLV_DEBUG, fmt, ##__VA_ARGS__) // 调试日志
/// <summary>
/// 输出一条日志，低于当前日志级别时直接忽略。实现在 utils/log.c，声明放这一层
/// 是为了让本文件的 LOG 宏不必反向依赖 utils/log.h。
/// 由此带来一条链接约束：本文件的 LOG_* 与 ASSERTAB 展开后都指向 utils/log.c，凡用到它们的
/// target 都必须链上 lib/utils，只链 base + containers 会在某处无关的断言上报未定义符号
/// </summary>
/// <param name="lv">日志级别，参见 log_level</param>
/// <param name="fmt">格式化字符串</param>
/// <param name="...">变参</param>
void slog(int32_t lv, const char *fmt, ...);
/// <summary>
/// ASSERTAB 专用：排空日志队列后把 abort 原因写进日志文件；无日志文件时不重复写原因行
/// （ASSERTAB 已打到 stderr），只把缓冲刷出去。声明放这一层的理由同 slog。
/// 尽力而为——不保证落盘，但每一步都有上限，不会把崩溃卡成挂起。可并发调用、可重复调用，
/// 各种情形下分别做什么见实现里的分支注释
/// </summary>
/// <param name="file">断言所在文件，由 ASSERTAB 传 __FILENAME__</param>
/// <param name="func">断言所在函数</param>
/// <param name="line">断言所在行</param>
/// <param name="msg">断言的错误描述，非空</param>
void log_abort(const char *file, const char *func, int32_t line, const char *msg);

#endif//MACRO_LOG_H_
