#ifndef WAITER_H_
#define WAITER_H_

#include "thread/cond.h"
#include "thread/futex.h"

#define SPINNER_CHECK (CPU_PAUSE_CYCLES >= 64 ? 1 : 64) // 空转每这么多圈读一次时钟,须是 2 的幂;CPU_PAUSE 贵的平台每圈都读,否则读到时早过了下限
#define SPINNER_WAKE_DEAR 20000 // 唤醒代价最小值(纳秒)到这么多才算唤醒极贵，才启用试转
#define SPINNER_TRIAL_MIN 256 // 两次试转之间至少隔这么多次只转下限的空转，试转落空就翻倍
#define SPINNER_TRIAL_MAX 4096 // 试转间隔的上限
#define SPINNER_TRIAL_LEN 16 // 一次试转连续按上限转这么多次，转中至少一半就切进转档

// 消费线程没活时怎么等，两块各自可单用：
// 1. waiter_ctx：睡眠握手。消费方 waiter_prepare 亮出睡眠态 → 自己复查队列(和退出标志)
//    → 有活 waiter_cancel、没活 waiter_commit(要睡到截止时刻、不被虚假唤醒提前叫回的用 waiter_commit_until)；
//    生产方先入队，再 waiter_state_get 读睡眠态，按自己的规则决定叫不叫，叫就 waiter_wake。两边各是"先写自己的、
//    再读对方的"，至少一方看得见对方，丢不掉唤醒；两步的顺序都不能颠倒。生产方入队那一步须是足序写或足序读改写，
//    或与消费方复查拿同一把锁；只有 release / relaxed 写时，读睡眠态之前先 ATOMIC_THREAD_FENCE_SEQCST()，
//    理由见 macro_atomic.h。不读睡眠态、无条件 wake 的不受此限。
//    只想让一个生产者去叫时，先 waiter_mark_waking 抢到再 wake。
//    叫醒方可能是不被 join 的线程时改用 waiter_wake_held。
//    两种实现，握手规则相同：WITH_FUTEX 为 1 时按地址等待(每次叫醒 epoch 加一，消费方等它变)，不用锁；否则 mutex + cond。
// 2. spinner_ctx：睡前空转。已知要等多久时，短于唤醒代价才值得转：间隔估计不超过唤醒代价就转
//    2 倍间隔(夹在上下限内)，否则只转下限。按时长不按圈数：CPU_PAUSE 在各架构上耗时差上百倍。
//    消费线程主循环：拿到活先 spinner_busy；没活就 spinner_spin，转到活回去取，转满没等到就睡，
//    睡醒把 waiter_commit 的返回值交给 spinner_wake。
//    唤醒极贵时(唤醒代价最小值 >= SPINNER_WAKE_DEAR，常见于虚拟机)，按估计会在闭环负载下卡在不转那一档，
//    所以估计说不转时隔一段连续按上限试转 SPINNER_TRIAL_LEN 次，转中至少一半就切进转档(每次按上限转)，
//    转档里转中率掉到 1/4 以下退回；试转落空就把间隔翻倍。
typedef enum waiter_state {
    WAITER_AWAKE = 0,   // 醒着
    WAITER_SLEEP,       // 在睡，有活就该叫
    WAITER_LINGER,      // 浅睡，哪些活值得叫由使用方定
    WAITER_WAKING       // 在睡，已有生产者抢到去叫，后来的不必再叫；挑"在睡的"时仍算在睡
}waiter_state;
typedef enum spinner_mode {
    SPINNER_EST = 0,    // 按估计转
    SPINNER_TRIAL,      // 试转中：每次按上限转，数转中了几次
    SPINNER_HOT         // 转档：每次按上限转，转中率低了退回
}spinner_mode;
typedef struct waiter_ctx {
    atomic_t state;     // waiter_state
#if WITH_FUTEX
    atomic_t epoch;     // 等待字：每次叫醒加一，消费方等它变
    uint32_t seen;      // 消费方 waiter_prepare 时读到的 epoch，只有消费方读写
    atomic64_t sig_at;  // 首个还没被领走的信号的发出时刻，0 为无
#else
    int32_t woken;      // waiter_prepare 之后有没有人叫过，waiter_commit_until 判叫醒用；受 mutex 保护
    uint64_t sig_at;    // 首个还没被领走的信号的发出时刻，0 为无；受 mutex 保护
    mutex_ctx mutex;
    cond_ctx cond;
#endif
}waiter_ctx;
typedef struct spinner_ctx {
    int32_t mode;       // spinner_mode
    int32_t hit_ratio;  // 转档里的转中率，满刻度 256，每次挪 1/8
    uint32_t trial_wait; // 再有这么多次只转下限的空转就开始下一次试转
    uint32_t trial_every; // 当前的试转间隔
    uint32_t trial_left; // 本次试转还剩几次
    uint32_t trial_hits; // 本次试转转中了几次
    int64_t gap_ns;     // 空闲间隔估计：进入空闲到活真正到来
    int64_t wake_ns;    // 唤醒代价估计：信号发出到醒来
    int64_t wake_min_ns; // 唤醒代价的衰减最小值，跟的是环境本身的唤醒成本，零星的慢样本抬不动它；0 表示还没样本
    int64_t lo_ns;      // 空转时长下限
    int64_t hi_ns;      // 空转时长上限；唤醒代价估计夹在它以内，间隔估计与唤醒代价最小值不超过它的 2 倍
    uint64_t idle_since; // 进入空闲的时刻，0 表示不在空闲
    uint64_t sig_at;    // 最近一次睡醒带回的信号发出时刻，0 表示最近一次不是被信号叫醒
    timer_ctx *timer;   // 取时刻(纳秒)用，只读，可与别处共用
}spinner_ctx;
typedef int32_t(*spinner_ready)(void *arg);// spinner_spin 每圈问一次有没有活，非 0 表示有

