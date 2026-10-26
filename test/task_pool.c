#include "task_pool.h"
#include "task_pub.h"

typedef struct task_pool_args {
    int32_t *ok; // 测试结果槽：跑完置 1，失败由 test_fail 写 -3
}task_pool_args;

// 假连接：只记计数与状态，不碰网络
typedef struct fake_conn {
    int32_t ready;        // 1=可用；quit / ping 失败清零
    int32_t connects;     // connect 调用次数
    int32_t pings;        // ping 调用次数
    int32_t quits;        // quit 调用次数
    int32_t fail_connect; // >0 时本次 connect 失败并递减
    int32_t fail_ping;    // >0 时本次 ping 失败并递减
    int32_t dirty;        // 1=带着没结束的会话状态，reset 时清掉
    int32_t resets;       // reset 实际清理的次数（干净时不算）
    int32_t fail_reset;   // >0 时本次 reset 失败并递减
    uint32_t connect_ms;  // connect 里模拟的耗时
    uint32_t op_ms;       // >0 时 ping / quit / reset 挂起这么久，要配 task
    task_ctx *task;       // op_ms 挂起用
}fake_conn;

static int32_t _fake_ready(void *conn) {
    return 0 != ((fake_conn *)conn)->ready;
}
static int32_t _fake_connect(task_ctx *task, void *conn) {
    fake_conn *f = (fake_conn *)conn;
    f->connects++;
    if (f->connect_ms > 0) {
        coro_sleep(task, f->connect_ms);
    }
    if (f->fail_connect > 0) {
        f->fail_connect--;
        return ERR_FAILED;
    }
    f->ready = 1;
    return ERR_OK;
}
static void _fake_op_wait(fake_conn *f) {
    if (f->op_ms > 0
        && NULL != f->task) {
        coro_sleep(f->task, f->op_ms);
    }
}
static int32_t _fake_ping(void *conn) {
    fake_conn *f = (fake_conn *)conn;
    _fake_op_wait(f);
    f->pings++;
    if (f->fail_ping > 0) {
        f->fail_ping--;
        f->ready = 0;
        return ERR_FAILED;
    }
    return ERR_OK;
}
static void _fake_quit(void *conn) {
    fake_conn *f = (fake_conn *)conn;
    _fake_op_wait(f);
    f->quits++;
    f->ready = 0;
}
static int32_t _fake_reset(void *conn) {
    fake_conn *f = (fake_conn *)conn;
    if (0 == f->dirty) {
        return ERR_OK;
    }
    _fake_op_wait(f);
    f->resets++;
    if (f->fail_reset > 0) {
        f->fail_reset--;
        return ERR_FAILED;
    }
    f->dirty = 0;
    return ERR_OK;
}
static const db_pool_ops _fake_ops = {
    _fake_ready, _fake_connect, _fake_ping, _fake_quit, _fake_reset
};

typedef struct pool_shared {
    db_pool_ctx *pool; // 被测池
    int32_t in_use;    // 当前借出中的协程数
    int32_t peak;      // in_use 的峰值
    int32_t fail;      // 借连接失败的次数
}pool_shared;

typedef struct worker_arg {
    pool_shared *sh;   // 共享状态
    uint32_t hold_ms;  // 借到后持有多少毫秒
    int32_t id;        // FIFO 用例里的编号
    int32_t *order;    // 成功借到的顺序；NULL 不记
    int32_t *norder;   // order 的写入位置
    int32_t rtn;       // db_pool_get 的返回
}worker_arg;

// 借、记并发峰值、持有一会、还
static void _worker(void *owner, void *arg) {
    task_ctx *task = (task_ctx *)owner;
    worker_arg *a = (worker_arg *)arg;
    void *conn = NULL;
    a->rtn = db_pool_get(a->sh->pool, 2000, &conn);
    if (ERR_OK != a->rtn) {
        a->sh->fail++;
        return;
    }
    if (NULL != a->order) {
        a->order[*a->norder] = a->id;
        (*a->norder)++;
    }
    a->sh->in_use++;
    if (a->sh->in_use > a->sh->peak) {
        a->sh->peak = a->sh->in_use;
    }
    if (a->hold_ms > 0) {
        coro_sleep(task, a->hold_ms);
    }
    a->sh->in_use--;
    db_pool_put(a->sh->pool, conn);
}

// ── 1. add 的容量/重复指针校验 ───────────────────────────────────────────
static int32_t _test_add(task_ctx *task) {
    (void)task;
    fake_conn c1, c2;
    ZERO(&c1, sizeof(c1));
    ZERO(&c2, sizeof(c2));
    db_pool_cfg cfg = { .cap = 1, .backoff_ms = 20 };
    db_pool_ctx *pool = db_pool_new(task, &_fake_ops, &cfg);
    if (NULL == pool) {
        LOG_ERROR("%s", "pool add: db_pool_new failed.");
        return ERR_FAILED;
    }
    int32_t rtn = ERR_OK;
    if (ERR_OK != db_pool_add(pool, &c1)) {
        LOG_ERROR("%s", "pool add: first add failed.");
        rtn = ERR_FAILED;
    }
    if (ERR_OK == db_pool_add(pool, &c1)) {
        LOG_ERROR("%s", "pool add: duplicate conn accepted.");
        rtn = ERR_FAILED;
    }
    if (ERR_OK == db_pool_add(pool, &c2)) {
        LOG_ERROR("%s", "pool add: overflow conn accepted.");
        rtn = ERR_FAILED;
    }
    db_pool_free(pool);
    return rtn;
}

// ── 2. 懒建连：第一次借出才 connect，归还后再借不重连 ───────────────────
static int32_t _test_lazy(task_ctx *task) {
    fake_conn c1;
    ZERO(&c1, sizeof(c1));
    db_pool_cfg cfg = { .cap = 1, .backoff_ms = 20 };
    db_pool_ctx *pool = db_pool_new(task, &_fake_ops, &cfg);
    if (NULL == pool
        || ERR_OK != db_pool_add(pool, &c1)) {
        LOG_ERROR("%s", "pool lazy: setup failed.");
        if (NULL != pool) {
            db_pool_free(pool);
        }
        return ERR_FAILED;
    }
    int32_t rtn = ERR_OK;
    void *conn = NULL;
    if (ERR_OK != db_pool_get(pool, 0, &conn)) {
        LOG_ERROR("%s", "pool lazy: first get failed.");
        rtn = ERR_FAILED;
        goto out;
    }
    if (1 != c1.connects) {
        LOG_ERROR("pool lazy: expect connects=1, got %d.", c1.connects);
        rtn = ERR_FAILED;
    }
    db_pool_put(pool, conn);
    conn = NULL;
    if (ERR_OK != db_pool_get(pool, 0, &conn)) {
        LOG_ERROR("%s", "pool lazy: second get failed.");
        rtn = ERR_FAILED;
        goto out;
    }
    if (1 != c1.connects) {
        LOG_ERROR("pool lazy: reconnect on idle checkout, connects=%d.", c1.connects);
        rtn = ERR_FAILED;
    }
    db_pool_put(pool, conn);
out:
    db_pool_free(pool);
    return rtn;
}

