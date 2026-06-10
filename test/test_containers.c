#include "test_containers.h"
#include "lib.h"

/* =======================================================================
 * mpq —— 无锁多生产者有界队列（消费者侧两种 API：mpq_pop 多消费者 / mpq_pop_sc 单消费者）
 * ======================================================================= */

/* 单线程：基本入队出队、FIFO 顺序（mpq_pop 路径） */
static void test_mpq_basic(CuTest *tc) {
    mpq_ctx q;
    uintptr_t v, out;
    mpq_init(&q, sizeof(uintptr_t), 0);    /* 0 → 默认容量 1024 */

    CuAssertTrue(tc, 1024 == q.capacity);
    CuAssertTrue(tc, 0 == mpq_size(&q));
    CuAssertTrue(tc, ERR_FAILED == mpq_pop(&q, &out));  /* 空队列出队返回 ERR_FAILED */

    /* 入队 10 个整数值 */
    for (uintptr_t i = 1; i <= 10; i++) {
        v = i;
        CuAssertTrue(tc, ERR_OK == mpq_trypush(&q, &v));
    }
    CuAssertTrue(tc, 10 == mpq_size(&q));

    /* FIFO 顺序验证 */
    for (uintptr_t i = 1; i <= 10; i++) {
        CuAssertTrue(tc, ERR_OK == mpq_pop(&q, &out) && out == i);
    }
    CuAssertTrue(tc, 0 == mpq_size(&q));
    CuAssertTrue(tc, ERR_FAILED == mpq_pop(&q, &out));
    mpq_free(&q);
}

/* 单线程：基本入队出队、FIFO 顺序（mpq_pop_sc 路径，独立验证单消费者算法） */
static void test_mpq_basic_sc(CuTest *tc) {
    mpq_ctx q;
    uintptr_t v, out;
    mpq_init(&q, sizeof(uintptr_t), 0);

    CuAssertTrue(tc, ERR_FAILED == mpq_pop_sc(&q, &out));   /* 空队列出队返回 ERR_FAILED */

    for (uintptr_t i = 1; i <= 10; i++) {
        v = i;
        CuAssertTrue(tc, ERR_OK == mpq_trypush(&q, &v));
    }
    for (uintptr_t i = 1; i <= 10; i++) {
        CuAssertTrue(tc, ERR_OK == mpq_pop_sc(&q, &out) && out == i);
    }
    CuAssertTrue(tc, 0 == mpq_size(&q));
    CuAssertTrue(tc, ERR_FAILED == mpq_pop_sc(&q, &out));
    mpq_free(&q);
}

/* 边界：队列填满后拒绝入队；非 2 的幂容量自动向上对齐 */
static void test_mpq_boundary(CuTest *tc) {
    mpq_ctx q;
    uintptr_t v, out;

    /* 容量 4，填满后 push 返回 ERR_FAILED */
    mpq_init(&q, sizeof(uintptr_t), 4);
    CuAssertTrue(tc, 4 == q.capacity);
    for (uintptr_t i = 1; i <= 4; i++) {
        v = i;
        CuAssertTrue(tc, ERR_OK == mpq_trypush(&q, &v));
    }
    v = 5;
    CuAssertTrue(tc, ERR_FAILED == mpq_trypush(&q, &v));

    /* 消费 2 个后可再入队 2 个 */
    CuAssertTrue(tc, ERR_OK == mpq_pop(&q, &out) && 1 == out);
    CuAssertTrue(tc, ERR_OK == mpq_pop(&q, &out) && 2 == out);
    v = 5; CuAssertTrue(tc, ERR_OK == mpq_trypush(&q, &v));
    v = 6; CuAssertTrue(tc, ERR_OK == mpq_trypush(&q, &v));
    v = 7; CuAssertTrue(tc, ERR_FAILED == mpq_trypush(&q, &v));

    /* 出队顺序 */
    CuAssertTrue(tc, ERR_OK == mpq_pop(&q, &out) && 3 == out);
    CuAssertTrue(tc, ERR_OK == mpq_pop(&q, &out) && 4 == out);
    CuAssertTrue(tc, ERR_OK == mpq_pop(&q, &out) && 5 == out);
    CuAssertTrue(tc, ERR_OK == mpq_pop(&q, &out) && 6 == out);
    CuAssertTrue(tc, ERR_FAILED == mpq_pop(&q, &out));
    mpq_free(&q);

    /* 容量 5（非 2 的幂）→ 自动对齐为 8 */
    mpq_init(&q, sizeof(uintptr_t), 5);
    CuAssertTrue(tc, 8 == q.capacity);
    for (uintptr_t i = 1; i <= 8; i++) {
        v = i;
        CuAssertTrue(tc, ERR_OK == mpq_trypush(&q, &v));
    }
    v = 9;
    CuAssertTrue(tc, ERR_FAILED == mpq_trypush(&q, &v));
    mpq_free(&q);
}

/* 并发用例共用的 producer / consumer 框架 */
#define _MPQ_PROD_CNT    4
#define _MPQ_CONS_CNT    4
#define _MPQ_ITEMS_EACH  50000
#define _MPQ_MC_TOTAL    (_MPQ_PROD_CNT * _MPQ_ITEMS_EACH)
#define _MPQ_SC_TOTAL    (_MPQ_PROD_CNT * _MPQ_ITEMS_EACH)

typedef struct { mpq_ctx *q; int id; } _mpq_prod_arg;

static void _mpq_producer(void *arg) {
    _mpq_prod_arg *a = (_mpq_prod_arg *)arg;
    uintptr_t start = (uintptr_t)a->id * _MPQ_ITEMS_EACH + 1;
    uintptr_t end   = start + _MPQ_ITEMS_EACH;
    uintptr_t v;
    for (v = start; v < end; v++) {
        mpq_push(a->q, &v);
    }
}

static atomic_t   _mpq_consumed;
static atomic64_t _mpq_sum;

