#include "task_dc_client.h"

typedef struct task_dc_client_args {
    int32_t *ok;
    const char *base_name;
    const char *dc_name;
}task_dc_client_args;

// 文件级 dc_name:由 _startup 从 arg 读出后写入,所有子段/fork 协程共享
static name_t _dc_name;
// 多 waiter 测试共享状态
static atomic_t _multi_waiter_received;
#define MULTI_WAITER_N 3

static void _multi_waiter(task_ctx *task, void *arg) {
    const char *key = (const char *)arg;
    size_t sz;
    int32_t erro;
    void *val = coro_dc_wait(task, _dc_name, key, &sz, &erro);
    if (NULL != val && 5 == sz && 0 == memcmp(val, "hello", 5)) {
        ATOMIC_ADD(&_multi_waiter_received, 1);
    }
}
// F-DC-2 幽灵响应捕获器:coro_dc_* 的响应由 coro_sess 匹配后直接 resume 协程,不经此回调;
// 只有 coro_sess 已不存在的迟到响应(陈旧 waiter 被唤醒)才进这里,故计数即"幽灵"次数
static atomic_t _ghost_resp;
static void _on_ghost_response(task_ctx *task, subtype_t reqtype, uint64_t sess, int32_t error, void *data, size_t size) {
    (void)task;
    (void)reqtype;
    (void)sess;
    (void)error;
    (void)data;
    (void)size;
    ATOMIC_ADD(&_ghost_resp, 1);
}

// 子段 1:set + get 同步 round-trip
static int32_t _test_set_get(task_ctx *task) {
    if (ERR_OK != coro_dc_set(task, _dc_name, "k1", "v1", 2)) {
        LOG_ERROR("dc set k1 failed");
        return ERR_FAILED;
    }
    size_t sz;
    int32_t erro;
    void *val = coro_dc_get(task, _dc_name, "k1", &sz, &erro);
    if (NULL == val || 2 != sz || 0 != memcmp(val, "v1", 2)) {
        LOG_ERROR("dc get k1: expect 'v1'(2),got %p(%zu)", val, sz);
        return ERR_FAILED;
    }
    // get 不存在的 key:返 NULL 且 erro=ERR_FAILED(key 不存在视为失败)
    val = coro_dc_get(task, _dc_name, "no_such_key", &sz, &erro);
    if (NULL != val || ERR_OK == erro) {
        LOG_ERROR("dc get no_such_key: expect NULL+fail,got %p erro=%d", val, erro);
        return ERR_FAILED;
    }
    return ERR_OK;
}

// 子段 2:wait 命中(set 在前)
static int32_t _test_wait_hit(task_ctx *task) {
    coro_dc_set(task, _dc_name, "k_hit", "ready", 5);
    size_t sz;
    int32_t erro;
    void *val = coro_dc_wait(task, _dc_name, "k_hit", &sz, &erro);
    if (NULL == val || 5 != sz || 0 != memcmp(val, "ready", 5)) {
        LOG_ERROR("dc wait k_hit hit path: expect 'ready'");
        return ERR_FAILED;
    }
    return ERR_OK;
}

// 子段 3:wait 未命中 → fork 另一协程稍后 set → waiter 唤醒
static void _delayed_setter(task_ctx *task, void *arg) {
    const char *key = (const char *)arg;
    coro_sleep(task, 100);
    coro_dc_set(task, _dc_name, key, "delayed", 7);
}
static int32_t _test_wait_miss_then_set(task_ctx *task) {
    coro_fork(task, _delayed_setter, (void *)"k_delay");
    size_t sz;
    int32_t erro;
    void *val = coro_dc_wait(task, _dc_name, "k_delay", &sz, &erro);  // 挂起 ~100ms
    if (NULL == val || 7 != sz || 0 != memcmp(val, "delayed", 7)) {
        LOG_ERROR("dc wait k_delay miss-then-set: expect 'delayed'");
        return ERR_FAILED;
    }
    return ERR_OK;
}

// 子段 4:multi waiter — 3 个协程 wait 同 key,1 次 set 唤醒所有 3 个
static int32_t _test_multi_waiter(task_ctx *task) {
    ATOMIC_SET(&_multi_waiter_received, 0);
    int32_t i;
    for (i = 0; i < MULTI_WAITER_N; i++) {
        coro_fork(task, _multi_waiter, (void *)"k_multi");
    }
    // 等所有 fork 都进入 wait(挂入 pending)
    coro_sleep(task, 50);
    coro_dc_set(task, _dc_name, "k_multi", "hello", 5);
    // 等所有 waiter 被唤醒
    int32_t poll;
    int32_t recvcnt = 0;
    for (poll = 0; poll < 40; poll++) {
        coro_sleep(task, 50);
        recvcnt = (int32_t)ATOMIC_GET(&_multi_waiter_received);
        if (MULTI_WAITER_N == recvcnt) {
            break;
        }
    }
    if (MULTI_WAITER_N != recvcnt) {
        LOG_ERROR("dc multi waiter: received %d/%d", recvcnt, MULTI_WAITER_N);
        return ERR_FAILED;
    }
    return ERR_OK;
}

