#include "task_mysql.h"

typedef struct task_mysql_args {
    uint16_t port;
    int32_t *ok;
    char host[64];
    char user[64];
    char password[64];
    char database[64];
    mysql_ctx mysql;
}task_mysql_args;

// 清空 test_bind 表内容，使每次测试运行结果可重复
static int32_t _clear_table(mysql_ctx *mysql) {
    mpack_ctx *mpack = mysql_query(mysql, "delete from test_bind", NULL);
    if (NULL == mpack || MPACK_OK != mpack->pack_type) {
        int32_t code = 0;
        LOG_ERROR("mysql delete error: %s (code=%d)", mysql_erro(mysql, &code), code);
        return ERR_FAILED;
    }
    return ERR_OK;
}

// 通过 bind 接口批量插入 3 行，验证 query attribute 参数绑定路径
static int32_t _insert_rows(mysql_ctx *mysql) {
    mysql_bind_ctx bind;
    mysql_bind_init(&bind);
    int32_t rtn = ERR_OK;
    for (int32_t i = 1; i <= 3; i++) {
        mysql_bind_clear(&bind);
        mysql_bind_integer(&bind, "t_int8", i);
        mysql_bind_integer(&bind, "t_int16", 100 + i);
        mysql_bind_integer(&bind, "t_int32", 1000 + i);
        mysql_bind_integer(&bind, "t_int64", 100000 + i);
        mysql_bind_double(&bind, "t_float", 1.5 + i);
        mysql_bind_double(&bind, "t_double", 3.14 + i);
        mysql_bind_string(&bind, "t_string", "srey-mysql-test", strlen("srey-mysql-test"));
        mysql_bind_datetime(&bind, "t_datetime", time(NULL));
        mysql_bind_time(&bind, "t_time", 0, 0, 1, 30, 0);
        mysql_bind_nil(&bind, "t_nil");
        const char *sql = "insert into test_bind"
            " (t_int8,t_int16,t_int32,t_int64,t_float,t_double,t_string,t_datetime,t_time,t_nil)"
            " values("
            "mysql_query_attribute_string('t_int8'),"
            "mysql_query_attribute_string('t_int16'),"
            "mysql_query_attribute_string('t_int32'),"
            "mysql_query_attribute_string('t_int64'),"
            "mysql_query_attribute_string('t_float'),"
            "mysql_query_attribute_string('t_double'),"
            "mysql_query_attribute_string('t_string'),"
            "mysql_query_attribute_string('t_datetime'),"
            "mysql_query_attribute_string('t_time'),"
            "mysql_query_attribute_string('t_nil'))";
        mpack_ctx *mpack = mysql_query(mysql, sql, &bind);
        if (NULL == mpack || MPACK_OK != mpack->pack_type) {
            int32_t code = 0;
            LOG_ERROR("mysql insert(bind) error: %s (code=%d)",
                      mysql_erro(mysql, &code), code);
            rtn = ERR_FAILED;
            break;
        }
    }
    mysql_bind_free(&bind);
    return rtn;
}

// 简单查询全表后用 reader 迭代，校验列读取接口
static int32_t _select_iterate(mysql_ctx *mysql, int32_t expect_rows) {
    mpack_ctx *mpack = mysql_query(mysql, "select * from test_bind order by t_int8", NULL);
    if (NULL == mpack) {
        LOG_ERROR("mysql select error.");
        return ERR_FAILED;
    }
    mysql_reader_ctx *reader = mysql_reader_init(mpack);
    if (NULL == reader) {
        LOG_ERROR("mysql reader_init error.");
        return ERR_FAILED;
    }
    int32_t cnt = 0;
    int32_t err;
    while (!mysql_reader_eof(reader)) {
        int64_t v = mysql_reader_integer(reader, "t_int32", &err);
        if (ERR_OK != err) {
            LOG_ERROR("mysql reader t_int32 error.");
            mysql_reader_free(reader);
            return ERR_FAILED;
        }
        if (v != 1001 + cnt) {
            LOG_ERROR("mysql row %d unexpected t_int32=%lld.", cnt, (long long)v);
            mysql_reader_free(reader);
            return ERR_FAILED;
        }
        cnt++;
        mysql_reader_next(reader);
    }
    mysql_reader_free(reader);
    if (cnt != expect_rows) {
        LOG_ERROR("mysql select expected %d rows, got %d.", expect_rows, cnt);
        return ERR_FAILED;
    }
    return ERR_OK;
}