/* 多消费者：走 mpq_pop（CAS 路径） */
static void _mpq_consumer_mc(void *arg) {
    mpq_ctx *q = (mpq_ctx *)arg;
    uintptr_t p;
    for (;;) {
        if (ERR_OK == mpq_pop(q, &p)) {
            uint32_t prev = ATOMIC_ADD(&_mpq_consumed, 1);
            ATOMIC64_ADD(&_mpq_sum, p);
            if (prev + 1 >= (uint32_t)_MPQ_MC_TOTAL) {
                break;
            }
        } else {
            if (ATOMIC_GET(&_mpq_consumed) >= (uint32_t)_MPQ_MC_TOTAL) {
                break;
            }
            CPU_PAUSE();
        }
    }
}

/* 单消费者：走 mpq_pop_sc（无 CAS 路径） */
static void _mpq_consumer_sc(void *arg) {
    mpq_ctx *q = (mpq_ctx *)arg;
    uintptr_t p;
    for (;;) {
        if (ERR_OK == mpq_pop_sc(q, &p)) {
            uint32_t prev = ATOMIC_ADD(&_mpq_consumed, 1);
            ATOMIC64_ADD(&_mpq_sum, p);
            if (prev + 1 >= (uint32_t)_MPQ_SC_TOTAL) {
                break;
            }
        } else {
            if (ATOMIC_GET(&_mpq_consumed) >= (uint32_t)_MPQ_SC_TOTAL) {
                break;
            }
            CPU_PAUSE();
        }
    }
}

/* 并发：4 生产者 × 4 消费者（mpq_pop），验证无丢失、无重复 */
static void test_mpq_concurrent_mc(CuTest *tc) {
    mpq_ctx q;
    mpq_init(&q, sizeof(uintptr_t), 1024);

    _mpq_consumed = 0;
    _mpq_sum      = 0;
    int64_t expected = (int64_t)_MPQ_MC_TOTAL * (_MPQ_MC_TOTAL + 1) / 2;

    pthread_t      producers[_MPQ_PROD_CNT];
    pthread_t      consumers[_MPQ_CONS_CNT];
    _mpq_prod_arg  pargs[_MPQ_PROD_CNT];
    int i;

    /* 先启动消费者，避免生产者长时间自旋 */
    for (i = 0; i < _MPQ_CONS_CNT; i++) {
        consumers[i] = thread_creat(_mpq_consumer_mc, &q);
    }
    for (i = 0; i < _MPQ_PROD_CNT; i++) {
        pargs[i].q  = &q;
        pargs[i].id = i;
        producers[i] = thread_creat(_mpq_producer, &pargs[i]);
    }
    for (i = 0; i < _MPQ_PROD_CNT; i++) thread_join(producers[i]);
    for (i = 0; i < _MPQ_CONS_CNT; i++) thread_join(consumers[i]);

    CuAssertTrue(tc, (uint32_t)_MPQ_MC_TOTAL == ATOMIC_GET(&_mpq_consumed));
    CuAssertTrue(tc, expected == (int64_t)ATOMIC64_GET(&_mpq_sum));
    mpq_free(&q);
}

/* 并发：4 生产者 × 1 消费者（mpq_pop_sc），验证无丢失、无重复 */
static void test_mpq_concurrent_sc(CuTest *tc) {
    mpq_ctx q;
    mpq_init(&q, sizeof(uintptr_t), 1024);

    _mpq_consumed = 0;
    _mpq_sum      = 0;
    int64_t expected = (int64_t)_MPQ_SC_TOTAL * (_MPQ_SC_TOTAL + 1) / 2;

    pthread_t      producers[_MPQ_PROD_CNT];
    pthread_t      consumer;
    _mpq_prod_arg  pargs[_MPQ_PROD_CNT];
    int i;

    consumer = thread_creat(_mpq_consumer_sc, &q);
    for (i = 0; i < _MPQ_PROD_CNT; i++) {
        pargs[i].q  = &q;
        pargs[i].id = i;
        producers[i] = thread_creat(_mpq_producer, &pargs[i]);
    }
    for (i = 0; i < _MPQ_PROD_CNT; i++) thread_join(producers[i]);
    thread_join(consumer);

    CuAssertTrue(tc, (uint32_t)_MPQ_SC_TOTAL == ATOMIC_GET(&_mpq_consumed));
    CuAssertTrue(tc, expected == (int64_t)ATOMIC64_GET(&_mpq_sum));
    mpq_free(&q);
}

/* =======================================================================
 * spsc —— 无锁单生产者单消费者有界队列
 * ======================================================================= */

/* 单线程：基本入队出队、FIFO 顺序 */
static void test_spsc_basic(CuTest *tc) {
    spsc_ctx q;
    uintptr_t v, out;
    spsc_init(&q, sizeof(uintptr_t), 0);    /* 0 → 默认容量 1024 */

    CuAssertTrue(tc, 1024 == q.capacity);
    CuAssertTrue(tc, 0 == spsc_size(&q));
    CuAssertTrue(tc, ERR_FAILED == spsc_pop(&q, &out));

    for (uintptr_t i = 1; i <= 10; i++) {
        v = i;
        CuAssertTrue(tc, ERR_OK == spsc_trypush(&q, &v));
    }
    CuAssertTrue(tc, 10 == spsc_size(&q));

    for (uintptr_t i = 1; i <= 10; i++) {
        CuAssertTrue(tc, ERR_OK == spsc_pop(&q, &out) && out == i);
    }
    CuAssertTrue(tc, 0 == spsc_size(&q));
    CuAssertTrue(tc, ERR_FAILED == spsc_pop(&q, &out));
    spsc_free(&q);
}