/// <summary>
/// 初始化 waiter_ctx
/// </summary>
/// <param name="w">waiter_ctx</param>
static inline void waiter_init(waiter_ctx *w) {
    ATOMIC_SET_RELAXED(&w->state, WAITER_AWAKE);
#if WITH_FUTEX
    ATOMIC_SET_RELAXED(&w->epoch, 0);
    w->seen = 0;
    ATOMIC64_SET_RELAXED(&w->sig_at, 0);
#else
    w->woken = 0;
    w->sig_at = 0;
    mutex_init(&w->mutex);
    cond_init(&w->cond);
#endif
}
/// <summary>
/// 释放 waiter_ctx，调用时不能再有人在等或在叫
/// </summary>
/// <param name="w">waiter_ctx</param>
static inline void waiter_free(waiter_ctx *w) {
#if WITH_FUTEX
    (void)w;
#else
    cond_free(&w->cond);
    mutex_free(&w->mutex);
#endif
}
/// <summary>
/// 消费方准备睡：亮出睡眠态、全屏障，并作废此前记下的信号时刻(那时没人在等)；mutex + cond 实现还会加锁。
/// 之后调用方复查自己的队列与退出标志，再调 waiter_cancel、waiter_commit、waiter_commit_until 三选一(锁由它们释放)
/// </summary>
/// <param name="w">waiter_ctx</param>
/// <param name="st">WAITER_SLEEP 或 WAITER_LINGER</param>
static inline void waiter_prepare(waiter_ctx *w, waiter_state st) {
#if WITH_FUTEX
    ATOMIC64_SET_RELAXED(&w->sig_at, 0);
    w->seen = (uint32_t)ATOMIC_GET(&w->epoch);// 要在复查之前读：此后来的叫醒都会让 epoch 与它不等，commit 就不会睡下去
#else
    mutex_lock(&w->mutex);
    w->woken = 0;
    w->sig_at = 0;
#endif
    ATOMIC_SET_SEQCST(&w->state, st);
    // 复查队列只是 acquire 读，弱序平台上会跑到置态之前：这边看不到新元素、生产者又看不到睡眠态，就丢了唤醒
    ATOMIC_THREAD_FENCE_SEQCST();
}
/// <summary>
/// 复查到有活(或要退出)，不睡：清睡眠态(并解锁)
/// </summary>
/// <param name="w">waiter_ctx</param>
static inline void waiter_cancel(waiter_ctx *w) {
    ATOMIC_SET_RELAXED(&w->state, WAITER_AWAKE);
#if !WITH_FUTEX
    mutex_unlock(&w->mutex);
#endif
}
/// <summary>
/// 复查后确实没活，睡：等到被叫醒或超时，醒来清睡眠态(并解锁)。可能虚假唤醒，调用方醒来照常回去取活
/// </summary>
/// <param name="w">waiter_ctx</param>
/// <param name="ms">最多等多少毫秒，0 表示一直等到被叫醒；按剩余时长算出来的值可能为 0 时，调用方要先挡掉</param>
/// <returns>叫醒这次睡眠的首个信号的发出时刻(waiter_wake 记下的)，超时与信号同时到也会带回；0 表示没收到带时刻的信号(超时、虚假唤醒，或叫醒方没给 timer)</returns>
static inline uint64_t waiter_commit(waiter_ctx *w, uint32_t ms) {
#if WITH_FUTEX
    if ((uint32_t)ATOMIC_GET(&w->epoch) == w->seen) {
        (void)futex_wait(&w->epoch, w->seen, ms);
    }
    ATOMIC_SET_RELAXED(&w->state, WAITER_AWAKE);
    return (uint64_t)ATOMIC64_SET(&w->sig_at, 0);
#else
    uint64_t sig_at;
    if (0 == ms) {
        cond_wait(&w->cond, &w->mutex);
    } else {
        (void)cond_timedwait(&w->cond, &w->mutex, ms);
    }
    ATOMIC_SET_RELAXED(&w->state, WAITER_AWAKE);
    sig_at = w->sig_at;
    w->sig_at = 0;
    mutex_unlock(&w->mutex);
    return sig_at;
#endif
}
// 到截止还剩多少毫秒：调用方保证 now < deadline，所以至少 1(传 0 是一直等)；封顶 UINT32_MAX - 1，Windows 上 UINT32_MAX 即一直等
static inline uint32_t _waiter_left_ms(uint64_t deadline, uint64_t now) {
    return deadline - now >= UINT32_MAX ? UINT32_MAX - 1 : (uint32_t)(deadline - now);
}
/// <summary>
/// 同 waiter_commit，但睡到截止时刻：虚假唤醒(含被信号打断)不算数，接着睡，直到被叫醒或到点；截止已过就不睡。
/// 不带回信号时刻，要量唤醒代价(接 spinner_wake)的用 waiter_commit
/// </summary>
/// <param name="w">waiter_ctx</param>
/// <param name="timer">取当前时刻用</param>
/// <param name="deadline">截止时刻，取 timer_cur_ms(timer) 的毫秒</param>
/// <returns>ERR_OK 被叫醒过(与到点同时也算)；1 到点。mutex + cond 实现等待出错时按 ERR_OK 返回，调用方照常复查</returns>
static inline int32_t waiter_commit_until(waiter_ctx *w, timer_ctx *timer, uint64_t deadline) {
    uint64_t now;
    int32_t rtn = ERR_OK;
#if WITH_FUTEX
    while ((uint32_t)ATOMIC_GET(&w->epoch) == w->seen) {
        now = timer_cur_ms(timer);
        if (now >= deadline) {
            rtn = 1;
            break;
        }
        (void)futex_wait(&w->epoch, w->seen, _waiter_left_ms(deadline, now));
    }
    ATOMIC_SET_RELAXED(&w->state, WAITER_AWAKE);
#else
    // 叫醒看标志不看 cond_timedwait 的返回值：叫醒方解锁后才 signal，报超时时标志可能已经置上
    while (0 == w->woken) {
        now = timer_cur_ms(timer);
        if (now >= deadline) {
            rtn = 1;
            break;
        }
        if (ERR_FAILED == cond_timedwait(&w->cond, &w->mutex, _waiter_left_ms(deadline, now))) {
            break;
        }
    }
    ATOMIC_SET_RELAXED(&w->state, WAITER_AWAKE);
    mutex_unlock(&w->mutex);
#endif
    return rtn;
}
/// <summary>
/// 生产方读睡眠态。必须在入队之后读，且是足序读，与 waiter_prepare 的屏障配对
/// </summary>
/// <param name="w">waiter_ctx</param>
/// <returns>WAITER_AWAKE 醒着；否则为对方 waiter_prepare 给的睡眠态，或已被抢成 WAITER_WAKING</returns>
static inline waiter_state waiter_state_get(waiter_ctx *w) {
    return (waiter_state)ATOMIC_GET_SEQCST(&w->state);
}
/// <summary>
/// 同 waiter_state_get，但不带屏障，读到的可能稍旧。只给"挑一个在睡的"这类不要求准确的判断用，不能据此决定叫不叫
/// </summary>
/// <param name="w">waiter_ctx</param>
/// <returns>同 waiter_state_get</returns>
static inline waiter_state waiter_state_hint(waiter_ctx *w) {
    return (waiter_state)ATOMIC_GET_RELAXED(&w->state);
}
/// <summary>
/// 把睡眠态 st 抢成 WAITER_WAKING：多个生产者同时看到对方在睡时，只让抢到的那个去 waiter_wake
/// </summary>
/// <param name="w">waiter_ctx</param>
/// <param name="st">waiter_state_get 读到的 WAITER_SLEEP 或 WAITER_LINGER；传 WAITER_WAKING 会抢成功，调用方须先排除</param>
/// <returns>非 0 表示抢到</returns>
static inline int32_t waiter_mark_waking(waiter_ctx *w, waiter_state st) {
    return ATOMIC_CAS(&w->state, (atomic_t)st, (atomic_t)WAITER_WAKING);
}
/// <summary>
/// 叫醒消费方。按地址等待：epoch 加一再按地址叫醒，不用锁。mutex + cond：加解锁一次是为跨过对方"复查→进入等待"那段，
/// signal 放在解锁之后，持锁 signal 会让对方醒来撞上这把锁再睡一次。
/// 叫醒方必须在 waiter_free 之前结束(被 join)，否则用 waiter_wake_held
/// </summary>
/// <param name="w">waiter_ctx</param>
/// <param name="timer">非 NULL 时取当前时刻记为信号发出时刻(只留首个)，由对方 waiter_commit 带回，量唤醒代价用</param>
static inline void waiter_wake(waiter_ctx *w, timer_ctx *timer) {
#if WITH_FUTEX
    // 记时刻要在 epoch 加一之前，对方见 epoch 变就会醒来取走它；两步之间被耽搁时可能把旧时刻带进对方下一觉，只多一个偏大的样本
    if (NULL != timer) {
        (void)ATOMIC64_CAS(&w->sig_at, 0, (atomic64_t)timer_cur(timer));
    }
    ATOMIC_ADD(&w->epoch, 1);
    futex_wake(&w->epoch);
#else
    mutex_lock(&w->mutex);
    w->woken = 1;
    if (NULL != timer
        && 0 == w->sig_at) {
        w->sig_at = timer_cur(timer);// 须在锁内取：锁外取时叫醒方拿锁前被耽搁，会把旧时刻写进对方的下一觉
    }
    mutex_unlock(&w->mutex);
    cond_signal(&w->cond);
#endif
}
/// <summary>
/// 同 waiter_wake(w, NULL)，叫醒方可能比 waiter 活得久(不被 join 的线程)时用它。mutex + cond 持锁 signal：
/// 解锁后再 signal，对方可能已经醒来并释放了 waiter，代价是对方醒来先撞上这把锁。
/// 按地址等待同 waiter_wake：epoch 加一时 waiter 还得活着，之后那次按地址叫醒对方即使已释放也不出错
/// </summary>
/// <param name="w">waiter_ctx</param>
static inline void waiter_wake_held(waiter_ctx *w) {
#if WITH_FUTEX
    waiter_wake(w, NULL);
#else
    mutex_lock(&w->mutex);
    w->woken = 1;
    cond_signal(&w->cond);
    mutex_unlock(&w->mutex);
#endif
}
/// <summary>
/// 同 waiter_wake_held，给崩溃路径用，宁可不唤醒也不卡住：mutex + cond 只 trylock、拿不到锁就不叫；
/// 按地址等待不会卡，照常叫
/// </summary>
/// <param name="w">waiter_ctx</param>
static inline void waiter_trywake(waiter_ctx *w) {
#if WITH_FUTEX
    waiter_wake(w, NULL);
#else
    if (ERR_OK == mutex_trylock(&w->mutex)) {
        w->woken = 1;
        cond_signal(&w->cond);
        mutex_unlock(&w->mutex);
    }
#endif
}

