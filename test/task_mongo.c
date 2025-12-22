#include "task_mongo.h"

static int32_t _prt = 1;
static mongo_ctx _mongo;

static void _startup(task_ctx *task) {
    mongo_init(&_mongo, "127.0.0.1", 0, NULL);
    if (ERR_OK != mongo_connect(task, &_mongo)){
        LOG_ERROR("mongo_connect error.");
    }
    mongo_db(&_mongo, "admin");
    mgopack_ctx *hel = mongo_hello(&_mongo);
    binary_ctx bson;
    bson_init(&bson, hel->doc, hel->dlens);
    if (ERR_OK != mongo_auth(&_mongo, "SCRAM-SHA-256", "admin", "12345678")) {
        LOG_ERROR("mongo_auth error.");
    }
    LOG_INFO("mongo tested.");
}
static void _closing_cb(task_ctx *task) {

}
void task_mongo_start(loader_ctx *loader, name_t name, int32_t pt) {
    _prt = pt;
    task_ctx *task = task_new(loader, name, NULL, NULL, NULL);
    task_register(task, _startup, _closing_cb);
}