/* 边界：队列填满后拒绝入队；非 2 的幂容量自动向上对齐 */
static void test_spsc_boundary(CuTest *tc) {
    spsc_ctx q;
    uintptr_t v, out;

    spsc_init(&q, sizeof(uintptr_t), 4);
    CuAssertTrue(tc, 4 == q.capacity);
    for (uintptr_t i = 1; i <= 4; i++) {
        v = i;
        CuAssertTrue(tc, ERR_OK == spsc_trypush(&q, &v));
    }
    v = 5;
    CuAssertTrue(tc, ERR_FAILED == spsc_trypush(&q, &v));

    CuAssertTrue(tc, ERR_OK == spsc_pop(&q, &out) && 1 == out);
    CuAssertTrue(tc, ERR_OK == spsc_pop(&q, &out) && 2 == out);
    v = 5; CuAssertTrue(tc, ERR_OK == spsc_trypush(&q, &v));
    v = 6; CuAssertTrue(tc, ERR_OK == spsc_trypush(&q, &v));
    v = 7; CuAssertTrue(tc, ERR_FAILED == spsc_trypush(&q, &v));

    CuAssertTrue(tc, ERR_OK == spsc_pop(&q, &out) && 3 == out);
    CuAssertTrue(tc, ERR_OK == spsc_pop(&q, &out) && 4 == out);
    CuAssertTrue(tc, ERR_OK == spsc_pop(&q, &out) && 5 == out);
    CuAssertTrue(tc, ERR_OK == spsc_pop(&q, &out) && 6 == out);
    CuAssertTrue(tc, ERR_FAILED == spsc_pop(&q, &out));
    spsc_free(&q);

    /* 容量 5（非 2 的幂）→ 自动对齐为 8 */
    spsc_init(&q, sizeof(uintptr_t), 5);
    CuAssertTrue(tc, 8 == q.capacity);
    for (uintptr_t i = 1; i <= 8; i++) {
        v = i;
        CuAssertTrue(tc, ERR_OK == spsc_trypush(&q, &v));
    }
    v = 9;
    CuAssertTrue(tc, ERR_FAILED == spsc_trypush(&q, &v));
    spsc_free(&q);
}

/* 并发：1 生产者 × 1 消费者，验证无丢失、无重复 + FIFO 严格顺序 */
#define _SPSC_ITEMS    200000

static atomic_t _spsc_done_prod;

static void _spsc_producer(void *arg) {
    spsc_ctx *q = (spsc_ctx *)arg;
    uintptr_t v;
    for (v = 1; v <= _SPSC_ITEMS; v++) {
        spsc_push(q, &v);
    }
    ATOMIC_SET(&_spsc_done_prod, 1);
}

static atomic_t _spsc_fail;     /* 顺序违例计数 */
static atomic_t _spsc_consumed;

static void _spsc_consumer(void *arg) {
    spsc_ctx *q = (spsc_ctx *)arg;
    uintptr_t p;
    uintptr_t expected = 1;
    for (;;) {
        if (ERR_OK == spsc_pop(q, &p)) {
            if (p != expected) {
                ATOMIC_ADD(&_spsc_fail, 1);
            }
            expected++;
            uint32_t cnt = ATOMIC_ADD(&_spsc_consumed, 1) + 1;
            if (cnt >= (uint32_t)_SPSC_ITEMS) {
                break;
            }
        } else {
            if (ATOMIC_GET(&_spsc_done_prod)
                && ATOMIC_GET(&_spsc_consumed) >= (uint32_t)_SPSC_ITEMS) {
                break;
            }
            CPU_PAUSE();
        }
    }
}

static void test_spsc_concurrent(CuTest *tc) {
    spsc_ctx q;
    spsc_init(&q, sizeof(uintptr_t), 1024);

    ATOMIC_SET(&_spsc_done_prod, 0);
    ATOMIC_SET(&_spsc_fail, 0);
    ATOMIC_SET(&_spsc_consumed, 0);

    pthread_t cons = thread_creat(_spsc_consumer, &q);
    pthread_t prod = thread_creat(_spsc_producer, &q);
    thread_join(prod);
    thread_join(cons);

    CuAssertTrue(tc, (uint32_t)_SPSC_ITEMS == ATOMIC_GET(&_spsc_consumed));
    CuAssertTrue(tc, 0 == ATOMIC_GET(&_spsc_fail));   /* FIFO 顺序严格成立 */
    spsc_free(&q);
}

/* =======================================================================
 * fsqu —— 平台自适应队列（macOS: queue+spin / 其它: mpq）
 * 仅测单线程基本功能；多线程 MPSC/MPMC 由 test_srey 集成测试覆盖。
 * 用 capacity 8（2 的幂）使两个分支的 fsqu_capacity 均返回 8（mpq 按 2 的幂对齐）。
 * ======================================================================= */

/* 基本：init 后 size==0、capacity==8；push/pop 往返 FIFO 顺序与值一致；空队列 pop 返回 ERR_FAILED */
static void test_fsqu_basic(CuTest *tc) {
    fsqu_ctx q;
    int32_t v, out;
    fsqu_init(&q, sizeof(int32_t), 8);

    CuAssertTrue(tc, 0 == fsqu_size(&q));
    CuAssertTrue(tc, 8 == fsqu_capacity(&q));
    CuAssertTrue(tc, ERR_FAILED == fsqu_pop(&q, &out));   /* 空队列出队 */

    /* push 8 个，size 随之递增 */
    for (v = 1; v <= 8; v++) {
        fsqu_push(&q, &v);
        CuAssertTrue(tc, (uint32_t)v == fsqu_size(&q));
    }

    /* FIFO 顺序出队，值一致，size 随之递减 */
    for (v = 1; v <= 8; v++) {
        CuAssertTrue(tc, ERR_OK == fsqu_pop(&q, &out) && out == v);
        CuAssertTrue(tc, (uint32_t)(8 - v) == fsqu_size(&q));
    }
    CuAssertTrue(tc, 0 == fsqu_size(&q));
    CuAssertTrue(tc, ERR_FAILED == fsqu_pop(&q, &out));   /* 取空后再 pop */
    fsqu_free(&q);
}

