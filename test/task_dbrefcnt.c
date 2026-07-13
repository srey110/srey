#include "task_dbrefcnt.h"

typedef struct dbrefcnt_args {
    int32_t *ok;
}dbrefcnt_args;

// 反复连接的轮数：>=2 才能覆盖"修复前第 1 轮误 free、第 2 轮 UAF"的场景
#define DBREFCNT_ROUNDS 8
// inet_pton 对 IPv4/IPv6 均失败 → ev_connect 内 netaddr_set 同步失败 → UD_FREE；不触发 DNS/真实连接
#define DBREFCNT_BADIP "255.255.255.256"

// 模拟 lbind：堆分配 ctx + ATOMIC_SET(ref,1) 等价 Lua 持有。
// 每轮 try_connect 走 prots_wrap 的 PROT_REF_ACQUIRE(+1)，与 ev_connect 同步失败路径
// UD_FREE→udfree(-1) 配对，ref 稳定回 1、块存活；末尾 PROT_REF_RELEASE 等价 __gc 归 0 释放。
// 修复前 try_connect 仅在成功后 +1，失败路径 udfree 把仅有的 1 份减到 0 误 FREE 块，
// 下一轮 try_connect / 末尾释放即落在已释放内存上 → UAF / double-free（ASan 可捕获）。
static void _refcnt_mysql(task_ctx *task) {
    mysql_ctx *ctx;
    int32_t i;
    MALLOC(ctx, sizeof(mysql_ctx));
    (void)mysql_init(ctx, DBREFCNT_BADIP, 3306, NULL, "u", "p", "db", "utf8mb4", 0);
    ATOMIC_SET(&ctx->ref, 1);
    for (i = 0; i < DBREFCNT_ROUNDS; i++) {
        mysql_try_connect(task, ctx, 1);
    }
    PROT_REF_RELEASE(ctx);
}
static void _refcnt_pgsql(task_ctx *task) {
    pgsql_ctx *ctx;
    int32_t i;
    MALLOC(ctx, sizeof(pgsql_ctx));
    (void)pgsql_init(ctx, DBREFCNT_BADIP, 5432, NULL, "u", "p", "db");
    ATOMIC_SET(&ctx->ref, 1);
    for (i = 0; i < DBREFCNT_ROUNDS; i++) {
        pgsql_try_connect(task, ctx, 1);
    }
    PROT_REF_RELEASE(ctx);
}
static void _refcnt_mongo(task_ctx *task) {
    mongo_ctx *ctx;
    int32_t i;
    MALLOC(ctx, sizeof(mongo_ctx));
    mongo_init(ctx, DBREFCNT_BADIP, 27017, NULL, "db");
    ATOMIC_SET(&ctx->ref, 1);
    for (i = 0; i < DBREFCNT_ROUNDS; i++) {
        mongo_try_connect(task, ctx, 1);
    }
    PROT_REF_RELEASE(ctx);
}
static void _refcnt_smtp(task_ctx *task) {
    smtp_ctx *ctx;
    int32_t i;
    MALLOC(ctx, sizeof(smtp_ctx));
    smtp_init(ctx, DBREFCNT_BADIP, 25, NULL, "u", "p");
    ATOMIC_SET(&ctx->ref, 1);
    for (i = 0; i < DBREFCNT_ROUNDS; i++) {
        smtp_try_connect(task, ctx, 1);
    }
    PROT_REF_RELEASE(ctx);
}

static void _startup(task_ctx *task) {
    dbrefcnt_args *arg = (dbrefcnt_args *)coro_get_arg(task);
    _refcnt_mysql(task);
    _refcnt_pgsql(task);
    _refcnt_mongo(task);
    _refcnt_smtp(task);
    *(arg->ok) = 1;
    LOG_INFO("db refcount paired.");
}
void task_dbrefcnt_start(loader_ctx *loader, const char *name, int32_t *ok) {
    dbrefcnt_args *arg;
    CALLOC(arg, 1, sizeof(dbrefcnt_args));
    arg->ok = ok;
    coro_task_register(loader, name, 0, _startup, NULL, _free, arg);
}
