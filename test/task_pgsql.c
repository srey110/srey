#include "task_pgsql.h"

static int32_t _prt = 1;
static pgsql_ctx _pg;

static void _startup(task_ctx *task) {
    struct evssl_ctx *evssl = NULL;
#if WITH_SSL
    evssl = evssl_qury(102);
#endif
    pgsql_init(&_pg, "127.0.0.1", 0, evssl, "postgres", "12345678", "postgres");
    if (ERR_OK != pgsql_connect(task, &_pg)) {
        LOG_ERROR("pgsql_connect error.");
    }
    pgsql_ping(&_pg);
    pgsql_quit(&_pg);
    pgsql_ping(&_pg);
    pgsql_selectdb(&_pg, "test2");
}
static void _closing_cb(task_ctx *task) {

}
void task_pgsql_start(loader_ctx *loader, name_t name, int32_t pt) {
    _prt = pt;
    task_ctx *task = task_new(loader, name, NULL, NULL, NULL);
    task_register(task, _startup, _closing_cb);
}
