#include "task_pgsql.h"

static int32_t _prt = 1;
static pgsql_ctx _pg;

static int32_t _test_check_read(pgpack_ctx *pgpack, int16_t fromat) {
    int32_t id, index = 0;
    char buf[64];
    pgpack_row *val;
    pgpack_field *field;
    pgsql_reader_ctx *reader = pgsql_reader_init(pgpack, fromat);
    while (!pgsql_reader_eof(reader)) {
        val = pgsql_reader_read(reader, "id", &field);
        if (FORMAT_TEXT == fromat) {
            memcpy(buf, val->val, val->lens);
            buf[val->lens] = '\0';
            id = (int32_t)strtol(buf, NULL, 10);
        } else {
            id = (int32_t)unpack_integer(val->val, val->lens, 0, 0);
        }
        index++;
        if (index != id) {
            pgsql_reader_free(reader);
            return ERR_FAILED;
        }
        pgsql_reader_next(reader);
    }
    pgsql_reader_free(reader);
    return ERR_OK;
}
static void _test_query(){
    pgpack_ctx *pgpack;
    pgpack = pgsql_query(&_pg, "delete from table_type1;");
    if (NULL == pgpack && PGPACK_ERR != pgpack->type) {
        LOG_ERROR("pgsql_query error.");
        return;
    }
    pgpack = pgsql_query(&_pg, "delete from table_type;");
    if (NULL == pgpack && PGPACK_OK != pgpack->type) {
        LOG_ERROR("pgsql_query error.");
        return;
    }
    pgpack = pgsql_query(&_pg, "BEGIN;");
    if (NULL == pgpack && PGPACK_OK != pgpack->type) {
        LOG_ERROR("pgsql_query error.");
        return;
    }
    for (int32_t i = 0; i < 5; i++) {
        char *sql = format_va("insert into table_type(id, bool_t, float8_t, int4_t) values (%d, true, %f, %d)", i + 1, (i + 1) + 0.12, i + 1);
        pgpack = pgsql_query(&_pg, sql);
        FREE(sql);
        if (NULL == pgpack && PGPACK_OK != pgpack->type) {
            LOG_ERROR("pgsql_query error.");
            return;
        }
    }
    pgpack = pgsql_query(&_pg, "COMMIT;");
    if (NULL == pgpack && PGPACK_OK != pgpack->type) {
        LOG_ERROR("pgsql_query error.");
        return;
    }
    pgpack = pgsql_query(&_pg, "select * from table_type;");
    if (NULL == pgpack && PGPACK_OK != pgpack->type) {
        LOG_ERROR("pgsql_query error.");
        return;
    }
    if (ERR_OK != _test_check_read(pgpack, FORMAT_TEXT)) {
        LOG_ERROR("_test_check_read error.");
    }
}
static void _test_stmt() {
    pgpack_ctx *pgpack;
    if (ERR_OK != pgsql_stmt_prepare(&_pg, "", "select * from table_type;", 0, NULL)) {
        LOG_ERROR("pgsql_prepare error.");
        return;
    }
    pgpack = pgsql_stmt_execute(&_pg, "", NULL, FORMAT_TEXT);
    if (NULL == pgpack || PGPACK_OK != pgpack->type) {
        LOG_ERROR("pgsql_execute error.");
        return;
    }
    if (ERR_OK != _test_check_read(pgpack, FORMAT_TEXT)) {
        LOG_ERROR("_test_check_read error.");
        return;
    }
    pgsql_bind_ctx bind;
    pgsql_bind_init(&bind, 1);
    uint32_t ids[1];
    ids[0] = INT4OID;
    if (ERR_OK != pgsql_stmt_prepare(&_pg, "stmt_test1", "select * from table_type where id=$1;", 1, ids)) {
        LOG_ERROR("pgsql_prepare error.");
        pgsql_bind_free(&bind);
        return;
    }
    int32_t id = ntohl(1);
    pgsql_bind(&bind, 0, (char *)&id, 4, FORMAT_BINARY);
    pgpack = pgsql_stmt_execute(&_pg, "stmt_test1", &bind, FORMAT_BINARY);
    if (NULL == pgpack || PGPACK_OK != pgpack->type) {
        LOG_ERROR("pgsql_execute error.");
        pgsql_bind_free(&bind);
        return;
    }
    if (ERR_OK != _test_check_read(pgpack, FORMAT_BINARY)) {
        LOG_ERROR("_test_check_read error.");
        return;
    }
    pgsql_stmt_close(&_pg, "stmt_test1");
    if (ERR_OK != pgsql_stmt_prepare(&_pg, "stmt_test1", "select * from table_type where id=$1;", 1, ids)) {
        LOG_ERROR("pgsql_prepare error.");
        pgsql_bind_free(&bind);
        return;
    }
    pgsql_stmt_close(&_pg, "stmt_test1");
    pgsql_bind_free(&bind);
}
static void _startup(task_ctx *task) {
    struct evssl_ctx *evssl = NULL;
#if WITH_SSL
    evssl = evssl_qury(102);
#endif
    pgsql_init(&_pg, "127.0.0.1", 0, evssl, "postgres", "12345678", "postgres");
    if (ERR_OK != pgsql_connect(task, &_pg)) {
        LOG_ERROR("pgsql_connect error.");
        return;
    }
    pgsql_ping(&_pg);
    pgsql_quit(&_pg);
    pgsql_ping(&_pg);
    pgsql_selectdb(&_pg, "test2");
    _test_query();
    _test_stmt();
    pgsql_quit(&_pg);
    LOG_INFO("pgsql tested.");
}
static void _closing_cb(task_ctx *task) {
}
void task_pgsql_start(loader_ctx *loader, name_t name, int32_t pt) {
    _prt = pt;
    task_ctx *task = task_new(loader, name, NULL, NULL, NULL);
    task_register(task, _startup, _closing_cb);
}