// ── 3. 借出期间断连：归还后下一次借出自动重连 ───────────────────────────
static int32_t _test_broken(task_ctx *task) {
    fake_conn c1;
    ZERO(&c1, sizeof(c1));
    db_pool_cfg cfg = { .cap = 1, .backoff_ms = 20 };
    db_pool_ctx *pool = db_pool_new(task, &_fake_ops, &cfg);
    if (NULL == pool
        || ERR_OK != db_pool_add(pool, &c1)) {
        LOG_ERROR("%s", "pool broken: setup failed.");
        if (NULL != pool) {
            db_pool_free(pool);
        }
        return ERR_FAILED;
    }
    int32_t rtn = ERR_OK;
    void *conn = NULL;
    if (ERR_OK != db_pool_get(pool, 0, &conn)) {
        LOG_ERROR("%s", "pool broken: first get failed.");
        rtn = ERR_FAILED;
        goto out;
    }
    c1.ready = 0;
    db_pool_put(pool, conn);
    conn = NULL;
    if (ERR_OK != db_pool_get(pool, 0, &conn)) {
        LOG_ERROR("%s", "pool broken: reconnect get failed.");
        rtn = ERR_FAILED;
        goto out;
    }
    if (2 != c1.connects) {
        LOG_ERROR("pool broken: expect connects=2, got %d.", c1.connects);
        rtn = ERR_FAILED;
    }
    db_pool_put(pool, conn);
out:
    db_pool_free(pool);
    return rtn;
}

// ── 4. 建连失败：按 backoff 冷却，冷却期内不再尝试 ─────────────────────
static int32_t _test_backoff(task_ctx *task) {
    fake_conn c1;
    ZERO(&c1, sizeof(c1));
    c1.fail_connect = 1;
    db_pool_cfg cfg = { .cap = 1, .backoff_ms = 30 };
    db_pool_ctx *pool = db_pool_new(task, &_fake_ops, &cfg);
    if (NULL == pool
        || ERR_OK != db_pool_add(pool, &c1)) {
        LOG_ERROR("%s", "pool backoff: setup failed.");
        if (NULL != pool) {
            db_pool_free(pool);
        }
        return ERR_FAILED;
    }
    int32_t rtn = ERR_OK;
    void *conn = NULL;
    if (ERR_OK == db_pool_get(pool, 0, &conn)) {
        LOG_ERROR("%s", "pool backoff: first get should fail.");
        db_pool_put(pool, conn);
        rtn = ERR_FAILED;
        goto out;
    }
    if (1 != c1.connects) {
        LOG_ERROR("pool backoff: expect connects=1, got %d.", c1.connects);
        rtn = ERR_FAILED;
    }
    if (ERR_OK == db_pool_get(pool, 0, &conn)) {
        LOG_ERROR("%s", "pool backoff: cooling conn handed out.");
        db_pool_put(pool, conn);
        rtn = ERR_FAILED;
        goto out;
    }
    if (1 != c1.connects) {
        LOG_ERROR("pool backoff: retried during cooling, connects=%d.", c1.connects);
        rtn = ERR_FAILED;
    }
    coro_sleep(task, 40);
    if (ERR_OK != db_pool_get(pool, 0, &conn)) {
        LOG_ERROR("%s", "pool backoff: get after cooling failed.");
        rtn = ERR_FAILED;
        goto out;
    }
    if (2 != c1.connects) {
        LOG_ERROR("pool backoff: expect connects=2, got %d.", c1.connects);
        rtn = ERR_FAILED;
    }
    db_pool_put(pool, conn);
out:
    db_pool_free(pool);
    return rtn;
}

// ── 5/6. 并发上界与 FIFO 交接 ───────────────────────────────────────────
static int32_t _test_bound(task_ctx *task) {
    fake_conn c1, c2;
    ZERO(&c1, sizeof(c1));
    ZERO(&c2, sizeof(c2));
    db_pool_cfg cfg = { .cap = 2, .backoff_ms = 20 };
    db_pool_ctx *pool = db_pool_new(task, &_fake_ops, &cfg);
    if (NULL == pool
        || ERR_OK != db_pool_add(pool, &c1)
        || ERR_OK != db_pool_add(pool, &c2)) {
        LOG_ERROR("%s", "pool bound: setup failed.");
        if (NULL != pool) {
            db_pool_free(pool);
        }
        return ERR_FAILED;
    }
    pool_shared sh;
    ZERO(&sh, sizeof(sh));
    sh.pool = pool;
    worker_arg a[4];
    coro_fn fns[4];
    void *args[4];
    ZERO(a, sizeof(a));
    for (int32_t i = 0; i < 4; i++) {
        a[i].sh = &sh;
        a[i].hold_ms = 20;
        a[i].id = i + 1;
        fns[i] = _worker;
        args[i] = &a[i];
    }
    int32_t rtn = ERR_OK;
    if (ERR_OK != coro_fork_wait(coro_task_co(task), fns, args, 4)) {
        LOG_ERROR("%s", "pool bound: coro_fork_wait failed.");
        rtn = ERR_FAILED;
    }
    db_pool_stat st;
    db_pool_stat_get(pool, &st);
    if (0 != sh.fail) {
        LOG_ERROR("pool bound: %d worker(s) failed to get.", sh.fail);
        rtn = ERR_FAILED;
    }
    if (2 != sh.peak) {
        LOG_ERROR("pool bound: expect peak=2, got %d.", sh.peak);
        rtn = ERR_FAILED;
    }
    if (0 != st.busy || 2 != st.idle) {
        LOG_ERROR("pool bound: expect busy=0 idle=2, got busy=%d idle=%d.", st.busy, st.idle);
        rtn = ERR_FAILED;
    }
    if (2 != c1.connects + c2.connects) {
        LOG_ERROR("pool bound: expect connects=2, got %d.", c1.connects + c2.connects);
        rtn = ERR_FAILED;
    }
    db_pool_free(pool);
    if (2 != c1.quits + c2.quits) {
        LOG_ERROR("pool bound: expect quits=2, got %d.", c1.quits + c2.quits);
        rtn = ERR_FAILED;
    }
    return rtn;
}
static int32_t _test_fifo(task_ctx *task) {
    fake_conn c1;
    ZERO(&c1, sizeof(c1));
    db_pool_cfg cfg = { .cap = 1, .backoff_ms = 20 };
    db_pool_ctx *pool = db_pool_new(task, &_fake_ops, &cfg);
    if (NULL == pool
        || ERR_OK != db_pool_add(pool, &c1)) {
        LOG_ERROR("%s", "pool fifo: setup failed.");
        if (NULL != pool) {
            db_pool_free(pool);
        }
        return ERR_FAILED;
    }
    pool_shared sh;
    ZERO(&sh, sizeof(sh));
    sh.pool = pool;
    int32_t order[4];
    int32_t norder = 0;
    ZERO(order, sizeof(order));
    worker_arg a[3];
    coro_fn fns[3];
    void *args[3];
    ZERO(a, sizeof(a));
    for (int32_t i = 0; i < 3; i++) {
        a[i].sh = &sh;
        a[i].hold_ms = (0 == i) ? 30 : 0;// 第一个持有一会，后两个才会排队
        a[i].id = i + 1;
        a[i].order = order;
        a[i].norder = &norder;
        fns[i] = _worker;
        args[i] = &a[i];
    }
    int32_t rtn = ERR_OK;
    if (ERR_OK != coro_fork_wait(coro_task_co(task), fns, args, 3)) {
        LOG_ERROR("%s", "pool fifo: coro_fork_wait failed.");
        rtn = ERR_FAILED;
    }
    if (0 != sh.fail) {
        LOG_ERROR("pool fifo: %d worker(s) failed to get.", sh.fail);
        rtn = ERR_FAILED;
    }
    if (3 != norder
        || 1 != order[0]
        || 2 != order[1]
        || 3 != order[2]) {
        LOG_ERROR("pool fifo: expect [1,2,3], got n=%d [%d,%d,%d].",
                  norder, order[0], order[1], order[2]);
        rtn = ERR_FAILED;
    }
    db_pool_free(pool);
    return rtn;
}

