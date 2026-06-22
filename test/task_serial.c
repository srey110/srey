#include "task_serial.h"

typedef struct task_serial_args {
    int32_t *ok;
}task_serial_args;

// ── 测试 1：单协程进入 ───────────────────────────────────────────────────
typedef struct single_arg {
    int32_t hit;
    int32_t expect_val;
}single_arg;

static void _single_cs(task_ctx *task, void *arg) {
    (void)task;
    single_arg *a = (single_arg *)arg;
    a->hit = a->expect_val;
}

static int32_t _test_single(task_ctx *task) {
    coro_serial_ctx *s = coro_serial_new(task);
    single_arg a = { .hit = 0, .expect_val = 42 };
    int32_t r = coro_serial_call(s, _single_cs, &a);
    if (ERR_OK != r) {
        LOG_ERROR("serial single: coro_serial_call returns %d.", r);
        coro_serial_free(s);
        return ERR_FAILED;
    }
    if (42 != a.hit) {
        LOG_ERROR("serial single: expect hit=42, got %d.", a.hit);
        coro_serial_free(s);
        return ERR_FAILED;
    }
    coro_serial_free(s);
    return ERR_OK;
}

// ── 测试 2：同协程嵌套 cs ─────────────────────────────────────────────────
typedef struct nested_arg {
    coro_serial_ctx *s;
    int32_t outer;
    int32_t inner;
}nested_arg;

static void _nested_inner(task_ctx *task, void *arg) {
    (void)task;
    nested_arg *a = (nested_arg *)arg;
    a->inner = 1;
}
static void _nested_outer(task_ctx *task, void *arg) {
    (void)task;
    nested_arg *a = (nested_arg *)arg;
    a->outer = 1;
    int32_t r = coro_serial_call(a->s, _nested_inner, a);
    if (ERR_OK != r) {
        a->inner = -1;
    }
}

static int32_t _test_nested(task_ctx *task) {
    coro_serial_ctx *s = coro_serial_new(task);
    nested_arg a = { .s = s, .outer = 0, .inner = 0 };
    int32_t r = coro_serial_call(s, _nested_outer, &a);
    if (ERR_OK != r) {
        LOG_ERROR("serial nested: outer coro_serial_call returns %d.", r);
        coro_serial_free(s);
        return ERR_FAILED;
    }
    if (1 != a.outer || 1 != a.inner) {
        LOG_ERROR("serial nested: expect outer=1 inner=1, got outer=%d inner=%d.",
                  a.outer, a.inner);
        coro_serial_free(s);
        return ERR_FAILED;
    }
    coro_serial_free(s);
    return ERR_OK;
}

// ── 测试 3：跨协程串行 + FIFO ────────────────────────────────────────────
typedef struct fifo_arg {
    coro_serial_ctx *s;
    int32_t *order;
    int32_t *cnt;
    int32_t label;     // 1=A 2=B 3=C
    uint32_t hold_ms;  // 0 表示不 sleep
}fifo_arg;

static void _fifo_cs(task_ctx *task, void *arg) {
    fifo_arg *a = (fifo_arg *)arg;
    a->order[(*a->cnt)++] = a->label;       // 进入标签
    if (0 != a->hold_ms) {
        coro_sleep(task, a->hold_ms);       // 持锁 yield
        a->order[(*a->cnt)++] = a->label;   // 离开标签
    }
}

static void _fifo_worker(task_ctx *task, void *arg) {
    (void)task;
    fifo_arg *a = (fifo_arg *)arg;
    coro_serial_call(a->s, _fifo_cs, a);
}