/* trypush：填满到 capacity 后再 trypush 返回 ERR_FAILED；消费一个后又可入队一个 */
static void test_fsqu_trypush_full(CuTest *tc) {
    fsqu_ctx q;
    int32_t v, out;
    fsqu_init(&q, sizeof(int32_t), 8);

    for (v = 1; v <= 8; v++) {
        CuAssertTrue(tc, ERR_OK == fsqu_trypush(&q, &v));
    }
    CuAssertTrue(tc, 8 == fsqu_size(&q));
    v = 9;
    CuAssertTrue(tc, ERR_FAILED == fsqu_trypush(&q, &v));   /* 已满拒绝 */

    /* 消费 1 个后腾出空位，可再入队 1 个；继续满则再拒绝 */
    CuAssertTrue(tc, ERR_OK == fsqu_pop(&q, &out) && 1 == out);
    v = 9;
    CuAssertTrue(tc, ERR_OK == fsqu_trypush(&q, &v));
    v = 10;
    CuAssertTrue(tc, ERR_FAILED == fsqu_trypush(&q, &v));

    /* 剩余出队顺序为 2..8,9 */
    for (v = 2; v <= 9; v++) {
        CuAssertTrue(tc, ERR_OK == fsqu_pop(&q, &out) && out == v);
    }
    CuAssertTrue(tc, ERR_FAILED == fsqu_pop(&q, &out));
    fsqu_free(&q);
}

/* push_batch + pop_batch：批量入队 6 个、批量出队，返回实际数与顺序值正确；max 大于剩余只取剩余 */
static void test_fsqu_batch(CuTest *tc) {
    fsqu_ctx q;
    int32_t in[6] = { 10, 20, 30, 40, 50, 60 };
    int32_t out[8];
    uint32_t n;
    fsqu_init(&q, sizeof(int32_t), 8);

    fsqu_push_batch(&q, in, 6);
    CuAssertTrue(tc, 6 == fsqu_size(&q));

    /* 一次最多取 4 个 */
    n = fsqu_pop_batch(&q, out, 4);
    CuAssertTrue(tc, 4 == n);
    CuAssertTrue(tc, 10 == out[0] && 20 == out[1] && 30 == out[2] && 40 == out[3]);
    CuAssertTrue(tc, 2 == fsqu_size(&q));

    /* max=8 但只剩 2 个，返回 2 */
    n = fsqu_pop_batch(&q, out, 8);
    CuAssertTrue(tc, 2 == n);
    CuAssertTrue(tc, 50 == out[0] && 60 == out[1]);
    CuAssertTrue(tc, 0 == fsqu_size(&q));

    /* 空队列批量出队返回 0 */
    n = fsqu_pop_batch(&q, out, 8);
    CuAssertTrue(tc, 0 == n);
    fsqu_free(&q);
}

/* pop_sc + pop_sc_batch：单消费者出队，顺序值正确；空队列返回 ERR_FAILED / 0 */
static void test_fsqu_pop_sc(CuTest *tc) {
    fsqu_ctx q;
    int32_t v, out;
    int32_t outs[8];
    uint32_t n;
    fsqu_init(&q, sizeof(int32_t), 8);

    CuAssertTrue(tc, ERR_FAILED == fsqu_pop_sc(&q, &out));   /* 空队列单消费者出队 */

    for (v = 1; v <= 6; v++) {
        fsqu_push(&q, &v);
    }
    /* pop_sc 逐个出队顺序一致 */
    for (v = 1; v <= 3; v++) {
        CuAssertTrue(tc, ERR_OK == fsqu_pop_sc(&q, &out) && out == v);
    }
    CuAssertTrue(tc, 3 == fsqu_size(&q));

    /* pop_sc_batch 取走剩余 3 个 */
    n = fsqu_pop_sc_batch(&q, outs, 8);
    CuAssertTrue(tc, 3 == n);
    CuAssertTrue(tc, 4 == outs[0] && 5 == outs[1] && 6 == outs[2]);
    CuAssertTrue(tc, 0 == fsqu_size(&q));

    /* 空队列单消费者批量出队返回 0 */
    n = fsqu_pop_sc_batch(&q, outs, 8);
    CuAssertTrue(tc, 0 == n);
    fsqu_free(&q);
}

/* 默认容量：capacity=0 走默认值，仍可正常 push/pop */
static void test_fsqu_default_cap(CuTest *tc) {
    fsqu_ctx q;
    int32_t v, out;
    fsqu_init(&q, sizeof(int32_t), 0);   /* 0 → 默认容量 */

    CuAssertTrue(tc, fsqu_capacity(&q) > 0);
    CuAssertTrue(tc, 0 == fsqu_size(&q));
    for (v = 1; v <= 16; v++) {
        CuAssertTrue(tc, ERR_OK == fsqu_trypush(&q, &v));
    }
    CuAssertTrue(tc, 16 == fsqu_size(&q));
    for (v = 1; v <= 16; v++) {
        CuAssertTrue(tc, ERR_OK == fsqu_pop(&q, &out) && out == v);
    }
    CuAssertTrue(tc, 0 == fsqu_size(&q));
    fsqu_free(&q);
}

/* =======================================================================
 * chan —— 多生产/多消费 buffered chan 并发回归
 * f0a94c5 修复：_buffered_chan_recv 拷贝 msg->data/lens 到栈再 unlock，
 * 防止满载循环槽位被 push 覆盖；本用例用紧 buffer + 多 PC 校验消息无丢失/重复/串扰。
 * ======================================================================= */
#define _CHAN_RACE_CAP       4    // 紧 buffer，强制 producer 等 consumer 取走再 push
#define _CHAN_RACE_PRODS     4
#define _CHAN_RACE_CONSS     4
#define _CHAN_RACE_PER_PROD  500
#define _CHAN_RACE_TOTAL     (_CHAN_RACE_PRODS * _CHAN_RACE_PER_PROD)

typedef struct _chan_race_msg {
    int32_t pid;
    int32_t seq;
    char padding[24];   // 32 字节，验证 chan 不串扰 padding
}_chan_race_msg;

typedef struct _chan_race_prod_arg {
    chan_ctx *chan;
    int32_t pid;
}_chan_race_prod_arg;