// ── 7. 空闲探活：超过 ping_idle 先 ping；失败冷却后再重连 ────────────────
static int32_t _test_ping(task_ctx *task) {
    fake_conn c1;
    ZERO(&c1, sizeof(c1));
    db_pool_cfg cfg = { .cap = 1, .ping_idle_ms = 10, .backoff_ms = 30 };
    db_pool_ctx *pool = db_pool_new(task, &_fake_ops, &cfg);
    if (NULL == pool
        || ERR_OK != db_pool_add(pool, &c1)) {
        LOG_ERROR("%s", "pool ping: setup failed.");
        if (NULL != pool) {
            db_pool_free(pool);
        }
        return ERR_FAILED;
    }
    int32_t rtn = ERR_OK;
    void *conn = NULL;
    if (ERR_OK != db_pool_get(pool, 0, &conn)) {
        LOG_ERROR("%s", "pool ping: first get failed.");
        rtn = ERR_FAILED;
        goto out;
    }
    db_pool_put(pool, conn);
    coro_sleep(task, 25);
    conn = NULL;
    if (ERR_OK != db_pool_get(pool, 0, &conn)) {
        LOG_ERROR("%s", "pool ping: idle get failed.");
        rtn = ERR_FAILED;
        goto out;
    }
    if (1 != c1.pings) {
        LOG_ERROR("pool ping: expect pings=1, got %d.", c1.pings);
        rtn = ERR_FAILED;
    }
    db_pool_put(pool, conn);
    c1.fail_ping = 1;
    coro_sleep(task, 25);
    conn = NULL;
    if (ERR_OK == db_pool_get(pool, 0, &conn)) {
        LOG_ERROR("%s", "pool ping: failed ping handed out.");
        db_pool_put(pool, conn);
        rtn = ERR_FAILED;
        goto out;
    }
    if (2 != c1.pings) {
        LOG_ERROR("pool ping: expect pings=2, got %d.", c1.pings);
        rtn = ERR_FAILED;
    }
    if (1 != c1.connects) {
        LOG_ERROR("pool ping: connect during cooling, connects=%d.", c1.connects);
        rtn = ERR_FAILED;
    }
    coro_sleep(task, 40);
    conn = NULL;
    if (ERR_OK != db_pool_get(pool, 0, &conn)) {
        LOG_ERROR("%s", "pool ping: get after cooling failed.");
        rtn = ERR_FAILED;
        goto out;
    }
    if (2 != c1.connects) {
        LOG_ERROR("pool ping: expect connects=2, got %d.", c1.connects);
        rtn = ERR_FAILED;
    }
    db_pool_put(pool, conn);
out:
    db_pool_free(pool);
    return rtn;
}

// ── 8. 等连接超时 ───────────────────────────────────────────────────────
static void _timeout_holder(void *owner, void *arg) {
    task_ctx *task = (task_ctx *)owner;
    worker_arg *a = (worker_arg *)arg;
    void *conn = NULL;
    a->rtn = db_pool_get(a->sh->pool, 0, &conn);
    if (ERR_OK != a->rtn) {
        a->sh->fail++;
        return;
    }
    coro_sleep(task, a->hold_ms);
    db_pool_put(a->sh->pool, conn);
}
static void _timeout_waiter(void *owner, void *arg) {
    (void)owner;
    worker_arg *a = (worker_arg *)arg;
    void *conn = NULL;
    a->rtn = db_pool_get(a->sh->pool, 500, &conn);
    if (ERR_OK == a->rtn) {
        db_pool_put(a->sh->pool, conn);
        a->sh->fail++;
    }
}
static int32_t _test_timeout(task_ctx *task) {
    fake_conn c1;
    ZERO(&c1, sizeof(c1));
    db_pool_cfg cfg = { .cap = 1, .backoff_ms = 20 };
    db_pool_ctx *pool = db_pool_new(task, &_fake_ops, &cfg);
    if (NULL == pool
        || ERR_OK != db_pool_add(pool, &c1)) {
        LOG_ERROR("%s", "pool timeout: setup failed.");
        if (NULL != pool) {
            db_pool_free(pool);
        }
        return ERR_FAILED;
    }
    pool_shared sh;
    ZERO(&sh, sizeof(sh));
    sh.pool = pool;
    worker_arg holder, waiter;
    ZERO(&holder, sizeof(holder));
    ZERO(&waiter, sizeof(waiter));
    holder.sh = &sh;
    holder.hold_ms = 2000;// 比超时扫描晚，确保等待者先超时
    waiter.sh = &sh;
    coro_fn fns[2] = { _timeout_holder, _timeout_waiter };
    void *args[2] = { &holder, &waiter };
    int32_t rtn = ERR_OK;
    if (ERR_OK != coro_fork_wait(coro_task_co(task), fns, args, 2)) {
        LOG_ERROR("%s", "pool timeout: coro_fork_wait failed.");
        rtn = ERR_FAILED;
    }
    if (ERR_OK != holder.rtn) {
        LOG_ERROR("pool timeout: holder rtn=%d.", holder.rtn);
        rtn = ERR_FAILED;
    }
    if (DB_POOL_TIMEDOUT != waiter.rtn) {
        LOG_ERROR("pool timeout: expect timedout=%d, got %d.", DB_POOL_TIMEDOUT, waiter.rtn);
        rtn = ERR_FAILED;
    }
    db_pool_stat st;
    db_pool_stat_get(pool, &st);
    if (1 != st.timeouts) {
        LOG_ERROR("pool timeout: expect timeouts=1, got %u.", st.timeouts);
        rtn = ERR_FAILED;
    }
    void *conn = NULL;
    if (ERR_OK != db_pool_get(pool, 0, &conn)) {
        LOG_ERROR("%s", "pool timeout: get after timeout failed.");
        rtn = ERR_FAILED;
    } else {
        db_pool_put(pool, conn);
    }
    db_pool_free(pool);
    return rtn;
}

