#include "utils/log.h"
#include "utils/utils.h"

#define  LOG_FMT "[%s][%s]%s\n"
static FILE *_handle = NULL;
static int32_t _log_lv = LOGLV_DEBUG;
#ifdef OS_WIN
static HANDLE _console = NULL;
static CONSOLE_SCREEN_BUFFER_INFO _def_console;
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
