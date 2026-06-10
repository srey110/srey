#include "utils/log.h"
#include "utils/utils.h"
#include "thread/cond.h"
#include "thread/thread.h"
#include "containers/fsqu.h"

#define LOG_FMT "[%s][%s]%s\n"
#define LOG_INLINE_SIZE 256
#define LOG_POP_BATCH   128

typedef struct {
    int32_t lv;
    char   *msg;                       // 指向 inline_buf 或独立 heap 分配
    char    time[TIME_LENS];
    char    inline_buf[LOG_INLINE_SIZE]; // 短消息内嵌，避免 _format_va 第二次 malloc
} log_item;

static FILE      *_handle  = NULL;
static atomic_t   _log_lv  = LOGLV_DEBUG;
static atomic_t   _running = 0; /* atomic 保证跨平台内存可见 */
static atomic_t   _sleeping = 0;
static pthread_t  _th;
static fsqu_ctx  _que;
static mutex_ctx  _mtx;
static cond_ctx   _cond;
#ifdef OS_WIN
static HANDLE _console = NULL;
static CONSOLE_SCREEN_BUFFER_INFO _def_console;
#endif

static const char *_log_lvstr(int32_t lv) {
    switch (lv) {
    case LOGLV_FATAL: return "fatal";
    case LOGLV_ERROR: return "error";
    case LOGLV_WARN:  return "warning";
    case LOGLV_INFO:  return "info";
    case LOGLV_DEBUG: return "debug";
    }
    return "";
}
static void _log_write_item(const log_item *item) {
    if (NULL == _handle) {
#ifdef OS_WIN
        switch (item->lv) {
        case LOGLV_FATAL:
        case LOGLV_ERROR:
            if (NULL != _console) {
                SetConsoleTextAttribute(_console, 0xc);
                fprintf(stdout, LOG_FMT, item->time, _log_lvstr(item->lv), item->msg);
                fflush(stdout);
                SetConsoleTextAttribute(_console, _def_console.wAttributes);
            } else {
                fprintf(stdout, LOG_FMT, item->time, _log_lvstr(item->lv), item->msg);
            }
            break;
        case LOGLV_WARN:
            if (NULL != _console) {
                SetConsoleTextAttribute(_console, 0x6);
                fprintf(stdout, LOG_FMT, item->time, _log_lvstr(item->lv), item->msg);
                fflush(stdout);
                SetConsoleTextAttribute(_console, _def_console.wAttributes);
            } else {
                fprintf(stdout, LOG_FMT, item->time, _log_lvstr(item->lv), item->msg);
            }
            break;
        default:
            fprintf(stdout, LOG_FMT, item->time, _log_lvstr(item->lv), item->msg);
            break;
        }
#else
        switch (item->lv) {
        case LOGLV_FATAL:
        case LOGLV_ERROR:
            fprintf(stdout, "\033[0;31m"LOG_FMT"\033[0m", item->time, _log_lvstr(item->lv), item->msg);
            fflush(stdout);
            break;
        case LOGLV_WARN:
            fprintf(stdout, "\033[0;33m"LOG_FMT"\033[0m", item->time, _log_lvstr(item->lv), item->msg);
            fflush(stdout);
            break;
        default:
            fprintf(stdout, LOG_FMT, item->time, _log_lvstr(item->lv), item->msg);
            break;
        }
#endif
    } else {
        fprintf(_handle, LOG_FMT, item->time, _log_lvstr(item->lv), item->msg);
        if (item->lv <= LOGLV_WARN) {
            fflush(_handle);
        }
    }
}
static void _log_write_all(log_item **items) {
    log_item *item;
    uint32_t n, i;
    while ((n = fsqu_pop_sc_batch(&_que, items, LOG_POP_BATCH)) > 0) {
        for (i = 0; i < n; i++) {
            item = items[i];
            _log_write_item(item);
            if (item->msg != item->inline_buf) {
                FREE(item->msg);
            }
            FREE(item);
        }
    }
}
static void _log_loop(void *arg) {
    (void)arg;
    log_item *items[LOG_POP_BATCH];
    while (ATOMIC_GET(&_running)) {
        _log_write_all(items);
        mutex_lock(&_mtx);
        while (0 == fsqu_size(&_que) && ATOMIC_GET(&_running)) {
            ATOMIC_SET(&_sleeping, 1);
            // 防丢失唤醒：在 _sleeping=1 之后再次检查队列。
            // 若生产者已 push 但因读到旧 _sleeping=0 跳过 signal，此处会发现非空并跳出，
            // 避免无人唤醒导致永久阻塞
            if (fsqu_size(&_que) > 0) {
                ATOMIC_SET(&_sleeping, 0);
                break;
            }
            cond_wait(&_cond, &_mtx);
            ATOMIC_SET(&_sleeping, 0);
        }
        mutex_unlock(&_mtx);
    }
    //打印日志线程退出
    log_item logexit;
    logexit.lv = LOGLV_INFO;
    (void)mstostr(nowms(), "%Y-%m-%d %H:%M:%S", logexit.time);
    SNPRINTF(logexit.inline_buf, sizeof(logexit.inline_buf),
        "[%s %s %d] %s", __FILENAME__(__FILE__), __FUNCTION__, __LINE__, "log thread exited.");
    logexit.msg = logexit.inline_buf;
    _log_write_item(&logexit);
}
void log_init(FILE *file, uint32_t capacity) {
    _handle = file;
#ifdef OS_WIN
    if (NULL == _handle) {
        _console = GetStdHandle(STD_OUTPUT_HANDLE);
        GetConsoleScreenBufferInfo(_console, &_def_console);
    }
#endif
    fsqu_init(&_que, sizeof(log_item *), 0 == capacity ? 4 * ONEK : capacity);
    mutex_init(&_mtx);
    cond_init(&_cond);
    ATOMIC_SET(&_running, 1); 
    _th = thread_creat(_log_loop, NULL);
}
/* 调用约定：log_free 必须在所有可能调用 slog 的线程停止后才能调用。
 * _running 置 0 与 fsqu_trypush 之间没有临界区，若有线程在检查 _running==1
 * 之后、fsqu_trypush 之前被抢占，等到 fsqu_free 执行后再恢复则会 UAF。
 * 正确关闭顺序：先 join 所有业务线程 → 再调用 log_free。*/
