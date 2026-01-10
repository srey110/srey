#include "task_pgsql.h"

static int32_t _prt = 1;
static pgsql_ctx _pg;

static int32_t _test_check_read(pgpack_ctx *pgpack, int16_t fromat) {
    int32_t int4val, index = 0;
    char buf[64];
    pgpack_row *val;
    pgpack_field *field;
    pgsql_reader_ctx *reader = pgsql_reader_init(pgpack, fromat);
    while (!pgsql_reader_eof(reader)) {
        val = pgsql_reader_name(reader, "int4_t", &field);
        if (FORMAT_TEXT == fromat) {
            memcpy(buf, val->val, val->lens);
            buf[val->lens] = '\0';
            int4val = (int32_t)strtol(buf, NULL, 10);
        } else {
            int4val = (int32_t)unpack_integer(val->val, val->lens, 0, 0);
        }
        index++;
        if (index != int4val) {
            pgsql_reader_free(reader);
            return ERR_FAILED;
        }
        pgsql_reader_next(reader);
    }
    pgsql_reader_free(reader);
    return ERR_OK;
}
static int32_t _test_query(){
    pgpack_ctx *pgpack;
    pgpack = pgsql_query(&_pg, "delete from table_type1;");
    if (NULL == pgpack || PGPACK_ERR != pgpack->type) {
        LOG_ERROR("pgsql_query error.");
        return ERR_FAILED;
    }
    pgpack = pgsql_query(&_pg, "delete from table_type;");
    if (NULL == pgpack || PGPACK_OK != pgpack->type) {
        LOG_ERROR("pgsql_query error.");
        return ERR_FAILED;
    }
    pgpack = pgsql_query(&_pg, "BEGIN;");
    if (NULL == pgpack || PGPACK_OK != pgpack->type) {
        LOG_ERROR("pgsql_query error.");
        return ERR_FAILED;
    }
    int32_t lastId;
    char buf[64];
    pgpack_row *val;
    pgsql_reader_ctx *reader;
    for (int32_t i = 0; i < 5; i++) {
        char *sql = format_va("insert into table_type(bool_t, float8_t, int4_t) values (true, %f, %d) RETURNING id", (i + 1) + 0.12, i + 1);
        pgpack = pgsql_query(&_pg, sql);
        FREE(sql);
        if (NULL == pgpack || PGPACK_OK != pgpack->type || 1 != pgsql_affected_rows(pgpack)) {
            LOG_ERROR("pgsql_query error.");
            return ERR_FAILED;
        }
        reader = pgsql_reader_init(pgpack, FORMAT_TEXT);
        if (NULL == reader || pgsql_reader_eof(reader)) {
            LOG_ERROR("get last insert id error.");
            return ERR_FAILED;
        }
        val = pgsql_reader_name(reader, "id", NULL);
        memcpy(buf, val->val, val->lens);
        buf[val->lens] = '\0';
        lastId = (int32_t)strtol(buf, NULL, 10);
        pgsql_reader_free(reader);
        if (_prt) {
            printf("pgsql last insert id %d\n", lastId);
        }
    }
    pgpack = pgsql_query(&_pg, "COMMIT;");
    if (NULL == pgpack || PGPACK_OK != pgpack->type) {
        LOG_ERROR("pgsql_query error.");
        return ERR_FAILED;
    }
    pgpack = pgsql_query(&_pg, "select * from table_type;");
    if (NULL == pgpack || PGPACK_OK != pgpack->type || 5 != pgsql_affected_rows(pgpack)) {
        LOG_ERROR("pgsql_query error.");
        return lastId;
    }
    if (ERR_OK != _test_check_read(pgpack, FORMAT_TEXT)) {
        LOG_ERROR("_test_check_read error.");
    }
    return lastId;
}
static void _test_stmt(int32_t lastId) {
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
    int32_t id = ntohl(lastId);
    pgsql_bind(&bind, (char *)&id, 4, FORMAT_BINARY);
    pgpack = pgsql_stmt_execute(&_pg, "stmt_test1", &bind, FORMAT_BINARY);
    if (NULL == pgpack || PGPACK_OK != pgpack->type) {
        LOG_ERROR("pgsql_execute error.");
        pgsql_bind_free(&bind);
        return;
    }
    pgsql_reader_ctx *reader = pgsql_reader_init(pgpack, FORMAT_BINARY);
    if (NULL == reader) {
        LOG_ERROR("pgsql_reader_init error.");
        return;
    }
    pgpack_row *val = pgsql_reader_name(reader, "id", NULL);
    if (NULL == reader) {
        LOG_ERROR("pgsql_reader_read error.");
        return;
    } 
    if (lastId != unpack_integer(val->val, val->lens, 0, 0)) {
        LOG_ERROR("pgsql_reader_read id error.");
        return;
    }
    pgsql_reader_free(reader);

    pgsql_stmt_close(&_pg, "stmt_test1");
    if (ERR_OK != pgsql_stmt_prepare(&_pg, "stmt_test1", "delete from table_type where id=$1;", 1, ids)) {
        LOG_ERROR("pgsql_prepare error.");
        pgsql_bind_free(&bind);
        return;
    }
    pgpack = pgsql_stmt_execute(&_pg, "stmt_test1", &bind, FORMAT_BINARY);
    if (NULL == pgpack || PGPACK_OK != pgpack->type) {
        LOG_ERROR("pgsql_execute error.");
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
    int32_t lastId = _test_query();
    if (ERR_FAILED != lastId) {
        _test_stmt(lastId);
    }
    pgsql_quit(&_pg);
    LOG_INFO("pgsql tested.");
}
static void _closing_cb(task_ctx *task) {
}
void task_pgsql_start(loader_ctx *loader, name_t name, int32_t pt) {
    _prt = pt;
    coro_task_register(loader, name, _startup, _closing_cb);
}
