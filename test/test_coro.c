#include "test_coro.h"
#include "lib.h"

// 不起 loader，在当前线程上直接驱动 coro_ctx。
// 协程里不许 CuAssert：失败走 longjmp 会跳出协程栈。结果一律记进参数，跑完、清理完再断言；
// 每个用例结束前把挂起的协程全部唤醒，否则 coro_free 收不回它们的栈

#define CO_OVERFLOW_N 130 // 比协程池容量(coro.c 的 COROPOOL_CAP=128)多，逼出回池失败

// 在新协程里跑的一段测试动作
typedef void (*co_step)(coro_ctx *co, void *arg);
typedef struct co_job {
    co_step fn;
    void *arg;
}co_job;
// owner：数 before_run / after_run 各被调了几次
typedef struct co_owner {
    int32_t before;
    int32_t after;
    coro_ctx *co;
}co_owner;
// 一次 coro_wait 的参数与结果
typedef struct co_waiter {
    int32_t tag;
    int32_t keep;
    int32_t woke;      // 醒来的次序(从 1 起)，0 = 还没醒
    uint32_t ms;
    uint64_t sess;
    void *payload;     // 醒来拿到的数据
}co_waiter;
// coro_expire 的 ud：记 mk 被调的情况，msg 是交给到期者的数据
typedef struct co_timeout {
    int32_t calls;
    int32_t tag;
    int32_t msg;
    uint64_t sess;
}co_timeout;
// fork_wait 用例的参数
typedef struct co_forkwait {
    int32_t cnt;
    int32_t done;
    int32_t rtn;
    co_waiter w;
}co_forkwait;
// 协程里起协程用例的参数
typedef struct co_nested {
    int32_t running;   // 子协程挂起回到父协程后，父协程是否仍认得自己在协程里
    co_waiter child;
    co_waiter parent;
}co_nested;
// coro_serial_call 用例的参数
typedef struct co_scall {
    int32_t cnt;
    int32_t r1;
    int32_t r2;
    coro_serial_ctx *s;
}co_scall;
// serial 用例的参数
typedef struct co_serial {
    int32_t n;
    int32_t rtn;
    int32_t order[4];
    coro_serial_ctx *s;
    co_waiter w;
}co_serial;

static int32_t _seq;// 醒来次序

static void _co_run(void *owner, void *payload) {
    co_job job = *(co_job *)payload;// payload 在调用方栈上，先拷走
    job.fn(((co_owner *)owner)->co, job.arg);
}
static void _co_before(void *owner) {
    ((co_owner *)owner)->before++;
}
static void _co_after(void *owner) {
    ((co_owner *)owner)->after++;
}
static const char *_co_tagstr(int32_t tag) {
    return 1 == tag ? "ONE" : "OTHER";
}
static coro_ctx *_co_new(co_owner *o, int32_t withtag) {
    coro_hooks hooks = { _co_run, _co_before, _co_after, withtag ? _co_tagstr : NULL };
    ZERO(o, sizeof(co_owner));
    o->co = coro_new(&hooks, o, 0);
    _seq = 0;
    return o->co;
}
static void _co_spawn(coro_ctx *co, co_step fn, void *arg) {
    co_job job = { fn, arg };
    coro_spawn(co, &job);
}
static void _co_waiter_set(co_waiter *w, uint64_t sess, int32_t tag, int32_t keep, uint32_t ms) {
    ZERO(w, sizeof(co_waiter));
    w->sess = sess;
    w->tag = tag;
    w->keep = keep;
    w->ms = ms;
}
static void _step_wait(coro_ctx *co, void *arg) {
    co_waiter *w = arg;
    w->payload = coro_wait(co, w->sess, w->tag, w->keep, w->ms);
    w->woke = ++_seq;
}
// 从 coro_dump 末行抠出 sess 条目数；取不到返回 -1
static int32_t _co_sessions(coro_ctx *co) {
    size_t lens = 0;
    char *dump = coro_dump(co, &lens);
    int32_t n = -1;
    char *p = strstr(dump, " sessions");
    if (NULL != p) {
        while (p > dump && ' ' != p[-1]) {
            p--;
        }
        n = atoi(p);
    }
    FREE(dump);
    return n;
}