static int32_t _test_fifo(task_ctx *task) {
    coro_serial_ctx *s = coro_serial_new(task);
    int32_t order[8] = { 0 };
    int32_t cnt = 0;
    fifo_arg ja = { .s = s, .order = order, .cnt = &cnt, .label = 1, .hold_ms = 30 }; // A 持锁 sleep
    fifo_arg jb = { .s = s, .order = order, .cnt = &cnt, .label = 2, .hold_ms = 0 };
    fifo_arg jc = { .s = s, .order = order, .cnt = &cnt, .label = 3, .hold_ms = 0 };
    // 三个协程依次 fork，进入 cs 的先后顺序 = fork 顺序（_drain_fork_queue 顺序起协程）
    void (*fifo_fns[3])(task_ctx *, void *) = { _fifo_worker, _fifo_worker, _fifo_worker };
    void *fifo_args[3] = { &ja, &jb, &jc };
    coro_fork_wait(task, 3, fifo_fns, fifo_args);
    // 期望 order = [1(A 进), 1(A 出), 2(B), 3(C)]：A 完全退出后 B 才能进
    if (4 != cnt || 1 != order[0] || 1 != order[1] || 2 != order[2] || 3 != order[3]) {
        LOG_ERROR("serial fifo: expect [1,1,2,3], got cnt=%d [%d,%d,%d,%d].",
                  cnt, order[0], order[1], order[2], order[3]);
        coro_serial_free(s);
        return ERR_FAILED;
    }
    coro_serial_free(s);
    return ERR_OK;
}

// ── 测试 4：不在协程上下文调用 → ERR_FAILED ──────────────────────────────
//   注：在 _startup 内已经在协程上下文，所以本测试在 _startup 外通过
//   coro_serial_call 直接调用（loader 主线程上下文）覆盖不了，因为 coro_serial_new 也需要 task。
//   折中做法：在 _startup 协程内已无法触发"非协程上下文"路径，跳过此 case 直接判定通过。
//   真实场景由 ASSERTAB / LOG_WARN 在生产中兜底。

// ── 测试 5：多 serial 实例独立 ───────────────────────────────────────────
typedef struct indep_arg {
    coro_serial_ctx *s;
    int32_t *flag;
    int32_t set_val;
    uint32_t sleep_ms;
}indep_arg;

static void _indep_cs(task_ctx *task, void *arg) {
    indep_arg *a = (indep_arg *)arg;
    if (0 != a->sleep_ms) {
        coro_sleep(task, a->sleep_ms);
    }
    *a->flag = a->set_val;
}

static void _indep_worker(task_ctx *task, void *arg) {
    (void)task;
    indep_arg *a = (indep_arg *)arg;
    coro_serial_call(a->s, _indep_cs, a);
}

static int32_t _test_indep(task_ctx *task) {
    coro_serial_ctx *s1 = coro_serial_new(task);
    coro_serial_ctx *s2 = coro_serial_new(task);
    int32_t f1 = 0, f2 = 0;
    indep_arg a1 = { .s = s1, .flag = &f1, .set_val = 1, .sleep_ms = 30 }; // s1 持锁 30ms
    indep_arg a2 = { .s = s2, .flag = &f2, .set_val = 2, .sleep_ms = 0  }; // s2 立即完成
    coro_fork(task, _indep_worker, &a1);
    coro_fork(task, _indep_worker, &a2);
    coro_sleep(task, 10);                   // 短等：s2 应该已完成，s1 仍在 sleep
    if (2 != f2) {
        LOG_ERROR("serial indep: s2 should finish quickly, f2=%d.", f2);
        coro_serial_free(s1); coro_serial_free(s2);
        return ERR_FAILED;
    }
    if (0 != f1) {
        LOG_ERROR("serial indep: s1 should still be in sleep, f1=%d.", f1);
        coro_serial_free(s1); coro_serial_free(s2);
        return ERR_FAILED;
    }
    coro_sleep(task, 50);                   // 等 s1 完成
    if (1 != f1) {
        LOG_ERROR("serial indep: s1 should finish, f1=%d.", f1);
        coro_serial_free(s1); coro_serial_free(s2);
        return ERR_FAILED;
    }
    coro_serial_free(s1);
    coro_serial_free(s2);
    return ERR_OK;
}

// ── 测试 6：cs 内 yield 期间互斥（peak == 1） ─────────────────────────────
typedef struct mutex_arg {
    coro_serial_ctx *s;
    int32_t *in_cs;
    int32_t *peak;
}mutex_arg;

static void _mutex_cs(task_ctx *task, void *arg) {
    mutex_arg *a = (mutex_arg *)arg;
    (*a->in_cs)++;
    if (*a->in_cs > *a->peak) {
        *a->peak = *a->in_cs;
    }
    coro_sleep(task, 20);                   // yield 期间另一个协程会试图进 cs
    (*a->in_cs)--;
}

