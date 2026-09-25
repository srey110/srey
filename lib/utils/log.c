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
#define LOG_FLUSH_WAIT  200 // log_abort 的等待上限(毫秒)
#define LOG_FLUSH_STEP  10 // log_abort 每次让出的毫秒数
#define LOG_ABORT_SLACK 3 // log_abort 输家比赢家多等的步数,覆盖赢家排空之后写那一行的时间
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
// log_abort 的三态选举，并发断言时的分工见 base.h 上的声明
typedef enum log_abort_state {
    LOG_ABORT_IDLE = 0,
    LOG_ABORT_WRITING,
    LOG_ABORT_DONE
}log_abort_state;
typedef struct {
    int32_t lv;
    uint64_t ms;// 入队时刻；格式化推迟到日志线程，不占业务线程
    char *msg;// 指向 inline_buf 或独立 heap 分配
    char inline_buf[LOG_INLINE_SIZE]; // 短消息内嵌，避免 _format_va 第二次 malloc
} log_item;
FSQU_DECL(logq, log_item *)

static FILE *_handle = NULL;
static atomic_t _log_lv = LOGLV_DEBUG;
static atomic_t _running = 0; /* atomic 保证跨平台内存可见 */
static atomic_t _sleeping = 0; /* 日志线程睡前置 1；生产者 CAS 抢到 1→0 的那个负责唤醒，其余不再重复 signal */
static atomic_t _aborting = LOG_ABORT_IDLE; /* 取值见 log_abort_state */
static atomic_t _drained = 0; /* 完成一轮排空自增,log_abort 据此判断落盘 */
static pthread_t _th;
// 本线程是不是日志线程本身。断言若发生在它身上，log_abort 据此跳过排空
static THREAD_LOCAL int32_t _in_logth = 0;
static logq _que;
static pool_ctx _itempool;
static mutex_ctx _mtx;
static cond_ctx _cond;
#ifdef OS_WIN
static HANDLE _console = NULL;
static CONSOLE_SCREEN_BUFFER_INFO _def_console;
#endif

