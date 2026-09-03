#ifndef POOL_H_
#define POOL_H_

#include "base/structs.h"
#include "utils/load_trend.h"
#include "containers/fsqu.h"

typedef enum pool_ops {
    POOL_OP_NOCLEAR = 0x01,// 不执行 _pool_elclear。对象带着 _elclear 该释放的东西进池,
                           // 而 pool_free 只走 _elfree,那部分归调用方自己收
    POOL_OP_NOFREE = 0x02,// 不执行 _pool_elfree
    POOL_OP_NORESET= 0x04// 不执行 _pool_elreset
}pool_ops;

typedef void *(*_el_new)(void *args);// 新建
typedef void (*_el_reset)(void *data, void *args);// 重置
typedef void (*_el_clear)(void *data);// 清理
// 对象回调;_elnew 与 _elfree 须成对:要么都为 NULL(默认 CALLOC/FREE),要么都自定义(同一分配器),否则分配/释放器不匹配
typedef struct pool_cbs {
    _el_new _elnew;
    free_cb _elfree;
    _el_reset _elreset;
    _el_clear _elclear;
}pool_cbs;
// 对象池
typedef struct pool_ctx {
    uint32_t elsize;// 对象大小
    uint32_t nkeep;
    int32_t thsafe;// 非 0 用 qu.safe_qu, 否则 qu.normal_qu; pool_init 时定死，之后只读
    pool_cbs elcbs;
    union {
        queue_ctx normal_qu;// 非线程安全
        fsqu_ctx safe_qu;// 线程安全
    }qu;
    load_trend_ctx trend;
}pool_ctx;

static inline void *_pool_elnew(pool_ctx *pool, void *args) {
    if (NULL != pool->elcbs._elnew) {
        return pool->elcbs._elnew(args);
    } else {
        void *data;
        CALLOC(data, 1, pool->elsize);
        return data;
    }
}
static inline void _pool_elfree(pool_ctx *pool, void *data) {
    if(NULL != pool->elcbs._elfree) {
        pool->elcbs._elfree(data);
    } else {
        FREE(data);
    }
}
static inline void _pool_elreset(pool_ctx *pool, void *data, void *args) {
    if (NULL != pool->elcbs._elreset) {
        pool->elcbs._elreset(data, args);
    }
}
static inline void _pool_elclear(pool_ctx *pool, void *data) {
    if (NULL != pool->elcbs._elclear) {
        pool->elcbs._elclear(data);
    }
}
// 取一个空闲对象。安全池下 fsqu_pop 的三态在这里压成两态：元素被生产者抢占尚未发布(返回 1)
// 与真的没有一样当没取到,pool_pop 会改走新建。想区分的调用方得自己去用 fsqu_pop
static inline int32_t _pool_qu_pop(pool_ctx *pool, void **out) {
    if (pool->thsafe) {
        return fsqu_pop(&pool->qu.safe_qu, out);
    }
    void **elem = (void **)queue_pop(&pool->qu.normal_qu);
    if (NULL == elem) {
        return ERR_FAILED;
    }
    *out = *elem;
    return ERR_OK;
}
static inline uint32_t _pool_qu_size(pool_ctx *pool) {
    return pool->thsafe ? fsqu_size(&pool->qu.safe_qu) : queue_size(&pool->qu.normal_qu);
}
// 释放 nfree 个空闲对象。安全池按批出队摊薄原子操作，普通池逐个取。
// 不做 inline:批量出队的落地数组有 1KB，内联进来会把每个 pool_shrink 调用方的栈帧撑大
void _pool_qu_nelfree(pool_ctx *pool, uint32_t nfree);
/// <summary>
/// 初始化对象池
/// </summary>
/// <param name="pool">pool_ctx</param>
/// <param name="elsize">对象大小(字节);未设 _elnew 时按此大小 CALLOC 新建对象</param>
/// <param name="capacity">底层队列容量,0 用默认值。实际容量会向上取整(thsafe 走 fsqu 取到
///   2 的幂,否则 queue 取到偶数),pool_capacity 返回的是取整后的值</param>
/// <param name="nkeep">收缩时保留的最小空闲对象数</param>
/// <param name="thsafe">非 0 启用线程安全(fsqu 底层);0 用普通 queue(非线程安全)</param>
/// <param name="elcbs">对象回调(new/free/reset/clear),NULL 走默认 CALLOC/FREE</param>
void pool_init(pool_ctx *pool, size_t elsize, uint32_t capacity,
               uint32_t nkeep, int32_t thsafe, pool_cbs *elcbs);