static atomic_t _chan_race_consumed;
static int32_t _chan_race_recv[_CHAN_RACE_PRODS][_CHAN_RACE_PER_PROD];
static mutex_ctx _chan_race_mu;

static void _chan_race_producer(void *arg) {
    _chan_race_prod_arg *p = (_chan_race_prod_arg *)arg;
    _chan_race_msg msg;
    int32_t i;
    for (i = 0; i < _CHAN_RACE_PER_PROD; i++) {
        ZERO(&msg, sizeof(msg));
        msg.pid = p->pid;
        msg.seq = i;
        memset(msg.padding, 0xAB, sizeof(msg.padding));
        chan_send(p->chan, &msg, sizeof(msg), 1);   // copy=1，chan 持有 heap 副本
    }
}

static void _chan_race_consumer(void *arg) {
    chan_ctx *chan = (chan_ctx *)arg;
    size_t lens;
    _chan_race_msg *msg;
    int32_t i;
    while (1) {
        msg = (_chan_race_msg *)chan_recv(chan, &lens);
        if (NULL == msg) {
            // chan_close 后 recv 返回 NULL，consumer 退出
            break;
        }
        if (lens == sizeof(_chan_race_msg)
            && msg->pid >= 0 && msg->pid < _CHAN_RACE_PRODS
            && msg->seq >= 0 && msg->seq < _CHAN_RACE_PER_PROD) {
            mutex_lock(&_chan_race_mu);
            _chan_race_recv[msg->pid][msg->seq]++;
            mutex_unlock(&_chan_race_mu);
            // 同时验证 padding 没被串扰
            for (i = 0; i < (int32_t)sizeof(msg->padding); i++) {
                if ((unsigned char)0xAB != (unsigned char)msg->padding[i]) {
                    mutex_lock(&_chan_race_mu);
                    _chan_race_recv[msg->pid][msg->seq] = -1;
                    mutex_unlock(&_chan_race_mu);
                    break;
                }
            }
        }
        FREE(msg);
        ATOMIC_ADD(&_chan_race_consumed, 1);
    }
}

static void test_chan_buffered_race(CuTest *tc) {
    chan_ctx *chan = chan_init(_CHAN_RACE_CAP);
    CuAssertPtrNotNull(tc, chan);

    ZERO(_chan_race_recv, sizeof(_chan_race_recv));
    mutex_init(&_chan_race_mu);
    ATOMIC_SET(&_chan_race_consumed, 0);

    pthread_t conss[_CHAN_RACE_CONSS];
    pthread_t prods[_CHAN_RACE_PRODS];
    _chan_race_prod_arg pargs[_CHAN_RACE_PRODS];

    int32_t i;
    // 先消费者，避免 producer 满载阻塞太久
    for (i = 0; i < _CHAN_RACE_CONSS; i++) {
        conss[i] = thread_creat(_chan_race_consumer, chan);
    }
    for (i = 0; i < _CHAN_RACE_PRODS; i++) {
        pargs[i].chan = chan;
        pargs[i].pid = i;
        prods[i] = thread_creat(_chan_race_producer, &pargs[i]);
    }
    for (i = 0; i < _CHAN_RACE_PRODS; i++) {
        thread_join(prods[i]);
    }
    // 等 consumer 把全部消息取走
    while ((int32_t)ATOMIC_GET(&_chan_race_consumed) < _CHAN_RACE_TOTAL) {
        CPU_PAUSE();
    }
    chan_close(chan);
    for (i = 0; i < _CHAN_RACE_CONSS; i++) {
        thread_join(conss[i]);
    }

    CuAssertTrue(tc, _CHAN_RACE_TOTAL == (int32_t)ATOMIC_GET(&_chan_race_consumed));
    // 每个 (pid, seq) 应正好被收到一次；race 会表现为重复/丢失/-1（padding 串扰）
    int32_t bad = 0;
    int32_t p, s;
    for (p = 0; p < _CHAN_RACE_PRODS; p++) {
        for (s = 0; s < _CHAN_RACE_PER_PROD; s++) {
            if (1 != _chan_race_recv[p][s]) {
                bad++;
            }
        }
    }
    CuAssertTrue(tc, 0 == bad);
    mutex_free(&_chan_race_mu);
    chan_free(chan);
}

/* =======================================================================
 * hashmap
 * ======================================================================= */

typedef struct { char key[32]; int val; } _kv;

static uint64_t _kv_hash(const void *item, uint64_t s0, uint64_t s1) {
    const _kv *kv = (const _kv *)item;
    return hashmap_sip(kv->key, strlen(kv->key), s0, s1);
}
static int _kv_cmp(const void *a, const void *b, void *udata) {
    (void)udata;
    return strcmp(((const _kv *)a)->key, ((const _kv *)b)->key);
}

static void test_hashmap(CuTest *tc) {
    struct hashmap *map = hashmap_new(sizeof(_kv), 0, 0, 0,
                                     _kv_hash, _kv_cmp, NULL, NULL);
    CuAssertPtrNotNull(tc, map);
    CuAssertTrue(tc, 0 == hashmap_count(map));

    /* 插入 100 条记录 */
    char key[32];
    for (int i = 0; i < 100; i++) {
        SNPRINTF(key, sizeof(key), "key_%d", i);
        _kv kv;
        SNPRINTF(kv.key, sizeof(kv.key), "key_%d", i);
        kv.val = i * 10;
        hashmap_set(map, &kv);
    }
    CuAssertTrue(tc, 100 == (int)hashmap_count(map));

    /* 查找验证 */
    for (int i = 0; i < 100; i++) {
        _kv lookup;
        SNPRINTF(lookup.key, sizeof(lookup.key), "key_%d", i);
        const _kv *found = (const _kv *)hashmap_get(map, &lookup);
        CuAssertPtrNotNull(tc, found);
        CuAssertTrue(tc, i * 10 == found->val);
    }

    /* 查找不存在的 key */
    _kv miss;
    SNPRINTF(miss.key, sizeof(miss.key), "not_exist");
    CuAssertTrue(tc, NULL == hashmap_get(map, &miss));

    /* 删除，数量减少 */
    _kv del;
    SNPRINTF(del.key, sizeof(del.key), "key_0");
    hashmap_delete(map, &del);
    CuAssertTrue(tc, 99 == (int)hashmap_count(map));
    CuAssertTrue(tc, NULL == hashmap_get(map, &del));

    /* 更新：同 key 再次 set 覆盖旧值 */
    _kv upd;
    SNPRINTF(upd.key, sizeof(upd.key), "key_1");
    upd.val = 9999;
    hashmap_set(map, &upd);
    const _kv *got = (const _kv *)hashmap_get(map, &upd);
    CuAssertTrue(tc, 9999 == got->val);
    CuAssertTrue(tc, 99 == (int)hashmap_count(map));

    hashmap_free(map);
}

