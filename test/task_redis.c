#include "task_redis.h"

#if WITH_CORO

static int32_t _prt = 0;
SOCKET _fd;
uint64_t _skid;
static void _net_close(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client) {
    if (_prt) {
        LOG_WARN("redis connection closed");
    }
    _fd = INVALID_SOCK;
}
static void _net_recv(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client, uint8_t slice, void *data, size_t size) {
    redis_pack_ctx *rtn = data;
    if (0 != _memicmp(rtn->data, "PONG", (size_t)rtn->len)) {
        LOG_WARN("PING error.");
    }
}
static void _timeout(task_ctx *task, uint64_t sess) {
    if (INVALID_SOCK == _fd) {
        return;
    }
    size_t rsize;
    char *req = redis_pack(&rsize, "PING");
    redis_pack_ctx *rtn = coro_send(task, _fd, _skid, req, rsize, &rsize, 0);
    if (RESP_STRING != rtn->proto
        || 0 != _memicmp(rtn->data, "PONG", (size_t)rtn->len)) {
        LOG_WARN("PING error.");
    }
    req = redis_pack(&rsize, "INFO");
    rtn = coro_send(task, _fd, _skid, req, rsize, &rsize, 0);
    if (RESP_BSTRING != rtn->proto
        && RESP_VERB != rtn->proto) {
        LOG_WARN("INFO error.");
    }
    req = redis_pack(&rsize, "SELECT 9");
    rtn = coro_send(task, _fd, _skid, req, rsize, &rsize, 0);
    if (0 != _memicmp(rtn->data, "ok", (size_t)rtn->len)) {
        LOG_WARN("SELECT error.");
    }
    req = redis_pack(&rsize, "DBSIZE");
    rtn = coro_send(task, _fd, _skid, req, rsize, &rsize, 0);
    if (RESP_INTEGER != rtn->proto) {
        LOG_WARN("DBSIZE error.");
    }
    req = redis_pack(&rsize, "hello %d", 3);
    rtn = coro_send(task, _fd, _skid, req, rsize, &rsize, 0);
    if (RESP_MAP != rtn->proto) {
        LOG_WARN("hello error.");
    }
    req = redis_pack(&rsize, "SET srey 123456");
    rtn = coro_send(task, _fd, _skid, req, rsize, &rsize, 0);
    if (0 != _memicmp(rtn->data, "ok", (size_t)rtn->len)) {
        LOG_WARN("SET error.");
    }
    req = redis_pack(&rsize, "GET srey");
    rtn = coro_send(task, _fd, _skid, req, rsize, &rsize, 0);
    if (0 != _memicmp(rtn->data, "123456", (size_t)rtn->len)) {
        LOG_WARN("GET error.");
    }
    req = redis_pack(&rsize, "DEL srey");
    rtn = coro_send(task, _fd, _skid, req, rsize, &rsize, 0);
    if (1 != rtn->ival) {
        LOG_WARN("DEL error.");
    }
    req = redis_pack(&rsize, "GET nokey");
    rtn = coro_send(task, _fd, _skid, req, rsize, &rsize, 0);
    if (RESP_NIL != rtn->proto) {
        LOG_WARN("GET nokey error.");
    }
    req = redis_pack(&rsize, "LPUSH srey 123 456 789");
    rtn = coro_send(task, _fd, _skid, req, rsize, &rsize, 0);
    if (3 != rtn->ival) {
        LOG_WARN("LPUSH error.");
    }
    req = redis_pack(&rsize, "LRANGE srey 0 -1");
    rtn = coro_send(task, _fd, _skid, req, rsize, &rsize, 0);
    if (RESP_ARRAY != rtn->proto
        || 3 != rtn->nelem) {
        LOG_WARN("LRANGE error.");
    }
    req = redis_pack(&rsize, "DEL srey");
    rtn = coro_send(task, _fd, _skid, req, rsize, &rsize, 0);
    if (1 != rtn->ival) {
        LOG_WARN("DEL error.");
    }
    req = redis_pack(&rsize, "HMSET srey ke1 123 key2 456");
    rtn = coro_send(task, _fd, _skid, req, rsize, &rsize, 0);
    if (0 != _memicmp(rtn->data, "ok", (size_t)rtn->len)) {
        LOG_WARN("HMSET error.");
    }
    req = redis_pack(&rsize, "HGETALL srey");
    rtn = coro_send(task, _fd, _skid, req, rsize, &rsize, 0);
    if (RESP_MAP != rtn->proto) {
        LOG_WARN("HMSET error.");
    }
    req = redis_pack(&rsize, "DEL srey");
    rtn = coro_send(task, _fd, _skid, req, rsize, &rsize, 0);
    if (1 != rtn->ival) {
        LOG_WARN("DEL error.");
    }

    req = redis_pack(&rsize, "PING");
    buffer_ctx pipe;
    buffer_init(&pipe);
    buffer_append(&pipe, req, rsize);
    buffer_append(&pipe, req, rsize);
    buffer_append(&pipe, req, rsize);
    FREE(req);
    char tmp[1024] = { 0 };
    rsize = buffer_size(&pipe);
    buffer_copyout(&pipe, 0, tmp, rsize);
    ev_send(&task->scheduler->netev, _fd, _skid, tmp, rsize, 1);
    buffer_free(&pipe);
    trigger_timeout(task, 0, 3000, _timeout);
}
static void _startup(task_ctx *task) {
    on_recved(task, _net_recv);
    on_closed(task, _net_close);
    _fd = coro_redis_connect(task, NULL, "127.0.0.1", 6379, "123456", &_skid, 0);
    if (INVALID_FD == _fd) {
        LOG_ERROR("connect redis error.");
        return;
    }
    trigger_timeout(task, 0, 1000, _timeout);
}
void task_redis_start(scheduler_ctx *scheduler, name_t name, int32_t pt) {
    _prt = pt;
    task_ctx *task = task_new(scheduler, name, NULL, NULL, NULL);
    task_register(task, _startup, NULL);
}

#endif
