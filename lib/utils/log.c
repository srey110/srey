#include "utils/log.h"
#include "utils/utils.h"
#include "thread/cond.h"
#include "thread/thread.h"
#include "utils/pool.h"
#include "utils/timer.h"

#define LOG_FMT "[%s %03d][%s]%s\n"
#define LOG_TIME_FMT "%Y-%m-%d %H:%M:%S" // 秒级部分;毫秒由调用方另拼
#define LOG_INLINE_SIZE 256
#define LOG_POP_BATCH   128
#ifdef OS_WIN
#define LOG_COLOR_SUFFIX ""
#else
#define LOG_COLOR_SUFFIX "\033[0m"
#endif

// 哪个级别配什么颜色只判这一次，两个平台各自负责怎么上色
typedef enum log_color {
    LOG_COLOR_NONE = 0,
    LOG_COLOR_RED,
    LOG_COLOR_YELLOW
}log_color;
typedef struct {
    int32_t lv;
    uint64_t ms;// 入队时刻；格式化推迟到日志线程，不占业务线程
    char *msg;// 指向 inline_buf 或独立 heap 分配
    char inline_buf[LOG_INLINE_SIZE]; // 短消息内嵌，避免 _format_va 第二次 malloc
} log_item;

static FILE *_handle = NULL;
static atomic_t _log_lv = LOGLV_DEBUG;
static atomic_t _running = 0; /* atomic 保证跨平台内存可见 */
static atomic_t _sleeping = 0;
static pthread_t _th;
static fsqu_ctx _que;
static pool_ctx _itempool;
static mutex_ctx _mtx;
static cond_ctx _cond;
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
static log_color _log_color_of(int32_t lv) {
    switch (lv) {
    case LOGLV_FATAL:
    case LOGLV_ERROR:
        return LOG_COLOR_RED;
    case LOGLV_WARN:
        return LOG_COLOR_YELLOW;
    }
    return LOG_COLOR_NONE;
}
// 上色分两半：进字节流的前后缀交给同一次 fprintf 打出去，不进字节流的（Windows 控制台属性）
// 由 _log_color_begin / _log_color_end 处理。stdio 每次调用只锁一次流，整行必须走一次 fprintf——
// stdout 不归日志线程独占，PRINT 就是裸 printf，插在转义头与复位码之间会让终端一直停在彩色
#ifdef OS_WIN
static const char *_log_color_begin(log_color color) {
    if (NULL != _console) {
        SetConsoleTextAttribute(_console, LOG_COLOR_RED == color ? 0xc : 0x6);
    }
    return "";
}
// 控制台属性作用于字节写出去的那一刻，得先把缓冲刷到控制台，再改回默认色
static void _log_color_end(void) {
    fflush(stdout);
    if (NULL != _console) {
        SetConsoleTextAttribute(_console, _def_console.wAttributes);
    }
}
#else
static const char *_log_color_begin(log_color color) {
    return LOG_COLOR_RED == color ? "\033[0;31m" : "\033[0;33m";
}
static void _log_color_end(void) {
    fflush(stdout);
}
#endif
// 唯一的成行出口。时间串由调用方给：算它要过 localtime_r 那把 libc 时区锁，
// N 个业务线程一起写日志就在一把与本程序无关的锁上串起来，所以正常路径推迟到日志线程；
// 业务线程只在 _log_stderr 那两条兜底上碰得到它。
// pre/post 是上色前后缀，必须与正文同一次 fprintf 打出去，整行才不会被别的 stdout 写方插断。
// 秒串与毫秒分两个参数传，由本函数一次成型
static void _log_fprint(FILE *f, const log_item *item, const char *time, int32_t msec,
                        const char *msg, const char *pre, const char *post) {
    fprintf(f, "%s"LOG_FMT"%s", pre, time, msec, _log_lvstr(item->lv), msg, post);
}
// 秒级部分按秒缓存：一批日志基本落在同一秒里，省掉 localtime_r 与 strftime。
// 缓存是无锁静态，只许 _log_write_item 这条串行路径用(日志线程，以及 thread_join
// 之后的 log_free)；业务线程的 _log_stderr 自己现算
static const char *_log_timestr(uint64_t ms) {
    static uint64_t cache_sec = 0;
    static char cache[TIME_LENS] = { 0 };
    uint64_t sec = ms / 1000;
    if ('\0' == cache[0]
        || sec != cache_sec) {
        if (ERR_OK != sectostr(sec, LOG_TIME_FMT, cache)) {
            return "";// sectostr 失败即把 cache 置空串, 下次重算
        }
        cache_sec = sec;
    }
    return cache;
}
static void _log_write_item(const log_item *item) {
    const char *time = _log_timestr(item->ms);
    int32_t msec = (int32_t)(item->ms % 1000);
    if (NULL != _handle) {
        _log_fprint(_handle, item, time, msec, item->msg, "", "");
        if (item->lv <= LOGLV_WARN) {
            fflush(_handle);
        }
        return;
    }
    log_color color = _log_color_of(item->lv);
    const char *pre = "";
    const char *post = "";
    if (LOG_COLOR_NONE != color) {
        pre = _log_color_begin(color);
        post = LOG_COLOR_SUFFIX;
    }
    _log_fprint(stdout, item, time, msec, item->msg, pre, post);
    if (LOG_COLOR_NONE != color) {
        _log_color_end();
    }
}
// 对象池 _elclear：归还前释放长消息独立缓冲（短消息走 inline_buf 不分配）
static void _log_item_clear(void *data) {
    log_item *it = (log_item *)data;
    if (it->msg != it->inline_buf) {
        FREE(it->msg);
    }
    it->msg = it->inline_buf;
}
// 返回本轮写出的条数：0 而 fsqu_size 非 0 即撞上在途元素，调用方据此退避而不是空转
static uint32_t _log_write_all(log_item **items) {
    log_item *item;
    uint32_t n, i, total = 0;
    while ((n = fsqu_pop_sc_batch(&_que, items, LOG_POP_BATCH)) > 0) {
        for (i = 0; i < n; i++) {
            item = items[i];
            _log_write_item(item);
            pool_push(&_itempool, item, 0);
        }
        total += n;
    }
    return total;
}
// 日志线程退出行。必须等队列排空后再写, 否则日志里会有业务日志排在"已退出"后面
static void _log_write_exit(void) {
    log_item logexit;
    logexit.lv = LOGLV_INFO;
    logexit.ms = nowms();
    SNPRINTF(logexit.inline_buf, sizeof(logexit.inline_buf),
        CONCAT2(LOG_PREFIX_FMT, "%s"), __FILENAME__(__FILE__), __FUNCTION__, __LINE__,
        "log thread exited.");
    logexit.msg = logexit.inline_buf;
    _log_write_item(&logexit);
}
static void _log_loop(void *arg) {
    (void)arg;
    log_item *items[LOG_POP_BATCH];
    timer_ctx timer;
    timer_init(&timer);
    uint64_t now, shrink_start = timer_cur_ms(&timer);
    uint32_t spins = 0;
    while (ATOMIC_GET(&_running)) {
        if (0 == _log_write_all(items)
            && fsqu_size(&_que) > 0) {
            spin_backoff(&spins);
            continue;
        }
        spins = 0;
        // 空闲时按 SHRINK_TIME 门控回落 log_item 池（锁外执行）
        now = timer_cur_ms(&timer);
        if (now - shrink_start >= SHRINK_TIME) {
            shrink_start = now;
            pool_shrink(&_itempool);
        }
        mutex_lock(&_mtx);
        // 单次带守卫等待，外层循环负责重试：超时上限 SHRINK_TIME 保证每 ≤SHRINK_TIME 重跑一次以收缩
        if (0 == fsqu_size(&_que) && ATOMIC_GET(&_running)) {
            ATOMIC_SET(&_sleeping, 1);
            // 防丢失唤醒：置 _sleeping 后再查一次队列，仍空才等。中间那道 fence 不能省:
            // fsqu_size 是 acquire 读, 在部分 ARM 上会跑到置位之前, 于是这边看不到刚入队的
            // 元素、生产者又还没看到 _sleeping, 两边同时看漏就是漏唤醒
            ATOMIC_THREAD_FENCE_SEQCST();
            if (0 == fsqu_size(&_que)) {
                cond_timedwait(&_cond, &_mtx, SHRINK_TIME);
            }
            ATOMIC_SET(&_sleeping, 0);
        }
        mutex_unlock(&_mtx);
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
    uint32_t cap = 0 == capacity ? 4 * ONEK : capacity;
    fsqu_init(&_que, sizeof(log_item *), cap);
    pool_cbs _logitem_cbs = { NULL, NULL, NULL, _log_item_clear };
    pool_init(&_itempool, sizeof(log_item), cap, cap / 4, 1, &_logitem_cbs);
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
    _log_write_exit();
    fsqu_free(&_que);
    pool_free(&_itempool);
    mutex_free(&_mtx);
    cond_free(&_cond);
}
void log_setlv(log_level lv) {
    ATOMIC_SET(&_log_lv, (int32_t)lv);
}
log_level log_getlv(void) {
    return (log_level)ATOMIC_GET(&_log_lv);
}
// stderr 兜底：格式化失败或队列满时走这里，跑在业务线程上，进不了日志线程那条路径
static void _log_stderr(const log_item *item, const char *msg) {
    char time[TIME_LENS];
    // 不碰 _log_timestr 的缓存：这里跑在业务线程上，那份静态只属于日志线程那条串行路径
    if (ERR_OK != sectostr(item->ms / 1000, LOG_TIME_FMT, time)) {
        time[0] = '\0';
    }
    _log_fprint(stderr, item, time, (int32_t)(item->ms % 1000), msg, "", "");
    fflush(stderr);
}
void slog(int32_t lv, const char *fmt, ...) {
    if (lv > (int32_t)ATOMIC_GET(&_log_lv)
        || 0 == ATOMIC_GET(&_running)) {
        return;
    }
    log_item *item = (log_item *)pool_pop(&_itempool, NULL, 0);
    item->lv = lv;
    item->ms = nowms();
    //先尝试写入 inline_buf，短消息（典型场景）至此完成单次 malloc；
    //超长消息再单独 heap 分配，行为与原 _format_va 等价。
    va_list args, args2;
    va_start(args, fmt);
    va_copy(args2, args);
    int32_t rtn = vsnprintf(item->inline_buf, LOG_INLINE_SIZE, fmt, args);
    va_end(args);
    if (rtn < 0) {
        va_end(args2);
        _log_stderr(item, fmt);
        pool_push(&_itempool, item, 0);
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
            _log_stderr(item, fmt);
            FREE(heap_msg);
            pool_push(&_itempool, item, 0);
            return;
        }
        item->msg = heap_msg;
    }
    //队列满时不阻塞业务线程，直接丢弃并写 stderr 兜底
    if (ERR_OK != fsqu_trypush(&_que, &item)) {
        _log_stderr(item, item->msg);
        pool_push(&_itempool, item, 0);
        return;
    }
    // 与消费者那道 fence 对称: 入队之后必须用足序版本读 _sleeping, 否则两边同时看漏就漏唤醒
    if (ATOMIC_GET_SEQCST(&_sleeping)) {
        mutex_lock(&_mtx);
        cond_signal(&_cond);
        mutex_unlock(&_mtx);
    }
}