// ── 9. 关闭唤醒等待者，借出连接在 put 时断开 ───────────────────────────
static void _free_holder(void *owner, void *arg) {
    task_ctx *task = (task_ctx *)owner;
    worker_arg *a = (worker_arg *)arg;
    void *conn = NULL;
    a->rtn = db_pool_get(a->sh->pool, 0, &conn);
    if (ERR_OK != a->rtn) {
        a->sh->fail++;
        return;
    }
    coro_sleep(task, a->hold_ms);
    db_pool_put(a->sh->pool, conn);
}
static void _free_waiter(void *owner, void *arg) {
    (void)owner;
    worker_arg *a = (worker_arg *)arg;
    void *conn = NULL;
    a->rtn = db_pool_get(a->sh->pool, 0, &conn);
    if (ERR_OK == a->rtn) {
        db_pool_put(a->sh->pool, conn);
        a->sh->fail++;
    }
}
static void _free_closer(void *owner, void *arg) {
    task_ctx *task = (task_ctx *)owner;
    worker_arg *a = (worker_arg *)arg;
    coro_sleep(task, 10);
    db_pool_free(a->sh->pool);
}
static int32_t _test_free_waiters(task_ctx *task) {
    fake_conn c1;
    ZERO(&c1, sizeof(c1));
    db_pool_cfg cfg = { .cap = 1, .backoff_ms = 20 };
    db_pool_ctx *pool = db_pool_new(task, &_fake_ops, &cfg);
    if (NULL == pool
        || ERR_OK != db_pool_add(pool, &c1)) {
        LOG_ERROR("%s", "pool free: setup failed.");
        if (NULL != pool) {
            db_pool_free(pool);
        }
        return ERR_FAILED;
    }
    pool_shared sh;
    ZERO(&sh, sizeof(sh));
    sh.pool = pool;
    worker_arg holder, waiter, closer;
    ZERO(&holder, sizeof(holder));
    ZERO(&waiter, sizeof(waiter));
    ZERO(&closer, sizeof(closer));
    holder.sh = &sh;
    holder.hold_ms = 50;
    waiter.sh = &sh;
    closer.sh = &sh;
    coro_fn fns[3] = { _free_holder, _free_waiter, _free_closer };
    void *args[3] = { &holder, &waiter, &closer };
    int32_t rtn = ERR_OK;
    if (ERR_OK != coro_fork_wait(coro_task_co(task), fns, args, 3)) {
        LOG_ERROR("%s", "pool free: coro_fork_wait failed.");
        rtn = ERR_FAILED;
    }
    if (ERR_OK != holder.rtn) {
        LOG_ERROR("pool free: holder rtn=%d.", holder.rtn);
        rtn = ERR_FAILED;
    }
    if (ERR_OK == waiter.rtn) {
        LOG_ERROR("%s", "pool free: waiter got a connection after close.");
        rtn = ERR_FAILED;
    }
    if (1 != c1.quits) {
        LOG_ERROR("pool free: expect quits=1, got %d.", c1.quits);
        rtn = ERR_FAILED;
    }
    return rtn;
}

// max_lifetime 到期：借出时先 quit 再重连，连接对象不换
static int32_t _test_lifetime(task_ctx *task) {
    fake_conn c1;
    ZERO(&c1, sizeof(c1));
    db_pool_cfg cfg = { .cap = 1, .max_lifetime_ms = 10, .backoff_ms = 20 };
    db_pool_ctx *pool = db_pool_new(task, &_fake_ops, &cfg);
    if (NULL == pool
        || ERR_OK != db_pool_add(pool, &c1)) {
        LOG_ERROR("%s", "pool lifetime: setup failed.");
        if (NULL != pool) {
            db_pool_free(pool);
        }
        return ERR_FAILED;
    }
    int32_t rtn = ERR_OK;
    void *conn = NULL;
    if (ERR_OK != db_pool_get(pool, 2000, &conn)) {
        LOG_ERROR("%s", "pool lifetime: first get failed.");
        rtn = ERR_FAILED;
    } else {
        db_pool_put(pool, conn);
    }
    if (ERR_OK == rtn) {
        coro_sleep(task, 20);
        conn = NULL;
        if (ERR_OK != db_pool_get(pool, 2000, &conn)) {
            LOG_ERROR("%s", "pool lifetime: second get failed.");
            rtn = ERR_FAILED;
        } else {
            db_pool_put(pool, conn);
        }
    }
    if (ERR_OK == rtn
        && (2 != c1.connects || 1 != c1.quits)) {
        LOG_ERROR("pool lifetime: connects=%d quits=%d, want 2/1.", c1.connects, c1.quits);
        rtn = ERR_FAILED;
    }
    db_pool_free(pool);
    return rtn;
}

// idle_timeout：维护协程把空闲超过阈值的连接断开，槽留着下次借出重连；协程超时是秒级粒度，等 2.3 秒
static int32_t _test_idle_reap(task_ctx *task) {
    fake_conn c1, c2;
    ZERO(&c1, sizeof(c1));
    ZERO(&c2, sizeof(c2));
    db_pool_cfg cfg = { .cap = 2, .min_idle = 0, .idle_timeout_ms = 10, .backoff_ms = 20 };
    db_pool_ctx *pool = db_pool_new(task, &_fake_ops, &cfg);
    if (NULL == pool
        || ERR_OK != db_pool_add(pool, &c1)
        || ERR_OK != db_pool_add(pool, &c2)) {
        LOG_ERROR("%s", "pool idle reap: setup failed.");
        if (NULL != pool) {
            db_pool_free(pool);
        }
        return ERR_FAILED;
    }
    int32_t rtn = ERR_OK;
    void *ca = NULL;
    void *cb = NULL;
    if (ERR_OK != db_pool_get(pool, 2000, &ca)
        || ERR_OK != db_pool_get(pool, 2000, &cb)) {
        LOG_ERROR("%s", "pool idle reap: get failed.");
        rtn = ERR_FAILED;
    } else {
        db_pool_put(pool, ca);
        coro_sleep(task, 2300);// 维护协程断开空闲的 ca；cb 还借着，不受影响
        db_pool_put(pool, cb);
    }
    fake_conn *kept = (fake_conn *)cb;
    if (ERR_OK == rtn) {
        fake_conn *old = (fake_conn *)ca;
        db_pool_stat st;
        db_pool_stat_get(pool, &st);
        if (1 != old->quits || 2 != st.idle) {
            LOG_ERROR("pool idle reap: quits=%d idle=%d, want 1/2.", old->quits, st.idle);
            rtn = ERR_FAILED;
        }
    }
    // 下次借出先给还连着的 cb，不去重连刚断开的 ca
    if (ERR_OK == rtn) {
        void *again = NULL;
        if (ERR_OK != db_pool_get(pool, 2000, &again)
            || again != cb
            || 1 != kept->connects
            || 1 != ((fake_conn *)ca)->connects) {
            LOG_ERROR("%s", "pool idle reap: connected conn not preferred.");
            rtn = ERR_FAILED;
        }
        if (NULL != again) {
            db_pool_put(pool, again);
        }
    }
    db_pool_free(pool);
    return rtn;
}

