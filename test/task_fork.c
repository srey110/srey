#include "task_fork.h"

typedef struct task_fork_args {
    int32_t *ok;
}task_fork_args;

// ── 测试 1：单个 fork ─────────────────────────────────────────────────────
typedef struct single_arg {
    int32_t hit;
    int32_t expect_val;
}single_arg;

static void _single_worker(task_ctx *task, void *arg) {
    (void)task;
    single_arg *a = (single_arg *)arg;
    a->hit = a->expect_val;
}

static int32_t _test_single(task_ctx *task) {
    single_arg a = { .hit = 0, .expect_val = 42 };
    coro_fork(task, _single_worker, &a);
    // fork 不立即执行，需 yield 让出当前协程
    coro_sleep(task, 30);
    if (42 != a.hit) {
        LOG_ERROR("fork single: expect hit=42, got %d.", a.hit);
        return ERR_FAILED;
    }
    return ERR_OK;
}

// ── 测试 2：多个 fork 顺序 ────────────────────────────────────────────────
static int32_t g_multi_count;

static void _multi_worker(task_ctx *task, void *arg) {
    (void)task;
    (void)arg;
    ++g_multi_count;
}

static int32_t _test_multi(task_ctx *task) {
    g_multi_count = 0;
    coro_fork(task, _multi_worker, NULL);
    coro_fork(task, _multi_worker, NULL);
    coro_fork(task, _multi_worker, NULL);
    coro_sleep(task, 50);
    if (3 != g_multi_count) {
        LOG_ERROR("fork multi: expect 3, got %d.", g_multi_count);
        return ERR_FAILED;
    }
    return ERR_OK;
}

// ── 测试 3：fork 内 yield ─────────────────────────────────────────────────
typedef struct yield_arg {
    int32_t before;
    int32_t after;
}yield_arg;

static void _yield_worker(task_ctx *task, void *arg) {
    yield_arg *a = (yield_arg *)arg;
    a->before = 1;
    coro_sleep(task, 20);
    a->after = 2;
}

static int32_t _test_yield(task_ctx *task) {
    yield_arg a = { .before = 0, .after = 0 };
    coro_fork(task, _yield_worker, &a);
    coro_sleep(task, 100);
    if (1 != a.before || 2 != a.after) {
        LOG_ERROR("fork yield: expect before=1 after=2, got before=%d after=%d.",
                  a.before, a.after);
        return ERR_FAILED;
    }
    return ERR_OK;
}

// ── 测试 4：嵌套 fork ─────────────────────────────────────────────────────
static int32_t g_nested_depth;
static int32_t g_nested_inner_hit;

static void _nested_inner(task_ctx *task, void *arg) {
    (void)task;
    (void)arg;
    ++g_nested_inner_hit;
}

static void _nested_outer(task_ctx *task, void *arg) {
    (void)arg;
    ++g_nested_depth;
    coro_fork(task, _nested_inner, NULL);
    coro_fork(task, _nested_inner, NULL);
}

static int32_t _test_nested(task_ctx *task) {
    g_nested_depth = 0;
    g_nested_inner_hit = 0;
    coro_fork(task, _nested_outer, NULL);
    // 等外层 + 两个内层 fork 全跑完
    coro_sleep(task, 80);
    if (1 != g_nested_depth || 2 != g_nested_inner_hit) {
        LOG_ERROR("fork nested: expect depth=1 inner=2, got depth=%d inner=%d.",
                  g_nested_depth, g_nested_inner_hit);
        return ERR_FAILED;
    }
    return ERR_OK;
}

// ── 测试 5：scatter-gather（fork_wait 并发 sleep） ────────────────────────
typedef struct scatter_arg {
    uint32_t sleep_ms;
    uint64_t finish_ms;
}scatter_arg;

static void _scatter_worker(task_ctx *task, void *arg) {
    scatter_arg *a = (scatter_arg *)arg;
    coro_sleep(task, a->sleep_ms);
    a->finish_ms = nowms();
}

static int32_t _test_scatter(task_ctx *task) {
    scatter_arg sa[3] = {
        { .sleep_ms = 50, .finish_ms = 0 },
        { .sleep_ms = 50, .finish_ms = 0 },
        { .sleep_ms = 50, .finish_ms = 0 },
    };
    void *args[3] = { &sa[0], &sa[1], &sa[2] };
    void (*funcs[3])(task_ctx *, void *) = {
        _scatter_worker, _scatter_worker, _scatter_worker,
    };
    uint64_t t0 = nowms();
    int32_t r = coro_fork_wait(task, 3, funcs, args);
    uint64_t elapsed = nowms() - t0;
    if (ERR_OK != r) {
        LOG_ERROR("fork_wait scatter: return %d.", r);
        return ERR_FAILED;
    }
    if (0 == sa[0].finish_ms || 0 == sa[1].finish_ms || 0 == sa[2].finish_ms) {
        LOG_ERROR("fork_wait scatter: not all workers finished.");
        return ERR_FAILED;
    }
    // 3 个 sleep(50) 并发，总耗时应 < 串行 150ms（留 30ms 余量）
    if (elapsed > 120) {
        LOG_ERROR("fork_wait scatter: expected concurrent (<120ms), got %llums.",
                  (unsigned long long)elapsed);
        return ERR_FAILED;
    }
    return ERR_OK;
}

