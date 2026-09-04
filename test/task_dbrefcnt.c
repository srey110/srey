#include "task_dbrefcnt.h"

typedef struct dbrefcnt_args {
    int32_t *ok;
}dbrefcnt_args;

// 反复连接的轮数：>=2 才能覆盖"修复前第 1 轮误 free、第 2 轮 UAF"的场景
#define DBREFCNT_ROUNDS 8
// inet_pton 对 IPv4/IPv6 均失败 → ev_connect 内 netaddr_set 同步失败 → UD_FREE；不触发 DNS/真实连接
#define DBREFCNT_BADIP "255.255.255.256"
// 初始 ref 取一个够大的哨兵而不是 1：漏 acquire 时每轮净 -1，若从 1 起算第 1 轮就减到 0
// 触发 SECURE_FREE，之后连读 ref 都是 UAF，断言无从下手。取 1000 保证块在整个循环里必然存活
#define DBREFCNT_GUARD 1000

// 模拟 lbind：堆分配 ctx + 非零 ref 等价上层持有。每轮 try_connect 走 prots_wrap 的
// PROT_REF_ACQUIRE(+1)，与 ev_connect 同步失败路径 UD_FREE→udfree(-1) 配对，
// 故每轮结束 ref 必须回到哨兵；漏 acquire 时它逐轮下降，当场断言失败。
// 修复前 try_connect 仅在成功后 +1，失败路径 udfree 把持有者那份减掉，块被误 FREE。
// 四个客户端只差类型 / 初始化实参 / try_connect 三处，其余逐字相同，故用宏展开
#define DBREFCNT_CASE(name, type, initcall, tryfn) \
static int32_t _refcnt_##name(task_ctx *task) { \
    type *ctx; \
    int32_t i; \
    atomic_t ref; \
    int32_t rtn = ERR_OK; \
    MALLOC(ctx, sizeof(type)); \
    if (ERR_OK != initcall) { \
        LOG_ERROR(#name "_init(%s) failed.", DBREFCNT_BADIP); \
        FREE(ctx); \
        return ERR_FAILED; \
    } \
    ATOMIC_SET(&ctx->ref, DBREFCNT_GUARD); \
    for (i = 0; i < DBREFCNT_ROUNDS; i++) { \
        tryfn(task, ctx, 1); \
        ref = ATOMIC_GET(&ctx->ref); \
        if ((atomic_t)DBREFCNT_GUARD != ref) { \
            LOG_ERROR(#name " ref unpaired at round %d: %u", i, (uint32_t)ref); \
            rtn = ERR_FAILED; \
            break; \
        } \
    } \
    ATOMIC_SET(&ctx->ref, 1); \
    PROT_REF_RELEASE(ctx); \
    return rtn; \
}

DBREFCNT_CASE(mysql, mysql_ctx,
    mysql_init(ctx, DBREFCNT_BADIP, 3306, NULL, "u", "p", "db", "utf8mb4", 0), mysql_try_connect)
DBREFCNT_CASE(pgsql, pgsql_ctx,
    pgsql_init(ctx, DBREFCNT_BADIP, 5432, NULL, "u", "p", "db"), pgsql_try_connect)
DBREFCNT_CASE(mongo, mongo_ctx,
    mongo_init(ctx, DBREFCNT_BADIP, 27017, NULL, "db"), mongo_try_connect)
DBREFCNT_CASE(smtp, smtp_ctx,
    smtp_init(ctx, DBREFCNT_BADIP, 25, NULL, "u", "p"), smtp_try_connect)

static void _startup(task_ctx *task) {
    dbrefcnt_args *arg = (dbrefcnt_args *)coro_get_arg(task);
    if (ERR_OK != _refcnt_mysql(task)
        || ERR_OK != _refcnt_pgsql(task)
        || ERR_OK != _refcnt_mongo(task)
        || ERR_OK != _refcnt_smtp(task)) {
        return;
    }
    *(arg->ok) = 1;
    LOG_INFO("db refcount paired.");
}
void task_dbrefcnt_start(loader_ctx *loader, const char *name, int32_t *ok) {
    dbrefcnt_args *arg;
    CALLOC(arg, 1, sizeof(dbrefcnt_args));
    arg->ok = ok;
    coro_task_register(loader, name, 0, _startup, NULL, _free, arg);
}