/* =======================================================================
 * heap —— 最小堆（compare 返回非零表示 lhs 优先于 rhs）
 * ======================================================================= */

typedef struct { heap_node node; int val; } _hnode;

static int _heap_lt(const heap_node *a, const heap_node *b) {
    return UPCAST(a, _hnode, node)->val < UPCAST(b, _hnode, node)->val;
}

static void test_heap(CuTest *tc) {
    heap_ctx h;
    heap_init(&h, _heap_lt);
    CuAssertTrue(tc, 0 == h.nelts);

    /* 无序插入 5 个节点 */
    int vals[] = { 30, 10, 50, 20, 40 };
    _hnode nodes[5];
    /* heap_insert 不清零子指针，栈变量必须手动清零，否则 sift-up 时
     * _heap_swap 会把垃圾 child->left/right 当合法指针解引用 */
    ZERO(nodes, sizeof(nodes));
    for (int i = 0; i < 5; i++) {
        nodes[i].val = vals[i];
        heap_insert(&h, &nodes[i].node);
    }
    CuAssertTrue(tc, 5 == h.nelts);

    /* 堆顶始终是最小值 */
    CuAssertTrue(tc, 10 == UPCAST(h.root, _hnode, node)->val);

    /* 逐个出堆，顺序应为升序 */
    int expected[] = { 10, 20, 30, 40, 50 };
    for (int i = 0; i < 5; i++) {
        CuAssertTrue(tc, expected[i] == UPCAST(h.root, _hnode, node)->val);
        heap_dequeue(&h);
    }
    CuAssertTrue(tc, 0 == h.nelts);

    /* 插入后随机删除中间节点 */
    /* heap_dequeue 最后一次摘除单元素时走提前返回路径，未清零节点指针；
     * 重复使用同一批节点前需再次清零，避免残留指针引发 sift 崩溃 */
    ZERO(nodes, sizeof(nodes));
    for (int i = 0; i < 5; i++) {
        nodes[i].val = vals[i];
        heap_insert(&h, &nodes[i].node);
    }
    /* 删除值为 20 的节点（nodes[3]）*/
    heap_remove(&h, &nodes[3].node);
    CuAssertTrue(tc, 4 == h.nelts);
    /* 堆顶仍是 10 */
    CuAssertTrue(tc, 10 == UPCAST(h.root, _hnode, node)->val);
}

/* =======================================================================
 * queue_ctx —— 环形队列（自动扩容）
 * ======================================================================= */

static void test_queue(CuTest *tc) {
    queue_ctx q;
    queue_init(&q, sizeof(int), 4);

    CuAssertTrue(tc, 0 == queue_size(&q));
    CuAssertTrue(tc, 4 == queue_maxsize(&q));
    CuAssertTrue(tc, queue_empty(&q));
    CuAssertTrue(tc, NULL == queue_pop(&q));
    CuAssertTrue(tc, NULL == queue_peek(&q));

    /* 入队 5 个，超出初始容量后自动扩容 */
    for (int i = 0; i < 5; i++) {
        queue_push(&q, &i);
    }
    CuAssertTrue(tc, 5 == (int)queue_size(&q));

    /* peek 不改变队列大小 */
    CuAssertTrue(tc, 0 == *(int *)queue_peek(&q));
    CuAssertTrue(tc, 5 == (int)queue_size(&q));

    /* FIFO 顺序出队 */
    for (int i = 0; i < 5; i++) {
        CuAssertTrue(tc, i == *(int *)queue_pop(&q));
    }
    CuAssertTrue(tc, queue_empty(&q));

    /* at() 访问 */
    for (int i = 0; i < 8; i++) {
        queue_push(&q, &i);
    }
    CuAssertTrue(tc, 0 == *(int *)queue_at(&q, 0));
    CuAssertTrue(tc, 7 == *(int *)queue_at(&q, 7));
    CuAssertTrue(tc, NULL == queue_at(&q, 8));

    /* clear 后队列为空 */
    queue_clear(&q);
    CuAssertTrue(tc, queue_empty(&q));

    queue_free(&q);
}

/* =======================================================================
 * array_ctx —— 动态数组（按值定长元素）
 * ======================================================================= */