// 子段 5:wait 超时 — 临时降 request_timeout,等不存在的 key
static int32_t _test_wait_timeout(task_ctx *task) {
    uint32_t old_to = task_get_request_timeout(task);
    task_set_request_timeout(task, 200);  // 200ms(实际超时由 _timeout_monitor 1s 粒度兜底)
    size_t sz;
    int32_t erro;
    void *val = coro_dc_wait(task, _dc_name, "k_never", &sz, &erro);
    task_set_request_timeout(task, old_to);
    // 超时:返 NULL 且 erro != ERR_OK(失败,区别于 key 不存在的 ERR_OK)
    if (NULL != val || ERR_OK == erro) {
        LOG_ERROR("dc wait k_never timeout: expect NULL+fail,got %p erro=%d", val, erro);
        return ERR_FAILED;
    }
    return ERR_OK;
}

// 子段 6:delete — set 后 delete,get 返回 NULL
static int32_t _test_delete(task_ctx *task) {
    coro_dc_set(task, _dc_name, "k_del", "x", 1);
    size_t sz;
    int32_t erro;
    void *val = coro_dc_get(task, _dc_name, "k_del", &sz, &erro);
    if (NULL == val) {
        LOG_ERROR("dc delete: pre-check failed");
        return ERR_FAILED;
    }
    if (ERR_OK != coro_dc_del(task, _dc_name, "k_del")) {
        LOG_ERROR("dc delete failed");
        return ERR_FAILED;
    }
    val = coro_dc_get(task, _dc_name, "k_del", &sz, &erro);
    if (NULL != val || ERR_OK == erro) {
        LOG_ERROR("dc delete: expect NULL+fail after del,got %p erro=%d", val, erro);
        return ERR_FAILED;
    }
    return ERR_OK;
}

// 子段 7:list_keys — set 3 key + list → 含 3 个 key
static int32_t _test_list_keys(task_ctx *task) {
    coro_dc_set(task, _dc_name, "lk_a", "1", 1);
    coro_dc_set(task, _dc_name, "lk_b", "2", 1);
    coro_dc_set(task, _dc_name, "lk_c", "3", 1);
    size_t sz;
    int32_t erro;
    void *buf = coro_dc_keys(task, _dc_name, &sz, &erro);
    if (EMPTYPTR(buf, sz)) {
        LOG_ERROR("dc list_keys: empty");
        return ERR_FAILED;
    }
    // 验 3 个 key 都在 buf 内(buf 是 | u16 klen | key | 帧序列)
    int32_t got_a = 0;
    int32_t got_b = 0;
    int32_t got_c = 0;
    binary_ctx br;
    binary_init(&br, (char *)buf, sz, 0);
    dc_key k;
    while (br.offset < br.size) {
        if (ERR_OK != dc_parse_keys(&br, &k)) {
            break;
        }
        if (4 == k.klen && 0 == memcmp(k.key, "lk_a", 4)) {
            got_a = 1;
        }
        if (4 == k.klen && 0 == memcmp(k.key, "lk_b", 4)) {
            got_b = 1;
        }
        if (4 == k.klen && 0 == memcmp(k.key, "lk_c", 4)) {
            got_c = 1;
        }
    }
    if (!(got_a && got_b && got_c)) {
        LOG_ERROR("dc list_keys: lk_a=%d lk_b=%d lk_c=%d", got_a, got_b, got_c);
        return ERR_FAILED;
    }
    // 清理
    coro_dc_del(task, _dc_name, "lk_a");
    coro_dc_del(task, _dc_name, "lk_b");
    coro_dc_del(task, _dc_name, "lk_c");
    return ERR_OK;
}

// 子段 8:set value=NULL 软清空 → get 返回 NULL
static int32_t _test_set_null(task_ctx *task) {
    coro_dc_set(task, _dc_name, "k_clr", "data", 4);
    size_t sz;
    int32_t erro;
    void *val = coro_dc_get(task, _dc_name, "k_clr", &sz, &erro);
    if (NULL == val) {
        LOG_ERROR("dc set_null: pre-check failed");
        return ERR_FAILED;
    }
    coro_dc_set(task, _dc_name, "k_clr", NULL, 0);
    // 软清空后 key 仍存在(空值)→ get 返 NULL 但 erro=ERR_OK(区别于真不存在的 ERR_FAILED)
    val = coro_dc_get(task, _dc_name, "k_clr", &sz, &erro);
    if (NULL != val || ERR_OK != erro) {
        LOG_ERROR("dc set_null: expect NULL+ERR_OK (empty present),got %p erro=%d", val, erro);
        return ERR_FAILED;
    }
    return ERR_OK;
}