// 同批被唤醒的等待者里先醒的调 free，后醒的不能碰已释放的池（ASan 下看 UAF）
typedef struct wake_free_arg {
    db_pool_ctx *pool;
    int32_t rtn[3];
    int32_t freed;
}wake_free_arg;
static void _wf_getter(void *owner, void *arg) {
    (void)owner;
    wake_free_arg *a = (wake_free_arg *)arg;
    void *conn = NULL;
    a->rtn[0] = db_pool_get(a->pool, 0, &conn);// connect 挂起 30ms 后失败
}
static void _wf_waiter(void *owner, void *arg, int32_t i) {
    (void)owner;
    wake_free_arg *a = (wake_free_arg *)arg;
    void *conn = NULL;
    a->rtn[i] = db_pool_get(a->pool, 0, &conn);
    if (ERR_OK != a->rtn[i]
        && 0 == a->freed) {
        a->freed = 1;
        db_pool_free(a->pool);
    }
}
static void _wf_waiter1(void *owner, void *arg) {
    _wf_waiter(owner, arg, 1);
}
static void _wf_waiter2(void *owner, void *arg) {
    _wf_waiter(owner, arg, 2);
}
static int32_t _test_wake_free(task_ctx *task) {
    fake_conn c1;
    ZERO(&c1, sizeof(c1));
    c1.connect_ms = 30;
    c1.fail_connect = 1;
    db_pool_cfg cfg = { .cap = 1, .backoff_ms = 1000 };
    wake_free_arg a;
    ZERO(&a, sizeof(a));
    a.pool = db_pool_new(task, &_fake_ops, &cfg);
    if (NULL == a.pool
        || ERR_OK != db_pool_add(a.pool, &c1)) {
        LOG_ERROR("%s", "pool wake free: setup failed.");
        if (NULL != a.pool) {
            db_pool_free(a.pool);
        }
        return ERR_FAILED;
    }
    coro_fn fns[3] = { _wf_getter, _wf_waiter1, _wf_waiter2 };
    void *args[3] = { &a, &a, &a };
    if (ERR_OK != coro_fork_wait(coro_task_co(task), fns, args, 3)) {
        LOG_ERROR("%s", "pool wake free: coro_fork_wait failed.");
        return ERR_FAILED;
    }
    if (ERR_OK == a.rtn[0] || ERR_OK == a.rtn[1] || ERR_OK == a.rtn[2]
        || 1 != a.freed
        || 1 != c1.quits) {
        LOG_ERROR("pool wake free: rtn=%d/%d/%d freed=%d quits=%d.",
                  a.rtn[0], a.rtn[1], a.rtn[2], a.freed, c1.quits);
        return ERR_FAILED;
    }
    return ERR_OK;
}

// 还有别的连接借出时，断连归还空出的槽也要叫醒等待者，不能等到别人归还
typedef struct slot_wake_arg {
    db_pool_ctx *pool;
    task_ctx *task;
    int32_t long_put;  // 长持有者已归还
    int32_t early;     // 等待者在长持有者归还前就借到了
    int32_t rtn;
}slot_wake_arg;
static void _sw_long(void *owner, void *arg) {
    slot_wake_arg *a = (slot_wake_arg *)arg;
    void *conn = NULL;
    if (ERR_OK != db_pool_get(a->pool, 0, &conn)) {
        return;
    }
    coro_sleep((task_ctx *)owner, 300);
    a->long_put = 1;
    db_pool_put(a->pool, conn);
}
static void _sw_broken(void *owner, void *arg) {
    slot_wake_arg *a = (slot_wake_arg *)arg;
    void *conn = NULL;
    if (ERR_OK != db_pool_get(a->pool, 0, &conn)) {
        return;
    }
    coro_sleep((task_ctx *)owner, 20);
    ((fake_conn *)conn)->ready = 0;// 借出期间断了
    db_pool_put(a->pool, conn);
}
static void _sw_waiter(void *owner, void *arg) {
    (void)owner;
    slot_wake_arg *a = (slot_wake_arg *)arg;
    void *conn = NULL;
    a->rtn = db_pool_get(a->pool, 0, &conn);
    if (ERR_OK == a->rtn) {
        a->early = (0 == a->long_put);
        db_pool_put(a->pool, conn);
    }
}
static int32_t _test_slot_wake(task_ctx *task) {
    fake_conn c1, c2;
    ZERO(&c1, sizeof(c1));
    ZERO(&c2, sizeof(c2));
    db_pool_cfg cfg = { .cap = 2, .backoff_ms = 20 };
    slot_wake_arg a;
    ZERO(&a, sizeof(a));
    a.pool = db_pool_new(task, &_fake_ops, &cfg);
    if (NULL == a.pool
        || ERR_OK != db_pool_add(a.pool, &c1)
        || ERR_OK != db_pool_add(a.pool, &c2)) {
        LOG_ERROR("%s", "pool slot wake: setup failed.");
        if (NULL != a.pool) {
            db_pool_free(a.pool);
        }
        return ERR_FAILED;
    }
    coro_fn fns[3] = { _sw_long, _sw_broken, _sw_waiter };
    void *args[3] = { &a, &a, &a };
    int32_t rtn = ERR_OK;
    if (ERR_OK != coro_fork_wait(coro_task_co(task), fns, args, 3)) {
        LOG_ERROR("%s", "pool slot wake: coro_fork_wait failed.");
        rtn = ERR_FAILED;
    }
    if (ERR_OK != a.rtn
        || 0 == a.early) {
        LOG_ERROR("pool slot wake: rtn=%d early=%d.", a.rtn, a.early);
        rtn = ERR_FAILED;
    }
    db_pool_free(a.pool);
    return rtn;
}

