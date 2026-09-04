#include "task_pgsql.h"

#define _CONC_N 4

typedef struct task_pgsql_args {
    uint16_t port;
    int32_t *ok;
    char host[64];
    char user[64];
    char password[64];
    char database[64];
    pgsql_ctx pg;
}task_pgsql_args;

// 重建测试表，使每次运行从干净状态开始
static int32_t _setup_table(pgsql_ctx *pg) {
    pgpack_ctx *p = pgsql_query(pg, "drop table if exists srey_test");
    if (NULL == p || PGPACK_OK != p->type) {
        LOG_ERROR("pgsql drop table error.");
        return ERR_FAILED;
    }
    p = pgsql_query(pg,
        "create table srey_test (id int primary key, name text not null, score double precision)");
    if (NULL == p || PGPACK_OK != p->type) {
        LOG_ERROR("pgsql create table error.");
        return ERR_FAILED;
    }
    return ERR_OK;
}

// 简单 INSERT 验证 query 路径
static int32_t _simple_insert(pgsql_ctx *pg) {
    pgpack_ctx *p = pgsql_query(pg,
        "insert into srey_test (id, name, score) values (1, 'alice', 90.5), (2, 'bob', 75.0)");
    if (NULL == p || PGPACK_OK != p->type) {
        LOG_ERROR("pgsql simple insert error.");
        return ERR_FAILED;
    }
    if (2 != pgsql_affected_rows(p)) {
        LOG_ERROR("pgsql expected 2 rows inserted, got %"PRId64".", pgsql_affected_rows(p));
        return ERR_FAILED;
    }
    return ERR_OK;
}

// SELECT + reader 迭代
static int32_t _select_iterate(pgsql_ctx *pg, int32_t expect_rows) {
    pgpack_ctx *p = pgsql_query(pg, "select id, name, score from srey_test order by id");
    if (NULL == p || PGPACK_OK != p->type) {
        LOG_ERROR("pgsql select error.");
        return ERR_FAILED;
    }
    pgsql_reader_ctx *reader = pgsql_reader_iter(p, FORMAT_TEXT);
    if (NULL == reader) {
        LOG_ERROR("pgsql reader_iter error.");
        return ERR_FAILED;
    }
    int32_t cnt = 0;
    int32_t err;
    while (!pgsql_reader_eof(reader)) {
        // 仅校验 id 列可读，不假定具体值（COPY IN 用了非连续 id 10/11/12）
        (void)pgsql_reader_integer(reader, "id", &err);
        if (ERR_OK != err) {
            LOG_ERROR("pgsql reader id error.");
            pgsql_reader_free(reader);
            return ERR_FAILED;
        }
        cnt++;
        pgsql_reader_next(reader);
    }
    pgsql_reader_free(reader);
    if (cnt != expect_rows) {
        LOG_ERROR("pgsql select expected %d rows, got %d.", expect_rows, cnt);
        return ERR_FAILED;
    }
    return ERR_OK;
}

// 执行非法 SQL，校验 wire 上的 ErrorResponse 解析路径
static int32_t _query_syntax_error(pgsql_ctx *pg) {
    pgpack_ctx *p = pgsql_query(pg, "selct 1");
    if (NULL == p) {
        LOG_ERROR("pgsql syntax_error: expected ERR pack, got NULL.");
        return ERR_FAILED;
    }
    if (PGPACK_ERR != p->type) {
        LOG_ERROR("pgsql syntax_error: expected PGPACK_ERR, got %d.", p->type);
        return ERR_FAILED;
    }
    if (NULL == p->pack) {
        LOG_ERROR("pgsql syntax_error: pack payload missing.");
        return ERR_FAILED;
    }
    return ERR_OK;
}