void log_free(void) {
    mutex_lock(&_mtx);
    ATOMIC_SET(&_running, 0);
    cond_signal(&_cond);
    mutex_unlock(&_mtx);
    thread_join(_th);
    log_item *items[LOG_POP_BATCH];
    _log_write_all(items);
    fsqu_free(&_que);
    mutex_free(&_mtx);
    cond_free(&_cond);
}
void log_setlv(LOG_LEVEL lv) {
    ATOMIC_SET(&_log_lv, (int32_t)lv);
}
LOG_LEVEL log_getlv(void) {
    return (LOG_LEVEL)ATOMIC_GET(&_log_lv);
}
void slog(int32_t lv, const char *fmt, ...) {
    if (lv > (int32_t)ATOMIC_GET(&_log_lv)
        || 0 == ATOMIC_GET(&_running)) {
        return;
    }
    log_item *item;
    MALLOC(item, sizeof(log_item));
    item->lv = lv;
    (void)mstostr(nowms(), "%Y-%m-%d %H:%M:%S", item->time);
    //先尝试写入 inline_buf，短消息（典型场景）至此完成单次 malloc；
    //超长消息再单独 heap 分配，行为与原 _format_va 等价。
    va_list args, args2;
    va_start(args, fmt);
    va_copy(args2, args);
    int32_t rtn = vsnprintf(item->inline_buf, LOG_INLINE_SIZE, fmt, args);
    va_end(args);
    if (rtn < 0) {
        va_end(args2);
        fprintf(stderr, LOG_FMT, item->time, _log_lvstr(item->lv), fmt);
        fflush(stderr);
        FREE(item);
        return;
    }
    if (rtn < LOG_INLINE_SIZE) {
        item->msg = item->inline_buf;
        va_end(args2);
    } else {
        char *heap_msg;
        MALLOC(heap_msg, (size_t)rtn + 1);
        rtn = vsnprintf(heap_msg, (size_t)rtn + 1, fmt, args2);
        va_end(args2);
        if (rtn < 0) {
            fprintf(stderr, LOG_FMT, item->time, _log_lvstr(item->lv), fmt);
            fflush(stderr);
            FREE(heap_msg);
            FREE(item);
            return;
        }
        item->msg = heap_msg;
    }
    //队列满时不阻塞业务线程，直接丢弃并写 stderr 兜底
    if (ERR_OK != fsqu_trypush(&_que, &item)) {
        fprintf(stderr, LOG_FMT, item->time, _log_lvstr(item->lv), item->msg);
        fflush(stderr);
        if (item->msg != item->inline_buf) {
            FREE(item->msg);
        }
        FREE(item);
        return;
    }
    if (ATOMIC_GET(&_sleeping)) {
        mutex_lock(&_mtx);
        cond_signal(&_cond);
        mutex_unlock(&_mtx);
    }
}
