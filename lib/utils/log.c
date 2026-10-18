#include "utils/log.h"
#include "thread/waiter.h"
#include "thread/thread.h"
#include "containers/fsqu.h"

#define LOG_FMT "[%s %03d][%s]%s" // 行首时间、级别与正文，换行由 _log_fprint 补
#define LOG_LINE_STACK 512 // _log_fprint 栈上拼行的缓冲，装不下的长行退回 fprintf
#define LOG_TIME_FMT "%Y-%m-%d %H:%M:%S" // 秒级部分;毫秒由调用方另拼
#define LOG_SLEEP_MAX   10000 // 空闲深睡时长(毫秒);新日志或退出会提前叫醒。浅睡 LOG_LINGER_MS 时谁来叫醒见 _log_commit
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
// log_abort 的三态选举，并发断言时的分工见 macro_log.h 上的声明
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
static atomic_t _aborting = LOG_ABORT_IDLE; /* 取值见 log_abort_state */
static atomic_t _drained = 0; /* 完成一轮排空自增,log_abort 据此判断落盘 */
static pthread_t _th;
// 本线程是不是日志线程本身。断言若发生在它身上，log_abort 据此跳过排空
TLS_DEFINE(int32_t, _in_logth, 1)
// 业务线程走 _log_sync 兜底时自己的秒级时间串缓存，见 _log_timestr_cached
TLS_DEFINE(uint64_t, _sync_sec, 1)
TLS_DEFINE(char, _sync_time, TIME_LENS)
// 日志文件模式下攒的行，只许 _log_write_item 这条串行路径用，口径同 _log_timestr
static char _batch[LOG_BATCH_BYTES];
static size_t _nbatch = 0;
static logq _que;
static waiter_ctx _waiter;// 日志线程的睡眠握手，深睡 WAITER_SLEEP、浅睡 WAITER_LINGER
static uint32_t _linger_full = LOG_POP_BATCH;// 浅睡时攒到这么多条就叫醒，log_init 按队列容量定
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
// 上色分两半：进字节流的前后缀与正文同一次 stdio 写调用打出去，不进字节流的（Windows 控制台属性）
// 由 _log_color_begin / _log_color_end 处理。stdio 每次调用只锁一次流，整行必须一次写出——
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
// 颜色在字节流里，写完不必单独刷；WARN 及以上由 _log_write_all 每批刷一次
static inline void _log_color_end(void) {
}
#endif
// 拼行处：整行拼进 dst，返回字节数；装不下返回 0，dst 不动(调用方改走 LOG_FMT 兜底)。
// 时间串由调用方给(取法见 _log_timestr)，毫秒 msec 取值 [0, 999]。
// pre/post 是上色前后缀，必须与正文同一次写出去，整行才不会被别的 stdout 写方插断。
// 复位码排在换行之前——行缓冲的 stdout 一遇换行就刷，排在后面会单独多出一次 write
static inline size_t _log_line(char *dst, size_t cap, const log_item *item, const char *time, int32_t msec,
                               const char *msg, const char *pre, const char *post) {
    char *p = dst;
    const char *lv = _log_lvstr(item->lv);
    const size_t fixed = sizeof("[ 000][]\n") - 1;
    size_t npre = strlen(pre), ntime = strlen(time), nlv = strlen(lv), nmsg = strlen(msg), npost = strlen(post);
    if (npre + ntime + nlv + nmsg + npost + fixed > cap) {
        return 0;
    }
    memcpy(p, pre, npre);
    p += npre;
    *p++ = '[';
    memcpy(p, time, ntime);
    p += ntime;
    *p++ = ' ';
    *p++ = (char)('0' + msec / 100);
    *p++ = (char)('0' + msec / 10 % 10);
    *p++ = (char)('0' + msec % 10);
    *p++ = ']';
    *p++ = '[';
    memcpy(p, lv, nlv);
    p += nlv;
    *p++ = ']';
    memcpy(p, msg, nmsg);
    p += nmsg;
    memcpy(p, post, npost);
    p += npost;
    *p++ = '\n';
    return (size_t)(p - dst);
}
// 单行直接写：栈上拼好一次 fwrite，太长才退回 fprintf
static inline void _log_fprint(FILE *f, const log_item *item, const char *time, int32_t msec,
                               const char *msg, const char *pre, const char *post) {
    char line[LOG_LINE_STACK];
    size_t n = _log_line(line, sizeof(line), item, time, msec, msg, pre, post);
    if (0 == n) {
        fprintf(f, "%s"LOG_FMT"%s\n", pre, time, msec, _log_lvstr(item->lv), msg, post);
        return;
    }
    fwrite(line, 1, n, f);
}
// 兜底输出流。必须与 _log_write_item 同口径：无日志文件时走 stdout 而非 stderr，
// 否则兜底行与正文分家，-b 模式下 stderr 已 dup2 到 /dev/null，那几行会直接消失
static inline FILE *_log_out(void) {
    return NULL != _handle ? _handle : stdout;
}
// 秒级时间串按秒缓存：一批日志基本落在同一秒里，省掉 localtime_r 与 strftime。localtime_r 要过
// libc 的时区锁，N 个线程一起算就在这把与本程序无关的锁上串起来，所以正常路径推迟到日志线程。
// 缓存无锁，每条路径用自己那份：日志线程见 _log_timestr，业务线程见 _log_sync
static inline const char *_log_timestr_cached(uint64_t ms, uint64_t *csec, char cache[TIME_LENS]) {
    uint64_t sec = ms / 1000;
    if ('\0' == cache[0]
        || sec != *csec) {
        if (ERR_OK != sectostr(sec, LOG_TIME_FMT, cache)) {
            return "";// sectostr 失败即把 cache 置空串, 下次重算
        }
        *csec = sec;
    }
    return cache;
}
// 日志线程那份缓存，只许 _log_write_item 这条串行路径用(日志线程，以及 thread_join 之后的 log_free)
static inline const char *_log_timestr(uint64_t ms) {
    static uint64_t cache_sec = 0;
    static char cache[TIME_LENS] = { 0 };
    return _log_timestr_cached(ms, &cache_sec, cache);
}
// 业务线程上的同步写：不入队、不加锁，时间串用本线程自己的缓存。
// 用在格式化失败、队列满、以及 log_abort 三条进不了日志线程的路径上。
// WARN 及以上立刻刷，其余留给日志线程入睡前那次刷
static inline void _log_sync(FILE *f, const log_item *item, const char *msg) {
    _log_fprint(f, item, _log_timestr_cached(item->ms, _sync_sec_tls(), _sync_time_tls()),
                (int32_t)(item->ms % 1000), msg, "", "");
    if (item->lv <= LOGLV_WARN) {
        fflush(f);
    }
}
// 攒下的行写进日志文件
static inline void _log_batch_flush(void) {
    if (0 != _nbatch) {
        fwrite(_batch, 1, _nbatch, _handle);
        _nbatch = 0;
    }
}
// 日志文件模式攒进 _batch，由调用方在一批末尾 _log_batch_flush；控制台模式逐行写：
// Windows 上色是调 API 不在字节流里，没法攒
static inline void _log_write_item(const log_item *item) {
    const char *time = _log_timestr(item->ms);
    int32_t msec = (int32_t)(item->ms % 1000);
    if (NULL != _handle) {
        size_t n = _log_line(_batch + _nbatch, sizeof(_batch) - _nbatch, item, time, msec, item->msg, "", "");
        if (0 == n) {
            _log_batch_flush();
            n = _log_line(_batch, sizeof(_batch), item, time, msec, item->msg, "", "");
            if (0 == n) {
                _log_fprint(_handle, item, time, msec, item->msg, "", "");
                return;
            }
        }
        _nbatch += n;
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
// 释放一条日志项:先放掉长消息的独立缓冲(短消息 msg 即 inline_buf),再放 item 本身
static void _log_item_free(log_item *it) {
    if (it->msg != it->inline_buf) {
        FREE(it->msg);
    }
    FREE(it);
}
// 返回本轮写出的条数：0 而队列非空即撞上在途元素，调用方据此退避而不是空转。
// 批里有 WARN 及以上就在这批写完后刷一次，不逐条刷：告警风暴时逐条刷就是每条一次 write
static uint32_t _log_write_all(log_item **items) {
    log_item *item;
    uint32_t n, i, total = 0;
    int32_t urgent;
    while ((n = logq_pop_sc_batch(&_que, items, LOG_POP_BATCH)) > 0) {
        urgent = 0;
        for (i = 0; i < n; i++) {
            item = items[i];
            _log_write_item(item);
            urgent |= item->lv <= LOGLV_WARN;
            _log_item_free(item);
        }
        _log_batch_flush();
        if (urgent) {
            fflush(_log_out());
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
    _log_batch_flush();
}
// 日志线程主循环。排空后准备睡之前刷一次输出流：日志文件是全缓冲，不刷的话低频日志要攒满缓冲、
// 或等到下一条 WARN 才落盘。上次入睡以来写过行就浅睡 LOG_LINGER_MS，否则深睡；浅睡时谁来叫醒见 _log_commit
static void _log_loop(void *arg) {
    (void)arg;
    *_in_logth_tls() = 1;
    log_item *items[LOG_POP_BATCH];
    uint32_t spins = 0, nwrote;
    int32_t wrote = 0;
    while (ATOMIC_GET(&_running)) {
        nwrote = _log_write_all(items);
        if (0 == nwrote
            && !logq_empty(&_que)) {
            spin_backoff(&spins);
            continue;
        }
        wrote |= (0 != nwrote);
        ATOMIC_ADD(&_drained, 1);
        spins = 0;
        if (logq_empty(&_que)) {
            fflush(_log_out());
        }
        // 单次带守卫等待，外层循环负责重试；新日志或退出会提前叫醒。握手见 waiter.h
        waiter_prepare(&_waiter, wrote ? WAITER_LINGER : WAITER_SLEEP);
        if (!logq_empty(&_que)
            || 0 == ATOMIC_GET(&_running)) {
            waiter_cancel(&_waiter);
        } else {
            (void)waiter_commit(&_waiter, wrote ? LOG_LINGER_MS : LOG_SLEEP_MAX);
            wrote = 0;
        }
    }
}
// 唤醒日志线程并等它把队列排空，上限 LOG_FLUSH_WAIT。崩溃路径上用 waiter_trywake，宁可不唤醒也不卡住；
// 不看睡眠态直接叫：睡眠态可能已被别的生产者抢成 WAKING、叫醒却还没发出。
// 只有在业务线程上调用才有意义：断言若发生在日志线程自己身上，唯一能推进队列的就是它，
// 等下去必然空转满整个上限，故调用方须先判 _in_logth
static void _log_drain_wait(void) {
    atomic_t gen = ATOMIC_GET(&_drained);
    waiter_trywake(&_waiter);
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
    // 队列配得小时攒满一批就够不着了，改取容量一半，留出余量不溢出
    _linger_full = logq_capacity(&_que) / 2;
    if (_linger_full > LOG_POP_BATCH) {
        _linger_full = LOG_POP_BATCH;
    }
    waiter_init(&_waiter);
    ATOMIC_SET_RELEASE(&_running, 1); 
    _th = thread_creat(_log_loop, NULL);
}
// 调用约定见 log.h：_running 置 0 与 logq_trypush 之间没有临界区，还在打日志的线程会碰到已释放的队列
void log_free(void) {
    ATOMIC_SET_RELAXED(&_running, 0);
    waiter_wake(&_waiter, NULL);// 先置 0 再叫(握手见 waiter.h)：日志线程要么复查时看到 0，要么已睡下被叫醒
    thread_join(_th);
    log_item *items[LOG_POP_BATCH];
    _log_write_all(items);
    _log_write_exit();
    logq_free(&_que);
    waiter_free(&_waiter);
}
void log_abort(const char *file, const char *func, int32_t line, const char *msg) {
    stack_print(stderr);// 放最前：下面几条早退的路径也要有栈
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
    // 断言发生在日志线程自己身上时没人能推进队列，等也是白等，只把它手里攒的那批写出去
    if (0 == *_in_logth_tls()) {
        _log_drain_wait();
    } else {
        _log_batch_flush();
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
        stack_print(_handle);
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
// slog 一族的开头：级别过滤并取一个 item，被滤掉或日志线程未起返回 NULL
static inline log_item *_log_take(int32_t lv) {
    if (lv > (int32_t)ATOMIC_GET(&_log_lv)
        || 0 == ATOMIC_GET(&_running)) {
        return NULL;
    }
    log_item *item;
    MALLOC(item, sizeof(log_item));
    item->msg = item->inline_buf;// 初始即指向内嵌缓冲,_log_item_free 判独立缓冲时不误放
    item->lv = lv;
    item->ms = nowms();
    return item;
}
// 进不了队列时在业务线程上同步写掉并归还 item
static void _log_drop(log_item *item, const char *msg) {
    _log_sync(_log_out(), item, msg);
    _log_item_free(item);
}
// slog 一族的收尾：入队并按需唤醒日志线程；队列满不阻塞业务线程，同步写出兜底。
// 浅睡时只有 WARN 及以上或攒到 _linger_full 条才叫醒，攒满就叫是给突发留出队列余量；队列长度用的是不拿锁的近似值，
// 看漏了也只是晚一个 LOG_LINGER_MS 才写，不丢行
static inline void _log_commit(log_item *item) {
    int32_t lv = item->lv;// 入队后 item 归日志线程，不能再读
    if (ERR_OK != logq_trypush(&_que, &item)) {
        _log_drop(item, item->msg);
        return;
    }
    waiter_state st = waiter_state_get(&_waiter);
    if ((WAITER_SLEEP == st
         || (WAITER_LINGER == st && (lv <= LOGLV_WARN || logq_size_fast(&_que) >= _linger_full)))
        && waiter_mark_waking(&_waiter, st)) {
        waiter_wake_held(&_waiter);// 打日志的可能是不被 join 的线程，所以用 wake_held
    }
}
void slog(int32_t lv, const char *fmt, ...) {
    log_item *item = _log_take(lv);
    if (NULL == item) {
        return;
    }
    //先尝试写入 inline_buf，短消息（典型场景）至此完成单次 malloc；
    //超长消息再单独 heap 分配，行为与原 _format_va 等价。
    va_list args, args2;
    va_start(args, fmt);
    va_copy(args2, args);
    int32_t rtn = vsnprintf(item->inline_buf, LOG_INLINE_SIZE, fmt, args);
    va_end(args);
    if (rtn < 0) {
        va_end(args2);
        _log_drop(item, fmt);
        return;
    }
    if (rtn < LOG_INLINE_SIZE) {
        va_end(args2);
    } else {
        char *heap_msg;
        MALLOC(heap_msg, (size_t)rtn + 1);
        rtn = vsnprintf(heap_msg, (size_t)rtn + 1, fmt, args2);
        va_end(args2);
        if (rtn < 0) {
            FREE(heap_msg);
            _log_drop(item, fmt);
            return;
        }
        item->msg = heap_msg;
    }
    _log_commit(item);
}
void slog_parts(int32_t lv, const char *const *parts, const size_t *lens, uint32_t n) {
    log_item *item = _log_take(lv);
    if (NULL == item) {
        return;
    }
    size_t total = 0;
    uint32_t i;
    for (i = 0; i < n; i++) {
        total += lens[i];
    }
    char *p = item->inline_buf;
    if (total >= LOG_INLINE_SIZE) {
        MALLOC(p, total + 1);
    }
    item->msg = p;
    for (i = 0; i < n; i++) {
        memcpy(p, parts[i], lens[i]);
        p += lens[i];
    }
    *p = '\0';
    _log_commit(item);
}
