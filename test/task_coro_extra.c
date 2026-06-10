#include "task_coro_extra.h"

typedef struct task_coro_extra_args {
    uint16_t httpport;
    const char *rpcname;
    int32_t *ok;
}task_coro_extra_args;

// coro_sleep 1500ms：跨过 tv1（256ms）触发 tv2 cascade 路径
static int32_t _test_sleep_cascade(task_ctx *task) {
    uint64_t t0 = nowms();
    coro_sleep(task, 1500);
    uint64_t diff = nowms() - t0;
    // 容忍 ±150ms 抖动；上界放宽是因为 worker 被其他 task 占用时唤醒会延后
    if (diff < 1450 || diff > 1800) {
        LOG_ERROR("coro_sleep cascade: expected ~1500ms, got %llums.",
                  (unsigned long long)diff);
        return ERR_FAILED;
    }
    return ERR_OK;
}

// coro_connect 到 127.0.0.1:1（保留端口，必拒绝），验证错误返回
static int32_t _test_connect_refused(task_ctx *task) {
    SOCKET fd;
    uint64_t skid;
    int32_t r = coro_connect(task, PACK_HTTP, NULL, "127.0.0.1", 1, 0, NULL, &fd, &skid);
    if (ERR_OK == r) {
        // 不应该连成功；连上了立即关掉再报错
        ev_close(&task->loader->netev, fd, skid, 1);
        LOG_ERROR("coro_connect refused: 127.0.0.1:1 unexpectedly accepted.");
        return ERR_FAILED;
    }
    return ERR_OK;
}

// 先连上 http server，立即 ev_close 后再调 coro_send，验证 send 在断开 fd 上正确失败
static int32_t _test_send_after_close(task_ctx *task, uint16_t httpport) {
    SOCKET fd;
    uint64_t skid;
    if (ERR_OK != coro_connect(task, PACK_HTTP, NULL, "127.0.0.1", httpport, 0, NULL, &fd, &skid)) {
        LOG_ERROR("coro_send-after-close: pre-connect to http_sv failed.");
        return ERR_FAILED;
    }
    ev_close(&task->loader->netev, fd, skid, 1);
    // 等关连接消息穿过事件循环；时间轮粒度 1ms，50ms 足够
    coro_sleep(task, 50);
    // 构造一个最小 HTTP GET 包发送，预期 coro_send 返回 NULL
    const char *req = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    size_t rsize = 0;
    void *resp = coro_send(task, fd, skid, (void *)req, strlen(req), &rsize, 1);
    if (NULL != resp) {
        LOG_ERROR("coro_send-after-close: expected NULL, got resp size=%zu.", rsize);
        return ERR_FAILED;
    }
    return ERR_OK;
}

// dns_lookup 走 PACK_DNS 协议 + coro_sendto/coro_send，覆盖 DNS 协程路径
static int32_t _test_dns_lookup(task_ctx *task) {
    size_t cnt = 0;
    // 走 UDP（udp=1），dns_set_ip("8.8.8.8") 已在 main.c 配置；example.com 是 IANA 保留稳定域名
    dns_ip *ips = dns_lookup(task, "example.com", 0, 1, &cnt);
    if (NULL == ips || 0 == cnt) {
        LOG_ERROR("dns_lookup(example.com): no IPs returned.");
        if (NULL != ips) {
            FREE(ips);
        }
        return ERR_FAILED;
    }
    FREE(ips);
    return ERR_OK;
}

// 向 rpc task 发未知 rtype 请求；rpc 的 default 分支不回包，触发请求超时返回 ERR_FAILED
static int32_t _test_request_timeout(task_ctx *task, const char *rpcname) {
    task_ctx *dst = task_grab(task->loader, task_find_name(task->loader, rpcname));
    if (NULL == dst) {
        LOG_ERROR("coro_request timeout: rpc task %s not found.", rpcname);
        return ERR_FAILED;
    }
    // 默认 timeout_request=3000ms，缩短到 300ms 让用例快速结束
    uint32_t old_to = task_get_request_timeout(task);
    task_set_request_timeout(task, 300);
    int32_t erro = ERR_OK;
    size_t lens = 0;
    (void)coro_request(dst, task, 99, "x", 1, 1, &erro, &lens);
    task_set_request_timeout(task, old_to);
    task_ungrab(dst);
    if (ERR_FAILED != erro) {
        LOG_ERROR("coro_request timeout: expected ERR_FAILED, got erro=%d.", erro);
        return ERR_FAILED;
    }
    return ERR_OK;
}

static void _startup(task_ctx *task) {
    task_coro_extra_args *arg = (task_coro_extra_args *)coro_get_arg(task);
    if (ERR_OK != _test_sleep_cascade(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_connect_refused(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_send_after_close(task, arg->httpport)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_dns_lookup(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_request_timeout(task, arg->rpcname)) {
        return;
    }
    *(arg->ok) = 1;
    LOG_INFO("coro_extra tested.");
}

void task_coro_extra_start(loader_ctx *loader, const char *name, uint16_t httpport, const char *rpcname, int32_t *ok) {
    if (NULL == ok || 0 == httpport) {
        return;
    }
    task_coro_extra_args *arg;
    CALLOC(arg, 1, sizeof(task_coro_extra_args));
    arg->httpport = httpport;
    arg->rpcname = rpcname;
    arg->ok = ok;
    coro_task_register(loader, name, 0, _startup, NULL, _free, arg);
}
