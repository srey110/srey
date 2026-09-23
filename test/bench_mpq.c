#include "bench_mpq.h"
#include "lib.h"
#include "thread/thread.h"
#include "containers/mpq.h"
#include "containers/bbq.h"
#include "containers/fsqu.h"

// 比的是 FSQU_FAST_MODEL 的三个取值:0 queue+spin / 1 mpq / 2 bbq。
// 三家直接用 fsqu 的本体实例化(FSQU_SPIN_DECL / FSQU_RING_DECL),测的就是产线那套入队出队。
// 容量与元素尺寸取产线实参:msgq 256 / taskq 1024 / cmdq·twq·logq 4096;
// 6 个 FSQU_DECL 实例里 4 个是 8 字节指针,另两个是 72 字节结构体。
// 消费侧分单条与批量两个维度:产线真正在用的是批量(loader 32,log/tw/cmd 128)。
#define BQ_TOTAL   2000000 // MPSC 两个维度的总入队条数;太小的话计时窗口只有几毫秒,全是噪声
#define BQ_ROUNDS  9       // 逐轮换起手取中位,单跑一次测不出东西
#define BQ_MAXPROD 8
#define BQ_MAXMS   60000   // 单场景耗时上限:队列丢元素时消费者要能退出
#define BQ_BATMAX  128     // 批量缓冲上限,取 LOG_POP_BATCH
// 纯入队那一维要容量装得下全部元素。bbq 总容量封顶 16 块 x 65536 槽,
// 所以这一维单独用小一号的总量,不能沿用 BQ_TOTAL。
// 1048576 正好是 bbq 的天花板(16 x 65536),再大要先放宽 BBQ_MAX_BLKSZ 或块数上限。
// 计时窗口因此只有几十毫秒,这一维只够看数量级,细微差别要看下面两维
#define BQ_PUSH_TOTAL 1000000
#define BQ_PUSH_CAP   1048576

// 三个后端,下标即 FSQU_FAST_MODEL 的取值
#define BQ_SPIN 0
#define BQ_MPQ  1
#define BQ_BBQ  2
#define BQ_NBK  3

// 产线元素尺寸的两个代表
typedef struct bq8 { uint64_t v; }bq8;
typedef struct bq72 { uint64_t v; uint64_t pad[8]; }bq72;
// 单个后端跑一轮的入口,cell 按后端下标取
typedef uint64_t (*bq_run_fn)(int32_t nprod, int32_t per, uint32_t cap, int32_t mpsc, uint32_t batch);

static const char *_bq_bkname[BQ_NBK] = { "queue+spin", "mpq", "bbq" };
static atomic_t _bq_gate;// 起跑门:线程创建不进计时区