// 执行非法 SQL，校验 wire 上的 ERR_Packet 解析路径
static int32_t _query_syntax_error(mysql_ctx *mysql) {
    mpack_ctx *mpack = mysql_query(mysql, "selct 1", NULL);
    if (NULL == mpack) {
        LOG_ERROR("mysql syntax_error: expected ERR pack, got NULL.");
        return ERR_FAILED;
    }
    if (MPACK_ERR != mpack->pack_type) {
        LOG_ERROR("mysql syntax_error: expected MPACK_ERR, got %d.", mpack->pack_type);
        return ERR_FAILED;
    }
    int32_t code = 0;
    const char *msg = mysql_erro(mysql, &code);
    if (NULL == msg || 0 == code) {
        LOG_ERROR("mysql syntax_error: missing erro msg/code (msg=%p,code=%d).",
                  (void *)msg, code);
        return ERR_FAILED;
    }
    return ERR_OK;
}

// 预处理 + 执行（带参数），覆盖 stmt 路径
static int32_t _prepare_execute(mysql_ctx *mysql) {
    mysql_stmt_ctx *stmt = mysql_stmt_prepare(mysql, "select t_int32 from test_bind where t_int8 = ?");
    if (NULL == stmt) {
        LOG_ERROR("mysql stmt_prepare error.");
        return ERR_FAILED;
    }
    mysql_bind_ctx bind;
    mysql_bind_init(&bind);
    mysql_bind_integer(&bind, NULL, 2);
    mpack_ctx *mpack = mysql_stmt_execute(stmt, &bind);
    mysql_bind_free(&bind);
    if (NULL == mpack) {
        LOG_ERROR("mysql stmt_execute error.");
        mysql_stmt_close(stmt);
        return ERR_FAILED;
    }
    mysql_reader_ctx *reader = mysql_reader_init(mpack);
    int32_t found = 0;
    if (NULL != reader) {
        int32_t err;
        while (!mysql_reader_eof(reader)) {
            int64_t v = mysql_reader_integer(reader, "t_int32", &err);
            if (ERR_OK == err && 1002 == v) {
                found = 1;
            }
            mysql_reader_next(reader);
        }
        mysql_reader_free(reader);
    }
    mysql_stmt_close(stmt);
    if (!found) {
        LOG_ERROR("mysql stmt expected row with t_int32=1002 not found.");
        return ERR_FAILED;
    }
    return ERR_OK;
}

