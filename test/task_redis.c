#include "task_redis.h"

// 键名带 C 侧专属前缀，与 Lua 的 db_redis.lua 同时跑也互不干扰(那边用 srey:test 等)
#define KEY_STR  "srey:ctest:test"
#define KEY_HASH "srey:ctest:hash"
#define KEY_CNT  "srey:ctest:counter"

typedef struct task_redis_args {
    uint16_t port;
    int32_t *ok;
    sock_ctx sk;
    char host[64];
    char key[64];           // 密码；空字符串表示无 AUTH
}task_redis_args;

// 发送一条已组好的 Redis 请求包并校验响应；strvalue 非 NULL 时还要求字符串内容相等
// pack 由 redis_pack 返回，调用方传入；ev_send 在底层接管 pack 所有权
static int32_t _exec(task_ctx *task, sock_ctx *sk, char *pack, size_t size,
                     int32_t expect_prot, const char *strvalue, int64_t *intval) {
    if (NULL == pack) {
        return ERR_FAILED;
    }
    if (task_isclosing(task)) {// 收尾已开始就不再发，免得每步都等满超时
        FREE(pack);
        return ERR_FAILED;
    }
    redis_pack_ctx *rsp = coro_send(task, sk, pack, size, NULL, 0);
    if (NULL == rsp) {
        return ERR_FAILED;
    }
    if (rsp->prot != expect_prot) {
        return ERR_FAILED;
    }
    if (NULL != strvalue) {
        size_t lens = strlen(strvalue);
        if ((int64_t)lens != rsp->len || 0 != memcmp(rsp->data, strvalue, lens)) {
            return ERR_FAILED;
        }
    }
    if (NULL != intval) {
        *intval = rsp->ival;
    }
    return ERR_OK;
}

// 对 hash 键执行 GET 触发 WRONGTYPE，校验 wire 上的 RESP error 路径
static int32_t _wrong_type_error(task_ctx *task, sock_ctx *sk) {
    size_t size;
    // 复用前面已 HSET 过的 hash 键，对它执行字符串型 GET
    char *pack = redis_pack(&size, "GET %s", KEY_HASH);
    if (NULL == pack) {
        return ERR_FAILED;
    }
    redis_pack_ctx *rsp = coro_send(task, sk, pack, size, NULL, 0);
    if (NULL == rsp) {
        LOG_ERROR("redis WRONGTYPE: no response.");
        return ERR_FAILED;
    }
    if (RESP_ERROR != rsp->prot) {
        LOG_ERROR("redis WRONGTYPE: expected RESP_ERROR, got '%c'.", rsp->prot);
        return ERR_FAILED;
    }
    static const char prefix[] = "WRONGTYPE";
    if (rsp->len < (int64_t)(sizeof(prefix) - 1)
        || 0 != memcmp(rsp->data, prefix, sizeof(prefix) - 1)) {
        LOG_ERROR("redis WRONGTYPE: unexpected error msg.");
        return ERR_FAILED;
    }
    return ERR_OK;
}

static void _startup(task_ctx *task) {
    task_redis_args *arg = (task_redis_args *)coro_get_arg(task);
    const char *key = EMPTYSTR(arg->key) ? NULL : arg->key;
    ATOMIC_SET((atomic_t *)arg->ok, -2);// 正在连：汇总 task 不把它当终态，连不上再回 0
    if (ERR_OK != redis_connect(task, NULL, arg->host, arg->port, key, 0, &arg->sk)) {
        LOG_ERROR("redis connect error.");
        ATOMIC_SET((atomic_t *)arg->ok, 0);
        return;
    }
    // 连上了：此后任何失败都是真失败，不能再被 optional 白名单吞成 network error
    ATOMIC_SET((atomic_t *)arg->ok, -1);
    size_t size;
    char *pack;
    // 先清掉上次中断留下的键，否则下面 HSET / INCR 的计数断言永远对不上
    pack = redis_pack(&size, "DEL %s %s %s", KEY_STR, KEY_HASH, KEY_CNT);
    if (ERR_OK != _exec(task, &arg->sk, pack, size, RESP_INTEGER, NULL, NULL)) {
        LOG_ERROR("redis pre-clean DEL error.");
        goto END;
    }
    // SET KEY_STR hello → +OK
    pack = redis_pack(&size, "SET %s %s", KEY_STR, "hello");
    if (ERR_OK != _exec(task, &arg->sk, pack, size, RESP_STRING, "OK", NULL)) {
        LOG_ERROR("redis SET error.");
        goto END;
    }
    // GET KEY_STR → $5\r\nhello
    pack = redis_pack(&size, "GET %s", KEY_STR);
    if (ERR_OK != _exec(task, &arg->sk, pack, size, RESP_BSTRING, "hello", NULL)) {
        LOG_ERROR("redis GET error.");
        goto END;
    }
    // DEL KEY_STR → :1
    int64_t deleted = 0;
    pack = redis_pack(&size, "DEL %s", KEY_STR);
    if (ERR_OK != _exec(task, &arg->sk, pack, size, RESP_INTEGER, NULL, &deleted)
        || 1 != deleted) {
        LOG_ERROR("redis DEL error.");
        goto END;
    }
    // HSET KEY_HASH f1 v1 → :1
    int64_t added = 0;
    pack = redis_pack(&size, "HSET %s %s %s", KEY_HASH, "f1", "v1");
    if (ERR_OK != _exec(task, &arg->sk, pack, size, RESP_INTEGER, NULL, &added)
        || 1 != added) {
        LOG_ERROR("redis HSET error.");
        goto END;
    }
    // HGET KEY_HASH f1 → $2\r\nv1
    pack = redis_pack(&size, "HGET %s %s", KEY_HASH, "f1");
    if (ERR_OK != _exec(task, &arg->sk, pack, size, RESP_BSTRING, "v1", NULL)) {
        LOG_ERROR("redis HGET error.");
        goto END;
    }
    // GET KEY_HASH → -WRONGTYPE ...
    if (task_isclosing(task)
        || ERR_OK != _wrong_type_error(task, &arg->sk)) {
        LOG_ERROR("redis WRONGTYPE error.");
        goto END;
    }
    // INCR KEY_CNT → :1（首次自增）
    int64_t cnt = 0;
    pack = redis_pack(&size, "INCR %s", KEY_CNT);
    if (ERR_OK != _exec(task, &arg->sk, pack, size, RESP_INTEGER, NULL, &cnt)
        || 1 != cnt) {
        LOG_ERROR("redis INCR error.");
        goto END;
    }
    // 清理测试键
    pack = redis_pack(&size, "DEL %s %s", KEY_HASH, KEY_CNT);
    _exec(task, &arg->sk, pack, size, RESP_INTEGER, NULL, NULL);
    ATOMIC_SET((atomic_t *)arg->ok, 1);
    LOG_INFO("redis tested.");
END:
    ev_close(&task->loader->netev, &arg->sk);
}

void task_redis_start(loader_ctx *loader, const char *name,
                      const char *host, uint16_t port,
                      const char *key, int32_t *ok) {
    if (NULL == ok
        || NULL == host || strlen(host) >= 64
        || (NULL != key && strlen(key) >= 64)) {
        return;
    }
    task_redis_args *arg;
    CALLOC(arg, 1, sizeof(task_redis_args));
    arg->port = port;
    arg->ok = ok;
    sock_set_invalid(&arg->sk);
    safe_fill_str(arg->host, sizeof(arg->host), host);
    if (NULL != key) {
        safe_fill_str(arg->key, sizeof(arg->key), key);
    }
    coro_task_register(loader, name, 0, _startup, NULL, _free, arg);
}
