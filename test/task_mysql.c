#include "task_mysql.h"

#if WITH_CORO

static int32_t _prt = 0;
static mysql_ctx _mysql;
static mysql_bind_ctx _bind;

static void _net_connect(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, int32_t erro) {
    if (_prt) {
        LOG_INFO("mysql connect: %d", erro);
    }
}
static void _net_close(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client) {
    if (_prt) {
        LOG_INFO("mysql connection closed.");
    }
}
static void _show_print(mysql_reader_ctx *reader) {
    int32_t err;
    int8_t i8;
    int16_t i16;
    int32_t i32;
    int64_t i64;
    uint64_t ui64;
    float f;
    double d;
    char str[ONEK];
    size_t lens;
    i64 = mysql_reader_integer(reader, "id", &err);
    if (1 == err) {
        printf("nil   ");
    } else {
        printf("%d   ", (uint32_t)i64);
    }
    i8 = (int8_t)mysql_reader_integer(reader, "t_int8", &err);
    if (1 == err) {
        printf("nil   ");
    } else {
        printf("%d   ", (uint8_t)i8);
    }
    i16 = (int16_t)mysql_reader_integer(reader, "t_int16", &err);
    if (1 == err) {
        printf("nil   ");
    } else {
        printf("%d   ", (uint8_t)i16);
    }
    i32 = (int32_t)mysql_reader_integer(reader, "t_int32", &err);
    if (1 == err) {
        printf("nil   ");
    }
    else {
        printf("%d   ", i32);
    }
    ui64 = mysql_reader_uinteger(reader, "t_int64", &err);
    if (1 == err) {
        printf("nil   ");
    } else {
        printf("%d   ", (uint32_t)ui64);
    }
    f = (float)mysql_reader_double(reader, "t_float", &err);
    if (1 == err) {
        printf("nil   ");
    } else {
        printf("%f   ", f);
    }
    d = mysql_reader_double(reader, "t_double", &err);
    if (1 == err) {
        printf("nil   ");
    } else {
        printf("%f   ", d);
    }
    char *val = mysql_reader_string(reader, "t_string", &lens, &err);
    if (1 == err) {
        printf("nil   ");
    } else {
        memcpy(str, val, lens);
        str[lens] = '\0';
        printf("%s   ", str);
    }
    ui64 = mysql_reader_datetime(reader, "t_datetime", &err);
    if (1 == err) {
        printf("nil   ");
    } else {
        printf("%d   ", (uint32_t)ui64);
    }
    struct tm dt;
    mysql_reader_time(reader, "t_time", &dt, &err);
    if (1 == err) {
        printf("nil   ");
    } else {
        printf("%d   ", (uint32_t)ui64);
    }
}
static void _show_result1(mpack_ctx *pack, int32_t eof) {
    if (!_prt) {
        return;
    }
    mysql_reader_ctx *reader = mysql_reader_init(pack);
    if (NULL == reader) {
        return;
    }
    printf("--------------------------------------------------------------------\n");
    if (eof) {
        while (!mysql_reader_eof(reader)) {
            _show_print(reader);
            printf("\n");
            mysql_reader_next(reader);
        }
    } else {
        size_t count = mysql_reader_size(reader);
        for (size_t i = 0; i < count; i++) {
            mysql_reader_seek(reader, i);
            _show_print(reader);
            printf("\n");
        }
    }
    mysql_reader_free(reader);
}
static void _test_bind(task_ctx *task) {
    for (int i = 0; i < 10; i++) {
        mysql_bind_integer(&_bind, "t_int8", i + 1);
        mysql_bind_integer(&_bind, "t_int16", i + 2);
        mysql_bind_integer(&_bind, "t_int32", i + 3);
        mysql_bind_integer(&_bind, "t_int64", i + 4);
        mysql_bind_double(&_bind, "t_float", (float)(i + 5) + 0.123456f);
        mysql_bind_double(&_bind, "t_double", (double)(i + 5) + 0.789);
        mysql_bind_string(&_bind, "t_string", "this is test.", strlen("this is test."));
        mysql_bind_datetime(&_bind, "t_datetime", time(NULL) + i + 6);
        mysql_bind_time(&_bind, "t_time", 1, 2, 15, 30 + i, 29);
        mysql_bind_nil(&_bind, "t_nil");
        const char *sql1 = "insert into test_bind (t_int8,t_int16,t_int32,t_int64,t_float,t_double, t_string,t_datetime,t_time,t_nil) \
         values(mysql_query_attribute_string('t_int8'),\
         mysql_query_attribute_string('t_int16'),\
         mysql_query_attribute_string('t_int32'),\
         mysql_query_attribute_string('t_int64'),\
         mysql_query_attribute_string('t_float'),\
         mysql_query_attribute_string('t_double'),\
         mysql_query_attribute_string('t_string'),\
         mysql_query_attribute_string('t_datetime'),\
         mysql_query_attribute_string('t_time'),\
         mysql_query_attribute_string('t_nil')\
        )";
        mpack_ctx *pack = mysql_query(task, &_mysql, sql1, &_bind);
        if (NULL == pack || MPACK_OK != pack->pack_type) {
            LOG_WARN("mysql_query error.");
        }
        mysql_bind_clear(&_bind);
    }
    mpack_ctx *pack = mysql_query(task, &_mysql,
        "insert into test1_bind (t_int8,t_int16,t_int32,t_int64,t_float,t_double, t_string,t_datetime,t_time) values (...)", NULL);
    if (MPACK_ERR != pack->pack_type) {
        LOG_WARN("mysql_query error.");
    } else {
        if (_prt) {
            LOG_WARN("%s", mysql_erro(&_mysql, NULL));
        }
    }
    pack = mysql_query(task, &_mysql, "select * from test_bind", NULL);
    _show_result1(pack, 1);
}
static void _test_stmt(task_ctx *task) {
    const char *sql1 = "insert into test_bind (t_int8,t_int16,t_int32,t_int64,t_float,t_double,t_string,t_datetime,t_time,t_nil) \
         values(?,?,?,?,?,?,?,?,?,?)";
    uint64_t curts = nowsec();
    mysql_stmt_ctx *stmt = mysql_stmt_prepare(task, &_mysql, sql1);
    mysql_bind_integer(&_bind, NULL, 125);
    mysql_bind_integer(&_bind, NULL, 2024);
    mysql_bind_integer(&_bind, NULL, 127);
    mysql_bind_integer(&_bind, NULL, curts);
    mysql_bind_double(&_bind, NULL, 128.0123);
    mysql_bind_double(&_bind, NULL, 129.0123);
    mysql_bind_string(&_bind, NULL, "this is text.", strlen("this is text."));
    mysql_bind_datetime(&_bind, "t_datetime", (time_t)curts);
    mysql_bind_time(&_bind, "t_time", 0, 2, 15, 30, 29);
    mysql_bind_nil(&_bind, NULL);
    mpack_ctx *pack = mysql_stmt_execute(task, stmt, &_bind);
    mysql_bind_clear(&_bind);
    if (NULL == pack
        || MPACK_OK !=  pack->pack_type) {
        LOG_WARN("mysql_stmt_execute %s", mysql_erro(&_mysql, NULL));
    }
    mysql_stmt_close(task, stmt);

    const char *sql2 = "select * from test_bind where t_int64=?";
    stmt = mysql_stmt_prepare(task, &_mysql, sql2);
    mysql_bind_uinteger(&_bind, NULL, curts);
    pack = mysql_stmt_execute(task, stmt, &_bind);
    _show_result1(pack, 1);
    mysql_stmt_close(task, stmt);

    const char *sql3 = "select * from test_bind";
    stmt = mysql_stmt_prepare(task, &_mysql, sql3);
    pack = mysql_stmt_execute(task, stmt, &_bind);
    _show_result1(pack, 0);
    mysql_stmt_close(task, stmt);

    const char *sql4 = "delete from test_bind";
    stmt = mysql_stmt_prepare(task, &_mysql, sql4);
    pack = mysql_stmt_execute(task, stmt, NULL);
    if (NULL == pack
        || pack->pack_type != MPACK_OK) {
        LOG_WARN("mysql_stmt_execute error");
    }
    mysql_stmt_close(task, stmt);
}
static void _timeout(task_ctx *task, uint64_t sess) {
    if (ERR_FAILED != mysql_selectdb(task, &_mysql, "tes1t")) {
        LOG_WARN("selectdb wrong db error.");
    }
    if (ERR_OK != mysql_selectdb(task, &_mysql, "test")) {
        LOG_WARN("selectdb error.");
    }
    mysql_quit(task, &_mysql);
    if (ERR_OK != mysql_ping(task, &_mysql)) {
        LOG_WARN("coro_mysql_ping error.");
    }
    mpack_ctx * mpack = mysql_query(task, &_mysql, "select * from admin", NULL);
    if (NULL == mpack
        || MPACK_QUERY != mpack->pack_type) {
        LOG_WARN("mysql_query error.");
    } else {
        if (_prt) {
            mysql_reader_ctx *rst = mpack->pack;
            LOG_INFO("field %d, rows %d", rst->field_count, arr_ptr_size(&rst->arr_rows));
        }
    }
    _test_bind(task);
    _test_stmt(task);
}
static void _startup(task_ctx *task) {
    on_closed(task, _net_close);
    on_connected(task, _net_connect);
    struct evssl_ctx *evssl = NULL;
#if WITH_SSL
    evssl = evssl_qury(102);
#endif
    if (ERR_OK != mysql_init(&_mysql, "192.168.8.3", 3306, evssl, "admin", "12345678", "test", "utf8", 0, 1)) {
        LOG_WARN("mysql_init error.");
        return;
    }
    if (ERR_OK != mysql_connect(task, &_mysql)) {
        LOG_ERROR("mysql connect error. %s", mysql_erro(&_mysql, NULL));
        return;
    }
    if (_prt) {
        LOG_INFO("mysql connected.");
    }
    mysql_bind_init(&_bind);
    task_timeout(task, 0, 1000, _timeout);
}
void _closing_cb(task_ctx *task) {
    mysql_bind_free(&_bind);
}
void task_mysql_start(loader_ctx *loader, name_t name, int32_t pt) {
    _prt = pt;
    task_ctx *task = task_new(loader, name, NULL, NULL, NULL);
    task_register(task, _startup, _closing_cb);
}

#endif
