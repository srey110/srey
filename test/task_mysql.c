#include "task_mysql.h"

#if WITH_CORO

static int32_t _prt = 0;
SOCKET _fd;
uint64_t _skid;

static void _startup(task_ctx *task) {
    _fd = coro_connect(task, PACK_MYSQL, NULL, "127.0.0.1", 3306, &_skid, 0);
    if (INVALID_FD == _fd) {
        if (_prt) {
            LOG_ERROR("connect redis error.");
        }
        return;
    }
}
void task_mysql_start(scheduler_ctx *scheduler, name_t name, int32_t pt) {
    _prt = pt;
    task_ctx *task = task_new(scheduler, name, NULL, NULL, NULL);
    task_register(task, _startup, NULL);
}

#endif
