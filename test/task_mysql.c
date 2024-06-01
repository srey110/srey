#include "task_mysql.h"

#if WITH_CORO

static int32_t _prt = 0;
SOCKET _fd;
uint64_t _skid;
static void _net_auth_ssl(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client) {
    LOG_INFO("mysql auth ssl.");
}
static void _net_close(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client) {
    LOG_WARN("mysql connection closed.");
}
void _net_handshake(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client, int32_t erro) {
    LOG_INFO("mysql handshaked.");
}
static void _startup(task_ctx *task) {
    on_authssl(task, _net_auth_ssl);
    on_closed(task, _net_close);
    on_handshaked(task, _net_handshake);
    //struct evssl_ctx *evssl = srey_ssl_qury(task->scheduler, 102);
    _fd = mysql_connect(task,"192.168.8.3", 3306, NULL, "admin", "12345678", "mysql", "utf8", 0, &_skid);
    if (INVALID_FD == _fd) {
        LOG_ERROR("connect mysql error.");
        return;
    }
}
void task_mysql_start(scheduler_ctx *scheduler, name_t name, int32_t pt) {
    _prt = pt;
    task_ctx *task = task_new(scheduler, name, NULL, NULL, NULL);
    task_register(task, _startup, NULL);
}

#endif