static void _mutex_worker(task_ctx *task, void *arg) {
    (void)task;
    mutex_arg *a = (mutex_arg *)arg;
    coro_serial_call(a->s, _mutex_cs, a);
}

static int32_t _test_mutex(task_ctx *task) {
    coro_serial_ctx *s = coro_serial_new(task);
    int32_t in_cs = 0, peak = 0;
    mutex_arg a = { .s = s, .in_cs = &in_cs, .peak = &peak };
    void (*mutex_fns[2])(task_ctx *, void *) = { _mutex_worker, _mutex_worker };
    void *mutex_args[2] = { &a, &a };
    coro_fork_wait(task, 2, mutex_fns, mutex_args);
    if (1 != peak) {
        LOG_ERROR("serial mutex: expect peak=1, got %d.", peak);
        coro_serial_free(s);
        return ERR_FAILED;
    }
    if (0 != in_cs) {
        LOG_ERROR("serial mutex: expect in_cs=0 after all done, got %d.", in_cs);
        coro_serial_free(s);
        return ERR_FAILED;
    }
    coro_serial_free(s);
    return ERR_OK;
}

// ── 测试 7：cs 出口 curco 还原 ───────────────────────────────────────────
// 触发条件：A 持锁 sleep 期间 B 排队入 cs；A 完成 release 唤醒 B,B 在 cs 内 yield
// 后 release 返回,A 的 coro_serial_call 返回；A 继续调 coro_sleep —— 修复前
// curco stale=B 触发 mco_yield(MCO_NOT_RUNNING) abort,修复后 curco 已还原为 A
typedef struct curco_arg {
    coro_serial_ctx *s;
    int32_t *a_done;
    int32_t *b_done;
}curco_arg;

static void _curco_cs(task_ctx *task, void *arg) {
    (void)arg;
    coro_sleep(task, 20);                       // cs 内持锁 yield
}

// A: cs 出口后必须再调一次 coro_sleep,这是 B05 触发点
static void _curco_worker_a(task_ctx *task, void *arg) {
    curco_arg *a = (curco_arg *)arg;
    coro_serial_call(a->s, _curco_cs, NULL);
    coro_sleep(task, 5);                        // 修复前 curco stale=B → ABORT
    *a->a_done = 1;
}

// B: 在 A 持锁 sleep 期间 fork 进入,走跨协程路径 mco_yield 入队
static void _curco_worker_b(task_ctx *task, void *arg) {
    (void)task;
    curco_arg *a = (curco_arg *)arg;
    coro_serial_call(a->s, _curco_cs, NULL);
    *a->b_done = 1;
}

static int32_t _test_curco_restore(task_ctx *task) {
    coro_serial_ctx *s = coro_serial_new(task);
    int32_t a_done = 0, b_done = 0;
    curco_arg arg = { .s = s, .a_done = &a_done, .b_done = &b_done };
    void (*curco_fns[2])(task_ctx *, void *) = { _curco_worker_a, _curco_worker_b };
    void *curco_args[2] = { &arg, &arg };
    coro_fork_wait(task, 2, curco_fns, curco_args);// A 先 fork → 占锁 sleep，B 后 fork → 跨协程路径入队
    if (1 != a_done) {
        LOG_ERROR("serial curco_restore: A did not finish post-cs coro_sleep (curco stale?), a_done=%d.", a_done);
        coro_serial_free(s);
        return ERR_FAILED;
    }
    if (1 != b_done) {
        LOG_ERROR("serial curco_restore: B did not finish, b_done=%d.", b_done);
        coro_serial_free(s);
        return ERR_FAILED;
    }
    coro_serial_free(s);
    return ERR_OK;
}

static void _startup(task_ctx *task) {
    task_serial_args *arg = (task_serial_args *)coro_get_arg(task);
    if (ERR_OK != _test_single(task)) {
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
    if (ERR_OK != _test_fifo(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_indep(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_mutex(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_curco_restore(task)) {
        return;
    }
    *(arg->ok) = 1;
    LOG_INFO("serial tested.");
}

void task_serial_start(loader_ctx *loader, const char *name, int32_t *ok) {
    if (NULL == ok) {
        return;
    }
    task_serial_args *arg;
    CALLOC(arg, 1, sizeof(task_serial_args));
    arg->ok = ok;
    coro_task_register(loader, name, 0, _startup, NULL, _free, arg);
}
