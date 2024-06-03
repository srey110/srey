#include "task_mysql.h"

#if WITH_CORO

static int32_t _prt = 0;

static void _net_close(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client) {
    if (_prt) {
        LOG_INFO("mysql connection closed.");
    }
}
static void _timeout(task_ctx *task, uint64_t sess) {
    uint64_t skid;
    struct evssl_ctx *evssl = srey_ssl_qury(task->scheduler, 102);
    SOCKET fd = coro_mysql_connect(task, "127.0.0.1", 3306, evssl, "admin", "12345678", "mysql", "utf8", 0, &skid);
    if (INVALID_FD == fd) {
        LOG_ERROR("connect mysql error.");
        return;
    }
    if (_prt) {
        LOG_INFO("mysql connected.");
    }
    void *req;
    size_t rlens;
    req = mysql_pack_selectdb("test", &rlens);
    mysql_pack_ctx *mpk = coro_send(task, fd, skid, req, rlens, &rlens, 0);
    if (MYSQL_OK != mpk->command) {
        LOG_WARN("mysql_select_db error.");
    }
    req = mysql_pack_ping(&rlens);
    mpk = coro_send(task, fd, skid, req, rlens, &rlens, 0);
    if (MYSQL_OK != mpk->command) {
        LOG_WARN("mysql_ping error.");
    }

    req = mysql_pack_quit(&rlens);
    ev_send(&task->scheduler->netev, fd, skid, req, rlens, 0);
    //trigger_timeout(task, 0, 2000, _timeout);
}
static void _startup(task_ctx *task) {
    on_closed(task, _net_close);
    trigger_timeout(task, 0, 1000, _timeout);
}
void task_mysql_start(scheduler_ctx *scheduler, name_t name, int32_t pt) {
    _prt = pt;
    task_ctx *task = task_new(scheduler, name, NULL, NULL, NULL);
    task_register(task, _startup, NULL);
}

#endif
