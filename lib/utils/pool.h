#ifndef POOL_H_
#define POOL_H_

#include "base/structs.h"
#include "utils/load_trend.h"
#include "containers/fsqu.h"

typedef enum pool_ops {
    POOL_OP_NOCLEAR = 0x01,// 不执行 _pool_elclear
    POOL_OP_NOFREE = 0x02,// 不执行 _pool_elfree
    POOL_OP_NORESET= 0x04// 不执行 _pool_elreset
}pool_ops;

typedef void *(*_el_new)(void *args);// 新建
typedef void (*_el_reset)(void *data, void *args);// 重置
typedef void (*_el_clear)(void *data);// 清理
// 对象回调;_elnew 与 _elfree 须成对:要么都为 NULL(默认 CALLOC/FREE),要么都自定义(同一分配器),否则分配/释放器不匹配
typedef struct el_cbs {
    _el_new _elnew;
    free_cb _elfree;
    _el_reset _elreset;
    _el_clear _elclear;
}el_cbs;
// 对象池
typedef struct pool_ctx {
    uint32_t elsize;// 对象大小
    uint32_t nkeep;
    void *cur_qu;// 当前使用的queue
    // cur_qu 函数指针
    int32_t (*_qu_trypush)(void *qu, const void *data);
    int32_t (*_qu_pop)(void *qu, void *out);
    void (*_qu_free)(void *qu);
    void (*_qu_nelfree)(struct pool_ctx *pool, uint32_t nfree);
    uint32_t (*_qu_size)(void *qu);
    uint32_t (*_qu_capacity)(void *qu);
    el_cbs elcbs;
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
/// <summary>
/// 初始化对象池
/// </summary>
/// <param name="pool">pool_ctx</param>
/// <param name="elsize">对象大小(字节);未设 _elnew 时按此大小 CALLOC 新建对象</param>
/// <param name="capacity">底层队列容量,0 用默认值</param>
/// <param name="nkeep">收缩时保留的最小空闲对象数</param>
/// <param name="thsafe">非 0 启用线程安全(fsqu 底层);0 用普通 queue(非线程安全)</param>
/// <param name="elcbs">对象回调(new/free/reset/clear),NULL 走默认 CALLOC/FREE</param>
void pool_init(pool_ctx *pool, size_t elsize, uint32_t capacity,
               uint32_t nkeep, int32_t thsafe, el_cbs *elcbs);
/// <summary>
/// 释放池内所有空闲对象(经 _elfree)并销毁底层队列;不释放 pool 本身
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
    if (ERR_OK == pool->_qu_trypush(pool->cur_qu, &data)) {
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
    if (ERR_OK == pool->_qu_pop(pool->cur_qu, &data)) {
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
    return pool->_qu_size(pool->cur_qu);
}
/// <summary>
/// 底层队列容量
/// </summary>
/// <param name="pool">pool_ctx</param>
/// <returns>容量</returns>
static inline uint32_t pool_capacity(pool_ctx *pool) {
    return pool->_qu_capacity(pool->cur_qu);
}
/// <summary>
/// 收缩空闲对象至 max(keep, nkeep);load_trend 判 busy 时跳过本次。
/// push/pop 多线程安全,但 shrink 须由单一线程调用(内部更新 trend 无锁);并发 push/pop 下收缩量为尽力而为
/// </summary>
/// <param name="pool">pool_ctx</param>
/// <param name="keep">期望保留的空闲对象数(实际下限取 max(keep, nkeep))</param>
/// <param name="busy_num">load_trend busy 判定分子</param>
/// <param name="busy_den">load_trend busy 判定分母</param>
static inline void pool_shrink(pool_ctx *pool, uint32_t keep,
                               uint32_t busy_num, uint32_t busy_den) {
    uint32_t plsize = pool_size(pool);
    if (load_trend_busy(&pool->trend, plsize, busy_num, busy_den)) {
        return;
    }
    if (keep < pool->nkeep) {
        keep = pool->nkeep;
    }
    if (plsize > keep) {
        pool->_qu_nelfree(pool, plsize - keep);
    }
}

#endif//POOL_H_
