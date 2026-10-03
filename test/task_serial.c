#include "task_serial.h"

typedef struct task_serial_args {
    int32_t *ok;
}task_serial_args;

// ── 测试 1：单协程进入 ───────────────────────────────────────────────────
typedef struct single_arg {
    int32_t hit;
    int32_t expect_val;
}single_arg;

static void _single_cs(void *owner, void *arg) {
    (void)owner;
    single_arg *a = (single_arg *)arg;
    a->hit = a->expect_val;
}

static int32_t _test_single(task_ctx *task) {
    coro_serial_ctx *s = coro_serial_new(coro_task_co(task));
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

static void _nested_inner(void *owner, void *arg) {
    (void)owner;
    nested_arg *a = (nested_arg *)arg;
    a->inner = 1;
}
static void _nested_outer(void *owner, void *arg) {
    (void)owner;
    nested_arg *a = (nested_arg *)arg;
    a->outer = 1;
    int32_t r = coro_serial_call(a->s, _nested_inner, a);
    if (ERR_OK != r) {
        a->inner = -1;
    }
}

static int32_t _test_nested(task_ctx *task) {
    coro_serial_ctx *s = coro_serial_new(coro_task_co(task));
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

static void _fifo_cs(void *owner, void *arg) {
    task_ctx *task = owner;
    fifo_arg *a = (fifo_arg *)arg;
    a->order[(*a->cnt)++] = a->label;// 进入标签
    if (0 != a->hold_ms) {
        coro_sleep(task, a->hold_ms);// 持锁 yield
        a->order[(*a->cnt)++] = a->label;// 离开标签
    }
}

static void _fifo_worker(void *owner, void *arg) {
    (void)owner;
    fifo_arg *a = (fifo_arg *)arg;
    coro_serial_call(a->s, _fifo_cs, a);
}

static int32_t _test_fifo(task_ctx *task) {
    coro_serial_ctx *s = coro_serial_new(coro_task_co(task));
    int32_t order[8] = { 0 };
    int32_t cnt = 0;
    fifo_arg ja = { .s = s, .order = order, .cnt = &cnt, .label = 1, .hold_ms = 30 }; // A 持锁 sleep
    fifo_arg jb = { .s = s, .order = order, .cnt = &cnt, .label = 2, .hold_ms = 0 };
    fifo_arg jc = { .s = s, .order = order, .cnt = &cnt, .label = 3, .hold_ms = 0 };
    // 三个协程依次 fork，进入 cs 的先后顺序 = fork 顺序（coro_fork_drain 按 FIFO 起协程）
    coro_fn fifo_fns[3] = { _fifo_worker, _fifo_worker, _fifo_worker };
    void *fifo_args[3] = { &ja, &jb, &jc };
    coro_fork_wait(coro_task_co(task), fifo_fns, fifo_args, 3);
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

static void _indep_cs(void *owner, void *arg) {
    task_ctx *task = owner;
    indep_arg *a = (indep_arg *)arg;
    if (0 != a->sleep_ms) {
        coro_sleep(task, a->sleep_ms);
    }
    *a->flag = a->set_val;
}

static void _indep_worker(void *owner, void *arg) {
    (void)owner;
    indep_arg *a = (indep_arg *)arg;
    coro_serial_call(a->s, _indep_cs, a);
}

static int32_t _test_indep(task_ctx *task) {
    coro_serial_ctx *s1 = coro_serial_new(coro_task_co(task));
    coro_serial_ctx *s2 = coro_serial_new(coro_task_co(task));
    // 本用例故意在 10ms 处就判断，此时 a1 的 worker 还在 sleep(30)。失败路径一 return，
    // 栈上的 a1/f1 就没了，而 worker 醒来还要写 *a->flag —— 连同两个标志一起放 static
    static int32_t f1;
    static int32_t f2;
    static indep_arg a1;
    static indep_arg a2;
    f1 = 0;
    f2 = 0;
    a1.s = s1; a1.flag = &f1; a1.set_val = 1; a1.sleep_ms = 30;// s1 持锁 30ms
    a2.s = s2; a2.flag = &f2; a2.set_val = 2; a2.sleep_ms = 0;// s2 立即完成
    coro_fork(coro_task_co(task), _indep_worker, &a1);
    coro_fork(coro_task_co(task), _indep_worker, &a2);
    coro_sleep(task, 10);// 短等：s2 应该已完成，s1 仍在 sleep
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
    coro_sleep(task, 50);// 等 s1 完成
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

static void _mutex_cs(void *owner, void *arg) {
    task_ctx *task = owner;
    mutex_arg *a = (mutex_arg *)arg;
    (*a->in_cs)++;
    if (*a->in_cs > *a->peak) {
        *a->peak = *a->in_cs;
    }
    coro_sleep(task, 20);// yield 期间另一个协程会试图进 cs
    (*a->in_cs)--;
}

static void _mutex_worker(void *owner, void *arg) {
    (void)owner;
    mutex_arg *a = (mutex_arg *)arg;
    coro_serial_call(a->s, _mutex_cs, a);
}

static int32_t _test_mutex(task_ctx *task) {
    coro_serial_ctx *s = coro_serial_new(coro_task_co(task));
    int32_t in_cs = 0, peak = 0;
    mutex_arg a = { .s = s, .in_cs = &in_cs, .peak = &peak };
    coro_fn mutex_fns[2] = { _mutex_worker, _mutex_worker };
    void *mutex_args[2] = { &a, &a };
    coro_fork_wait(coro_task_co(task), mutex_fns, mutex_args, 2);
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
// curco stale=B 触发 mco_yield 报 MCO_STACK_OVERFLOW abort(minicoro 先判栈范围后判状态,
// &dummy 落在 A 栈上、不在 B 的范围内),修复后 curco 已还原为 A
typedef struct curco_arg {
    coro_serial_ctx *s;
    int32_t *a_done;
    int32_t *b_done;
}curco_arg;

static void _curco_cs(void *owner, void *arg) {
    task_ctx *task = owner;
    (void)arg;
    coro_sleep(task, 20);// cs 内持锁 yield
}

// A: cs 出口后必须再调一次 coro_sleep,这是 B05 触发点
static void _curco_worker_a(void *owner, void *arg) {
    task_ctx *task = owner;
    curco_arg *a = (curco_arg *)arg;
    coro_serial_call(a->s, _curco_cs, NULL);
    coro_sleep(task, 5);// 修复前 curco stale=B → ABORT
    *a->a_done = 1;
}

// B: 在 A 持锁 sleep 期间 fork 进入,走跨协程路径 mco_yield 入队
static void _curco_worker_b(void *owner, void *arg) {
    (void)owner;
    curco_arg *a = (curco_arg *)arg;
    coro_serial_call(a->s, _curco_cs, NULL);
    *a->b_done = 1;
}

static int32_t _test_curco_restore(task_ctx *task) {
    coro_serial_ctx *s = coro_serial_new(coro_task_co(task));
    int32_t a_done = 0, b_done = 0;
    curco_arg arg = { .s = s, .a_done = &a_done, .b_done = &b_done };
    coro_fn curco_fns[2] = { _curco_worker_a, _curco_worker_b };
    void *curco_args[2] = { &arg, &arg };
    coro_fork_wait(coro_task_co(task), curco_fns, curco_args, 2);// A 先 fork → 占锁 sleep，B 后 fork → 跨协程路径入队
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

// ── 测试 8：serial_node 池复用（多轮跨协程排队）──────────────────────────
// 反复令多协程争用同一 serial：队头持锁 sleep 期间其余走跨协程路径 pool_pop 排队，
// 释放时逐个 pool_push 出队。覆盖 serial_node_pool 高频 pop/push 复用 + serial 跨轮重用
typedef struct spool_arg {
    coro_serial_ctx *s;
    int32_t *cnt;
    uint32_t hold_ms;
}spool_arg;

static void _spool_cs(void *owner, void *arg) {
    task_ctx *task = owner;
    spool_arg *a = (spool_arg *)arg;
    if (0 != a->hold_ms) {
        coro_sleep(task, a->hold_ms);
    }
    ++(*a->cnt);
}

static void _spool_worker(void *owner, void *arg) {
    (void)owner;
    spool_arg *a = (spool_arg *)arg;
    coro_serial_call(a->s, _spool_cs, a);
}

static int32_t _test_serial_pool_reuse(task_ctx *task) {
    enum { ROUNDS = 16, CONTEND = 4 };
    coro_serial_ctx *s = coro_serial_new(coro_task_co(task));
    int32_t cnt = 0;
    spool_arg holder = { .s = s, .cnt = &cnt, .hold_ms = 5 };// 队头持锁 5ms 迫使其余排队
    spool_arg rest = { .s = s, .cnt = &cnt, .hold_ms = 0 };
    coro_fn funcs[CONTEND];
    void *args[CONTEND];
    int32_t i, r;
    funcs[0] = _spool_worker;
    args[0] = &holder;
    for (i = 1; i < CONTEND; i++) {
        funcs[i] = _spool_worker;
        args[i] = &rest;
    }
    for (r = 0; r < ROUNDS; r++) {
        if (ERR_OK != coro_fork_wait(coro_task_co(task), funcs, args, CONTEND)) {
            LOG_ERROR("serial pool reuse: round %d fork_wait failed.", r);
            coro_serial_free(s);
            return ERR_FAILED;
        }
    }
    if (ROUNDS * CONTEND != cnt) {
        LOG_ERROR("serial pool reuse: expect %d, got %d.", ROUNDS * CONTEND, cnt);
        coro_serial_free(s);
        return ERR_FAILED;
    }
    coro_serial_free(s);
    return ERR_OK;
}

// ── 测试 9：销毁时另有协程持锁 ──────────────────────────────────────────
// A 持锁 sleep，B/C 排在队列里，D 中途 coro_serial_free：
// B/C 被唤醒后 enter 返回失败（锁不交接给它们），A 不受影响照常跑完临界区，
// 对象由 A 最后那次 leave 释放——free 返回时并没有真正释放
typedef struct sfree_arg {
    coro_serial_ctx *s;
    int32_t *nfail;    // 排队者拿到失败返回的次数
    int32_t *nhold;    // 持锁者跑完临界区的次数
    uint32_t hold_ms;  // 非 0 = 持锁者，进临界区后睡这么久；killer 借它当起手延时
}sfree_arg;

static void _sfree_worker(void *owner, void *arg) {
    task_ctx *task = owner;
    sfree_arg *a = (sfree_arg *)arg;
    if (ERR_OK != coro_serial_enter(a->s)) {
        ++(*a->nfail);
        return;
    }
    if (0 != a->hold_ms) {
        coro_sleep(task, a->hold_ms);
        // 睡醒时 killer 那次 free 已经发生：对象必须还活着（释放推迟到下面这次 leave），
        // 且已标记 closed，故连本协程的嵌套 enter 也该被拒——两者都对才算跑完
        if (ERR_OK != coro_serial_enter(a->s)) {
            ++(*a->nhold);
        }
    }
    coro_serial_leave(a->s);
}

static void _sfree_killer(void *owner, void *arg) {
    task_ctx *task = owner;
    sfree_arg *a = (sfree_arg *)arg;
    coro_sleep(task, a->hold_ms);// 等 A 拿到锁、B/C 排进队列
    coro_serial_free(a->s);
}

static int32_t _test_serial_free_busy(task_ctx *task) {
    enum { HOLD_MS = 30, KILL_MS = 5 };
    coro_serial_ctx *s = coro_serial_new(coro_task_co(task));
    int32_t nfail = 0;
    int32_t nhold = 0;
    sfree_arg holder = { .s = s, .nfail = &nfail, .nhold = &nhold, .hold_ms = HOLD_MS };
    sfree_arg waiter = { .s = s, .nfail = &nfail, .nhold = &nhold, .hold_ms = 0 };
    sfree_arg killer = { .s = s, .nfail = &nfail, .nhold = &nhold, .hold_ms = KILL_MS };
    coro_fn funcs[4] = { _sfree_worker, _sfree_worker, _sfree_worker, _sfree_killer };
    void *args[4] = { &holder, &waiter, &waiter, &killer };
    if (ERR_OK != coro_fork_wait(coro_task_co(task), funcs, args, 4)) {
        LOG_ERROR("serial free busy: fork_wait failed.");
        coro_serial_free(s);// 没有协程跑起来，killer 那次 free 也就没发生
        return ERR_FAILED;
    }
    // s 已由持锁者的 leave 释放，此处不能再 free
    if (2 != nfail || 1 != nhold) {
        LOG_ERROR("serial free busy: nfail=%d nhold=%d, want 2,1.", nfail, nhold);
        return ERR_FAILED;
    }
    return ERR_OK;
}

// ── 测试 10：持锁者在自己临界区内销毁 ───────────────────────────────────
// 结果集回调里调 mysql_quit 就是这个形状：free 只标记不释放，推迟到本次 leave；
// 标记之后连本协程的嵌套 enter 也一并拒绝
static int32_t _test_serial_free_self(task_ctx *task) {
    coro_serial_ctx *s = coro_serial_new(coro_task_co(task));
    if (ERR_OK != coro_serial_enter(s)) {
        LOG_ERROR("serial free self: enter failed.");
        coro_serial_free(s);
        return ERR_FAILED;
    }
    coro_serial_free(s);// 持锁中销毁：只标记，此处不得释放
    int32_t nested = coro_serial_enter(s);
    coro_serial_leave(s);// ref 归 0，对象在这里才真正释放
    if (ERR_OK == nested) {
        LOG_ERROR("serial free self: nested enter should fail after free.");
        return ERR_FAILED;
    }
    return ERR_OK;
}

// ── 测试 11：销毁排在在途命令之后（四个 *_quit 包锁后的形状）───────────────
// A 持锁 sleep 期间 B 走"上锁再销毁再摘指针"那一套：B 必须等 A 的临界区跑完才动手，
// 而不是把 A 拦腰打断；A 出来时执行器仍在（B 的 free 只标记），由 B 的 leave 真正回收。
// 另外两个协程模拟"第二次 quit"的两条无操作路径：
//   C 与 B 同时发起，槽位还没被摘掉，它拿到的是已 closed 的执行器，enter 失败后什么都不做；
//   D 迟到，读到的槽位已是 NULL，直接退。
// 槽位置空排在 free 之后是生产代码的硬要求（见 coro_utils.c mysql_quit 的规则说明）：
// 反过来写的话，free 同步唤醒的排队者会读到 NULL，把命令无锁发出去
typedef struct sq_arg {
    coro_serial_ctx **slot;   // 指向共享的执行器槽位，模拟 xxx->serial 字段
    int32_t *order;           // 记录事件顺序的游标
    int32_t *ev;              // 事件序列
    int32_t *cnoop;           // C 走"摘到 NULL 直接退"的次数
    uint32_t hold_ms;
}sq_arg;

static void _sq_holder(void *owner, void *arg) {
    task_ctx *task = owner;
    sq_arg *a = (sq_arg *)arg;
    // 进来就把指针捏住:销毁方会在我们 sleep 期间把槽位置空,leave 时再去读槽位
    // 拿到的是 NULL —— 正是 coro_serial_free 文档里那条"加解锁须捏同一个指针"的反面
    coro_serial_ctx *held = *a->slot;
    if (ERR_OK != coro_serial_enter(held)) {
        return;
    }
    coro_sleep(task, a->hold_ms);
    a->ev[(*a->order)++] = 1;// 1 = 持锁者跑完临界区
    coro_serial_leave(held);
}
// 与 mysql_quit / smtp_quit 等同一套：摘指针 → 上锁 → 干活 → free → 摘指针 → unlock。
// 真接口在 NULL 那一档是"不排队直接断连"（无执行器的连接本就不串行），这里的复刻件不做断连动作，
// 故 NULL 时只记一次 noop——本用例考的是上锁与 free 的交接，不是断连本身
static void _sq_quit(void *owner, void *arg) {
    task_ctx *task = owner;
    sq_arg *a = (sq_arg *)arg;
    coro_sleep(task, a->hold_ms);// 让持锁者先进临界区
    coro_serial_ctx *held = *a->slot;
    if (NULL == held) {
        ++(*a->cnoop);// 别人已接手销毁并摘掉了槽位，本次什么都不做
        return;
    }
    if (ERR_OK != coro_serial_enter(held)) {
        // 另一次 quit 已经把它关掉：free 见 closed 直接返回，回收归先到的那位
        ++(*a->cnoop);
        coro_serial_free(held);
        *a->slot = NULL;
        return;
    }
    a->ev[(*a->order)++] = 2;// 2 = 销毁方拿到锁开始干活
    coro_serial_free(held);
    *a->slot = NULL;
    coro_serial_leave(held);
}

static int32_t _test_serial_quit_order(task_ctx *task) {
    enum { HOLD_MS = 30, QUIT_MS = 5, LATE_MS = HOLD_MS + 20 };
    coro_serial_ctx *slot = coro_serial_new(coro_task_co(task));
    int32_t order = 0;
    int32_t ev[4] = { 0 };
    int32_t cnoop = 0;
    sq_arg holder = { .slot = &slot, .order = &order, .ev = ev, .cnoop = &cnoop, .hold_ms = HOLD_MS };
    sq_arg quitter = { .slot = &slot, .order = &order, .ev = ev, .cnoop = &cnoop, .hold_ms = QUIT_MS };
    sq_arg late = { .slot = &slot, .order = &order, .ev = ev, .cnoop = &cnoop, .hold_ms = LATE_MS };
    coro_fn funcs[4] = { _sq_holder, _sq_quit, _sq_quit, _sq_quit };
    void *args[4] = { &holder, &quitter, &quitter, &late };
    if (ERR_OK != coro_fork_wait(coro_task_co(task), funcs, args, 4)) {
        LOG_ERROR("serial quit order: fork_wait failed.");
        coro_serial_free(slot);
        return ERR_FAILED;
    }
    // slot 已被销毁方回收，此处不能再 free
    if (2 != order || 1 != ev[0] || 2 != ev[1]) {
        LOG_ERROR("serial quit order: ev=%d,%d (order=%d), want 1,2 -- Destroy raced ahead of the lock holder.",
                  ev[0], ev[1], order);
        return ERR_FAILED;
    }
    // 两条无操作路径各命中一次：并发那位 enter 失败，迟到那位读到 NULL
    if (2 != cnoop) {
        LOG_ERROR("serial quit order: redundant quit noop=%d, want 2.", cnoop);
        return ERR_FAILED;
    }
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
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_serial_pool_reuse(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_serial_free_busy(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_serial_free_self(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_serial_quit_order(task)) {
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