// 以 int 元素覆盖核心 API：init/push_back/扩容/swap/add/del/pop_back/del_nomove/clear
static void test_array(CuTest *tc) {
    array_ctx a;
    array_init(&a, sizeof(int), 4);

    CuAssertTrue(tc, 0 == array_size(&a));
    CuAssertTrue(tc, 4 == a.maxsize);
    CuAssertTrue(tc, array_empty(&a));
    CuAssertTrue(tc, NULL == array_front(&a));
    CuAssertTrue(tc, NULL == array_back(&a));

    /* push_back 超出容量后自动扩容 */
    for (int i = 1; i <= 8; i++) {
        array_push_back(&a, &i);
    }
    CuAssertTrue(tc, 8 == (int)array_size(&a));
    CuAssertTrue(tc, 1 == *(int *)array_front(&a));
    CuAssertTrue(tc, 8 == *(int *)array_back(&a));
    CuAssertTrue(tc, 3 == *(int *)array_at(&a, 2));

    /* 交换 */
    array_swap(&a, 0, 7);
    CuAssertTrue(tc, 8 == *(int *)array_front(&a));
    CuAssertTrue(tc, 1 == *(int *)array_back(&a));
    array_swap(&a, 0, 7);

    /* 在指定位置插入 */
    int v = 99;
    array_add(&a, &v, 2);
    CuAssertTrue(tc, 99 == *(int *)array_at(&a, 2));
    CuAssertTrue(tc, 9 == (int)array_size(&a));

    /* 删除（保持顺序）*/
    array_del(&a, 2);
    CuAssertTrue(tc, 3 == *(int *)array_at(&a, 2));
    CuAssertTrue(tc, 8 == (int)array_size(&a));

    /* pop_back */
    CuAssertTrue(tc, 8 == *(int *)array_pop_back(&a));
    CuAssertTrue(tc, 7 == (int)array_size(&a));

    /* del_nomove：用末尾元素填充被删位置 */
    array_del_nomove(&a, 0);
    CuAssertTrue(tc, 7 == *(int *)array_front(&a));   /* 末尾元素移到首位 */
    CuAssertTrue(tc, 6 == (int)array_size(&a));

    /* clear 不释放内存 */
    array_clear(&a);
    CuAssertTrue(tc, array_empty(&a));
    CuAssertTrue(tc, NULL == array_pop_back(&a));

    array_free(&a);
}

// 以 void * 元素再过一遍，验证指针类型场景同样工作
static void test_array_ptr(CuTest *tc) {
    array_ctx a;
    array_init(&a, sizeof(void *), 4);

    CuAssertTrue(tc, 0 == array_size(&a));
    CuAssertTrue(tc, array_empty(&a));
    CuAssertTrue(tc, NULL == array_front(&a));
    CuAssertTrue(tc, NULL == array_back(&a));

    /* push_back 超出初始容量后自动扩容 */
    void *p1 = (void *)0x1111;
    void *p2 = (void *)0x2222;
    void *p3 = (void *)0x3333;
    void *p4 = (void *)0x4444;
    void *p5 = (void *)0x5555;
    array_push_back(&a, &p1);
    array_push_back(&a, &p2);
    array_push_back(&a, &p3);
    array_push_back(&a, &p4);
    array_push_back(&a, &p5); /* 触发扩容 */

    CuAssertTrue(tc, 5 == array_size(&a));
    CuAssertTrue(tc, p1 == *(void **)array_front(&a));
    CuAssertTrue(tc, p5 == *(void **)array_back(&a));
    CuAssertTrue(tc, p3 == *(void **)array_at(&a, 2));

    /* pop_back */
    CuAssertTrue(tc, p5 == *(void **)array_pop_back(&a));
    CuAssertTrue(tc, 4 == array_size(&a));

    /* del_nomove：用末尾元素填充被删位置 */
    array_del_nomove(&a, 0);
    CuAssertTrue(tc, p4 == *(void **)array_front(&a));
    CuAssertTrue(tc, 3 == array_size(&a));

    /* swap：交换两个位置（当前数组为 [p4, p2, p3]，交换 0 和 1 → [p2, p4, p3]）*/
    array_swap(&a, 0, 1);
    CuAssertTrue(tc, p2 == *(void **)array_front(&a));
    array_swap(&a, 0, 1); /* 换回 [p4, p2, p3] */

    /* clear 后为空 */
    array_clear(&a);
    CuAssertTrue(tc, array_empty(&a));
    CuAssertTrue(tc, NULL == array_pop_back(&a));

    array_free(&a);
}

/* =======================================================================
 * hashmap —— scan / iter / clear
 * ======================================================================= */

static int _scan_sum;

static bool _scan_cb(const void *item, void *udata) {
    (void)udata;
    _scan_sum += ((const _kv *)item)->val;
    return true; /* true=继续遍历 */
}

static bool _scan_stop_cb(const void *item, void *udata) {
    int *count = (int *)udata;
    (*count)++;
    (void)item;
    return (*count < 3); /* 访问 3 个后停止 */
}

static void test_hashmap_scan_iter(CuTest *tc) {
    struct hashmap *map = hashmap_new(sizeof(_kv), 0, 0, 0,
                                     _kv_hash, _kv_cmp, NULL, NULL);
    /* 插入 5 条，val 为 1~5 */
    for (int i = 1; i <= 5; i++) {
        _kv kv;
        SNPRINTF(kv.key, sizeof(kv.key), "k%d", i);
        kv.val = i;
        hashmap_set(map, &kv);
    }
    CuAssertTrue(tc, 5 == (int)hashmap_count(map));

    /* scan：遍历所有元素，求和 = 1+2+3+4+5 = 15 */
    _scan_sum = 0;
    hashmap_scan(map, _scan_cb, NULL);
    CuAssertTrue(tc, 15 == _scan_sum);

    /* scan 提前终止：回调返回 false 时停止，访问计数为 3 */
    int count = 0;
    hashmap_scan(map, _scan_stop_cb, &count);
    CuAssertTrue(tc, 3 == count);

    /* iter：用游标方式遍历所有元素 */
    size_t i = 0;
    void *item;
    int iter_count = 0;
    while (hashmap_iter(map, &i, &item)) {
        iter_count++;
    }
    CuAssertTrue(tc, 5 == iter_count);

    /* clear(false) 后计数为 0，仍可重新插入 */
    hashmap_clear(map, false);
    CuAssertTrue(tc, 0 == (int)hashmap_count(map));
    _kv kv;
    SNPRINTF(kv.key, sizeof(kv.key), "after_clear");
    kv.val = 999;
    hashmap_set(map, &kv);
    CuAssertTrue(tc, 1 == (int)hashmap_count(map));

    hashmap_free(map);
}

/* =======================================================================
 * hashmap 边界与高级 API：
 *   - hashmap_clear(true) 容量回缩到初始
 *   - hashmap_*_with_hash 预算哈希变体（用 sip/murmur/xxhash3 计算后传入）
 *   - hashmap_probe 直接位置探针访问
 *   - hashmap_set_grow_by_power / hashmap_set_load_factor 配置
 *   - hashmap_oom 标志
 * ======================================================================= */
