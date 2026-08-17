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

// query / stmt_execute 改回调式后的三个共用回调
// 只认 OK 包（INSERT/UPDATE/DELETE/USE 这类）
static int32_t _cb_expect_ok(mpack_ctx *mpack, void *udata) {
    (void)udata;
    return MPACK_OK == mpack->pack_type ? ERR_OK : ERR_FAILED;
}
// 取走结果集：mysql_reader_init 是所有权转移，拿到手后不受"下次挂起即失效"的限制。
// 只接一个结果集，被回调第二次说明用例挑错了 SQL——直接失败，别把前一个 reader 覆盖漏掉
static int32_t _cb_take_reader(mpack_ctx *mpack, void *udata) {
    mysql_reader_ctx **out = (mysql_reader_ctx **)udata;
    if (NULL != *out) {
        LOG_ERROR("_cb_take_reader: more than one result set.");
        return ERR_FAILED;
    }
    *out = mysql_reader_init(mpack);
    return NULL != *out ? ERR_OK : ERR_FAILED;
}
// 逐个记下 pack_type 与 more，供多结果集 / ERR 包用例校验
typedef struct {
    int32_t n;
    int32_t types[4];
    int32_t mores[4];
} _mres_rec;
static int32_t _cb_record(mpack_ctx *mpack, void *udata) {
    _mres_rec *r = (_mres_rec *)udata;
    if (r->n < (int32_t)(sizeof(r->types) / sizeof(r->types[0]))) {
        r->types[r->n] = (int32_t)mpack->pack_type;
        r->mores[r->n] = mysql_more(mpack);
        r->n++;
    }
    return ERR_OK;
}
// 清空 test_bind 表内容，使每次测试运行结果可重复
static int32_t _clear_table(mysql_ctx *mysql) {
    if (ERR_OK != mysql_query(mysql, "delete from test_bind", NULL, _cb_expect_ok, NULL)) {
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
        if (ERR_OK != mysql_query(mysql, sql, &bind, _cb_expect_ok, NULL)) {
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
    mysql_reader_ctx *reader = NULL;
    if (ERR_OK != mysql_query(mysql, "select * from test_bind order by t_int8", NULL,
                              _cb_take_reader, &reader)) {
        LOG_ERROR("mysql select error.");
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
    _mres_rec rec = { 0, { 0 }, { 0 } };
    // 回调恒返 ERR_OK，故这里的失败只可能来自组包 / 网络，ERR 包本身算"读到了"
    if (ERR_OK != mysql_query(mysql, "selct 1", NULL, _cb_record, &rec)) {
        LOG_ERROR("mysql syntax_error: query failed before reading ERR pack.");
        return ERR_FAILED;
    }
    if (1 != rec.n || MPACK_ERR != rec.types[0]) {
        LOG_ERROR("mysql syntax_error: expected 1 ERR pack, got n=%d type=%d.", rec.n, rec.types[0]);
        return ERR_FAILED;
    }
    int32_t code = 0;
    const char *msg = mysql_erro(mysql, &code);
    if (NULL == msg || 0 == code) {
        LOG_ERROR("mysql syntax_error: missing erro msg/code (msg=%p,code=%d).",
                  (void *)msg, code);
        return ERR_FAILED;
    }
    // cb 为 NULL 时由库代判 ERR 应答：同一条非法 SQL 必须报失败，否则主键冲突这类会被静默当成功
    if (ERR_OK == mysql_query(mysql, "selct 1", NULL, NULL, NULL)) {
        LOG_ERROR("mysql syntax_error: NULL cb must fail on ERR pack.");
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
    // stmt_id 是服务端按连接分配的,关闭时要靠 skid 判断"中途有没有重连过"。
    // 分配点漏填的话这里恒为 0,重连后 mysql_stmt_close 就会把新连接上同 id 的语句关掉
    if (0 == stmt->skid
        || stmt->skid != mysql->client.sk.skid) {
        LOG_ERROR("mysql stmt skid not snapshotted at prepare: %"PRIu64" vs %"PRIu64,
                  stmt->skid, mysql->client.sk.skid);
        mysql_stmt_close(stmt);
        return ERR_FAILED;
    }
    mysql_bind_ctx bind;
    mysql_bind_init(&bind);
    mysql_bind_integer(&bind, NULL, 2);
    mysql_reader_ctx *reader = NULL;
    int32_t exrtn = mysql_stmt_execute(stmt, &bind, _cb_take_reader, &reader);
    mysql_bind_free(&bind);
    if (ERR_OK != exrtn) {
        LOG_ERROR("mysql stmt_execute error.");
        mysql_stmt_close(stmt);
        return ERR_FAILED;
    }
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

// 多结果集：多语句查询产生多个结果集，验证库内部把它们全部读完并逐个回调，
// 且 more 在结果集(reader)与 OK 包两条路径上都被正确标记。
// 续读已收进 mysql_query，用例不再自己 _coro_wait——那样会跟库抢同一条连接上的包
static int32_t _multi_result(mysql_ctx *mysql) {
    // 路径一：SELECT 结果集的 more（行阶段 EOF 带 SERVER_MORE_RESULTS_EXISTS）
    _mres_rec r1 = { 0, { 0 }, { 0 } };
    if (ERR_OK != mysql_query(mysql, "select 1;select 2", NULL, _cb_record, &r1)) {
        LOG_ERROR("mysql multi-result: 'select;select' failed.");
        return ERR_FAILED;
    }
    if (2 != r1.n || 1 != r1.mores[0] || 0 != r1.mores[1]) {
        LOG_ERROR("mysql multi-result: resultset mismatch (n=%d more=%d,%d), want 2,1,0.",
                  r1.n, r1.mores[0], r1.mores[1]);
        return ERR_FAILED;
    }
    // 路径二：OK 包的 more（SET 语句返回 OK 包，多语句时带 SERVER_MORE_RESULTS_EXISTS）
    _mres_rec r2 = { 0, { 0 }, { 0 } };
    if (ERR_OK != mysql_query(mysql, "set @srey_t=1;select 1", NULL, _cb_record, &r2)) {
        LOG_ERROR("mysql multi-result: 'set;select' failed.");
        return ERR_FAILED;
    }
    if (2 != r2.n || MPACK_OK != r2.types[0] || 1 != r2.mores[0] || 0 != r2.mores[1]) {
        LOG_ERROR("mysql multi-result: ok-packet mismatch (n=%d type=%d more=%d,%d), want 2,OK,1,0.",
                  r2.n, r2.types[0], r2.mores[0], r2.mores[1]);
        return ERR_FAILED;
    }
    return ERR_OK;
}

// 会话跟踪：query("USE x") 改的是服务端当前库，client.database 须由 OK 包尾部的
// session-state-change 跟上，否则重连按旧库名握手会静默落到原库
static int32_t _session_track(mysql_ctx *mysql, const char *back) {
    int32_t code = 0;
    if (ERR_OK != mysql_query(mysql, "USE information_schema", NULL, _cb_expect_ok, NULL)) {
        LOG_ERROR("mysql USE information_schema error: %s (code=%d)", mysql_erro(mysql, &code), code);
        return ERR_FAILED;
    }
    if (0 != strcmp(mysql->client.database, "information_schema")) {
        LOG_ERROR("mysql session track failed, client.database=%s want information_schema.",
                  mysql->client.database);
        return ERR_FAILED;
    }
    char sql[64];
    SNPRINTF(sql, sizeof(sql), "USE %s", back);
    if (ERR_OK != mysql_query(mysql, sql, NULL, _cb_expect_ok, NULL)) {
        LOG_ERROR("mysql USE %s error: %s (code=%d)", back, mysql_erro(mysql, &code), code);
        return ERR_FAILED;
    }
    if (0 != strcmp(mysql->client.database, back)) {
        LOG_ERROR("mysql session track failed, client.database=%s want %s.", mysql->client.database, back);
        return ERR_FAILED;
    }
    return ERR_OK;
}
// 并发：多个协程同时在同一条连接上查询。没有串行化时它们的命令会交错上线——
// MySQL 半双工不允许，而 mysql_ctx 的解析状态（id / parse_status / cur_cmd / mpack）
// 又是每连接一份，交错即互相覆盖，表现为串号、少行、乃至解析崩掉。
// 每个协程查一个只属于自己的常量，回读必须原样拿回来
typedef struct {
    int32_t want;      // 本协程期望读回的值
    int32_t got;       // 实际读回
    int32_t done;      // 1 = 已跑完
    mysql_ctx *mysql;
} _conc_arg;
static int32_t _cb_conc(mpack_ctx *mpack, void *udata) {
    _conc_arg *a = (_conc_arg *)udata;
    mysql_reader_ctx *rd = mysql_reader_init(mpack);
    if (NULL == rd) {
        return ERR_FAILED;
    }
    int32_t err;
    if (!mysql_reader_eof(rd)) {
        a->got = (int32_t)mysql_reader_integer(rd, "v", &err);
    }
    mysql_reader_free(rd);
    return ERR_OK;
}
static void _conc_worker(task_ctx *task, void *arg) {
    (void)task;
    _conc_arg *a = (_conc_arg *)arg;
    char sql[64];
    // sleep(0) 让服务端把每条查询的响应拉开，放大交错窗口
    SNPRINTF(sql, sizeof(sql), "select %d as v from (select sleep(0)) t", a->want);
    for (int32_t i = 0; i < 8; i++) {
        a->got = -1;
        if (ERR_OK != mysql_query(a->mysql, sql, NULL, _cb_conc, a)
            || a->got != a->want) {
            a->done = -1;
            return;
        }
    }
    a->done = 1;
}
#define _CONC_N 4
static int32_t _concurrent_query(mysql_ctx *mysql) {
    _conc_arg args[_CONC_N];
    fork_serial_cb funcs[_CONC_N];
    void *argp[_CONC_N];
    int32_t i;
    for (i = 0; i < _CONC_N; i++) {
        args[i].want = 1000 + i;
        args[i].got = -1;
        args[i].done = 0;
        args[i].mysql = mysql;
        funcs[i] = _conc_worker;
        argp[i] = &args[i];
    }
    if (ERR_OK != coro_fork_wait(mysql->task, _CONC_N, funcs, argp)) {
        LOG_ERROR("mysql concurrent: fork_wait error.");
        return ERR_FAILED;
    }
    for (i = 0; i < _CONC_N; i++) {
        if (1 != args[i].done) {
            LOG_ERROR("mysql concurrent: coro %d got %d want %d (commands interleaved).",
                      i, args[i].got, args[i].want);
            return ERR_FAILED;
        }
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
    if (ERR_OK != _session_track(&arg->mysql, arg->database)) {
        mysql_quit(&arg->mysql);
        return;
    }
    if (ERR_OK != _concurrent_query(&arg->mysql)) {
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