// 子段 9:超长 key 早返 — client helper 校验 key 长度 >= DC_KEY_MAX 时不下发(mirror unit_dc_client.lua)
static int32_t _test_key_too_long(task_ctx *task) {
    // DC_KEY_MAX 字节 key(达到上限),helper 应早返不下发到 datacenter
    char long_key[DC_KEY_MAX + 1];
    memset(long_key, 'k', DC_KEY_MAX);
    long_key[DC_KEY_MAX] = '\0';
    if (ERR_FAILED != coro_dc_set(task, _dc_name, long_key, "v", 1)) {
        LOG_ERROR("dc set long key: expect ERR_FAILED");
        return ERR_FAILED;
    }
    size_t sz;
    int32_t erro;
    if (NULL != coro_dc_get(task, _dc_name, long_key, &sz, &erro) || ERR_OK == erro) {
        LOG_ERROR("dc get long key: expect NULL+fail, erro=%d", erro);
        return ERR_FAILED;
    }
    if (ERR_FAILED != coro_dc_del(task, _dc_name, long_key)) {
        LOG_ERROR("dc del long key: expect ERR_FAILED");
        return ERR_FAILED;
    }
    return ERR_OK;
}

// 子段 10:waiter 超时过期后,迟到 set 不再唤醒它(不产生幽灵响应,F-DC-2)
static int32_t _test_late_set_no_ghost(task_ctx *task) {
    size_t sz;
    int32_t erro;
    uint32_t old_to = task_get_request_timeout(task);
    task_set_request_timeout(task, 200);
    void *val = coro_dc_wait(task, _dc_name, "k_ghost", &sz, &erro);  // 未命中挂 waiter(deadline=入队+200ms),超时返 NULL
    task_set_request_timeout(task, old_to);
    if (NULL != val) {
        LOG_ERROR("late_set_no_ghost: expect NULL on timeout");
        return ERR_FAILED;
    }
    coro_sleep(task, 600);          // 睡过 waiter 的 200ms deadline,确保其已过期
    ATOMIC_SET(&_ghost_resp, 0);
    coro_dc_set(task, _dc_name, "k_ghost", "late", 4);   // 迟到 set:过期 waiter 应被跳过,不唤醒
    coro_sleep(task, 200);          // 给可能的幽灵响应到达时间
    if (0 != ATOMIC_GET(&_ghost_resp)) {
        LOG_ERROR("late_set_no_ghost: stale waiter woken, ghost resp=%d", (int32_t)ATOMIC_GET(&_ghost_resp));
        return ERR_FAILED;
    }
    return ERR_OK;
}

static void _startup(task_ctx *task) {
    task_dc_client_args *arg = (task_dc_client_args *)coro_get_arg(task);
    _dc_name = task_find_name(task->loader, arg->dc_name);
    task_responsed(task, _on_ghost_response);   // 注册幽灵响应捕获器(F-DC-2)

    if (ERR_OK != _test_set_get(task))             { return; }
    if (ERR_OK != _test_wait_hit(task))            { return; }
    if (ERR_OK != _test_wait_miss_then_set(task))  { return; }
    if (ERR_OK != _test_multi_waiter(task))        { return; }
    if (ERR_OK != _test_wait_timeout(task))        { return; }
    if (ERR_OK != _test_delete(task))              { return; }
    if (ERR_OK != _test_list_keys(task))           { return; }
    if (ERR_OK != _test_set_null(task))            { return; }
    if (ERR_OK != _test_key_too_long(task))        { return; }
    if (ERR_OK != _test_late_set_no_ghost(task))   { return; }

    *(arg->ok) = 1;
    LOG_INFO("dc_client tested: 10/10 subtests passed.");
}

void task_dc_client_start(loader_ctx *loader, const char *base_name, const char *dc_name, int32_t *ok) {
    if (NULL == ok) {
        return;
    }
    task_dc_client_args *arg;
    CALLOC(arg, 1, sizeof(task_dc_client_args));
    arg->ok = ok;
    arg->base_name = base_name;
    arg->dc_name = dc_name;
    coro_task_register(loader, base_name, 0, _startup, NULL, _free, arg);
}