// ── 测试 6：fork_wait 0 任务立即返回 ──────────────────────────────────────
static int32_t _test_empty_wait(task_ctx *task) {
    int32_t r = coro_fork_wait(task, 0, NULL, NULL);
    if (ERR_OK != r) {
        LOG_ERROR("fork_wait empty: expect ERR_OK, got %d.", r);
        return ERR_FAILED;
    }
    return ERR_OK;
}

// ── 测试 7：fork_wait worker 多次 yield ───────────────────────────────────
typedef struct multi_yield_arg {
    int32_t step;
}multi_yield_arg;

static void _multi_yield_worker(task_ctx *task, void *arg) {
    multi_yield_arg *a = (multi_yield_arg *)arg;
    coro_sleep(task, 10);
    a->step = 1;
    coro_sleep(task, 10);
    a->step = 2;
    coro_sleep(task, 10);
    a->step = 3;
}

static int32_t _test_multi_yield(task_ctx *task) {
    multi_yield_arg ma[2] = { { .step = 0 }, { .step = 0 } };
    void *args[2] = { &ma[0], &ma[1] };
    void (*funcs[2])(task_ctx *, void *) = {
        _multi_yield_worker, _multi_yield_worker,
    };
    int32_t r = coro_fork_wait(task, 2, funcs, args);
    if (ERR_OK != r) {
        LOG_ERROR("fork_wait multi_yield: return %d.", r);
        return ERR_FAILED;
    }
    if (3 != ma[0].step || 3 != ma[1].step) {
        LOG_ERROR("fork_wait multi_yield: expect step=3,3, got %d,%d.",
                  ma[0].step, ma[1].step);
        return ERR_FAILED;
    }
    return ERR_OK;
}

// ── 测试 8：并发 fork_wait 非 LIFO 完成 ───────────────────────────────────
// 两个独立协程各 coro_fork_wait：先入 fork_barriers 链表者(A)其 worker 先完成 →
// 移除的是链表非头节点，回归"按节点解链不假设 LIFO"，否则关闭时 _coro_ctx_free 崩
typedef struct concurrent_arg {
    uint32_t sleep_ms;
    int32_t done;
    int32_t *ok;
}concurrent_arg;

static void _concurrent_worker(task_ctx *task, void *arg) {
    concurrent_arg *a = (concurrent_arg *)arg;
    coro_sleep(task, a->sleep_ms);
    a->done = 1;
}

static void _concurrent_driver(task_ctx *task, void *arg) {
    concurrent_arg *a = (concurrent_arg *)arg;
    void *wargs[1] = { a };
    void (*wfuncs[1])(task_ctx *, void *) = { _concurrent_worker };
    if (ERR_OK == coro_fork_wait(task, 1, wfuncs, wargs) && 1 == a->done) {
        *(a->ok) = 1;
    }
}

static int32_t _test_concurrent_fork_wait(task_ctx *task) {
    int32_t a_ok = 0;
    int32_t b_ok = 0;
    concurrent_arg ca = { .sleep_ms = 20, .done = 0, .ok = &a_ok };
    concurrent_arg cb = { .sleep_ms = 60, .done = 0, .ok = &b_ok };
    // A 先 fork（先入链表，处于链表尾），B 后 fork（链表头）；A 先完成 → 非 LIFO 移除
    coro_fork(task, _concurrent_driver, &ca);
    coro_fork(task, _concurrent_driver, &cb);
    coro_sleep(task, 140);// 等 A(~20ms) 与 B(~60ms) 两个 fork_wait 都完成
    if (1 != a_ok || 1 != b_ok) {
        LOG_ERROR("fork_wait concurrent: expect both ok, got a=%d b=%d.", a_ok, b_ok);
        return ERR_FAILED;
    }
    return ERR_OK;
}

static void _startup(task_ctx *task) {
    task_fork_args *arg = (task_fork_args *)coro_get_arg(task);
    if (ERR_OK != _test_single(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_multi(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_yield(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_nested(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_scatter(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_empty_wait(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_multi_yield(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_concurrent_fork_wait(task)) {
        return;
    }
    *(arg->ok) = 1;
    LOG_INFO("fork tested.");
}

void task_fork_start(loader_ctx *loader, const char *name, int32_t *ok) {
    if (NULL == ok) {
        return;
    }
    task_fork_args *arg;
    CALLOC(arg, 1, sizeof(task_fork_args));
    arg->ok = ok;
    coro_task_register(loader, name, 0, _startup, NULL, _free, arg);
}
