#include "task_pgsql.h"

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
        LOG_ERROR("pgsql expected 2 rows inserted, got %d.", pgsql_affected_rows(p));
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
    pgsql_reader_ctx *reader = pgsql_reader_init(p, FORMAT_TEXT);
    if (NULL == reader) {
        LOG_ERROR("pgsql reader_init error.");
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
    pgsql_reader_ctx *reader = pgsql_reader_init(p, FORMAT_TEXT);
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
        LOG_ERROR("pgsql copy_in expected 3 rows, got %d.", pgsql_affected_rows(p));
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