// 多语句 simple query：一次响应含多个结果，按 CommandComplete 边界拆分
static int32_t _multi_statement(pgsql_ctx *pg) {
    int32_t err;
    int32_t lens;
    int64_t val;
    uint32_t i;
    const char *name;
    pgsql_reader_ctx *rd;
    // 1) 双 SELECT：两个结果集，逐个取 reader 验证互不串扰
    pgpack_ctx *p = pgsql_query(pg, "select 1 as a; select 2 as b");
    if (NULL == p || PGPACK_OK != p->type) {
        LOG_ERROR("pgsql multi: double select error.");
        return ERR_FAILED;
    }
    if (2 != pgsql_result_count(p)) {
        LOG_ERROR("pgsql multi: expected 2 results, got %u.", pgsql_result_count(p));
        return ERR_FAILED;
    }
    for (i = 0; i < 2; i++) {
        rd = pgsql_reader_at(p, i, FORMAT_TEXT);
        if (NULL == rd) {
            LOG_ERROR("pgsql multi: result %u reader missing.", i);
            return ERR_FAILED;
        }
        val = pgsql_reader_integer(rd, 0 == i ? "a" : "b", &err);
        pgsql_reader_free(rd);
        if (ERR_OK != err || (int64_t)(i + 1) != val) {
            LOG_ERROR("pgsql multi: result %u expected %u, got %"PRId64".", i, i + 1, val);
            return ERR_FAILED;
        }
    }
    // 同一下标的 reader 只能取走一次。取到即断言失败，但所有权已转过来，得先释放再退
    rd = pgsql_reader_at(p, 0, FORMAT_TEXT);
    if (NULL != rd) {
        LOG_ERROR("pgsql multi: result 0 taken twice.");
        pgsql_reader_free(rd);
        return ERR_FAILED;
    }
    // 2) INSERT + SELECT 混合：无结果集语句占位 NULL，affected 各归各
    p = pgsql_query(pg,
        "insert into srey_test (id, name, score) values (100, 'multi', 1.0);"
        " select name from srey_test where id = 100");
    if (NULL == p || PGPACK_OK != p->type || 2 != pgsql_result_count(p)) {
        LOG_ERROR("pgsql multi: insert+select error.");
        return ERR_FAILED;
    }
    rd = pgsql_reader_at(p, 0, FORMAT_TEXT);
    if (NULL != rd) {
        LOG_ERROR("pgsql multi: insert unexpectedly has a result set.");
        pgsql_reader_free(rd);
        return ERR_FAILED;
    }
    if (1 != pgsql_affected_at(p, 0)) {
        LOG_ERROR("pgsql multi: insert affected expected 1, got %"PRId64".",
                  pgsql_affected_at(p, 0));
        return ERR_FAILED;
    }
    rd = pgsql_reader_at(p, 1, FORMAT_TEXT);
    if (NULL == rd) {
        LOG_ERROR("pgsql multi: select result reader missing.");
        return ERR_FAILED;
    }
    name = pgsql_reader_text(rd, "name", &lens, &err);
    if (ERR_OK != err || 5 != lens || 0 != memcmp(name, "multi", 5)) {
        LOG_ERROR("pgsql multi: expected name='multi'.");
        pgsql_reader_free(rd);
        return ERR_FAILED;
    }
    pgsql_reader_free(rd);
    // 3) 两条写语句：前一条的 affected 不再被后一条覆盖丢失
    p = pgsql_query(pg,
        "insert into srey_test (id, name, score) values (101, 'multi2', 2.0);"
        " update srey_test set score = 9.0 where id in (100, 101)");
    if (NULL == p || PGPACK_OK != p->type || 2 != pgsql_result_count(p)) {
        LOG_ERROR("pgsql multi: insert+update error.");
        return ERR_FAILED;
    }
    if (1 != pgsql_affected_at(p, 0) || 2 != pgsql_affected_at(p, 1)) {
        LOG_ERROR("pgsql multi: affected expected 1/2, got %"PRId64"/%"PRId64".",
                  pgsql_affected_at(p, 0), pgsql_affected_at(p, 1));
        return ERR_FAILED;
    }
    // 单结果 API 保持"最后一条"语义
    if (2 != pgsql_affected_rows(p)) {
        LOG_ERROR("pgsql multi: affected_rows expected 2, got %"PRId64".", pgsql_affected_rows(p));
        return ERR_FAILED;
    }
    // 4) 第二条主键冲突（id=1 已存在）：多语句 simple query 是隐式单事务，
    //    首条已执行的 INSERT 整体回滚，响应为整包 ERR，已提交的结果一并丢弃
    p = pgsql_query(pg,
        "insert into srey_test (id, name, score) values (102, 'gone', 0);"
        " insert into srey_test (id, name, score) values (1, 'dup', 0)");
    if (NULL == p || PGPACK_ERR != p->type) {
        LOG_ERROR("pgsql multi: expected PGPACK_ERR on duplicate key.");
        return ERR_FAILED;
    }
    if (0 != pgsql_result_count(p)) {
        LOG_ERROR("pgsql multi: ERR pack expected 0 results, got %u.", pgsql_result_count(p));
        return ERR_FAILED;
    }
    p = pgsql_query(pg, "select name from srey_test where id = 102");
    if (NULL == p || PGPACK_OK != p->type) {
        LOG_ERROR("pgsql multi: rollback check query error.");
        return ERR_FAILED;
    }
    rd = pgsql_reader_iter(p, FORMAT_TEXT);
    if (NULL == rd) {
        LOG_ERROR("pgsql multi: rollback check reader missing.");
        return ERR_FAILED;
    }
    if (0 != pgsql_reader_size(rd)) {
        LOG_ERROR("pgsql multi: id=102 should have been rolled back.");
        pgsql_reader_free(rd);
        return ERR_FAILED;
    }
    pgsql_reader_free(rd);
    return ERR_OK;
}