/// <summary>
/// 释放池内所有空闲对象(只经 _elfree,不补 _elclear——正常入池的对象在 push 时已 clear 过)
/// 并销毁底层队列;不释放 pool 本身。
/// 须在没有并发 push/pop 时调用:安全池下有生产者正在写入会让出队提前报空,剩下的对象漏释放
/// </summary>
/// <param name="pool">pool_ctx</param>
void pool_free(pool_ctx *pool);
/// <summary>
/// 归还对象到池:先 _elclear,再尝试入池;池满则经 _elfree 释放
/// </summary>
/// <param name="pool">pool_ctx</param>
/// <param name="data">归还的对象指针</param>
/// <param name="ops">pool_ops, 控制是否执行 clear free</param>
/// <returns>ERR_OK 入池成功,ERR_FAILED 池满:含 POOL_OP_NOFREE 时对象仍归调用方,否则已被 _elfree 释放</returns>
static inline int32_t pool_push(pool_ctx *pool, void *data, int32_t ops) {
    if (!BIT_CHECK(ops, POOL_OP_NOCLEAR)) {
        _pool_elclear(pool, data);
    }
    if (ERR_OK == (pool->thsafe ? fsqu_trypush(&pool->qu.safe_qu, &data)
                                : queue_trypush(&pool->qu.normal_qu, &data))) {
        return ERR_OK;
    }
    if (!BIT_CHECK(ops, POOL_OP_NOFREE)) {
        _pool_elfree(pool, data);
    }
    return ERR_FAILED;
}
/// <summary>
/// 从池取一个对象:命中空闲则经 _elreset 复用,否则经 _elnew 新建
/// </summary>
/// <param name="pool">pool_ctx</param>
/// <param name="args">透传给 _elreset / _elnew 的参数</param>
/// <param name="ops">pool_ops, 控制是否执行 reset</param>
/// <returns>对象指针;自定义 _elnew 失败时可能为 NULL</returns>
static inline void *pool_pop(pool_ctx *pool, void *args, int32_t ops) {
    void *data = NULL;
    if (ERR_OK == _pool_qu_pop(pool, &data)) {
        if (!BIT_CHECK(ops, POOL_OP_NORESET)) {
            _pool_elreset(pool, data, args);
        }
    } else {
        data = _pool_elnew(pool, args);
    }
    return data;
}
/// <summary>
/// 当前空闲对象数(线程安全池下为近似值)
/// </summary>
/// <param name="pool">pool_ctx</param>
/// <returns>空闲对象数</returns>
static inline uint32_t pool_size(pool_ctx *pool) {
    return _pool_qu_size(pool);
}
/// <summary>
/// 底层队列容量
/// </summary>
/// <param name="pool">pool_ctx</param>
/// <returns>容量</returns>
static inline uint32_t pool_capacity(pool_ctx *pool) {
    return pool->thsafe ? fsqu_capacity(&pool->qu.safe_qu) : queue_maxsize(&pool->qu.normal_qu);
}
/// <summary>
/// 计算 pool_shrink 的保留量
/// </summary>
/// <param name="n">当前对象/元素数(pool_size 或 hashmap_count 结果)</param>
/// <returns>建议保留的空闲对象数</returns>
static inline uint32_t shrink_nkeep(size_t n) {
    return (uint32_t)(n - n / 5);
}
// 收缩本体, pool_shrink_to 与 pool_shrink 共用。plsize 由调用方传进来: keep 与 plsize
// 必须出自同一次采样, 各读一次 pool_size 会让相减出来的收缩量对不上
static inline void _pool_shrink_sized(pool_ctx *pool, uint32_t keep, uint32_t plsize) {
    if (load_trend_busy(&pool->trend, plsize, SHRINK_BUSY)) {
        return;
    }
    if (keep < pool->nkeep) {
        keep = pool->nkeep;
    }
    if (plsize > keep) {
        _pool_qu_nelfree(pool, plsize - keep);
    }
}
/// <summary>
/// 收缩空闲对象至 max(keep, nkeep);load_trend 按 SHRINK_BUSY 判 busy 时跳过本次。
/// 保留量由池外计数决定时用本函数,否则用 pool_shrink。
/// push/pop 多线程安全,但 shrink 须由单一线程调用(内部更新 trend 无锁);并发 push/pop 下收缩量为尽力而为
/// </summary>
/// <param name="pool">pool_ctx</param>
/// <param name="keep">期望保留的空闲对象数(实际下限取 max(keep, nkeep))</param>
static inline void pool_shrink_to(pool_ctx *pool, uint32_t keep) {
    _pool_shrink_sized(pool, keep, pool_size(pool));
}
/// <summary>
/// 按池自身占用收缩空闲对象(保留量 shrink_nkeep(pool_size))。"收多少、什么算忙"
/// 两个策略都在池内定,调用方不必逐处重述
/// </summary>
/// <param name="pool">pool_ctx</param>
static inline void pool_shrink(pool_ctx *pool) {
    uint32_t plsize = pool_size(pool);
    _pool_shrink_sized(pool, shrink_nkeep(plsize), plsize);
}
/// <summary>
/// 收缩节流:与 pool_shrink / pool_shrink_to 配合用,"多久收一次"这条策略只写在这里
/// </summary>
/// <param name="last_ms">出入参:上次收缩的时间戳(毫秒),到点时由本函数推到 now_ms</param>
/// <param name="now_ms">当前时间戳(毫秒)</param>
/// <returns>距 *last_ms 不足 SHRINK_TIME 返 0,到点返非 0</returns>
static inline int32_t pool_shrink_due(uint64_t *last_ms, uint64_t now_ms) {
    if (now_ms - *last_ms < SHRINK_TIME) {
        return 0;
    }
    *last_ms = now_ms;
    return 1;
}

#endif//POOL_H_