// 给一个后端生成整套 bench。tag 拼函数名,T 元素类型,Q 该后端的 fsqu 类型名,BK 后端下标(只用来打日志)
#define BQ_BACKEND(tag, T, Q, BK)                                                           \
typedef struct {                                                                            \
    int32_t n;                                                                              \
    uint32_t base;                                                                          \
    uint32_t batch;         /* 0 = 逐条 pop,否则单次批量上限 */                             \
    int32_t got;            /* 消费者实收数,供校验 */                                        \
    Q *q;                                                                                   \
}tag##_arg;                                                                                 \
static void tag##_producer(void *ud) {                                                      \
    tag##_arg *a = (tag##_arg *)ud;                                                         \
    T item;                                                                                 \
    int32_t i;                                                                              \
    ZERO(&item, sizeof(item));                                                              \
    _bq_wait_gate();                                                                        \
    for (i = 0; i < a->n; i++) {                                                            \
        item.v = (uint64_t)a->base + (uint64_t)i;                                           \
        Q##_push(a->q, &item);                                                              \
    }                                                                                       \
}                                                                                           \
/* 单次取一批(batch 为 0 时取一条)。返回本次取到的条数,在途按 0 算 */                       \
static uint32_t tag##_drain(tag##_arg *a, T *buf) {                                         \
    if (0 == a->batch) {                                                                    \
        return (ERR_OK == Q##_pop_sc(a->q, buf)) ? 1 : 0;                                   \
    }                                                                                       \
    return Q##_pop_sc_batch(a->q, buf, a->batch);                                           \
}                                                                                           \
static void tag##_consumer(void *ud) {                                                      \
    tag##_arg *a = (tag##_arg *)ud;                                                         \
    T buf[BQ_BATMAX];                                                                       \
    uint32_t n;                                                                             \
    int32_t got = 0;                                                                        \
    uint32_t spins = 0;                                                                     \
    uint64_t deadline;                                                                      \
    _bq_wait_gate();                                                                        \
    deadline = nowms() + BQ_MAXMS;                                                          \
    while (got < a->n) {                                                                    \
        n = tag##_drain(a, buf);                                                            \
        if (0 != n) {                                                                       \
            got += (int32_t)n;                                                              \
            continue;                                                                       \
        }                                                                                   \
        /* 每约 100 万圈才看一次表:空转是紧循环,逐圈 nowms 会压低读数 */                    \
        if (0 == (++spins & 0xFFFFF)                                                        \
            && nowms() > deadline) {                                                        \
            LOG_WARN("[bench_mpq] consumer timeout: backend=%s got=%d/%d", _bq_bkname[BK], got, a->n);\
            break;                                                                          \
        }                                                                                   \
    }                                                                                       \
    a->got = got;                                                                           \
}                                                                                           \
/* 计时前把环整圈写满再取空:页在计时区外摸进来,首次缺页不算进入队耗时 */                    \
static void tag##_prefault(Q *q) {                                                          \
    T item;                                                                                 \
    T buf[BQ_BATMAX];                                                                       \
    ZERO(&item, sizeof(item));                                                              \
    while (ERR_OK == Q##_trypush(q, &item)) {                                               \
    }                                                                                       \
    while (0 != Q##_pop_sc_batch(q, buf, BQ_BATMAX)) {                                      \
    }                                                                                       \
}                                                                                           \
/* 跑一轮。mpsc 非 0 时起消费者,batch 非 0 时消费者走批量。返回耗时,结果不对返回 0 */       \
static uint64_t tag##_run(int32_t nprod, int32_t per, uint32_t cap, int32_t mpsc, uint32_t batch) {\
    Q q;                                                                                    \
    tag##_arg pargs[BQ_MAXPROD];                                                            \
    pthread_t pths[BQ_MAXPROD];                                                             \
    tag##_arg carg;                                                                         \
    pthread_t cth = 0;                                                                      \
    uint64_t t0, ms;                                                                        \
    uint32_t left;                                                                          \
    int32_t i, total = nprod * per;                                                         \
    Q##_init(&q, cap);                                                                      \
    tag##_prefault(&q);                                                                     \
    ATOMIC_SET(&_bq_gate, 0);                                                               \
    for (i = 0; i < nprod; i++) {                                                           \
        pargs[i].n = per;                                                                   \
        pargs[i].base = (uint32_t)i * (uint32_t)per;                                        \
        pargs[i].batch = batch;                                                             \
        pargs[i].got = 0;                                                                   \
        pargs[i].q = &q;                                                                    \
        pths[i] = thread_creat(tag##_producer, &pargs[i]);                                  \
    }                                                                                       \
    if (0 != mpsc) {                                                                        \
        carg.n = total;                                                                     \
        carg.base = 0;                                                                      \
        carg.batch = batch;                                                                 \
        carg.got = 0;                                                                       \
        carg.q = &q;                                                                        \
        cth = thread_creat(tag##_consumer, &carg);                                          \
    }                                                                                       \
    t0 = nowms();                                                                           \
    ATOMIC_SET(&_bq_gate, 1);                                                               \
    for (i = 0; i < nprod; i++) {                                                           \
        thread_join(pths[i]);                                                               \
    }                                                                                       \
    if (0 != mpsc) {                                                                        \
        thread_join(cth);                                                                   \
    }                                                                                       \
    ms = nowms() - t0;                                                                      \
    /* 校验:元素一个都不能少,否则数字没有意义 */                                            \
    left = (0 != mpsc) ? (uint32_t)carg.got : Q##_size(&q);                                 \
    if (left != (uint32_t)total) {                                                          \
        LOG_ERROR("[bench_mpq] lost items: backend=%s %s=%u/%d", _bq_bkname[BK],            \
                  (0 != mpsc) ? "got" : "in queue", left, total);                           \
        ms = 0;                                                                             \
    }                                                                                       \
    Q##_free(&q);                                                                           \
    return ms;                                                                              \
}

// 给一种元素类型生成三个后端各一套 bench。tag 拼类型名,T 元素类型。
// 三个后端直接用 fsqu 的本体实例化,测的就是产线代码:_fs = 0 号 queue+spin,_fm = mpq,_fb = bbq
#define BQ_DEFINE(tag, T)                                                                   \
FSQU_SPIN_DECL(tag##_fsq, T)                                                                \
FSQU_RING_DECL(tag##_fmq, T, MPQ_DECL)                                                      \
FSQU_RING_DECL(tag##_fbq, T, BBQ_DECL)                                                      \
BQ_BACKEND(tag##_fs, T, tag##_fsq, BQ_SPIN)                                                 \
BQ_BACKEND(tag##_fm, T, tag##_fmq, BQ_MPQ)                                                  \
BQ_BACKEND(tag##_fb, T, tag##_fbq, BQ_BBQ)                                                  \
static void tag##_cell(const char *what, int32_t nprod, int32_t total, uint32_t cap,        \
                       int32_t mpsc, uint32_t batch) {                                      \
    const bq_run_fn runs[BQ_NBK] = { tag##_fs_run, tag##_fm_run, tag##_fb_run };            \
    _bq_cell(what, sizeof(T), runs, nprod, total, cap, mpsc, batch);                        \
}

// 取中位数,顺带排序(轮数很小,插入排序够了)
static uint64_t _bq_med(uint64_t *a, int32_t n) {
    int32_t i, j;
    uint64_t t;
    for (i = 1; i < n; i++) {
        t = a[i];
        for (j = i - 1; j >= 0 && a[j] > t; j--) {
            a[j + 1] = a[j];
        }
        a[j + 1] = t;
    }
    return a[n / 2];
}
static void _bq_wait_gate(void) {
    while (0 == ATOMIC_GET(&_bq_gate)) {
        CPU_PAUSE();
    }
}
// x 倍速:基准耗时 / 本家耗时,>1 表示比 queue+spin 快
static double _bq_x(uint64_t base, uint64_t v) {
    return (0 == v) ? 0.0 : (double)base / (double)v;
}
// 一个格子:三个后端逐轮换起手交错跑,各取中位。倍速以 queue+spin 为基准
static void _bq_cell(const char *what, size_t elsize, const bq_run_fn *runs, int32_t nprod,
                     int32_t total, uint32_t cap, int32_t mpsc, uint32_t batch) {
    uint64_t a[BQ_NBK][BQ_ROUNDS];
    uint64_t m[BQ_NBK];
    int32_t per = total / nprod;
    int32_t r, o, v;
    for (r = 0; r < BQ_ROUNDS; r++) {
        for (o = 0; o < BQ_NBK; o++) {
            v = (r + o) % BQ_NBK;// 每轮换个起手,免得固定顺序把漂移压在同一家身上
            a[v][r] = runs[v](nprod, per, cap, mpsc, batch);
        }
    }
    for (v = 0; v < BQ_NBK; v++) {
        m[v] = _bq_med(a[v], BQ_ROUNDS);
    }
    LOG_INFO("[bench_mpq] %-5s el=%2zuB cap=%6u prod=%d batch=%3u | %s=%5llums | %s=%5llums(%.2fx) | %s=%5llums(%.2fx)",
             what, elsize, cap, nprod, batch,
             _bq_bkname[BQ_SPIN], (unsigned long long)m[BQ_SPIN],
             _bq_bkname[BQ_MPQ], (unsigned long long)m[BQ_MPQ], _bq_x(m[BQ_SPIN], m[BQ_MPQ]),
             _bq_bkname[BQ_BBQ], (unsigned long long)m[BQ_BBQ], _bq_x(m[BQ_SPIN], m[BQ_BBQ]));
}

BQ_DEFINE(bq8, bq8)
BQ_DEFINE(bq72, bq72)

void bench_mpq(void) {
    int32_t prods[] = { 1, 2, 4, 8 };
    uint32_t caps[] = { 256, 1024, 4096 };// msgq / taskq / cmdq·twq·logq 三档
    uint32_t bszs[] = { 32, 128 };// TASK_MSG_BATCH / LOG_POP_BATCH
    int32_t np = (int32_t)(sizeof(prods) / sizeof(prods[0]));
    int32_t nc = (int32_t)(sizeof(caps) / sizeof(caps[0]));
    int32_t nb = (int32_t)(sizeof(bszs) / sizeof(bszs[0]));
    int32_t k, c, b;

    // 维度 1:纯入队争抢。容量装得下全部,环不会满,测的是抢入队位置本身
    LOG_INFO("[bench_mpq] === push-only, total=%d, rounds=%d (ring never fills) ===",
             BQ_PUSH_TOTAL, BQ_ROUNDS);
    for (k = 0; k < np; k++) {
        bq8_cell("push", prods[k], BQ_PUSH_TOTAL, BQ_PUSH_CAP, 0, 0);
    }
    for (k = 0; k < np; k++) {
        bq72_cell("push", prods[k], BQ_PUSH_TOTAL, BQ_PUSH_CAP, 0, 0);
    }
    // 维度 2:MPSC 逐条 pop,产线容量档。无锁环满了落溢出层
    LOG_INFO("[bench_mpq] === MPSC single pop, total=%d, rounds=%d ===", BQ_TOTAL, BQ_ROUNDS);
    for (c = 0; c < nc; c++) {
        for (k = 0; k < np; k++) {
            bq8_cell("mpsc", prods[k], BQ_TOTAL, caps[c], 1, 0);
        }
    }
    for (c = 0; c < nc; c++) {
        for (k = 0; k < np; k++) {
            bq72_cell("mpsc", prods[k], BQ_TOTAL, caps[c], 1, 0);
        }
    }
    // 维度 3:MPSC 批量 pop —— 产线真正在走的形状(loader / log / tw / cmd 都用 pop_sc_batch)
    LOG_INFO("[bench_mpq] === MPSC batch pop, total=%d, rounds=%d ===", BQ_TOTAL, BQ_ROUNDS);
    for (c = 1; c < nc; c++) {// 批量只看 1024 / 4096 两档
        for (b = 0; b < nb; b++) {
            bq8_cell("batch", 4, BQ_TOTAL, caps[c], 1, bszs[b]);
            bq8_cell("batch", 8, BQ_TOTAL, caps[c], 1, bszs[b]);
        }
    }
    for (c = 1; c < nc; c++) {
        for (b = 0; b < nb; b++) {
            bq72_cell("batch", 4, BQ_TOTAL, caps[c], 1, bszs[b]);
            bq72_cell("batch", 8, BQ_TOTAL, caps[c], 1, bszs[b]);
        }
    }
}
