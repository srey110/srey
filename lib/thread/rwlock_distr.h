#ifndef RWLOCK_DISTR_H_
#define RWLOCK_DISTR_H_

#include "base/macro.h"
#include "thread/rwlock.h"

// 分布式读锁:per-thread cache-line slot 消除原子计数器争用,适合读极多写极少
// 写锁开销与 reader 数线性相关(扫所有 slot),不适合写频繁场景
// 同线程禁止: 1)递归 rdlock;2)持 rdlock 后调 wrlock(扫 active 自死锁);
// 3)递归 wrlock(pthread_rwlock 默认不可重入)

// 同线程支持注册到至多 RWLOCK_DISTR_MAX_TLS 个不同 ctx;超出上限的 ctx
// 该线程走 fallback rwlock。每次 rdlock/runlock 多一次 N 元素线性扫描,
// N 通常 ≤ 4,开销几十 ns,远低于 fallback rwlock 的 cache 弹跳成本。
// 单线程可同时注册的 rwlock_distr_ctx 上限;扩大需评估线性扫描开销
#define RWLOCK_DISTR_MAX_TLS  4

// CPU cache line 大小,用于消除并发结构 false sharing
// 主流 x86_64 / ARM64 = 64;Apple Silicon / IBM POWER = 128;IBM z = 256;老 ARMv6 及以下 = 32
#if defined(__APPLE__) && defined(__aarch64__)
    #define CACHELINE_SIZE  128
#elif defined(__powerpc64__) || defined(__ppc64__) || defined(_ARCH_PPC64)
    #define CACHELINE_SIZE  128
#elif defined(__s390x__) || defined(__zarch__)
    #define CACHELINE_SIZE  256
#elif defined(__arm__) && (__ARM_ARCH < 7)
    #define CACHELINE_SIZE  32
#else
    #define CACHELINE_SIZE  64
#endif

// 分布式读锁单 slot,独占一条 cache line 避免 false sharing
typedef struct rwlock_distr_slot {
    atomic_t active;   // 0=空闲 / 1=本 slot 的 reader 持读锁
    atomic_t in_use;   // 0=未分配 / 1=已分配给某线程
    char _pad[CACHELINE_SIZE - sizeof(atomic_t) * 2];
} rwlock_distr_slot;
// 分布式读锁上下文
typedef struct rwlock_distr_ctx {
    atomic_t write_flag;          // writer 进入标志,reader 快路径检查
    uint32_t slot_count;          // slot 池容量
    void *slots_raw;              // MALLOC 原始指针,free 时归还
    rwlock_distr_slot *slots;     // cache-line 对齐后的数组首地址
    rwlock_ctx fallback;          // 写锁与未注册线程读锁
} rwlock_distr_ctx;

/// <summary>
/// 分布式读锁初始化
/// </summary>
/// <param name="ctx">rwlock_distr_ctx</param>
/// <param name="slot_count">slot 池容量,建议 = 长期读者线程数 + 临时余量</param>
void rwlock_distr_init(rwlock_distr_ctx *ctx, uint32_t slot_count);
/// <summary>
/// 分布式读锁释放
/// </summary>
/// <param name="ctx">rwlock_distr_ctx</param>
void rwlock_distr_free(rwlock_distr_ctx *ctx);
/// <summary>
/// 当前线程注册一个 slot,以走 cache-line 独占的读锁快路径
/// 必须在第一次 rwlock_distr_rdlock 之前调用,否则该线程走 fallback rwlock
/// 同一线程对同一 ctx 重复调用幂等返回 ERR_OK
/// 同一线程可同时注册到至多 RWLOCK_DISTR_MAX_TLS 个不同 ctx;超出上限返回 ERR_FAILED
/// </summary>
/// <param name="ctx">rwlock_distr_ctx</param>
/// <returns>ERR_OK 注册成功,ERR_FAILED slot 池已满或 TLS 数组已满,走 fallback</returns>
int32_t rwlock_distr_register(rwlock_distr_ctx *ctx);
/// <summary>
/// 当前线程释放 slot,归还给池供其他线程复用
/// 调用方须先 rwlock_distr_runlock,持读锁期间禁止 unregister
/// 未注册到本 ctx 的线程调用为 noop
/// </summary>
/// <param name="ctx">rwlock_distr_ctx</param>
void rwlock_distr_unregister(rwlock_distr_ctx *ctx);
/// <summary>
/// 读锁定。已注册线程走 per-slot 快路径;未注册线程走 fallback
/// 不支持递归(同线程对同 ctx 重复 rdlock 会卡 writer 路径)
/// </summary>
/// <param name="ctx">rwlock_distr_ctx</param>
void rwlock_distr_rdlock(rwlock_distr_ctx *ctx);
/// <summary>
/// 读锁解锁。必须与 rwlock_distr_rdlock 配对
/// </summary>
/// <param name="ctx">rwlock_distr_ctx</param>
void rwlock_distr_runlock(rwlock_distr_ctx *ctx);
/// <summary>
/// 写锁定。阻塞所有 reader,等已进入 reader 全部退出后独占
/// </summary>
/// <param name="ctx">rwlock_distr_ctx</param>
void rwlock_distr_wrlock(rwlock_distr_ctx *ctx);
/// <summary>
/// 写锁解锁。必须与 rwlock_distr_wrlock 配对
/// </summary>
/// <param name="ctx">rwlock_distr_ctx</param>
void rwlock_distr_wrunlock(rwlock_distr_ctx *ctx);

#endif//RWLOCK_DISTR_H_