// 一直有人排队时 max_lifetime 也要生效：归还时到期不交接，等待者拿到的是重连过的
static void _lt_waiter(void *owner, void *arg) {
    (void)owner;
    worker_arg *a = (worker_arg *)arg;
    void *conn = NULL;
    a->rtn = db_pool_get(a->sh->pool, 0, &conn);
    if (ERR_OK == a->rtn) {
        db_pool_put(a->sh->pool, conn);
    }
}
static int32_t _test_lifetime_handoff(task_ctx *task) {
    fake_conn c1;
    ZERO(&c1, sizeof(c1));
    db_pool_cfg cfg = { .cap = 1, .max_lifetime_ms = 30, .backoff_ms = 20 };
    db_pool_ctx *pool = db_pool_new(task, &_fake_ops, &cfg);
    if (NULL == pool
        || ERR_OK != db_pool_add(pool, &c1)) {
        LOG_ERROR("%s", "pool lifetime handoff: setup failed.");
        if (NULL != pool) {
            db_pool_free(pool);
        }
        return ERR_FAILED;
    }
    pool_shared sh;
    ZERO(&sh, sizeof(sh));
    sh.pool = pool;
    worker_arg holder, waiter;
    ZERO(&holder, sizeof(holder));
    ZERO(&waiter, sizeof(waiter));
    holder.sh = &sh;
    holder.hold_ms = 60;// 持有超过寿命，归还时已到期
    waiter.sh = &sh;
    coro_fn fns[2] = { _timeout_holder, _lt_waiter };
    void *args[2] = { &holder, &waiter };
    int32_t rtn = ERR_OK;
    if (ERR_OK != coro_fork_wait(coro_task_co(task), fns, args, 2)) {
        LOG_ERROR("%s", "pool lifetime handoff: coro_fork_wait failed.");
        rtn = ERR_FAILED;
    }
    if (ERR_OK != holder.rtn
        || ERR_OK != waiter.rtn
        || 2 != c1.connects
        || 1 != c1.quits) {
        LOG_ERROR("pool lifetime handoff: rtn=%d/%d connects=%d quits=%d, want 2/1.",
                  holder.rtn, waiter.rtn, c1.connects, c1.quits);
        rtn = ERR_FAILED;
    }
    db_pool_free(pool);
    return rtn;
}

// 有连接借出、空闲的在冷却：不快速失败，排队等归还或冷却到期
static void _cw_holder(void *owner, void *arg) {
    worker_arg *a = (worker_arg *)arg;
    void *conn = NULL;
    a->rtn = db_pool_get(a->sh->pool, 0, &conn);
    if (ERR_OK != a->rtn) {
        return;
    }
    coro_sleep((task_ctx *)owner, a->hold_ms);
    db_pool_put(a->sh->pool, conn);
}
static int32_t _test_cool_wait(task_ctx *task) {
    fake_conn c1, c2;
    ZERO(&c1, sizeof(c1));
    ZERO(&c2, sizeof(c2));
    db_pool_cfg cfg = { .cap = 2, .backoff_ms = 5000 };
    db_pool_ctx *pool = db_pool_new(task, &_fake_ops, &cfg);
    if (NULL == pool
        || ERR_OK != db_pool_add(pool, &c1)
        || ERR_OK != db_pool_add(pool, &c2)) {
        LOG_ERROR("%s", "pool cool wait: setup failed.");
        if (NULL != pool) {
            db_pool_free(pool);
        }
        return ERR_FAILED;
    }
    pool_shared sh;
    ZERO(&sh, sizeof(sh));
    sh.pool = pool;
    worker_arg holder, getter;
    ZERO(&holder, sizeof(holder));
    ZERO(&getter, sizeof(getter));
    holder.sh = &sh;
    holder.hold_ms = 100;
    getter.sh = &sh;
    // 栈顶先借给 holder，getter 拿到的那条建连失败进入冷却（5 秒，远长于 holder 持有）
    c1.fail_connect = 1;
    coro_fn fns[2] = { _cw_holder, _timeout_waiter };
    void *args[2] = { &holder, &getter };
    int32_t rtn = ERR_OK;
    if (ERR_OK != coro_fork_wait(coro_task_co(task), fns, args, 2)) {
        LOG_ERROR("%s", "pool cool wait: coro_fork_wait failed.");
        rtn = ERR_FAILED;
    }
    // _timeout_waiter 借到即算 fail：这里要的正是借到，看 sh.fail
    if (ERR_OK != holder.rtn
        || 1 != sh.fail) {
        LOG_ERROR("pool cool wait: holder=%d got=%d, want the waiter to get the returned conn.", holder.rtn, sh.fail);
        rtn = ERR_FAILED;
    }
    db_pool_free(pool);
    return rtn;
}

// 连续失败的冷却按 backoff_ms 翻倍
static int32_t _test_backoff_double(task_ctx *task) {
    fake_conn c1;
    ZERO(&c1, sizeof(c1));
    c1.fail_connect = 3;
    db_pool_cfg cfg = { .cap = 1, .backoff_ms = 100 };
    db_pool_ctx *pool = db_pool_new(task, &_fake_ops, &cfg);
    if (NULL == pool
        || ERR_OK != db_pool_add(pool, &c1)) {
        LOG_ERROR("%s", "pool backoff double: setup failed.");
        if (NULL != pool) {
            db_pool_free(pool);
        }
        return ERR_FAILED;
    }
    int32_t rtn = ERR_OK;
    void *conn = NULL;
    (void)db_pool_get(pool, 0, &conn);// t0 第 1 次失败：冷却 100ms
    coro_sleep(task, 150);
    (void)db_pool_get(pool, 0, &conn);// t150 第 2 次失败：冷却 200ms，到 t350
    coro_sleep(task, 100);
    if (ERR_OK == db_pool_get(pool, 0, &conn)// t250 仍在冷却；不翻倍的话 t250 早过了
        || 2 != c1.connects) {
        LOG_ERROR("pool backoff double: connects=%d, want 2 (second cool not doubled).", c1.connects);
        rtn = ERR_FAILED;
    }
    coro_sleep(task, 150);
    (void)db_pool_get(pool, 0, &conn);// t400 第 3 次失败：冷却 400ms，到 t800
    coro_sleep(task, 450);
    if (ERR_OK != db_pool_get(pool, 0, &conn)
        || 4 != c1.connects) {
        LOG_ERROR("pool backoff double: connects=%d, want 4.", c1.connects);
        rtn = ERR_FAILED;
    } else {
        db_pool_put(pool, conn);
    }
    db_pool_free(pool);
    return rtn;
}

// 寿命随机少最多 1/8：到 7/8 之前一定没到期，过了整个寿命一定到期
static int32_t _test_lifetime_jitter(task_ctx *task) {
    fake_conn c1;
    ZERO(&c1, sizeof(c1));
    db_pool_cfg cfg = { .cap = 1, .max_lifetime_ms = 160, .backoff_ms = 20 };
    db_pool_ctx *pool = db_pool_new(task, &_fake_ops, &cfg);
    if (NULL == pool
        || ERR_OK != db_pool_add(pool, &c1)) {
        LOG_ERROR("%s", "pool lifetime jitter: setup failed.");
        if (NULL != pool) {
            db_pool_free(pool);
        }
        return ERR_FAILED;
    }
    int32_t rtn = ERR_OK;
    void *conn = NULL;
    for (int32_t i = 0; i < 3 && ERR_OK == rtn; i++) {
        coro_sleep(task, 0 == i ? 0 : 40);// 借出时刻约 0 / 40 / 80ms，都在 140ms 之前
        if (ERR_OK != db_pool_get(pool, 0, &conn)) {
            rtn = ERR_FAILED;
            break;
        }
        db_pool_put(pool, conn);
    }
    if (ERR_OK != rtn
        || 1 != c1.connects) {
        LOG_ERROR("pool lifetime jitter: connects=%d, want 1 before 7/8 of the lifetime.", c1.connects);
        rtn = ERR_FAILED;
    }
    coro_sleep(task, 100);
    if (ERR_OK != db_pool_get(pool, 0, &conn)
        || 2 != c1.connects) {
        LOG_ERROR("pool lifetime jitter: connects=%d, want 2 after the lifetime.", c1.connects);
        rtn = ERR_FAILED;
    } else {
        db_pool_put(pool, conn);
    }
    db_pool_free(pool);
    return rtn;
}