// 预处理 + 执行（参数绑定）
static int32_t _prepare_execute(pgsql_ctx *pg) {
    // INT4OID=23, 用于 id 参数（详见 PostgreSQL pg_type catalog）
    uint32_t oids[1] = { 23 };
    if (ERR_OK != pgsql_stmt_prepare(pg, "stmt_select_id",
        "select name from srey_test where id = $1", 1, oids)) {
        LOG_ERROR("pgsql stmt_prepare error.");
        return ERR_FAILED;
    }
    pgsql_bind_ctx bind;
    pgsql_bind_init(&bind, 1);
    pgsql_bind_int32(&bind, 1);
    pgpack_ctx *p = pgsql_stmt_execute(pg, "stmt_select_id", &bind, FORMAT_TEXT);
    pgsql_bind_free(&bind);
    if (NULL == p || PGPACK_OK != p->type) {
        LOG_ERROR("pgsql stmt_execute error.");
        pgsql_stmt_close(pg, "stmt_select_id");
        return ERR_FAILED;
    }
    pgsql_reader_ctx *reader = pgsql_reader_iter(p, FORMAT_TEXT);
    int32_t found = 0;
    if (NULL != reader) {
        int32_t err;
        int32_t lens;
        const char *name = pgsql_reader_text(reader, "name", &lens, &err);
        if (ERR_OK == err && lens == 5 && 0 == memcmp(name, "alice", 5)) {
            found = 1;
        }
        pgsql_reader_free(reader);
    }
    pgsql_stmt_close(pg, "stmt_select_id");
    if (!found) {
        LOG_ERROR("pgsql stmt expected name='alice' for id=1.");
        return ERR_FAILED;
    }
    return ERR_OK;
}

// COPY IN 批量写入（文本格式 TSV，server 默认）
static int32_t _copy_in(pgsql_ctx *pg) {
    const char *data = "10\tcharlie\t60.0\n11\tdiana\t85.5\n12\teric\t95.25\n";
    pgpack_ctx *p = pgsql_copy_in(pg,
        "copy srey_test (id, name, score) from stdin", data, strlen(data));
    if (NULL == p || PGPACK_OK != p->type) {
        LOG_ERROR("pgsql copy_in error.");
        return ERR_FAILED;
    }
    if (3 != pgsql_affected_rows(p)) {
        LOG_ERROR("pgsql copy_in expected 3 rows, got %"PRId64".", pgsql_affected_rows(p));
        return ERR_FAILED;
    }
    return ERR_OK;
}

// COPY OUT 一次性导出
static int32_t _copy_out(pgsql_ctx *pg) {
    pgpack_ctx *p = pgsql_copy_out(pg, "copy srey_test to stdout");
    if (NULL == p || PGPACK_COPY_OUT != p->type) {
        LOG_ERROR("pgsql copy_out error.");
        return ERR_FAILED;
    }
    pgpack_copy_out_ctx *co = (pgpack_copy_out_ctx *)p->pack;
    if (NULL == co || 0 == co->data.offset) {
        LOG_ERROR("pgsql copy_out empty data.");
        return ERR_FAILED;
    }
    return ERR_OK;
}

// 并发：多个协程同时在同一条连接上查询。没有串行化时命令会交错上线——pgsql 一条命令
// 要读到 ReadyForQuery 才算完，交错会让响应对错协程，表现为读回别人的值或整条连接报错。
// 每个协程查一个只属于自己的常量，回读必须原样拿回来
typedef struct {
    int32_t want;   // 本协程期望读回的值
    int32_t got;    // 实际读回
    int32_t done;   // 1 = 已跑完，-1 = 失败
    pgsql_ctx *pg;
} _conc_arg;

static void _conc_worker(task_ctx *task, void *arg) {
    (void)task;
    _conc_arg *a = (_conc_arg *)arg;
    char sql[96];
    // pg_sleep(0) 让服务端把每条查询的响应拉开，放大交错窗口
    SNPRINTF(sql, sizeof(sql), "select %d as v, pg_sleep(0)", a->want);
    int32_t err;
    pgpack_ctx *p;
    pgsql_reader_ctx *rd;
    for (int32_t i = 0; i < 8; i++) {
        a->got = -1;
        p = pgsql_query(a->pg, sql);
        if (NULL == p || PGPACK_OK != p->type) {
            a->done = -1;
            return;
        }
        rd = pgsql_reader_iter(p, FORMAT_TEXT);
        if (NULL == rd) {
            a->done = -1;
            return;
        }
        err = ERR_FAILED;
        if (!pgsql_reader_eof(rd)) {
            a->got = (int32_t)pgsql_reader_integer(rd, "v", &err);
        }
        pgsql_reader_free(rd);
        // err 不看的话读失败时 got 是垃圾值，正好等于 want 就静默过去了
        if (ERR_OK != err || a->got != a->want) {
            a->done = -1;
            return;
        }
    }
    a->done = 1;
}