static inline const char *_log_lvstr(int32_t lv) {
    switch (lv) {
    case LOGLV_FATAL: return "fatal";
    case LOGLV_ERROR: return "error";
    case LOGLV_WARN:  return "warning";
    case LOGLV_INFO:  return "info";
    case LOGLV_DEBUG: return "debug";
    }
    return "";
}
static inline log_color _log_color_of(int32_t lv) {
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
static inline const char *_log_color_begin(log_color color) {
    if (NULL != _console) {
        SetConsoleTextAttribute(_console, LOG_COLOR_RED == color ? 0xc : 0x6);
    }
    return "";
}
// 控制台属性作用于字节写出去的那一刻，得先把缓冲刷到控制台，再改回默认色
static inline void _log_color_end(void) {
    fflush(stdout);
    if (NULL != _console) {
        SetConsoleTextAttribute(_console, _def_console.wAttributes);
    }
}
#else
static inline const char *_log_color_begin(log_color color) {
    return LOG_COLOR_RED == color ? "\033[0;31m" : "\033[0;33m";
}
static inline void _log_color_end(void) {
    fflush(stdout);
}
#endif
// 唯一的成行出口。时间串由调用方给：算它要过 localtime_r 那把 libc 时区锁，
// N 个业务线程一起写日志就在一把与本程序无关的锁上串起来，所以正常路径推迟到日志线程；
// 业务线程只在 _log_sync 那几条兜底上碰得到它。
// pre/post 是上色前后缀，必须与正文同一次 fprintf 打出去，整行才不会被别的 stdout 写方插断。
// 秒串与毫秒分两个参数传，由本函数一次成型
static inline void _log_fprint(FILE *f, const log_item *item, const char *time, int32_t msec,
                        const char *msg, const char *pre, const char *post) {
    fprintf(f, "%s"LOG_FMT"%s", pre, time, msec, _log_lvstr(item->lv), msg, post);
}
// 兜底输出流。必须与 _log_write_item 同口径：无日志文件时走 stdout 而非 stderr，
// 否则兜底行与正文分家，-b 模式下 stderr 已 dup2 到 /dev/null，那几行会直接消失
static inline FILE *_log_out(void) {
    return NULL != _handle ? _handle : stdout;
}
// 业务线程上的同步写：不入队、不加锁，也不碰 _log_timestr 的缓存（那份静态只属于日志线程）。
// 用在格式化失败、队列满、以及 log_abort 三条进不了日志线程的路径上
static inline void _log_sync(FILE *f, const log_item *item, const char *msg) {
    char time[TIME_LENS];
    if (ERR_OK != sectostr(item->ms / 1000, LOG_TIME_FMT, time)) {
        time[0] = '\0';
    }
    _log_fprint(f, item, time, (int32_t)(item->ms % 1000), msg, "", "");
    fflush(f);
}
// 秒级部分按秒缓存：一批日志基本落在同一秒里，省掉 localtime_r 与 strftime。
// 缓存是无锁静态，只许 _log_write_item 这条串行路径用(日志线程，以及 thread_join
// 之后的 log_free)；业务线程的 _log_sync 自己现算
static inline const char *_log_timestr(uint64_t ms) {
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
static inline void _log_write_item(const log_item *item) {
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
// 返回本轮写出的条数：0 而队列非空即撞上在途元素，调用方据此退避而不是空转
static uint32_t _log_write_all(log_item **items) {
    log_item *item;
    uint32_t n, i, total = 0;
    while ((n = logq_pop_sc_batch(&_que, items, LOG_POP_BATCH)) > 0) {
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
        CONCAT2(LOG_PREFIX_FMT, "%s"), __FILENAME__, __FUNCTION__, __LINE__,
        "log thread exited.");
    logexit.msg = logexit.inline_buf;
    _log_write_item(&logexit);
}
static void _log_loop(void *arg) {
    (void)arg;
    _in_logth = 1;
    log_item *items[LOG_POP_BATCH];
    timer_ctx timer;
    timer_init(&timer);
    uint64_t now, shrink_start = timer_cur_ms(&timer);
    uint32_t spins = 0, nwrote;
    while (ATOMIC_GET(&_running)) {
        nwrote = _log_write_all(items);
        if (0 == nwrote
            && !logq_empty(&_que)) {
            spin_backoff(&spins);
            continue;
        }
        ATOMIC_ADD(&_drained, 1);
        spins = 0;
        // 空闲时按 SHRINK_TIME 门控回落 log_item 池（锁外执行）
        now = timer_cur_ms(&timer);
        if (pool_shrink_due(&shrink_start, now)) {
            pool_shrink(&_itempool);
        }
        mutex_lock(&_mtx);
        // 单次带守卫等待，外层循环负责重试：超时上限 SHRINK_TIME 保证每 ≤SHRINK_TIME 重跑一次以收缩
        if (logq_empty(&_que) && ATOMIC_GET(&_running)) {
            ATOMIC_SET_SEQCST(&_sleeping, 1);
            // 防丢失唤醒：置 _sleeping 后再查一次队列，仍空才等。中间那道 fence 不能省:
            // logq_empty 是 acquire 读, 在部分 ARM 上会跑到置位之前, 于是这边看不到刚入队的
            // 元素、生产者又还没看到 _sleeping, 两边同时看漏就是漏唤醒
            ATOMIC_THREAD_FENCE_SEQCST();
            if (logq_empty(&_que)) {
                cond_timedwait(&_cond, &_mtx, SHRINK_TIME);
            }
            ATOMIC_SET_RELAXED(&_sleeping, 0);
        }
        mutex_unlock(&_mtx);
    }
}
// 日志线程是否正睡着、需要唤醒。必须是足序读，与上面置 _sleeping 前那道 fence 对称：
// 换成普通读，弱序平台上两边会同时看漏（消费者没看到新元素、生产者没看到 _sleeping）。
// 只有 slog 用它；_log_drain_wait 不看它：_sleeping 可能已被别的生产者清零、signal 却还没发出
static inline int32_t _log_need_wake(void) {
    return ATOMIC_GET_SEQCST(&_sleeping);
}
// 唤醒日志线程并等它把队列排空，上限 LOG_FLUSH_WAIT。崩溃路径上只 trylock，宁可不唤醒也不卡住。
// 只有在业务线程上调用才有意义：断言若发生在日志线程自己身上，唯一能推进队列的就是它，
// 等下去必然空转满整个上限，故调用方须先判 _in_logth
static void _log_drain_wait(void) {
    atomic_t gen = ATOMIC_GET(&_drained);
    if (ERR_OK == mutex_trylock(&_mtx)) {
        cond_signal(&_cond);
        mutex_unlock(&_mtx);
    }
    int32_t nstep = LOG_FLUSH_WAIT / LOG_FLUSH_STEP;
    while ((!logq_empty(&_que) || gen == ATOMIC_GET(&_drained))
        && nstep-- > 0) {
        MSLEEP(LOG_FLUSH_STEP);
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
    logq_init(&_que, cap);
    pool_cbs _logitem_cbs = { NULL, NULL, NULL, _log_item_clear };
    pool_init(&_itempool, sizeof(log_item), cap, cap / 4, POOL_THSAFE, &_logitem_cbs);
    mutex_init(&_mtx);
    cond_init(&_cond);
    ATOMIC_SET_RELEASE(&_running, 1); 
    _th = thread_creat(_log_loop, NULL);
}
/* 调用约定：log_free 必须在所有可能调用 slog 的线程停止后才能调用。
 * _running 置 0 与 logq_trypush 之间没有临界区，若有线程在检查 _running==1
 * 之后、logq_trypush 之前被抢占，等到 logq_free 执行后再恢复则会 UAF。
 * 正确关闭顺序：先 join 所有业务线程 → 再调用 log_free。*/
void log_free(void) {
    mutex_lock(&_mtx);
    ATOMIC_SET_RELAXED(&_running, 0);
    mutex_unlock(&_mtx);
    cond_signal(&_cond);
    thread_join(_th);
    log_item *items[LOG_POP_BATCH];
    _log_write_all(items);
    _log_write_exit();
    logq_free(&_que);
    pool_free(&_itempool);
    mutex_free(&_mtx);
    cond_free(&_cond);
}
void log_abort(const char *file, const char *func, int32_t line, const char *msg) {
    // 日志线程未起或已停，队列和锁都不能碰，只能靠 ASSERTAB 那行裸 fprintf
    if (0 == ATOMIC_GET(&_running)) {
        return;
    }
    // 抢不到写权的等赢家置 DONE 再走：调用方下一句就是 abort()，直接放行等于把还在
    // 排空的赢家连同整队日志一起带走。多等几步盖住赢家写那一行的时间，赢家卡住也不会挂死
    if (!ATOMIC_CAS(&_aborting, LOG_ABORT_IDLE, LOG_ABORT_WRITING)) {
        int32_t nstep = LOG_FLUSH_WAIT / LOG_FLUSH_STEP + LOG_ABORT_SLACK;
        while (LOG_ABORT_DONE != ATOMIC_GET(&_aborting)
            && nstep-- > 0) {
            MSLEEP(LOG_FLUSH_STEP);
        }
        return;
    }
    // 断言发生在日志线程自己身上时没人能推进队列，等也是白等
    if (0 == _in_logth) {
        _log_drain_wait();
    }
    // 控制台模式不重复写原因行（ASSERTAB 三行前已打到 stderr），只把刚排空进 stdout 的刷出去
    if (NULL != _handle) {
        log_item item;
        item.lv = LOGLV_FATAL;
        item.ms = nowms();
        SNPRINTF(item.inline_buf, sizeof(item.inline_buf),
            CONCAT2(LOG_PREFIX_FMT, "[ABORT] %s"), file, func, line, msg);
        item.msg = item.inline_buf;
        _log_sync(_handle, &item, item.msg);
    } else {
        fflush(stdout);
    }
    ATOMIC_SET_RELEASE(&_aborting, LOG_ABORT_DONE);
}
void log_setlv(log_level lv) {
    ATOMIC_SET_RELAXED(&_log_lv, (int32_t)lv);
}
log_level log_getlv(void) {
    return (log_level)ATOMIC_GET(&_log_lv);
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
    const char *syncmsg;
    va_list args, args2;
    va_start(args, fmt);
    va_copy(args2, args);
    int32_t rtn = vsnprintf(item->inline_buf, LOG_INLINE_SIZE, fmt, args);
    va_end(args);
    if (rtn < 0) {
        va_end(args2);
        syncmsg = fmt;
        goto sync;
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
            FREE(heap_msg);
            syncmsg = fmt;
            goto sync;
        }
        item->msg = heap_msg;
    }
    //队列满时不阻塞业务线程，直接丢弃并同步写出兜底
    if (ERR_OK != logq_trypush(&_que, &item)) {
        syncmsg = item->msg;
        goto sync;
    }
    if (_log_need_wake()
        && ATOMIC_CAS(&_sleeping, 1, 0)) {
        mutex_lock(&_mtx);
        cond_signal(&_cond);
        mutex_unlock(&_mtx);
    }
    return;
sync:
    _log_sync(_log_out(), item, syncmsg);
    pool_push(&_itempool, item, 0);
}
