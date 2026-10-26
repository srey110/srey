#ifndef DB_POOL_H_
#define DB_POOL_H_

#include "srey/task.h"

#define DB_POOL_DEF_BACKOFF_MS 1000 // 建连失败后默认冷却时长
#define DB_POOL_TIMEDOUT 1 // db_pool_get 的返回：等空闲连接超时

typedef struct db_pool_ctx db_pool_ctx;
// 连接操作表：池只认这几个动作，协议细节由各连接自己实现
typedef struct db_pool_ops {
    int32_t (*ready)(void *conn);                   // 1=当前可复用；不挂起
    int32_t (*connect)(task_ctx *task, void *conn); // 建连/重连；可挂起
    int32_t (*ping)(void *conn);                    // 空闲探活，失败时内部重连；可挂起；NULL=不探活
    void (*quit)(void *conn);                       // 断开；可挂起
    int32_t (*reset)(void *conn);                   // 归还时清会话状态（如回滚没结束的事务），干净时不该发包；
                                                    // 可挂起；非 ERR_OK 时池断开这条连接；NULL=不清
} db_pool_ops;
// 池配置
typedef struct db_pool_cfg {
    int32_t cap;            // 连接对象个数，必填，>0
    uint32_t ping_idle_ms;  // 空闲超过这么久先探活；0=不探活
    uint32_t backoff_ms;    // 建连/探活失败后该连接的冷却时长，连续失败按它翻倍、最多 32 倍，连上清零；0=默认 1000ms
    uint32_t max_lifetime_ms; // 连接寿命，每条随机少最多 1/8，免得同批连接同时到期；借出时到期先断开再重连，
                              // 归还时到期断开不交接；0=不限制
    int32_t min_idle;       // 至少保持的连着的空闲连接数：收缩时不低于它，维护协程每秒补足；0=不保留
    uint32_t idle_timeout_ms; // 空闲超过这么久断开，槽留着下次借出重连；维护协程每秒查一次；0=不收缩
    uint32_t leak_warn_ms;  // 借出超过这么久还没还，维护协程打一次 WARN；0=不查
} db_pool_cfg;
// 池的运行计数，db_pool_stat_get 一次性拷出
typedef struct db_pool_stat {
    int32_t idle;       // 当前空闲槽数（含冷却中/已收缩断开的）
    int32_t busy;       // 当前占用数：借出的、正在交接的，以及池内部正在建连/探活/断开/清会话的
    int32_t waiters;    // 当前挂起等连接的协程数
    uint32_t connects;  // 成功建连次数（含重连）
    uint32_t connfails; // 建连/探活失败次数
    uint32_t pings;     // 成功探活次数
    uint32_t timeouts;  // 等连接超时次数
    uint32_t leaks;     // 借出过久告警次数（leak_warn_ms）
} db_pool_stat;

/// <summary>
/// 建池。只登记容量，不建连；连接对象由调用方创建后用 db_pool_add 加进来。
/// 只能在协程 task 上建，池内所有调用都要在该 task 的协程里做。
/// 配了 idle_timeout_ms / min_idle / leak_warn_ms 时，第一次 db_pool_get 起一个维护协程，db_pool_free 时退出
/// </summary>
/// <param name="task">所属协程 task</param>
/// <param name="ops">连接操作表，生命周期须覆盖整个池</param>
/// <param name="cfg">配置；cap 必填</param>
/// <returns>池；task / ops / cfg 为 NULL、task 不是协程 task、ops 缺 ready / connect / quit、cap 小于等于 0 时返回 NULL</returns>
db_pool_ctx *db_pool_new(task_ctx *task, const db_pool_ops *ops, const db_pool_cfg *cfg);
/// <summary>
/// 登记一条连接对象（连没连着都行，没连的首次借出时建连）。当前有借出或等待者时不能加，
/// 一般在建池后、第一次借出前一次加齐
/// </summary>
/// <param name="pool">池</param>
/// <param name="conn">连接对象；所有权仍在调用方，池只借指针</param>
/// <returns>ERR_OK 成功；池已关闭/有借出或等待者/已满/指针重复返回 ERR_FAILED</returns>
int32_t db_pool_add(db_pool_ctx *pool, void *conn);
/// <summary>
/// 借一条连接。没有空闲时挂起当前协程，直到有人归还、冷却到期、池关闭或超时；
/// 没有借出中的连接、空闲的又全在冷却时不等。借出的连接已连上（断了会在池内重连）
/// </summary>
/// <param name="pool">池</param>
/// <param name="timeout_ms">等空闲连接的毫秒数；0=一直等。实际精度受 task 的超时扫描周期限制</param>
/// <param name="conn">出参：借到的连接，失败时置 NULL；pool / conn 为 NULL 或不在协程内时不动</param>
/// <returns>ERR_OK 成功；DB_POOL_TIMEDOUT 等超时；ERR_FAILED 已关闭/不在协程内/建连失败/全在冷却/一条连接都没加</returns>
int32_t db_pool_get(db_pool_ctx *pool, uint32_t timeout_ms, void **conn);
/// <summary>
/// 归还连接。借出期间连接断了也照常归还，下一次借出会自动重连；池关闭后归还的连接会被断开。
/// 操作表带 reset 时先清会话状态再交给下一个人，期间可能挂起。须在协程内调用（同池内其他接口）
/// </summary>
/// <param name="pool">池</param>
/// <param name="conn">db_pool_get 返回的那条连接</param>
void db_pool_put(db_pool_ctx *pool, void *conn);
/// <summary>
/// 销毁池：先唤醒所有等待者与维护协程，再断开空闲连接；还有借出、等待者没醒完或维护协程没退出时
/// 池内存留着，由最后一个离开的 put / get / 维护协程释放。
/// 应在所属 task 的 closing 回调里调：维护协程与一直等的 get 只有它能叫醒，不调 task 关不掉。
/// 调用方须保证此后没有协程再碰 pool。本函数不释放 conn 对象本身，conn 内存须活到所属 task 释放：
/// 维护协程或正在 get 的协程可能还挂在某条连接的建连 / 探活 / 断开上，返回后还会再碰它
/// </summary>
/// <param name="pool">池</param>
void db_pool_free(db_pool_ctx *pool);
/// <summary>
/// 拷出当前计数
/// </summary>
/// <param name="pool">池</param>
/// <param name="st">出参</param>
void db_pool_stat_get(const db_pool_ctx *pool, db_pool_stat *st);

#endif//DB_POOL_H_