// reset：归还时清掉会话状态再给下一个人；清不掉就断开
static int32_t _test_reset(task_ctx *task) {
    fake_conn c1;
    ZERO(&c1, sizeof(c1));
    db_pool_cfg cfg = { .cap = 1, .backoff_ms = 20 };
    db_pool_ctx *pool = db_pool_new(task, &_fake_ops, &cfg);
    if (NULL == pool
        || ERR_OK != db_pool_add(pool, &c1)) {
        LOG_ERROR("%s", "pool reset: setup failed.");
        if (NULL != pool) {
            db_pool_free(pool);
        }
        return ERR_FAILED;
    }
    int32_t rtn = ERR_OK;
    void *conn = NULL;
    // 干净的不算一次清理
    if (ERR_OK != db_pool_get(pool, 0, &conn)) {
        rtn = ERR_FAILED;
        goto out;
    }
    db_pool_put(pool, conn);
    // 脏的：清掉、不断开、不重连
    if (ERR_OK != db_pool_get(pool, 0, &conn)) {
        rtn = ERR_FAILED;
        goto out;
    }
    c1.dirty = 1;
    db_pool_put(pool, conn);
    if (1 != c1.resets || 0 != c1.dirty || 0 != c1.quits) {
        LOG_ERROR("pool reset: resets=%d dirty=%d quits=%d, want 1/0/0.", c1.resets, c1.dirty, c1.quits);
        rtn = ERR_FAILED;
        goto out;
    }
    // 清不掉：断开，下次借出重连
    if (ERR_OK != db_pool_get(pool, 0, &conn)) {
        rtn = ERR_FAILED;
        goto out;
    }
    c1.dirty = 1;
    c1.fail_reset = 1;
    db_pool_put(pool, conn);
    if (1 != c1.quits) {
        LOG_ERROR("pool reset: quits=%d after failed reset, want 1.", c1.quits);
        rtn = ERR_FAILED;
        goto out;
    }
    c1.dirty = 0;
    if (ERR_OK != db_pool_get(pool, 0, &conn)
        || 2 != c1.connects) {
        LOG_ERROR("pool reset: connects=%d, want 2.", c1.connects);
        rtn = ERR_FAILED;
        goto out;
    }
    db_pool_put(pool, conn);
out:
    db_pool_free(pool);
    return rtn;
}

// 维护协程：没人借还也收缩空闲；补足 min_idle；借出过久告警。协程超时是秒级粒度，等 2.3 秒
static int32_t _test_tick(task_ctx *task) {
    fake_conn c1, c2, c3;
    ZERO(&c1, sizeof(c1));
    ZERO(&c2, sizeof(c2));
    ZERO(&c3, sizeof(c3));
    db_pool_cfg cfg = { .cap = 3, .min_idle = 1, .idle_timeout_ms = 10, .leak_warn_ms = 500, .backoff_ms = 20 };
    db_pool_ctx *pool = db_pool_new(task, &_fake_ops, &cfg);
    if (NULL == pool
        || ERR_OK != db_pool_add(pool, &c1)
        || ERR_OK != db_pool_add(pool, &c2)
        || ERR_OK != db_pool_add(pool, &c3)) {
        LOG_ERROR("%s", "pool tick: setup failed.");
        if (NULL != pool) {
            db_pool_free(pool);
        }
        return ERR_FAILED;
    }
    int32_t rtn = ERR_OK;
    void *held = NULL;
    void *ca = NULL;
    void *cb = NULL;
    // 借三条：一条一直拿着（借出过久），两条马上还（空闲超时、但要留 min_idle=1 条）
    if (ERR_OK != db_pool_get(pool, 0, &held)
        || ERR_OK != db_pool_get(pool, 0, &ca)
        || ERR_OK != db_pool_get(pool, 0, &cb)) {
        LOG_ERROR("%s", "pool tick: get failed.");
        rtn = ERR_FAILED;
        goto out;
    }
    db_pool_put(pool, ca);
    db_pool_put(pool, cb);
    coro_sleep(task, 2300);
    db_pool_stat st;
    db_pool_stat_get(pool, &st);
    int32_t quits = ((fake_conn *)ca)->quits + ((fake_conn *)cb)->quits;
    if (1 != quits
        || 1 != st.leaks) {
        LOG_ERROR("pool tick: quits=%d leaks=%u, want 1/1.", quits, st.leaks);
        rtn = ERR_FAILED;
        goto out;
    }
    // 第一个池收尾
    db_pool_put(pool, held);
    held = NULL;
    db_pool_free(pool);
    pool = NULL;
    // 另起一个 min_idle=2 的池，只借还一次，等维护协程补齐
    fake_conn d1, d2;
    ZERO(&d1, sizeof(d1));
    ZERO(&d2, sizeof(d2));
    db_pool_cfg cfg2 = { .cap = 2, .min_idle = 2, .backoff_ms = 20 };
    pool = db_pool_new(task, &_fake_ops, &cfg2);
    if (NULL == pool
        || ERR_OK != db_pool_add(pool, &d1)
        || ERR_OK != db_pool_add(pool, &d2)
        || ERR_OK != db_pool_get(pool, 0, &ca)) {
        LOG_ERROR("%s", "pool tick: min_idle setup failed.");
        rtn = ERR_FAILED;
        goto out;
    }
    db_pool_put(pool, ca);
    coro_sleep(task, 2300);
    if (1 != d1.connects
        || 1 != d2.connects) {
        LOG_ERROR("pool tick: min_idle fill connects=%d/%d, want 1/1.", d1.connects, d2.connects);
        rtn = ERR_FAILED;
    }
out:
    if (NULL != pool) {
        if (NULL != held) {
            db_pool_put(pool, held);
        }
        db_pool_free(pool);
    }
    return rtn;
}