// 按本次间隔 gap 算新的间隔估计：每次挪 1/4。gap 先截到 2 倍上限，免得一次长空闲后要几十次才缓回来
static inline int64_t _spinner_gap(const spinner_ctx *sp, uint64_t gap) {
    const int64_t cap = 2 * sp->hi_ns;
    int64_t g = gap > (uint64_t)cap ? cap : (int64_t)gap;
    return sp->gap_ns + (g - sp->gap_ns) / 4;
}
// 空转预算(纳秒)：间隔估计不超过唤醒代价时转 2 倍间隔(夹在上下限内)，否则只转下限
static inline int64_t _spinner_budget(const spinner_ctx *sp) {
    int64_t budget = 2 * sp->gap_ns;
    if (sp->gap_ns > sp->wake_ns || budget < sp->lo_ns) {
        return sp->lo_ns;
    }
    return budget > sp->hi_ns ? sp->hi_ns : budget;
}
// 按本次唤醒延迟 lat 算新的唤醒代价估计：每次挪 1/8，夹在上下限内。
// 上限也要夹：间隔超过上限时转满也接不住，不能因为唤醒更贵就去转
static inline int64_t _spinner_wake(const spinner_ctx *sp, uint64_t lat) {
    int64_t wake_ns = sp->wake_ns + ((int64_t)lat - sp->wake_ns) / 8;
    if (wake_ns < sp->lo_ns) {
        return sp->lo_ns;
    }
    return wake_ns > sp->hi_ns ? sp->hi_ns : wake_ns;
}
// 按本次唤醒延迟 lat 算新的唤醒代价最小值：比它小直接取，比它大每次只往上挪 1/64，样本先截到 2 倍上限。
// 样本同 _spinner_wake 按有符号算，时钟回退出的负值当作最小
static inline int64_t _spinner_wake_min(const spinner_ctx *sp, uint64_t lat) {
    const int64_t cap = 2 * sp->hi_ns;
    int64_t l = (int64_t)lat > cap ? cap : (int64_t)lat;
    if (0 == sp->wake_min_ns || l < sp->wake_min_ns) {
        return l > 0 ? l : 1;
    }
    return sp->wake_min_ns + (l - sp->wake_min_ns) / 64;
}
// 这次是否按上限转：只在唤醒极贵时才可能。试转中或转档里都转；按估计只转下限时数次数，数满就开始一次试转。
// 门控关上时只是不按上限转、档位不动，重开后接着原档：门控常在门槛附近来回翻，翻一次就复位会回不到转档
static inline int32_t _spinner_hot(spinner_ctx *sp, int64_t budget) {
    if (sp->wake_min_ns < SPINNER_WAKE_DEAR) {
        return 0;
    }
    if (SPINNER_EST != sp->mode) {
        return 1;
    }
    if (budget > sp->lo_ns) {
        return 0;
    }
    if (sp->trial_wait > 0) {
        sp->trial_wait--;
    }
    if (0 != sp->trial_wait) {
        return 0;
    }
    sp->mode = SPINNER_TRIAL;
    sp->trial_left = SPINNER_TRIAL_LEN;
    sp->trial_hits = 0;
    return 1;
}
// 按上限转完一次后记账，只在 _spinner_hot 返回非 0 之后调(此时档位不是 SPINNER_EST)：试转满 SPINNER_TRIAL_LEN 次时
// 转中至少一半就切进转档，否则回去按估计转、试转间隔翻倍；转档里转中率掉到 1/4 以下就退回
static inline void _spinner_settle(spinner_ctx *sp, int32_t got) {
    if (SPINNER_TRIAL == sp->mode) {
        sp->trial_hits += (uint32_t)(0 != got);
        if (0 != --sp->trial_left) {
            return;
        }
        if (2 * sp->trial_hits >= SPINNER_TRIAL_LEN) {
            sp->mode = SPINNER_HOT;
            sp->hit_ratio = 256;
            sp->trial_every = SPINNER_TRIAL_MIN;
            return;
        }
        if (sp->trial_every < SPINNER_TRIAL_MAX) {
            sp->trial_every *= 2;
        }
    } else {
        sp->hit_ratio += ((0 != got ? 256 : 0) - sp->hit_ratio) / 8;
        if (sp->hit_ratio >= 256 / 4) {
            return;
        }
    }
    sp->mode = SPINNER_EST;
    sp->trial_wait = sp->trial_every;
}
/// <summary>
/// 初始化空转估计：间隔估计从 0 起，唤醒代价从下限起，唤醒代价最小值等第一个样本
/// </summary>
/// <param name="sp">spinner_ctx</param>
/// <param name="timer">取时刻用的 timer_ctx，须已 timer_init，生命期长于 sp</param>
/// <param name="lo_us">空转时长下限(微秒)：间隔长于唤醒代价时只转这么久，唤醒极贵时的试转与转档除外(见文件头)</param>
/// <param name="hi_us">空转时长上限(微秒)，须大于等于 lo_us；2 倍上限不到 SPINNER_WAKE_DEAR 时唤醒代价最小值够不着门槛，不会试转</param>
static inline void spinner_init(spinner_ctx *sp, timer_ctx *timer, uint32_t lo_us, uint32_t hi_us) {
    ZERO(sp, sizeof(spinner_ctx));
    sp->lo_ns = (int64_t)lo_us * 1000;
    sp->hi_ns = (int64_t)hi_us * 1000;
    sp->wake_ns = sp->lo_ns;
    sp->timer = timer;
    sp->trial_wait = SPINNER_TRIAL_MIN;
    sp->trial_every = SPINNER_TRIAL_MIN;
}
/// <summary>
/// 拿到活时调：刚结束一段空闲，就按"进入空闲到活真正到来"更新间隔估计。到来时刻优先用睡醒带回的信号时刻，
/// 没睡过或没测到才用现在——不能用睡醒时刻，那里含唤醒延迟，一旦开始睡就永远学不回"转"
/// </summary>
/// <param name="sp">spinner_ctx</param>
static inline void spinner_busy(spinner_ctx *sp) {
    uint64_t at;
    if (0 == sp->idle_since) {
        return;
    }
    at = sp->sig_at >= sp->idle_since ? sp->sig_at : timer_cur(sp->timer);
    sp->gap_ns = _spinner_gap(sp, at - sp->idle_since);
    sp->idle_since = 0;
}
// 进入空转：记下进入空闲的时刻(已在空闲就不动)，作废上一觉带回的信号时刻(醒来那份活被别人拿走时，
// 它不属于这一轮)，返回空转的截止时刻(timer_cur 的纳秒)；*hot 置这次是否按上限转
static inline uint64_t _spinner_until(spinner_ctx *sp, int32_t *hot) {
    uint64_t now = timer_cur(sp->timer);
    int64_t budget;
    if (0 == sp->idle_since) {
        sp->idle_since = now;
    }
    sp->sig_at = 0;
    budget = _spinner_budget(sp);
    *hot = _spinner_hot(sp, budget);
    if (0 != *hot) {
        budget = sp->hi_ns;
    }
    return now + (uint64_t)budget;
}
/// <summary>
/// 没活时调：按估计空转到截止时刻(唤醒极贵时可能改按上限转，见文件头)，每圈问一次 ready。
/// ready 只看本线程自己的队列、用不拿锁的近似判空，免得和投递方撞锁
/// </summary>
/// <param name="sp">spinner_ctx</param>
/// <param name="spinning">可为 NULL。非 NULL 时空转期间置 1、返回前清 0，给生产者挑投递对象时优先选它；叫不叫仍只看 waiter_state_get</param>
/// <param name="ready">判活回调</param>
/// <param name="arg">传给 ready</param>
/// <returns>非 0 表示转到了活，调用方回去取；0 表示转满没等到，调用方去睡</returns>
static inline int32_t spinner_spin(spinner_ctx *sp, atomic_t *spinning, spinner_ready ready, void *arg) {
    int32_t hot;
    uint64_t deadline = _spinner_until(sp, &hot);
    uint32_t n = 0;
    int32_t got = 0;
    if (NULL != spinning) {
        ATOMIC_SET_RELAXED(spinning, 1);
    }
    for (;;) {
        if (0 != ready(arg)) {
            got = 1;
            break;
        }
        CPU_PAUSE();
        if (0 == (++n & (SPINNER_CHECK - 1))
            && timer_cur(sp->timer) >= deadline) {
            break;
        }
    }
    if (NULL != spinning) {
        ATOMIC_SET_RELAXED(spinning, 0);
    }
    if (0 != hot) {
        _spinner_settle(sp, got);
    }
    return got;
}
/// <summary>
/// 睡醒后调：记下带回的信号时刻，测到了就按"信号发出到醒来"更新唤醒代价与它的最小值
/// </summary>
/// <param name="sp">spinner_ctx</param>
/// <param name="sig_at">waiter_commit 的返回值</param>
static inline void spinner_wake(spinner_ctx *sp, uint64_t sig_at) {
    uint64_t lat;
    sp->sig_at = sig_at;
    if (0 != sig_at) {
        lat = timer_cur(sp->timer) - sig_at;
        sp->wake_ns = _spinner_wake(sp, lat);
        sp->wake_min_ns = _spinner_wake_min(sp, lat);
    }
}

#endif//WAITER_H_
