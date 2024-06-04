#include "task_mysql.h"

#if WITH_CORO

static int32_t _prt = 0;
static mysql_ctx _mysql;

static void _net_close(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client) {
    if (_prt) {
        LOG_INFO("mysql connection closed.");
    }
}
static void _timeout(task_ctx *task, uint64_t sess) {
    if (ERR_OK != mysql_selectdb(task, &_mysql, "test")) {
        LOG_WARN("coro_mysql_selectdb error.");
    }
    if (ERR_OK != mysql_ping(task, &_mysql)) {
        LOG_WARN("coro_mysql_ping error.");
    }

    /*req = mysql_pack_query("SELECT * FROM `al_config`", &rlens);
    ev_send(&task->scheduler->netev, mysql.fd, mysql.skid, req, rlens, 0);
    mpk = coro_send(task, fd, skid, req, rlens, &rlens, 0);
    if (MYSQL_OK != mpk->command) {
        LOG_WARN("mysql_ping error.");
    }*/

    mysql_quit(task, &_mysql);
}
static void _startup(task_ctx *task) {
    on_closed(task, _net_close);
    struct evssl_ctx *evssl = srey_ssl_qury(task->scheduler, 102);
    if (ERR_OK != mysql_init(&_mysql, "192.168.8.3", 3306, evssl, "admin", "12345678", "test", "utf8", 0)) {
        LOG_WARN("mysql_init error.");
        return;
    }
    if (ERR_OK != mysql_connect(task, &_mysql)) {
        LOG_ERROR("connect mysql error.");
        return;
    }
    if (_prt) {
        LOG_INFO("mysql connected.");
    }
    trigger_timeout(task, 0, 1000, _timeout);
}
void task_mysql_start(scheduler_ctx *scheduler, name_t name, int32_t pt) {
    _prt = pt;
    task_ctx *task = task_new(scheduler, name, NULL, NULL, NULL);
    task_register(task, _startup, NULL);
}

#endif