// 池内部挂起（清会话 / 探活 / 收缩断开）期间被 free：回来后只断一次、最后离开的释放池（ASan 下看 UAF）
typedef struct suspend_arg {
    db_pool_ctx *pool;
    void *conn;
    int32_t rtn;
    uint32_t delay_ms; // _sus_free 等多久再 free
}suspend_arg;
static void _sus_put(void *owner, void *arg) {
    (void)owner;
    suspend_arg *a = (suspend_arg *)arg;
    db_pool_put(a->pool, a->conn);
}
static void _sus_get(void *owner, void *arg) {
    (void)owner;
    suspend_arg *a = (suspend_arg *)arg;
    void *conn = NULL;
    a->rtn = db_pool_get(a->pool, 0, &conn);
}
static void _sus_free(void *owner, void *arg) {
    suspend_arg *a = (suspend_arg *)arg;
    coro_sleep((task_ctx *)owner, a->delay_ms);
    db_pool_free(a->pool);
}
static int32_t _sus_setup(task_ctx *task, fake_conn *c, const db_pool_cfg *cfg, db_pool_ctx **pool) {
    ZERO(c, sizeof(*c));
    c->task = task;
    *pool = db_pool_new(task, &_fake_ops, cfg);
    if (NULL == *pool
        || ERR_OK != db_pool_add(*pool, c)) {
        LOG_ERROR("%s", "pool closed suspend: setup failed.");
        if (NULL != *pool) {
            db_pool_free(*pool);
        }
        return ERR_FAILED;
    }
    return ERR_OK;
}
static int32_t _test_closed_suspend(task_ctx *task) {
    static fake_conn c1;// 维护协程可能比本函数活得久，连接放静态区
    db_pool_ctx *pool;
    suspend_arg a;
    suspend_arg f;
    coro_fn fns[2];
    void *args[2] = { &a, &f };
    // 1. 归还时清会话挂起，期间被 free：清完由归还方断开一次
    db_pool_cfg cfg = { .cap = 1, .backoff_ms = 20, .ping_idle_ms = 1 };
    if (ERR_OK != _sus_setup(task, &c1, &cfg, &pool)
        || ERR_OK != db_pool_get(pool, 0, &a.conn)) {
        return ERR_FAILED;
    }
    c1.dirty = 1;
    c1.op_ms = 50;
    a.pool = pool;
    f.pool = pool;
    f.delay_ms = 10;
    fns[0] = _sus_put;
    fns[1] = _sus_free;
    if (ERR_OK != coro_fork_wait(coro_task_co(task), fns, args, 2)
        || 1 != c1.resets
        || 1 != c1.quits) {
        LOG_ERROR("pool closed suspend: reset resets=%d quits=%d, want 1/1.", c1.resets, c1.quits);
        return ERR_FAILED;
    }
    // 2. 借出时探活挂起，期间被 free：借连接失败，连接断开一次
    if (ERR_OK != _sus_setup(task, &c1, &cfg, &pool)
        || ERR_OK != db_pool_get(pool, 0, &a.conn)) {
        return ERR_FAILED;
    }
    db_pool_put(pool, a.conn);
    coro_sleep(task, 5);// 空闲超过 ping_idle_ms
    c1.op_ms = 50;
    a.pool = pool;
    a.rtn = ERR_OK;
    f.pool = pool;
    fns[0] = _sus_get;
    if (ERR_OK != coro_fork_wait(coro_task_co(task), fns, args, 2)
        || ERR_FAILED != a.rtn
        || 1 != c1.pings
        || 1 != c1.quits) {
        LOG_ERROR("pool closed suspend: ping rtn=%d pings=%d quits=%d, want -1/1/1.", a.rtn, c1.pings, c1.quits);
        return ERR_FAILED;
    }
    // 3. 维护协程收缩断开时挂起，期间被 free：不重复断开，维护协程回来后收尾释放池。
    // 第一次巡检在借出后 1~2 秒，quit 挂 1.5 秒，2.1 秒时 free 必落在 quit 期间
    db_pool_cfg cfg3 = { .cap = 1, .backoff_ms = 20, .idle_timeout_ms = 10 };
    if (ERR_OK != _sus_setup(task, &c1, &cfg3, &pool)
        || ERR_OK != db_pool_get(pool, 0, &a.conn)) {
        return ERR_FAILED;
    }
    db_pool_put(pool, a.conn);
    c1.op_ms = 1500;
    coro_sleep(task, 2100);
    db_pool_free(pool);
    coro_sleep(task, 1600);
    if (1 != c1.quits) {
        LOG_ERROR("pool closed suspend: reap quits=%d, want 1.", c1.quits);
        return ERR_FAILED;
    }
    return ERR_OK;
}
// 维护协程补 min_idle 时建连失败、已无借出：要叫醒只因它占着 busy 而排队的 get，让它快速失败。
// 巡检在首次 get 后 1~2 秒，建连挂 1.5 秒，2.1 秒时排队必落在建连期间
static int32_t _test_fill_wake(task_ctx *task) {
    static fake_conn c1;
    db_pool_ctx *pool;
    db_pool_cfg cfg = { .cap = 1, .min_idle = 1, .backoff_ms = 20 };
    if (ERR_OK != _sus_setup(task, &c1, &cfg, &pool)) {
        return ERR_FAILED;
    }
    c1.fail_connect = 1000;
    void *conn = NULL;
    if (ERR_OK == db_pool_get(pool, 0, &conn)) {
        LOG_ERROR("%s", "pool fill wake: first get should fail.");
        db_pool_free(pool);
        return ERR_FAILED;
    }
    c1.connect_ms = 1500;
    suspend_arg a;
    ZERO(&a, sizeof(a));
    a.pool = pool;
    a.rtn = 99;
    coro_sleep(task, 2100);
    coro_fork(coro_task_co(task), _sus_get, &a);
    coro_sleep(task, 1700);
    int32_t got = a.rtn;
    db_pool_free(pool);
    coro_sleep(task, 1600);// 维护协程可能还挂在下一次建连上
    if (ERR_FAILED != got) {
        LOG_ERROR("pool fill wake: waiter rtn=%d, want -1.", got);
        return ERR_FAILED;
    }
    return ERR_OK;
}
static void _startup_run(task_ctx *task) {
    task_pool_args *arg = (task_pool_args *)coro_get_arg(task);
    if (ERR_OK != _test_add(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_lazy(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_broken(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_backoff(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_bound(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_fifo(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_ping(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_lifetime(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_idle_reap(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_wake_free(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_slot_wake(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_lifetime_handoff(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_cool_wait(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_backoff_double(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_lifetime_jitter(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_reset(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_tick(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_timeout(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_free_waiters(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_closed_suspend(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_fill_wake(task)) {
        return;
    }
    ATOMIC_SET((atomic_t *)arg->ok, 1);
    LOG_INFO("%s", "db pool tested.");
}
static void _startup(task_ctx *task) {
    _startup_run(task);
    test_fail_unfinished(task, ((task_pool_args *)coro_get_arg(task))->ok);
}
static void _argfree(void *arg) {
    FREE(arg);
}
void task_pool_start(loader_ctx *loader, const char *name, int32_t *ok) {
    if (NULL == ok) {
        return;
    }
    task_pool_args *arg;
    CALLOC(arg, 1, sizeof(task_pool_args));
    arg->ok = ok;
    coro_task_register(loader, name, 0, _startup, NULL, _argfree, arg);
}