static int32_t _concurrent_query(pgsql_ctx *pg) {
    _conc_arg args[_CONC_N];
    fork_serial_cb funcs[_CONC_N];
    void *argp[_CONC_N];
    int32_t i;
    for (i = 0; i < _CONC_N; i++) {
        args[i].want = 1000 + i;
        args[i].got = -1;
        args[i].done = 0;
        args[i].pg = pg;
        funcs[i] = _conc_worker;
        argp[i] = &args[i];
    }
    if (ERR_OK != coro_fork_wait(pg->task, _CONC_N, funcs, argp)) {
        LOG_ERROR("pgsql concurrent: fork_wait error.");
        return ERR_FAILED;
    }
    for (i = 0; i < _CONC_N; i++) {
        if (1 != args[i].done) {
            LOG_ERROR("pgsql concurrent: coro %d got %d want %d (commands interleaved).",
                      i, args[i].got, args[i].want);
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}

static void _startup(task_ctx *task) {
    task_pgsql_args *arg = (task_pgsql_args *)coro_get_arg(task);
    if (ERR_OK != pgsql_init(&arg->pg, arg->host, arg->port, NULL,
                             arg->user, arg->password, arg->database)) {
        LOG_ERROR("pgsql_init error.");
        return;
    }
    if (ERR_OK != pgsql_connect(task, &arg->pg)) {
        LOG_ERROR("pgsql connect error.");
        return;
    }
    // 连上了：此后任何失败都是真失败，不能再被 optional 白名单吞成 network error
    *(arg->ok) = -1;
    if (ERR_OK != pgsql_ping(&arg->pg)) {
        LOG_ERROR("pgsql ping error.");
        pgsql_quit(&arg->pg);
        return;
    }
    if (ERR_OK != _setup_table(&arg->pg)) {
        pgsql_quit(&arg->pg);
        return;
    }
    if (ERR_OK != _simple_insert(&arg->pg)) {
        pgsql_quit(&arg->pg);
        return;
    }
    if (ERR_OK != _select_iterate(&arg->pg, 2)) {
        pgsql_quit(&arg->pg);
        return;
    }
    if (ERR_OK != _prepare_execute(&arg->pg)) {
        pgsql_quit(&arg->pg);
        return;
    }
    if (ERR_OK != _copy_in(&arg->pg)) {
        pgsql_quit(&arg->pg);
        return;
    }
    if (ERR_OK != _select_iterate(&arg->pg, 5)) {
        pgsql_quit(&arg->pg);
        return;
    }
    if (ERR_OK != _copy_out(&arg->pg)) {
        pgsql_quit(&arg->pg);
        return;
    }
    if (ERR_OK != _query_syntax_error(&arg->pg)) {
        pgsql_quit(&arg->pg);
        return;
    }
    if (ERR_OK != _multi_statement(&arg->pg)) {
        pgsql_quit(&arg->pg);
        return;
    }
    if (ERR_OK != _concurrent_query(&arg->pg)) {
        pgsql_quit(&arg->pg);
        return;
    }
    pgsql_quit(&arg->pg);
    *(arg->ok) = 1;
    LOG_INFO("pgsql tested.");
}

void task_pgsql_start(loader_ctx *loader, const char *name,
                      const char *host, uint16_t port,
                      const char *user, const char *password, const char *database,
                      int32_t *ok) {
    if (NULL == ok
        || NULL == host || strlen(host) >= 64
        || NULL == user || strlen(user) >= 64
        || NULL == password || strlen(password) >= 64
        || NULL == database || strlen(database) >= 64) {
        return;
    }
    task_pgsql_args *arg;
    CALLOC(arg, 1, sizeof(task_pgsql_args));
    arg->port = port;
    arg->ok = ok;
    safe_fill_str(arg->host, sizeof(arg->host), host);
    safe_fill_str(arg->user, sizeof(arg->user), user);
    safe_fill_str(arg->password, sizeof(arg->password), password);
    safe_fill_str(arg->database, sizeof(arg->database), database);
    coro_task_register(loader, name, 0, _startup, NULL, _free, arg);
}
