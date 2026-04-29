#include "utils/log.h"
#include "utils/utils.h"
#include "thread/cond.h"
#include "thread/thread.h"
#include "containers/mpmc.h"

#define LOG_FMT "[%s][%s]%s\n"

typedef struct {
    int32_t lv;
    char    time[TIME_LENS];
    char   *msg;
} log_item;

static FILE      *_handle  = NULL;
static int32_t    _log_lv  = LOGLV_DEBUG;
static atomic_t   _running = 0; /* atomic 保证跨平台内存可见 */
static atomic_t   _sleeping = 0;
static pthread_t  _th;
static mpmc_ctx   _mpmc;
static mutex_ctx  _mtx;
static cond_ctx   _cond;
#ifdef OS_WIN
static HANDLE _console = NULL;
static CONSOLE_SCREEN_BUFFER_INFO _def_console;
#endif

static const char *_lvstr(int32_t lv) {
    switch (lv) {
    case LOGLV_FATAL: return "fatal";
    case LOGLV_ERROR: return "error";
    case LOGLV_WARN:  return "warning";
    case LOGLV_INFO:  return "info";
    case LOGLV_DEBUG: return "debug";
    }
    return "";
}
static void _write_item(const log_item *item) {
    if (NULL == _handle) {
#ifdef OS_WIN
        switch (item->lv) {
        case LOGLV_FATAL:
        case LOGLV_ERROR:
            if (NULL != _console) {
                SetConsoleTextAttribute(_console, 0xc);
                fprintf(stdout, LOG_FMT, item->time, _lvstr(item->lv), item->msg);
                fflush(stdout);
                SetConsoleTextAttribute(_console, _def_console.wAttributes);
            } else {
                fprintf(stdout, LOG_FMT, item->time, _lvstr(item->lv), item->msg);
            }
            break;
        case LOGLV_WARN:
            if (NULL != _console) {
                SetConsoleTextAttribute(_console, 0x6);
                fprintf(stdout, LOG_FMT, item->time, _lvstr(item->lv), item->msg);
                fflush(stdout);
                SetConsoleTextAttribute(_console, _def_console.wAttributes);
            } else {
                fprintf(stdout, LOG_FMT, item->time, _lvstr(item->lv), item->msg);
            }
            break;
        default:
            fprintf(stdout, LOG_FMT, item->time, _lvstr(item->lv), item->msg);
            break;
        }
#else
        switch (item->lv) {
        case LOGLV_FATAL:
        case LOGLV_ERROR:
            fprintf(stdout, "\033[0;31m"LOG_FMT"\033[0m", item->time, _lvstr(item->lv), item->msg);
            break;
        case LOGLV_WARN:
            fprintf(stdout, "\033[0;33m"LOG_FMT"\033[0m", item->time, _lvstr(item->lv), item->msg);
            break;
        default:
            fprintf(stdout, LOG_FMT, item->time, _lvstr(item->lv), item->msg);
            break;
        }
#endif
    } else {
        fprintf(_handle, LOG_FMT, item->time, _lvstr(item->lv), item->msg);
        if (LOGLV_FATAL == item->lv) {
            fflush(_handle);
        }
    }
}
static void _drain(void) {
    log_item *item;
    while (NULL != (item = (log_item *)mpmc_pop(&_mpmc))) {
        _write_item(item);
        FREE(item->msg);
        FREE(item);
    }
}
static void _log_io_thread(void *arg) {
    (void)arg;
    int32_t should_exit;
    for (;;) {
        mutex_lock(&_mtx);
        while (0 == mpmc_size(&_mpmc) && ATOMIC_GET(&_running)) {
            ATOMIC_SET(&_sleeping, 1);
            cond_wait(&_cond, &_mtx);
            ATOMIC_SET(&_sleeping, 0);
        }
        should_exit = !ATOMIC_GET(&_running);
        mutex_unlock(&_mtx);
        _drain();
        /* mpmc_push 分两步：CAS 抢 enq_pos（size++）→ ATOMIC_SET 发布 sequence。
         * 两步之间 mpmc_size>0 但 mpmc_pop 返回 NULL。
         * 用 CPU_PAUSE 等发布完成，避免反复 mutex_lock/unlock 空转。 */
        while (mpmc_size(&_mpmc) > 0) {
            CPU_PAUSE();
            _drain();
        }
        if (should_exit) {
            break;
        }
    }
}
void log_init(FILE *file, uint32_t capacity) {
    _handle = file;
#ifdef OS_WIN
    if (NULL == _handle) {
        _console = GetStdHandle(STD_OUTPUT_HANDLE);
        GetConsoleScreenBufferInfo(_console, &_def_console);
    }
#endif
    mpmc_init(&_mpmc, 0 == capacity ? 4 * ONEK : capacity);
    mutex_init(&_mtx);
    cond_init(&_cond);
    ATOMIC_SET(&_running, 1); 
    _th = thread_creat(_log_io_thread, NULL);
}
void log_free(void) {
    mutex_lock(&_mtx);
    ATOMIC_SET(&_running, 0);
    cond_signal(&_cond);
    mutex_unlock(&_mtx);
    thread_join(_th);
    _drain();
    mpmc_free(&_mpmc);
    mutex_free(&_mtx);
    cond_free(&_cond);
}
void log_setlv(LOG_LEVEL lv) {
    _log_lv = lv;
}
void slog(int32_t lv, const char *fmt, ...) {
    if (lv > _log_lv || 0 == ATOMIC_GET(&_running)) {
        return;
    }
    log_item *item;
    MALLOC(item, sizeof(log_item));
    item->lv = lv;
    mstostr(nowms(), "%Y-%m-%d %H:%M:%S", item->time);
    va_list args;
    va_start(args, fmt);
    item->msg = _format_va(fmt, args);
    va_end(args);
    if (ERR_OK != mpmc_push(&_mpmc, item)) {
        /* 写 stderr：避免与 I/O 线程竞争 _handle，也避免写错目标 */
        fprintf(stderr, "mpmc queue full, drop log:"LOG_FMT,
                item->time, _lvstr(item->lv), item->msg);
        FREE(item->msg);
        FREE(item);
        return;
    }
    if (ATOMIC_GET(&_sleeping)) {
        mutex_lock(&_mtx);
        cond_signal(&_cond);
        mutex_unlock(&_mtx);
    }
}
