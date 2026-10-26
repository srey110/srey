#include "srey/db_pool.h"
#include "srey/coro_task.h"
#include "utils/idgen.h"

// _checkout 除 ERR_OK / ERR_FAILED 外的返回：池可能已释放（已关闭，或叫醒过等待者），这条连接与池都不能再碰
#define _DB_POOL_GONE 1
#define _DB_POOL_TICK_MS 1000 // 维护协程的巡检周期；协程超时本就是秒级粒度
#define _DB_POOL_BACKOFF_SHIFT 5 // 连续失败的冷却最多翻到 backoff_ms 的 32 倍

typedef struct db_pool_slot {
    int32_t  busy;        // 1=已借出（含正在交接、池内部正在建连/断开）
    int32_t  in_idle;     // 1=在空闲栈里
    int32_t  fails;       // 连续建连/探活失败次数，冷却按它翻倍；连上清零
    int32_t  warned;      // 本次借出已打过借出过久告警
    uint32_t life_ms;     // 本连接的寿命：max_lifetime_ms 随机减去最多 1/8，同批连接不同时到期
    uint64_t idle_at;     // 进入空闲栈的时刻；0=不在空闲栈或已断开
    uint64_t cool_until;  // 冷却截止；此前不再交给调用方
    uint64_t born_at;     // 上次连上的时刻，max_lifetime 用；0=没连着
    uint64_t out_at;      // 交给调用方的时刻，借出过久告警用；0=不在调用方手里
    void     *conn;       // 连接对象，池只借指针，不负责释放
}db_pool_slot;
struct db_pool_ctx {
    int32_t nslot;             // 已 add 的连接对象个数（<= cfg.cap）
    int32_t nidle;             // 空闲栈里的槽数（含冷却中的、已断开的）
    int32_t busy;              // 已借出的连接数（含正在交接的）
    int32_t waiters;           // 当前挂在 coro_wait 上的协程数
    int32_t closed;            // 1=已调 db_pool_free
    int32_t free_pending;      // free 已走完；还有借出、等待者或维护协程时由最后一个离开的释放
    int32_t ticking;           // 1=维护协程已起（它也算池里的一个在用者）
    db_pool_cfg cfg;           // 建池时拷贝的配置
    db_pool_stat stat;         // 累计计数，db_pool_stat_get 拷出
    uint64_t sess;             // 本池等待者的会话 id，createid 分配
    uint64_t tick_sess;        // 维护协程定时等待用的会话 id，与等待者分开（取等待者只看队头）
    task_ctx *task;            // 所属协程 task，也是连接操作表里传下去的那个
    coro_ctx *co;              // task 的协程调度器，coro_wait / coro_wake 用
    const db_pool_ops *ops;    // 连接操作表，生命周期须覆盖整个池
    db_pool_slot *slots;       // cap 个槽，下标即连接序号
    int32_t *idle;             // 空闲槽下标的栈，nidle 是栈顶
    timer_ctx timer;           // 本池的单调计时：空闲/冷却/寿命/超时都用它
};

