#include "log.h"
#include "utils.h"

#define  LOG_FMT "[%s][%s]%s\n"
static FILE *_handle = NULL;
static int32_t _log_lv = LOGLV_DEBUG;

void log_handle(FILE *file) {
    _handle = file;
}
void log_setlv(LOG_LEVEL lv) {
    _log_lv = lv;
}
static inline const char *_lvstr(int32_t lv) {
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
static inline void _slog(int32_t lv, const char *fmt, va_list args) {
    char time[TIME_LENS] = { 0 };
    mstostr(nowms(), "%Y-%m-%d %H:%M:%S", time);
    char out[4096];
    vsnprintf(out, sizeof(out) - 1, fmt, args);
    if (NULL == _handle) {
        if (lv <= LOGLV_ERROR) {
            fprintf(stderr, LOG_FMT, time, _lvstr(lv), out);
            fflush(stderr);
        } else {
            fprintf(stdout, LOG_FMT, time, _lvstr(lv), out);
            fflush(stdout);
        }
    } else {
        fprintf(_handle, LOG_FMT, time, _lvstr(lv), out);
        fflush(_handle);
    }
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
