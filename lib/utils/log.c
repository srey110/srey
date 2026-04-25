#include "utils/log.h"
#include "utils/utils.h"

#define  LOG_FMT "[%s][%s]%s\n"  //日志格式：[时间][级别]消息
static FILE *_handle = NULL;        //日志输出文件句柄，NULL 表示输出到控制台
static int32_t _log_lv = LOGLV_DEBUG; //当前日志级别，低于此级别的日志将被忽略
#ifdef OS_WIN
static HANDLE _console = NULL;                    //Windows 控制台句柄
static CONSOLE_SCREEN_BUFFER_INFO _def_console;   //保存控制台默认属性（用于恢复颜色）
// Windows 下带控制台颜色输出的日志宏
#define PRINT_COLOR_LOG(color)\
    if (NULL != _console){\
        SetConsoleTextAttribute(_console, color);\
        fprintf(stdout, LOG_FMT, time, _lvstr(lv), out);\
        fflush(stdout);\
        SetConsoleTextAttribute(_console, _def_console.wAttributes);\
    } else {\
        fprintf(stdout, LOG_FMT, time, _lvstr(lv), out);\
    }
#endif

void log_init(FILE *file) {
    _handle = file;
#ifdef OS_WIN
    if (NULL == _handle) {
        _console = GetStdHandle(STD_OUTPUT_HANDLE);
        GetConsoleScreenBufferInfo(_console, &_def_console);
    }
#endif
}
void log_setlv(LOG_LEVEL lv) {
    _log_lv = lv;
}
// 将日志级别枚举值转换为对应的字符串名称
static const char *_lvstr(int32_t lv) {
    switch (lv) {
    case LOGLV_FATAL:
        return "fatal";
    case LOGLV_ERROR:
        return "error";
    case LOGLV_WARN:
        return "warning";
    case LOGLV_INFO:
        return "info";
    case LOGLV_DEBUG:
        return "debug";
    }
    return "";
}
// 执行实际的日志输出，根据级别选择颜色/文件句柄输出
static void _slog(int32_t lv, const char *fmt, va_list args) {
    char time[TIME_LENS] = { 0 };
    mstostr(nowms(), "%Y-%m-%d %H:%M:%S", time);
    char *out = _format_va(fmt, args);
    if (NULL == _handle) {
#ifdef OS_WIN
        switch (lv) {
        case LOGLV_FATAL:
        case LOGLV_ERROR:
            PRINT_COLOR_LOG(0xc);
            break;
        case LOGLV_WARN:
            PRINT_COLOR_LOG(0x6);
            break;
        default:
            fprintf(stdout, LOG_FMT, time, _lvstr(lv), out);
            break;
        }
#else
        switch (lv) {
        case LOGLV_FATAL:
        case LOGLV_ERROR:
            fprintf(stdout, "\033[0;31m"LOG_FMT"\033[0m", time, _lvstr(lv), out);
            break;
        case LOGLV_WARN:
            fprintf(stdout, "\033[0;33m"LOG_FMT"\033[0m", time, _lvstr(lv), out);
            break;
        default:
            fprintf(stdout, LOG_FMT, time, _lvstr(lv), out);
            break;
        }
#endif
    } else {
        fprintf(_handle, LOG_FMT, time, _lvstr(lv), out);
        if (LOGLV_FATAL == lv) {
            fflush(_handle);
        }
    }
    FREE(out);
}
void slog(int32_t lv, const char *fmt, ...) {
    if (lv > _log_lv) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    _slog(lv, fmt, args);
    va_end(args);
}