// 唤醒只认队头且标签要对上；before_run / after_run 成对
static void test_coro_wait_wake(CuTest *tc) {
    co_owner o;
    coro_ctx *co = _co_new(&o, 0);
    co_waiter w;
    int32_t val = 0;
    _co_waiter_set(&w, 1, 7, 0, 0);
    _co_spawn(co, _step_wait, &w);
    int32_t susp = coro_suspended(co);
    int32_t running = coro_running(co);
    int32_t miss = coro_wake(co, 1, 8, &val);// 标签不符
    int32_t hit = coro_wake(co, 1, 7, &val);
    int32_t again = coro_wake(co, 1, 7, &val);// 已经摘走
    int32_t susp_end = coro_suspended(co);
    int32_t sessions = _co_sessions(co);
    void *owner = coro_owner(co);
    coro_free(co);
    CuAssertIntEquals(tc, 1, susp);
    CuAssertIntEquals(tc, 0, running);
    CuAssertIntEquals(tc, ERR_FAILED, miss);
    CuAssertIntEquals(tc, ERR_OK, hit);
    CuAssertIntEquals(tc, ERR_FAILED, again);
    CuAssertIntEquals(tc, 1, w.woke);
    CuAssertPtrEquals(tc, &val, w.payload);
    CuAssertIntEquals(tc, 0, susp_end);
    CuAssertIntEquals(tc, 0, sessions);
    CuAssertPtrEquals(tc, &o, owner);
    CuAssertIntEquals(tc, 1, o.before);
    CuAssertIntEquals(tc, 1, o.after);
}
// ② 只看队头：队头标签不符就当没有等待者，不越过它去唤醒后面的
static void test_coro_fifo_head_only(CuTest *tc) {
    co_owner o;
    coro_ctx *co = _co_new(&o, 0);
    co_waiter a, b;
    _co_waiter_set(&a, 2, 1, 0, 0);
    _co_waiter_set(&b, 2, 2, 0, 0);
    _co_spawn(co, _step_wait, &a);
    _co_spawn(co, _step_wait, &b);
    int32_t r1 = coro_wake(co, 2, 2, NULL);
    int32_t r2 = coro_wake(co, 2, 1, NULL);
    int32_t r3 = coro_wake(co, 2, 2, NULL);
    coro_free(co);
    CuAssertIntEquals(tc, ERR_FAILED, r1);
    CuAssertIntEquals(tc, ERR_OK, r2);
    CuAssertIntEquals(tc, ERR_OK, r3);
    CuAssertIntEquals(tc, 1, a.woke);
    CuAssertIntEquals(tc, 2, b.woke);
}
// ① keep 只在新建条目时定；③ 摘空且 !keep 才删
static void test_coro_keep(CuTest *tc) {
    co_owner o;
    coro_ctx *co = _co_new(&o, 0);
    co_waiter k, x, y;
    _co_waiter_set(&k, 3, 1, 1, 0);
    _co_spawn(co, _step_wait, &k);
    coro_wake(co, 3, 1, NULL);
    int32_t kept = _co_sessions(co);// keep 条目摘空后仍在
    coro_wake_all(co, 3, NULL, NULL);
    int32_t gone = _co_sessions(co);// 只有广播能删它
    _co_waiter_set(&x, 4, 1, 0, 0);// keep=0 先建条目
    _co_waiter_set(&y, 4, 1, 1, 0);// keep=1 追加，不改条目的 keep
    _co_spawn(co, _step_wait, &x);
    _co_spawn(co, _step_wait, &y);
    coro_wake(co, 4, 1, NULL);
    int32_t mid = _co_sessions(co);
    coro_wake(co, 4, 1, NULL);
    int32_t end = _co_sessions(co);
    coro_free(co);
    CuAssertIntEquals(tc, 1, k.woke);
    CuAssertIntEquals(tc, 1, kept);
    CuAssertIntEquals(tc, 0, gone);
    CuAssertIntEquals(tc, 1, mid);
    CuAssertIntEquals(tc, 0, end);
    CuAssertTrue(tc, 0 != x.woke && 0 != y.woke);
}
// ④ 广播：不看标签全部唤醒；起的新协程在同一 sess 上再等时，要追加进已清 keep 的旧条目。
// 若先重新查询、删了条目再起协程，新协程会新建一个 keep 条目，最后 sessions 就是 1
static void test_coro_wake_all_order(CuTest *tc) {
    co_owner o;
    coro_ctx *co = _co_new(&o, 0);
    co_waiter a, b, c;
    int32_t p = 0;
    _co_waiter_set(&a, 5, 1, 1, 0);
    _co_waiter_set(&b, 5, 2, 1, 0);
    _co_waiter_set(&c, 5, 3, 1, 0);
    _co_spawn(co, _step_wait, &a);
    _co_spawn(co, _step_wait, &b);
    co_job job = { _step_wait, &c };
    coro_wake_all(co, 5, &p, &job);
    int32_t susp = coro_suspended(co);
    int32_t mid = _co_sessions(co);
    int32_t r = coro_wake(co, 5, 3, NULL);
    int32_t end = _co_sessions(co);
    coro_free(co);
    CuAssertIntEquals(tc, 1, a.woke);
    CuAssertIntEquals(tc, 2, b.woke);
    CuAssertPtrEquals(tc, &p, a.payload);
    CuAssertPtrEquals(tc, &p, b.payload);
    CuAssertIntEquals(tc, 1, susp);
    CuAssertIntEquals(tc, 1, mid);
    CuAssertIntEquals(tc, ERR_OK, r);
    CuAssertIntEquals(tc, 3, c.woke);
    CuAssertIntEquals(tc, 0, end);
}
static void *_co_mk(void *ud, uint64_t sess, int32_t tag) {
    co_timeout *t = ud;
    t->calls++;
    t->sess = sess;
    t->tag = tag;
    return &t->msg;
}
// ⑤ 超时无视 keep；不带超时与没到期的不动
static void test_coro_expire(CuTest *tc) {
    co_owner o;
    coro_ctx *co = _co_new(&o, 0);
    co_waiter t, n, l;
    co_timeout tmo;
    ZERO(&tmo, sizeof(tmo));
    _co_waiter_set(&t, 6, 2, 1, 1);
    _co_waiter_set(&n, 7, 1, 0, 0);
    _co_waiter_set(&l, 8, 1, 0, 60 * 1000);
    _co_spawn(co, _step_wait, &t);
    _co_spawn(co, _step_wait, &n);
    _co_spawn(co, _step_wait, &l);
    MSLEEP(20);
    coro_expire(co, _co_mk, &tmo);
    int32_t susp = coro_suspended(co);
    int32_t sessions = _co_sessions(co);
    coro_wake(co, 7, 1, NULL);
    coro_wake(co, 8, 1, NULL);// 带超时的被正常唤醒，顺带摘掉堆节点
    coro_expire(co, _co_mk, &tmo);
    co_waiter z;// mk 传 NULL：到期者拿到 NULL
    _co_waiter_set(&z, 16, 1, 0, 1);
    z.payload = &z;
    _co_spawn(co, _step_wait, &z);
    MSLEEP(20);
    coro_expire(co, NULL, NULL);
    coro_free(co);
    CuAssertIntEquals(tc, 1, t.woke);
    CuAssertPtrEquals(tc, &tmo.msg, t.payload);
    CuAssertIntEquals(tc, 1, tmo.calls);
    CuAssertTrue(tc, 6 == tmo.sess);
    CuAssertIntEquals(tc, 2, tmo.tag);
    CuAssertIntEquals(tc, 2, susp);
    CuAssertIntEquals(tc, 2, sessions);// sess 6 的 keep 条目被超时路径删了
    CuAssertTrue(tc, 0 != n.woke && 0 != l.woke);
    CuAssertPtrEquals(tc, NULL, l.payload);
    CuAssertTrue(tc, 0 != z.woke);
    CuAssertPtrEquals(tc, NULL, z.payload);
}
static void _fn_inc(void *owner, void *arg) {
    (void)owner;
    (*(int32_t *)arg)++;
}
static void _fn_wait(void *owner, void *arg) {
    co_waiter *w = arg;
    w->payload = coro_wait(((co_owner *)owner)->co, w->sess, w->tag, w->keep, w->ms);
    w->woke = ++_seq;
}
static void _step_fork(coro_ctx *co, void *arg) {
    coro_fork(co, _fn_inc, arg);
    coro_fork(co, _fn_inc, arg);
}
static void _step_fork_wait(coro_ctx *co, void *arg) {
    co_forkwait *f = arg;
    coro_fn fns[2] = { _fn_inc, _fn_wait };
    void *args[2] = { &f->cnt, &f->w };
    f->rtn = coro_fork_wait(co, fns, args, 2);
    f->done = 1;
}
// fork 只排队、drain 才起；fork_wait 等全部子协程跑完才返回
static void test_coro_fork(CuTest *tc) {
    co_owner o;
    coro_ctx *co = _co_new(&o, 0);
    int32_t cnt = 0;
    _co_spawn(co, _step_fork, &cnt);
    int32_t before_drain = cnt;
    coro_fork_drain(co);
    int32_t after_drain = cnt;
    co_forkwait f;
    ZERO(&f, sizeof(f));
    _co_waiter_set(&f.w, 9, 1, 0, 0);
    _co_spawn(co, _step_fork_wait, &f);
    int32_t susp1 = coro_suspended(co);
    char *d = coro_dump(co, NULL);
    int32_t fw2 = NULL != strstr(d, "fork_wait pending=2 age=");
    FREE(d);
    coro_fork_drain(co);
    int32_t susp2 = coro_suspended(co);
    d = coro_dump(co, NULL);
    int32_t fw1 = NULL != strstr(d, "fork_wait pending=1 age=") && NULL != strstr(d, "1 fork_wait,");
    FREE(d);
    int32_t done_mid = f.done;
    coro_wake(co, 9, 1, NULL);
    int32_t susp3 = coro_suspended(co);
    int32_t r0 = coro_fork_wait(co, NULL, NULL, 0);
    int32_t ro = coro_fork_wait(co, NULL, NULL, 1);// 不在协程里，打一行 WARN 后拒绝
    coro_free(co);
    CuAssertIntEquals(tc, 0, before_drain);
    CuAssertIntEquals(tc, 2, after_drain);
    CuAssertIntEquals(tc, 1, susp1);
    CuAssertIntEquals(tc, 1, fw2);
    CuAssertIntEquals(tc, 2, susp2);// 父协程 + 挂着的子协程
    CuAssertIntEquals(tc, 1, fw1);
    CuAssertIntEquals(tc, 0, done_mid);
    CuAssertIntEquals(tc, 0, susp3);
    CuAssertIntEquals(tc, 1, f.done);
    CuAssertIntEquals(tc, ERR_OK, f.rtn);
    CuAssertIntEquals(tc, 1, f.cnt);
    CuAssertIntEquals(tc, ERR_OK, r0);
    CuAssertIntEquals(tc, ERR_FAILED, ro);
}
// 持锁后挂起；同协程再进一层验证可重入
static void _step_hold(coro_ctx *co, void *arg) {
    co_serial *x = arg;
    if (ERR_OK != coro_serial_enter(x->s)) {
        return;
    }
    if (ERR_OK == coro_serial_enter(x->s)) {
        x->order[x->n++] = 1;
        coro_serial_leave(x->s);
    }
    x->w.payload = coro_wait(co, x->w.sess, x->w.tag, 0, 0);
    coro_serial_leave(x->s);
}
// 排队进锁
static void _step_queue(coro_ctx *co, void *arg) {
    co_serial *x = arg;
    (void)co;
    x->rtn = coro_serial_enter(x->s);
    if (ERR_OK == x->rtn) {
        x->order[x->n++] = 2;
        coro_serial_leave(x->s);
    }
}
// 交接：持锁者出锁时把锁交给队头；中途关闭：排队者失败返回，释放推迟到持锁者出锁
static void test_coro_serial(CuTest *tc) {
    co_owner o;
    coro_ctx *co = _co_new(&o, 0);
    co_serial x;
    ZERO(&x, sizeof(x));
    x.s = coro_serial_new(co);
    _co_waiter_set(&x.w, 10, 1, 0, 0);
    _co_spawn(co, _step_hold, &x);
    _co_spawn(co, _step_queue, &x);
    int32_t susp1 = coro_suspended(co);
    coro_wake(co, 10, 1, NULL);
    int32_t susp2 = coro_suspended(co);
    coro_serial_free(x.s);
    co_serial y;
    ZERO(&y, sizeof(y));
    y.s = coro_serial_new(co);
    _co_waiter_set(&y.w, 11, 1, 0, 0);
    _co_spawn(co, _step_hold, &y);
    _co_spawn(co, _step_queue, &y);
    coro_serial_free(y.s);// 持锁者还在临界区：排队者被唤醒失败，对象等持锁者出锁再放
    int32_t susp3 = coro_suspended(co);
    coro_wake(co, 11, 1, NULL);
    int32_t susp4 = coro_suspended(co);
    coro_free(co);
    CuAssertIntEquals(tc, 2, susp1);
    CuAssertIntEquals(tc, 0, susp2);
    CuAssertIntEquals(tc, 2, x.n);
    CuAssertIntEquals(tc, 1, x.order[0]);
    CuAssertIntEquals(tc, 2, x.order[1]);
    CuAssertIntEquals(tc, ERR_OK, x.rtn);
    CuAssertIntEquals(tc, 1, susp3);
    CuAssertIntEquals(tc, ERR_FAILED, y.rtn);
    CuAssertIntEquals(tc, 1, y.n);
    CuAssertIntEquals(tc, 0, susp4);
}
// dump 格式与 Lua 侧 debug_request.lua 同格式，字段名 mtype 不许改
static void test_coro_dump(CuTest *tc) {
    co_owner o;
    coro_ctx *co = _co_new(&o, 1);
    co_waiter w;
    _co_waiter_set(&w, 12, 1, 0, 0);
    _co_spawn(co, _step_wait, &w);
    size_t lens = 0;
    char *d = coro_dump(co, &lens);
    int32_t line = NULL != strstr(d, "sess=12 co=") && NULL != strstr(d, " mtype=ONE age=");
    int32_t tail = NULL != strstr(d, "1 suspended, 1 sessions, 0 fork_wait, 0 serial, 1 yield total.");
    FREE(d);
    coro_wake(co, 12, 1, NULL);
    coro_free(co);
    co = _co_new(&o, 0);// 不给 tagstr 时打数字
    _co_waiter_set(&w, 13, 5, 0, 0);
    _co_spawn(co, _step_wait, &w);
    d = coro_dump(co, &lens);
    int32_t num = NULL != strstr(d, " mtype=5 age=");
    FREE(d);
    coro_wake(co, 13, 5, NULL);
    coro_free(co);
    CuAssertIntEquals(tc, 1, line);
    CuAssertIntEquals(tc, 1, tail);
    CuAssertTrue(tc, lens > 0);
    CuAssertIntEquals(tc, 1, num);
}
static void _step_nested_spawn(coro_ctx *co, void *arg) {
    co_nested *x = arg;
    _co_spawn(co, _step_wait, &x->child);
    x->running = coro_running(co);
    x->parent.payload = coro_wait(co, x->parent.sess, x->parent.tag, 0, 0);// curco 没还原回父协程这里就会挂错协程
    x->parent.woke = ++_seq;
}
// 协程里起协程：子协程挂起后回到父协程，父协程照常挂起、两者各自被唤醒
static void test_coro_nested_spawn(CuTest *tc) {
    co_owner o;
    coro_ctx *co = _co_new(&o, 0);
    co_nested x;
    ZERO(&x, sizeof(x));
    _co_waiter_set(&x.child, 14, 1, 0, 0);
    _co_waiter_set(&x.parent, 15, 1, 0, 0);
    _co_spawn(co, _step_nested_spawn, &x);
    int32_t susp = coro_suspended(co);
    int32_t running = coro_running(co);
    int32_t r1 = coro_wake(co, 14, 1, NULL);
    int32_t r2 = coro_wake(co, 15, 1, NULL);
    int32_t susp_end = coro_suspended(co);
    coro_free(co);
    CuAssertIntEquals(tc, 2, susp);
    CuAssertIntEquals(tc, 0, running);
    CuAssertIntEquals(tc, 1, x.running);
    CuAssertIntEquals(tc, ERR_OK, r1);
    CuAssertIntEquals(tc, ERR_OK, r2);
    CuAssertIntEquals(tc, 1, x.child.woke);
    CuAssertIntEquals(tc, 2, x.parent.woke);
    CuAssertIntEquals(tc, 0, susp_end);
    CuAssertIntEquals(tc, 2, o.before);
    CuAssertIntEquals(tc, 2, o.after);
}
// 同时挂起的协程比池容量多：唤醒后回池失败的协程跑完即死，由唤醒方回收(泄漏由 MEMORY_CHECK 兜)；钩子仍成对
static void test_coro_pool_full(CuTest *tc) {
    co_owner o;
    coro_ctx *co = _co_new(&o, 0);
    co_waiter *w;
    int32_t i;
    int32_t woke = 0;
    int32_t order_ok = 1;
    MALLOC(w, sizeof(co_waiter) * CO_OVERFLOW_N);
    for (i = 0; i < CO_OVERFLOW_N; i++) {
        _co_waiter_set(&w[i], (uint64_t)(100 + i), 1, 0, 0);
        _co_spawn(co, _step_wait, &w[i]);
    }
    int32_t susp = coro_suspended(co);
    for (i = 0; i < CO_OVERFLOW_N; i++) {
        if (ERR_OK == coro_wake(co, (uint64_t)(100 + i), 1, NULL)) {
            woke++;
        }
        if (i + 1 != w[i].woke) {
            order_ok = 0;
        }
    }
    int32_t susp_end = coro_suspended(co);
    FREE(w);
    coro_free(co);
    CuAssertIntEquals(tc, CO_OVERFLOW_N, susp);
    CuAssertIntEquals(tc, CO_OVERFLOW_N, woke);
    CuAssertIntEquals(tc, 1, order_ok);
    CuAssertIntEquals(tc, 0, susp_end);
    CuAssertIntEquals(tc, CO_OVERFLOW_N, o.before);
    CuAssertIntEquals(tc, CO_OVERFLOW_N, o.after);
}
// 同一 sess 上有两个等待者，只有一个带超时：超时摘掉它后条目还在，另一个照常被唤醒
static void test_coro_expire_shared(CuTest *tc) {
    co_owner o;
    coro_ctx *co = _co_new(&o, 0);
    co_waiter a, b;
    _co_waiter_set(&a, 17, 1, 0, 1);
    _co_waiter_set(&b, 17, 1, 0, 0);
    _co_spawn(co, _step_wait, &a);
    _co_spawn(co, _step_wait, &b);
    MSLEEP(20);
    coro_expire(co, NULL, NULL);
    int32_t mid = _co_sessions(co);
    int32_t r = coro_wake(co, 17, 1, NULL);
    int32_t end = _co_sessions(co);
    coro_free(co);
    CuAssertIntEquals(tc, 1, a.woke);
    CuAssertIntEquals(tc, 1, mid);
    CuAssertIntEquals(tc, ERR_OK, r);
    CuAssertIntEquals(tc, 2, b.woke);
    CuAssertIntEquals(tc, 0, end);
}
static void _step_serial_call(coro_ctx *co, void *arg) {
    co_scall *x = arg;
    (void)co;
    x->r1 = coro_serial_call(x->s, _fn_inc, &x->cnt);
    x->r2 = coro_serial_call(x->s, NULL, NULL);// 只进出一次，探测能否拿到锁
}
// serial 的边界：协程外进锁、重复关闭、对已关闭的进锁、coro_serial_call，以及 dump 的三种 serial 状态
static void test_coro_serial_edges(CuTest *tc) {
    co_owner o;
    coro_ctx *co = _co_new(&o, 0);
    co_serial x, late;
    int32_t cnt = 0;
    ZERO(&x, sizeof(x));
    x.s = coro_serial_new(co);
    _co_waiter_set(&x.w, 20, 1, 0, 0);
    int32_t out_enter = coro_serial_enter(x.s);// 不在协程里，打 WARN 后拒绝
    int32_t out_call = coro_serial_call(x.s, _fn_inc, &cnt);
    _co_spawn(co, _step_hold, &x);
    _co_spawn(co, _step_queue, &x);
    char *d = coro_dump(co, NULL);
    int32_t held_wait = NULL != strstr(d, " held=1 hold=") && NULL != strstr(d, " waiters=1 age=");
    FREE(d);
    coro_serial_free(x.s);// 排队者失败返回，对象等持锁者出锁再放
    coro_serial_free(x.s);// 已关闭：直接返回
    d = coro_dump(co, NULL);
    int32_t held_only = NULL != strstr(d, " held=1 hold=") && NULL != strstr(d, " waiters=0\n");
    FREE(d);
    ZERO(&late, sizeof(late));
    late.s = x.s;
    _co_spawn(co, _step_queue, &late);// 对已关闭的进锁：直接失败，不排队
    int32_t susp = coro_suspended(co);
    coro_wake(co, 20, 1, NULL);
    co_scall c;
    ZERO(&c, sizeof(c));
    c.s = coro_serial_new(co);
    _co_spawn(co, _step_serial_call, &c);
    d = coro_dump(co, NULL);
    int32_t idle = NULL == strstr(d, "serial=");// 空闲 serial 不出行
    FREE(d);
    coro_serial_free(c.s);
    coro_free(co);
    CuAssertIntEquals(tc, ERR_FAILED, out_enter);
    CuAssertIntEquals(tc, ERR_FAILED, out_call);
    CuAssertIntEquals(tc, 0, cnt);
    CuAssertIntEquals(tc, 1, held_wait);
    CuAssertIntEquals(tc, ERR_FAILED, x.rtn);
    CuAssertIntEquals(tc, 1, held_only);
    CuAssertIntEquals(tc, ERR_FAILED, late.rtn);
    CuAssertIntEquals(tc, 1, susp);
    CuAssertIntEquals(tc, ERR_OK, c.r1);
    CuAssertIntEquals(tc, ERR_OK, c.r2);
    CuAssertIntEquals(tc, 1, c.cnt);
    CuAssertIntEquals(tc, 1, idle);
}
// 零散分支：钩子 before_run / after_run 传 NULL、对不存在的 sess 广播、命中 keep 空条目、
// 协程外 fork，以及 coro_free 兜底收掉没跑的 fork 与没释放的 serial(泄漏由 MEMORY_CHECK 兜)
static void test_coro_misc(CuTest *tc) {
    co_owner o;
    coro_hooks hooks = { _co_run, NULL, NULL, NULL };
    ZERO(&o, sizeof(o));
    o.co = coro_new(&hooks, &o, 0);
    coro_ctx *co = o.co;
    co_waiter k;
    int32_t cnt = 0;
    _seq = 0;
    _co_waiter_set(&k, 30, 1, 1, 0);
    _co_spawn(co, _step_wait, &k);
    int32_t r1 = coro_wake(co, 30, 1, NULL);
    int32_t r2 = coro_wake(co, 30, 1, NULL);// keep 留下的空条目：当作没有等待者
    coro_wake_all(co, 999, NULL, NULL);// 不存在的 sess
    int32_t kept = _co_sessions(co);
    coro_wake_all(co, 30, NULL, NULL);
    int32_t gone = _co_sessions(co);
    coro_fork(co, _fn_inc, &cnt);// 不在协程里，打 WARN 后忽略
    coro_fork_drain(co);
    int32_t out_fork = cnt;
    _co_spawn(co, _step_fork, &cnt);// 排进待起队列后故意不 drain
    coro_serial_new(co);// 故意不 free
    coro_free(co);
    CuAssertIntEquals(tc, ERR_OK, r1);
    CuAssertIntEquals(tc, ERR_FAILED, r2);
    CuAssertIntEquals(tc, 1, kept);
    CuAssertIntEquals(tc, 0, gone);
    CuAssertIntEquals(tc, 0, out_fork);
    CuAssertIntEquals(tc, 0, cnt);
    CuAssertIntEquals(tc, 0, o.before);
    CuAssertIntEquals(tc, 0, o.after);
}
// 栈大小：0 与过小取下界，过大取上界，上下界之间原样
static void test_coro_stack_fit(CuTest *tc) {
    size_t lo = coro_stack_fit(0);
    size_t hi = coro_stack_fit(SIZE_MAX);
    CuAssertTrue(tc, lo >= 32 * 1024);
    CuAssertTrue(tc, hi > lo);
    CuAssertTrue(tc, lo == coro_stack_fit(1));
    CuAssertTrue(tc, hi == coro_stack_fit(hi + 1));
    CuAssertTrue(tc, lo + 16 == coro_stack_fit(lo + 16));
}

void test_coro(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_coro_wait_wake);
    SUITE_ADD_TEST(suite, test_coro_fifo_head_only);
    SUITE_ADD_TEST(suite, test_coro_keep);
    SUITE_ADD_TEST(suite, test_coro_wake_all_order);
    SUITE_ADD_TEST(suite, test_coro_expire);
    SUITE_ADD_TEST(suite, test_coro_fork);
    SUITE_ADD_TEST(suite, test_coro_serial);
    SUITE_ADD_TEST(suite, test_coro_nested_spawn);
    SUITE_ADD_TEST(suite, test_coro_pool_full);
    SUITE_ADD_TEST(suite, test_coro_expire_shared);
    SUITE_ADD_TEST(suite, test_coro_serial_edges);
    SUITE_ADD_TEST(suite, test_coro_misc);
    SUITE_ADD_TEST(suite, test_coro_dump);
    SUITE_ADD_TEST(suite, test_coro_stack_fit);
}