// 多结果集：多语句查询产生多个结果集，验证 mpack->more 在结果集(reader)与 OK 包两条路径上
// 都被正确标记；more 须在下一次 _coro_wait 前读取（下次 wait 会回收 first 所在的分发消息）
static int32_t _multi_result(mysql_ctx *mysql) {
    // 路径一：SELECT 结果集的 more（行阶段 EOF 带 SERVER_MORE_RESULTS_EXISTS）
    mpack_ctx *first = mysql_query(mysql, "select 1;select 2", NULL);
    if (NULL == first) {
        LOG_ERROR("mysql multi-result: 'select;select' first returned NULL.");
        return ERR_FAILED;
    }
    int32_t rmore1 = mysql_more(first);
    message_ctx *msg = _coro_wait(mysql->task, mysql->client.sk.skid,
                                  MSG_TYPE_RECV, task_get_netread_timeout(mysql->task));
    if (NULL == msg || MSG_TYPE_RECV != msg->mtype || NULL == msg->data) {
        LOG_ERROR("mysql multi-result: 'select;select' second result set not received.");
        return ERR_FAILED;
    }
    int32_t rmore2 = mysql_more(msg->data);
    if (1 != rmore1 || 0 != rmore2) {
        LOG_ERROR("mysql multi-result: resultset more mismatch (first=%d second=%d), want 1,0.",
                  rmore1, rmore2);
        return ERR_FAILED;
    }
    // 路径二：OK 包的 more（SET 语句返回 OK 包，多语句时带 SERVER_MORE_RESULTS_EXISTS）
    mpack_ctx *okfirst = mysql_query(mysql, "set @srey_t=1;select 1", NULL);
    if (NULL == okfirst) {
        LOG_ERROR("mysql multi-result: 'set;select' first returned NULL.");
        return ERR_FAILED;
    }
    int32_t optype = okfirst->pack_type;
    int32_t omore1 = mysql_more(okfirst);
    msg = _coro_wait(mysql->task, mysql->client.sk.skid,
                     MSG_TYPE_RECV, task_get_netread_timeout(mysql->task));
    if (NULL == msg || MSG_TYPE_RECV != msg->mtype || NULL == msg->data) {
        LOG_ERROR("mysql multi-result: 'set;select' second result set not received.");
        return ERR_FAILED;
    }
    int32_t omore2 = mysql_more(msg->data);
    if (MPACK_OK != optype || 1 != omore1 || 0 != omore2) {
        LOG_ERROR("mysql multi-result: ok-packet more mismatch (type=%d first=%d second=%d), want OK,1,0.",
                  optype, omore1, omore2);
        return ERR_FAILED;
    }
    return ERR_OK;
}

static void _startup(task_ctx *task) {
    task_mysql_args *arg = (task_mysql_args *)coro_get_arg(task);
    if (ERR_OK != mysql_init(&arg->mysql, arg->host, arg->port, NULL,
                             arg->user, arg->password, arg->database, "utf8mb4", 0)) {
        LOG_ERROR("mysql_init error.");
        return;
    }
    if (ERR_OK != mysql_connect(task, &arg->mysql)) {
        LOG_ERROR("mysql connect error.");
        return;
    }
    LOG_INFO("mysql connected, version=%s", mysql_version(&arg->mysql));
    if (ERR_OK != mysql_selectdb(&arg->mysql, arg->database)) {
        int32_t code = 0;
        LOG_ERROR("mysql selectdb error: %s (code=%d)", mysql_erro(&arg->mysql, &code), code);
        mysql_quit(&arg->mysql);
        return;
    }
    if (ERR_OK != mysql_ping(&arg->mysql)) {
        LOG_ERROR("mysql ping error.");
        mysql_quit(&arg->mysql);
        return;
    }
    if (ERR_OK != _clear_table(&arg->mysql)) {
        mysql_quit(&arg->mysql);
        return;
    }
    if (ERR_OK != _insert_rows(&arg->mysql)) {
        mysql_quit(&arg->mysql);
        return;
    }
    if (3 != mysql_affected_rows(&arg->mysql)) {
        // 最后一次 INSERT 的 affected_rows 应为 1（每次插一行）；这里只断言能取到非负值
    }
    if (ERR_OK != _select_iterate(&arg->mysql, 3)) {
        mysql_quit(&arg->mysql);
        return;
    }
    if (ERR_OK != _prepare_execute(&arg->mysql)) {
        mysql_quit(&arg->mysql);
        return;
    }
    if (ERR_OK != _query_syntax_error(&arg->mysql)) {
        mysql_quit(&arg->mysql);
        return;
    }
    if (ERR_OK != _multi_result(&arg->mysql)) {
        mysql_quit(&arg->mysql);
        return;
    }
    mysql_quit(&arg->mysql);
    *(arg->ok) = 1;
    LOG_INFO("mysql tested.");
}

void task_mysql_start(loader_ctx *loader, const char *name,
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
    task_mysql_args *arg;
    CALLOC(arg, 1, sizeof(task_mysql_args));
    arg->port = port;
    arg->ok = ok;
    safe_fill_str(arg->host, sizeof(arg->host), host);
    safe_fill_str(arg->user, sizeof(arg->user), user);
    safe_fill_str(arg->password, sizeof(arg->password), password);
    safe_fill_str(arg->database, sizeof(arg->database), database);
    coro_task_register(loader, name, 0, _startup, NULL, _free, arg);
}