static inline int32_t _slot_find(db_pool_ctx *pool, void *conn) {
    for (int32_t i = 0; i < pool->nslot; i++) {
        if (pool->slots[i].conn == conn) {
            return i;
        }
    }
    return -1;
}
static inline void _idle_push(db_pool_ctx *pool, int32_t idx) {
    pool->idle[pool->nidle++] = idx;
    pool->slots[idx].in_idle = 1;
}
static inline void _idle_remove_at(db_pool_ctx *pool, int32_t pos) {
    pool->slots[pool->idle[pos]].in_idle = 0;
    pool->nidle--;
    pool->idle[pos] = pool->idle[pool->nidle];
}
// 从空闲栈里取一条不在冷却中的，连着的优先（断开的要重连）；全在冷却返回 -1（此时 nidle 仍大于 0）。
// now 由调用方取，与随后的 _cool_wait 用同一个时刻，免得两次之间正好有冷却到点、两边都不认它
static inline int32_t _take_usable(db_pool_ctx *pool, uint64_t now) {
    int32_t pick = -1;
    int32_t idx;
    for (int32_t i = pool->nidle - 1; i >= 0; i--) {
        idx = pool->idle[i];
        if (pool->slots[idx].cool_until > now) {
            continue;
        }
        if (0 != pool->ops->ready(pool->slots[idx].conn)) {
            pick = i;
            break;
        }
        if (pick < 0) {
            pick = i;
        }
    }
    if (pick < 0) {
        return -1;
    }
    idx = pool->idle[pick];
    _idle_remove_at(pool, pick);
    return idx;
}
// 从空闲栈里取一条断开着且不在冷却中的；没有返回 -1
static inline int32_t _take_cold(db_pool_ctx *pool) {
    uint64_t now = timer_cur_ms(&pool->timer);
    int32_t idx;
    for (int32_t i = pool->nidle - 1; i >= 0; i--) {
        idx = pool->idle[i];
        if (pool->slots[idx].cool_until <= now
            && 0 == pool->ops->ready(pool->slots[idx].conn)) {
            _idle_remove_at(pool, i);
            return idx;
        }
    }
    return -1;
}
// 空闲栈里连着的连接数
static inline int32_t _ready_idle(db_pool_ctx *pool) {
    int32_t n = 0;
    for (int32_t i = 0; i < pool->nidle; i++) {
        if (0 != pool->ops->ready(pool->slots[pool->idle[i]].conn)) {
            n++;
        }
    }
    return n;
}
// 离最早一条冷却结束还有多少毫秒；没有冷却中的返回 0
static inline uint32_t _cool_wait(db_pool_ctx *pool, uint64_t now) {
    uint64_t min = 0;
    uint64_t cool;
    for (int32_t i = 0; i < pool->nidle; i++) {
        cool = pool->slots[pool->idle[i]].cool_until;
        if (cool > now
            && (0 == min || cool < min)) {
            min = cool;
        }
    }
    if (0 == min) {
        return 0;
    }
    return (min - now > UINT32_MAX) ? UINT32_MAX : (uint32_t)(min - now);
}
// 连上了：失败计数清零，起算寿命
static inline void _born(db_pool_ctx *pool, db_pool_slot *slot, uint64_t now) {
    slot->born_at = now;
    slot->fails = 0;
    uint32_t life = pool->cfg.max_lifetime_ms;
    slot->life_ms = life - (uint32_t)randrange(0, (int32_t)(life / 8));
}
static inline int32_t _expired(db_pool_ctx *pool, db_pool_slot *slot, uint64_t now) {
    return pool->cfg.max_lifetime_ms > 0
        && slot->born_at > 0
        && now - slot->born_at >= slot->life_ms;
}
// 建连/探活失败后的冷却截止：连续失败按 backoff_ms 翻倍，最多翻 _DB_POOL_BACKOFF_SHIFT 次
static inline uint64_t _cool_after_fail(db_pool_ctx *pool, db_pool_slot *slot) {
    int32_t shift = slot->fails < _DB_POOL_BACKOFF_SHIFT ? slot->fails : _DB_POOL_BACKOFF_SHIFT;
    slot->fails++;
    return timer_cur_ms(&pool->timer) + ((uint64_t)pool->cfg.backoff_ms << shift);
}
// 交给调用方：借出过久告警从这里起算
static inline void _hand_out(db_pool_ctx *pool, int32_t idx) {
    pool->slots[idx].out_at = timer_cur_ms(&pool->timer);
    pool->slots[idx].warned = 0;
}
static void _destroy(db_pool_ctx *pool) {
    FREE(pool->slots);
    FREE(pool->idle);
    FREE(pool);
}
// 归还到空闲栈：busy 清零，cool_until 为 0 时记空闲时刻，非 0 时视为冷却中（idle_at 置 0）
static inline void _put_idle(db_pool_ctx *pool, int32_t idx, uint64_t cool_until) {
    db_pool_slot *slot = &pool->slots[idx];
    slot->busy = 0;
    pool->busy--;
    slot->cool_until = cool_until;
    slot->idle_at = (0 == cool_until) ? timer_cur_ms(&pool->timer) : 0;
    _idle_push(pool, idx);
}
// free 之后最后一个离开池的（借出的 put、挂着的 get、维护协程）负责释放
static inline void _release_if_last(db_pool_ctx *pool) {
    if (0 != pool->free_pending
        && 0 == pool->busy
        && 0 == pool->waiters
        && 0 == pool->ticking) {
        _destroy(pool);
    }
}
// 关闭期间正在建连/探活的连接：断开（doquit 为 0 表示调用方已断过）、清 busy，必要时释放池
static inline int32_t _fail_closed(db_pool_ctx *pool, int32_t idx, int32_t doquit) {
    db_pool_slot *slot = &pool->slots[idx];
    // busy 先不减：quit 可能挂起，靠它把池留到这一段结束
    if (0 != doquit) {
        pool->ops->quit(slot->conn);
    }
    slot->busy = 0;
    pool->busy--;
    _release_if_last(pool);
    return _DB_POOL_GONE;
}
// 空闲收缩：把超出 min_idle 且空闲够久的连接断开，槽留在池里等下次借出重连
static void _reap(db_pool_ctx *pool) {
    if (0 == pool->cfg.idle_timeout_ms) {
        return;
    }
    // 额度只算连着的空闲连接，已断开的槽不占 min_idle；quit 会挂起，额度与 now 每轮现算
    uint64_t now;
    int32_t j;
    db_pool_slot *slot;
    for (int32_t i = 0; i < pool->nslot && _ready_idle(pool) > pool->cfg.min_idle; i++) {
        slot = &pool->slots[i];
        now = timer_cur_ms(&pool->timer);
        if (0 == slot->in_idle
            || 0 != slot->busy
            || 0 == slot->idle_at
            || now - slot->idle_at < pool->cfg.idle_timeout_ms
            || 0 == pool->ops->ready(slot->conn)) {
            continue;
        }
        // 先从空闲栈摘掉并标 busy：quit 可能挂起，别的协程不能同时借到它
        for (j = 0; j < pool->nidle; j++) {
            if (pool->idle[j] == i) {
                _idle_remove_at(pool, j);
                break;
            }
        }
        slot->busy = 1;
        pool->busy++;
        pool->ops->quit(slot->conn);
        if (0 != pool->closed) {
            _fail_closed(pool, i, 0);
            return;
        }
        slot->born_at = 0;
        _put_idle(pool, i, 0);
        slot->idle_at = 0;// 已断开，下次借出重连
    }
}
// 取出失败：冷却放回；已没有别的借出时叫醒等待者，让它们也快速失败。
// 醒来的可能当场 free 掉池，叫醒过就返回 GONE，调用方别再碰池
static inline int32_t _checkout_fail(db_pool_ctx *pool, int32_t idx) {
    _put_idle(pool, idx, _cool_after_fail(pool, &pool->slots[idx]));
    if (pool->waiters > 0
        && 0 == pool->busy) {
        coro_wake_all(pool->co, pool->sess, NULL, NULL);
        return _DB_POOL_GONE;
    }
    return ERR_FAILED;
}
// 取出的空闲连接：必要时建连、空闲太久则探活。返回 _DB_POOL_GONE 时连接与池都不能再碰（见宏定义）
static inline int32_t _checkout(db_pool_ctx *pool, int32_t idx, void **out) {
    db_pool_slot *slot = &pool->slots[idx];
    slot->busy = 1;
    pool->busy++;
    void *conn = slot->conn;
    uint64_t now = timer_cur_ms(&pool->timer);
    int32_t reconnect = 0;
    int32_t ready = pool->ops->ready(conn);
    if (0 != ready
        && 0 == slot->born_at) {
        _born(pool, slot, now);// 加进来时就已连着的，首次取出时从这里起算寿命
    }
    if (0 != ready
        && _expired(pool, slot, now)) {
        pool->ops->quit(conn);
        slot->born_at = 0;
        slot->idle_at = 0;
        reconnect = 1;
        if (0 != pool->closed) {
            return _fail_closed(pool, idx, 0);
        }
    }
    if (0 != reconnect || 0 == ready) {
        if (ERR_OK != pool->ops->connect(pool->task, conn)) {
            pool->stat.connfails++;
            if (0 != pool->closed) {
                return _fail_closed(pool, idx, 1);
            }
            return _checkout_fail(pool, idx);
        }
        pool->stat.connects++;
        _born(pool, slot, timer_cur_ms(&pool->timer));
        if (0 != pool->closed) {
            return _fail_closed(pool, idx, 1);
        }
    } else if (pool->cfg.ping_idle_ms > 0
        && NULL != pool->ops->ping
        && 0 != slot->idle_at
        && now - slot->idle_at >= pool->cfg.ping_idle_ms) {
        if (ERR_OK != pool->ops->ping(conn)) {
            pool->stat.connfails++;
            if (0 != pool->closed) {
                return _fail_closed(pool, idx, 1);
            }
            return _checkout_fail(pool, idx);
        }
        pool->stat.pings++;
        slot->fails = 0;
        if (0 != pool->closed) {
            return _fail_closed(pool, idx, 1);
        }
    }
    slot->idle_at = 0;
    slot->cool_until = 0;
    *out = conn;
    return ERR_OK;
}
// 借出超过 leak_warn_ms 还没还的，每次借出告警一次
static void _leak_check(db_pool_ctx *pool) {
    if (0 == pool->cfg.leak_warn_ms) {
        return;
    }
    uint64_t now = timer_cur_ms(&pool->timer);
    db_pool_slot *slot;
    for (int32_t i = 0; i < pool->nslot; i++) {
        slot = &pool->slots[i];
        if (0 != slot->busy
            && 0 != slot->out_at
            && 0 == slot->warned
            && now - slot->out_at >= pool->cfg.leak_warn_ms) {
            slot->warned = 1;
            pool->stat.leaks++;
            LOG_WARN("db_pool: connection %d checked out %"PRIu64"ms ago, not returned yet.", i, now - slot->out_at);
        }
    }
}
// 连着的空闲连接不足 min_idle 时，挑断开且不在冷却的槽建连补上；建好走归还路径（有人等就直接交接）。
// 一轮最多建 nslot 次：寿命极短时建好的归还即到期断开、又成了可补的断开槽，不设上限会一直重连
static void _fill_min(db_pool_ctx *pool) {
    void *conn;
    int32_t idx;
    for (int32_t n = 0; n < pool->nslot
        && 0 == pool->closed
        && _ready_idle(pool) < pool->cfg.min_idle; n++) {
        idx = _take_cold(pool);
        if (idx < 0
            || ERR_OK != _checkout(pool, idx, &conn)) {
            return;// 失败的已冷却放回（没有借出时已叫醒等待者）；GONE 时池已关闭
        }
        db_pool_put(pool, conn);
    }
}
// 维护协程：定时收缩空闲、查借出过久、补足 min_idle；free 叫醒它就退出
static void _tick_loop(void *owner, void *arg) {
    (void)owner;
    db_pool_ctx *pool = (db_pool_ctx *)arg;
    while (0 == pool->closed) {
        (void)coro_wait(pool->co, pool->tick_sess, CORO_TAG_DB_POOL_TICK, 0, _DB_POOL_TICK_MS);
        if (0 != pool->closed) {
            break;
        }
        _leak_check(pool);
        _reap(pool);
        _fill_min(pool);
    }
    pool->ticking = 0;
    _release_if_last(pool);
}
// 用到收缩、补 min_idle 或借出过久告警时，在第一次借连接时起维护协程（得在协程里起）
static inline void _tick_start(db_pool_ctx *pool) {
    if (0 != pool->ticking
        || (0 == pool->cfg.idle_timeout_ms
            && 0 == pool->cfg.min_idle
            && 0 == pool->cfg.leak_warn_ms)) {
        return;
    }
    pool->ticking = 1;
    coro_fork(pool->co, _tick_loop, pool);
}
db_pool_ctx *db_pool_new(task_ctx *task, const db_pool_ops *ops, const db_pool_cfg *cfg) {
    if (NULL == task
        || NULL == ops
        || NULL == cfg
        || NULL == ops->ready
        || NULL == ops->connect
        || NULL == ops->quit
        || cfg->cap <= 0) {
        return NULL;
    }
    coro_ctx *co = coro_task_co(task);
    if (NULL == co) {
        return NULL;
    }
    db_pool_ctx *pool;
    CALLOC(pool, 1, sizeof(db_pool_ctx));
    timer_init(&pool->timer);
    pool->task = task;
    pool->co = co;
    pool->ops = ops;
    pool->cfg = *cfg;
    if (0 == pool->cfg.backoff_ms) {
        pool->cfg.backoff_ms = DB_POOL_DEF_BACKOFF_MS;
    }
    if (pool->cfg.min_idle < 0) {
        pool->cfg.min_idle = 0;
    }
    if (pool->cfg.min_idle > pool->cfg.cap) {
        pool->cfg.min_idle = pool->cfg.cap;
    }
    pool->sess = createid();
    pool->tick_sess = createid();
    CALLOC(pool->slots, cfg->cap, sizeof(db_pool_slot));
    CALLOC(pool->idle, cfg->cap, sizeof(int32_t));
    return pool;
}
int32_t db_pool_add(db_pool_ctx *pool, void *conn) {
    if (NULL == pool
        || NULL == conn
        || 0 != pool->closed
        || 0 != pool->busy
        || 0 != pool->waiters
        || pool->nslot >= pool->cfg.cap
        || _slot_find(pool, conn) >= 0) {
        return ERR_FAILED;
    }
    int32_t idx = pool->nslot++;
    ZERO(&pool->slots[idx], sizeof(db_pool_slot));
    pool->slots[idx].conn = conn;
    _idle_push(pool, idx);
    return ERR_OK;
}
int32_t db_pool_get(db_pool_ctx *pool, uint32_t timeout_ms, void **conn) {
    if (NULL == pool
        || NULL == conn
        || 0 == coro_incoro(pool->task)) {
        return ERR_FAILED;
    }
    *conn = NULL;
    if (0 != pool->closed) {
        return ERR_FAILED;
    }
    _tick_start(pool);
    uint64_t deadline = 0;
    if (timeout_ms > 0) {
        deadline = timer_cur_ms(&pool->timer) + timeout_ms;
    }
    int32_t idx, rtn;
    uint32_t ms, cool;
    uint64_t now;
    void *item;
    for (;;) {
        if (0 != pool->closed) {
            return ERR_FAILED;
        }
        now = timer_cur_ms(&pool->timer);
        idx = _take_usable(pool, now);
        if (idx >= 0) {
            rtn = _checkout(pool, idx, conn);
            if (ERR_OK == rtn) {
                _hand_out(pool, idx);
                return ERR_OK;
            }
            if (_DB_POOL_GONE == rtn) {
                return ERR_FAILED;
            }
            continue;
        }
        if (0 != deadline
            && now >= deadline) {
            pool->stat.timeouts++;
            return DB_POOL_TIMEDOUT;
        }
        // 没有借出的连接、空闲的又全在冷却（或还没 add）：等也没人来叫醒，快速失败
        if (0 == pool->busy) {
            return ERR_FAILED;
        }
        ms = (0 != deadline) ? (uint32_t)(deadline - now) : 0;
        // 有冷却中的槽：最晚等到最早那条冷却结束就回来重试，不必干等别人归还
        cool = _cool_wait(pool, now);
        if (cool > 0
            && (0 == ms || cool < ms)) {
            ms = cool;
        }
        pool->waiters++;
        item = coro_wait(pool->co, pool->sess, CORO_TAG_DB_POOL, 0, ms);
        pool->waiters--;
        if (0 != pool->closed) {
            _release_if_last(pool);// db_pool_free 的广播；同批先醒的可能已调过 free
            return ERR_FAILED;
        }
        if (NULL == item) {
            continue;// 有空位了，回去重试
        }
        idx = _slot_find(pool, item);
        if (idx >= 0) {
            _hand_out(pool, idx);
            *conn = item;
            return ERR_OK;
        }
        // coro_expire 注入的超时消息：回去再看一次空闲栈，期限到了由循环开头报超时
    }
}
void db_pool_put(db_pool_ctx *pool, void *conn) {
    if (NULL == pool
        || NULL == conn) {
        return;
    }
    int32_t idx = _slot_find(pool, conn);
    if (idx < 0) {
        LOG_ERROR("%s", "db_pool_put: connection is not in this pool.");
        return;
    }
    db_pool_slot *slot = &pool->slots[idx];
    if (0 == slot->busy) {
        LOG_ERROR("%s", "db_pool_put: connection is not checked out.");
        return;
    }
    slot->out_at = 0;
    if (0 != pool->closed) {
        _fail_closed(pool, idx, 1);
        return;
    }
    // 不能复用的（断了，或 MySQL 结果集收到一半）一律断开；到寿命的不交接；没到的先清会话状态。
    // 都可能挂起，busy 仍占着
    int32_t quitted = 0;
    if (0 == pool->ops->ready(conn)
        || _expired(pool, slot, timer_cur_ms(&pool->timer))
        || (NULL != pool->ops->reset
            && ERR_OK != pool->ops->reset(conn))) {
        pool->ops->quit(conn);
        slot->born_at = 0;
        quitted = 1;
    }
    if (0 != pool->closed) {
        _fail_closed(pool, idx, !quitted);
        return;
    }
    slot->cool_until = 0;
    if (0 == quitted) {
        slot->idle_at = 0;
        if (ERR_OK == coro_wake(pool->co, pool->sess, CORO_TAG_DB_POOL, conn)) {
            return;// 已交接；对方跑起来后可能已经又把连接还回来了，这里不能再碰 slot
        }
        _put_idle(pool, idx, 0);
        return;
    }
    // 断开了、还没试过重连：不冷却，下一次借出会就地 connect；叫醒队头来重试
    _put_idle(pool, idx, 0);
    slot->idle_at = 0;
    if (pool->waiters > 0) {
        coro_wake(pool->co, pool->sess, CORO_TAG_DB_POOL, NULL);
    }
}
void db_pool_free(db_pool_ctx *pool) {
    if (NULL == pool
        || 0 != pool->closed) {
        return;
    }
    pool->closed = 1;
    coro_wake_all(pool->co, pool->sess, NULL, NULL);
    coro_wake_all(pool->co, pool->tick_sess, NULL, NULL);
    int32_t idx;
    db_pool_slot *slot;
    while (pool->nidle > 0) {
        idx = pool->idle[--pool->nidle];
        slot = &pool->slots[idx];
        slot->in_idle = 0;
        slot->idle_at = 0;
        // 不看 ready：半截结果集的连接也要把 fd 关掉；从未连过的由各协议 quit 自己早退
        pool->ops->quit(slot->conn);
    }
    pool->free_pending = 1;
    _release_if_last(pool);
}
void db_pool_stat_get(const db_pool_ctx *pool, db_pool_stat *st) {
    if (NULL == pool
        || NULL == st) {
        return;
    }
    *st = pool->stat;
    st->idle = pool->nidle;
    st->busy = pool->busy;
    st->waiters = pool->waiters;
}