static void test_hashmap_with_hash_variants(CuTest *tc) {
    struct hashmap *map = hashmap_new(sizeof(_kv), 0, 0, 0,
                                     _kv_hash, _kv_cmp, NULL, NULL);
    CuAssertPtrNotNull(tc, map);

    /* hashmap_set_with_hash：调用方预算 sip 哈希后直接传入 */
    _kv kv;
    SNPRINTF(kv.key, sizeof(kv.key), "alpha");
    kv.val = 42;
    uint64_t h = hashmap_sip(kv.key, strlen(kv.key), 0, 0);
    CuAssertTrue(tc, NULL == hashmap_set_with_hash(map, &kv, h));
    CuAssertTrue(tc, 1 == (int)hashmap_count(map));

    /* hashmap_get_with_hash：用同一 hash 查回 */
    _kv lookup;
    SNPRINTF(lookup.key, sizeof(lookup.key), "alpha");
    uint64_t hl = hashmap_sip(lookup.key, strlen(lookup.key), 0, 0);
    const _kv *got = (const _kv *)hashmap_get_with_hash(map, &lookup, hl);
    CuAssertPtrNotNull(tc, got);
    CuAssertTrue(tc, 42 == got->val);

    /* hashmap_delete_with_hash */
    const _kv *removed = (const _kv *)hashmap_delete_with_hash(map, &lookup, hl);
    CuAssertPtrNotNull(tc, removed);
    CuAssertTrue(tc, 0 == (int)hashmap_count(map));

    /* hashmap_oom 默认 false（无 OOM 发生） */
    CuAssertTrue(tc, !hashmap_oom(map));

    /* murmur / xxhash3 替代哈希函数：仅验证不同输入产生不同输出 */
    uint64_t hm1 = hashmap_murmur("abc", 3, 0, 0);
    uint64_t hm2 = hashmap_murmur("abd", 3, 0, 0);
    CuAssertTrue(tc, hm1 != hm2);
    uint64_t hx1 = hashmap_xxhash3("abc", 3, 0, 0);
    uint64_t hx2 = hashmap_xxhash3("abd", 3, 0, 0);
    CuAssertTrue(tc, hx1 != hx2);

    hashmap_free(map);
}

static void test_hashmap_clear_update_cap(CuTest *tc) {
    /* 大容量初始化后插入再 clear(true)：容量回缩到初始 */
    struct hashmap *map = hashmap_new(sizeof(_kv), 16, 0, 0,
                                     _kv_hash, _kv_cmp, NULL, NULL);
    CuAssertPtrNotNull(tc, map);

    /* 触发扩容：插入 256 条 */
    for (int i = 0; i < 256; i++) {
        _kv kv;
        SNPRINTF(kv.key, sizeof(kv.key), "k_%d", i);
        kv.val = i;
        hashmap_set(map, &kv);
    }
    CuAssertTrue(tc, 256 == (int)hashmap_count(map));

    /* clear(true) → 计数清零，并将容量降至初始 cap */
    hashmap_clear(map, true);
    CuAssertTrue(tc, 0 == (int)hashmap_count(map));

    /* 清空后仍可插入 */
    _kv k2;
    SNPRINTF(k2.key, sizeof(k2.key), "after_resize");
    k2.val = 1;
    hashmap_set(map, &k2);
    CuAssertTrue(tc, 1 == (int)hashmap_count(map));

    /* hashmap_set_grow_by_power / hashmap_set_load_factor：不应崩溃 */
    hashmap_set_grow_by_power(map, 2);
    hashmap_set_load_factor(map, 0.8);

    /* 再插入大量数据，扩容仍正常工作 */
    for (int i = 0; i < 100; i++) {
        _kv kv;
        SNPRINTF(kv.key, sizeof(kv.key), "extra_%d", i);
        kv.val = i;
        hashmap_set(map, &kv);
    }
    CuAssertTrue(tc, 101 == (int)hashmap_count(map));

    /* hashmap_probe：按 buckets 索引访问 */
    int probed = 0;
    for (size_t pos = 0; pos < 64 && probed < 5; pos++) {
        const void *item = hashmap_probe(map, pos);
        if (NULL != item) {
            probed++;
        }
    }
    CuAssertTrue(tc, probed > 0);

    hashmap_free(map);
}

/* ======================================================================= */

void test_containers(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_mpq_basic);
    SUITE_ADD_TEST(suite, test_mpq_basic_sc);
    SUITE_ADD_TEST(suite, test_mpq_boundary);
    SUITE_ADD_TEST(suite, test_mpq_concurrent_mc);
    SUITE_ADD_TEST(suite, test_mpq_concurrent_sc);
    SUITE_ADD_TEST(suite, test_spsc_basic);
    SUITE_ADD_TEST(suite, test_spsc_boundary);
    SUITE_ADD_TEST(suite, test_spsc_concurrent);
    SUITE_ADD_TEST(suite, test_fsqu_basic);
    SUITE_ADD_TEST(suite, test_fsqu_trypush_full);
    SUITE_ADD_TEST(suite, test_fsqu_batch);
    SUITE_ADD_TEST(suite, test_fsqu_pop_sc);
    SUITE_ADD_TEST(suite, test_fsqu_default_cap);
    SUITE_ADD_TEST(suite, test_chan_buffered_race);
    SUITE_ADD_TEST(suite, test_hashmap);
    SUITE_ADD_TEST(suite, test_hashmap_scan_iter);
    SUITE_ADD_TEST(suite, test_hashmap_with_hash_variants);
    SUITE_ADD_TEST(suite, test_hashmap_clear_update_cap);
    SUITE_ADD_TEST(suite, test_heap);
    SUITE_ADD_TEST(suite, test_queue);
    SUITE_ADD_TEST(suite, test_array);
    SUITE_ADD_TEST(suite, test_array_ptr);
}
