#include "task_debug.h"

// 与被测各层的固定文案同串，任一侧改文案本测试立即失败（正是要盯住的东西）
static const char *_NOTLUA  = "command not supported in C task.";// debug_console.c 的 _DBG_NOTLUA
static const char *_NOREQ   = "not register request callback function.";// task.c 的 _task_handle_request
static const char *_HASREQ  = "hasreq: rejected.";// 本文件靶子 task 自己的错误文案

typedef struct task_debug_args {
    uint16_t port;    // debug_console 监听端口
    int32_t *ok;      // main.c testlist 槽
    name_t noreq;     // 靶子 1：未注册 on_requested
    name_t hasreq;    // 靶子 2：注册了 on_requested，一律回 ERR_FAILED
}task_debug_args;

// ── 靶子 task ─────────────────────────────────────────────────────────────
static void _noreq_startup(task_ctx *task) {
    (void)task;// 只当 debug 命令的靶子，启动后什么都不做
}
static void _hasreq_requested(task_ctx *task, subtype_t reqtype, uint64_t sess,
                              name_t src, void *data, size_t size) {
    (void)data;
    (void)size;
    task_ctx *dst = task_grab(task->loader, src);
    if (NULL == dst) {
        return;
    }
    task_response(dst, reqtype, sess, ERR_FAILED, (void *)_HASREQ, strlen(_HASREQ), 1);
    task_ungrab(dst);
}
static void _hasreq_startup(task_ctx *task) {
    task_requested(task, _hasreq_requested);
}

// ── 断言小工具 ────────────────────────────────────────────────────────────
// 响应体非 NUL 结尾，不能用 strstr / strcmp
static int32_t _eq_text(const void *data, size_t len, const char *want) {
    return NULL != data && len == strlen(want) && 0 == memcmp(data, want, len);
}
// memstr 已覆盖 NULL / 长度不足 / want 为空各档，见 utils.h
static int32_t _has_text(const void *data, size_t len, const char *want) {
    return NULL != memstr(0, data, len, want, strlen(want));
}
// 日志用：把可能为 NULL / 非 NUL 结尾的响应体裁进栈缓冲
static void _snip(const void *data, size_t len, char *out, size_t cap) {
    size_t n = (NULL == data) ? 0 : (len < cap - 1 ? len : cap - 1);
    if (n > 0) {
        memcpy(out, data, n);
    }
    out[n] = '\0';
}
// 发一条 REQ_DEBUG；copy=1 故载荷仍由调用方持有。返回值仅在下次 yield 前有效
static void *_dbg_send(task_ctx *task, name_t handle, binary_ctx *bw,
                       int32_t *err, size_t *len) {
    *err = ERR_FAILED;
    *len = 0;
    task_ctx *dst = task_grab(task->loader, handle);
    if (NULL == dst) {
        return NULL;
    }
    void *rtn = coro_request(dst, task, REQ_DEBUG, bw->data, bw->offset, 1, err, len);
    task_ungrab(dst);
    return rtn;
}

// ── 测试 1：C 处理器自己实现的公共命令 ────────────────────────────────────
static int32_t _test_builtin(task_ctx *task, name_t noreq) {
    binary_ctx bw;
    int32_t err;
    size_t len;
    void *rtn;
    char snip[160];
    binary_init(&bw, NULL, 0, 0);
    seri_append_string(&bw, "stat", strlen("stat"));
    rtn = _dbg_send(task, noreq, &bw, &err, &len);
    if (ERR_OK != err
        || !_has_text(rtn, len, "MTYPE")
        || !_has_text(rtn, len, "TOTAL")) {
        _snip(rtn, len, snip, sizeof(snip));
        LOG_ERROR("debug test: stat err %d body '%s'.", err, snip);
        binary_free(&bw);
        return ERR_FAILED;
    }
    binary_free(&bw);
    // 靶子是 TASK_MCO，coro_dump 必有输出；非协程 task 走另一条文案，这里不涉及
    binary_init(&bw, NULL, 0, 0);
    seri_append_string(&bw, "coros", strlen("coros"));
    rtn = _dbg_send(task, noreq, &bw, &err, &len);
    if (ERR_OK != err || NULL == rtn || 0 == len) {
        LOG_ERROR("debug test: coros err %d len %zu.", err, len);
        binary_free(&bw);
        return ERR_FAILED;
    }
    binary_free(&bw);
    return ERR_OK;
}

// ── 测试 2：loglv 三档 ────────────────────────────────────────────────────
// 合法值用当前级别，设回同一个值，不污染其余用例的日志输出
static int32_t _test_loglv(task_ctx *task, name_t noreq) {
    binary_ctx bw;
    int32_t err;
    size_t len;
    void *rtn;
    char snip[160];
    char want[32];
    log_level lv0 = log_getlv();
    SNPRINTF(want, sizeof(want), "log level => %d", (int32_t)lv0);
    binary_init(&bw, NULL, 0, 0);
    seri_append_string(&bw, "loglv", strlen("loglv"));
    seri_append_int(&bw, (int64_t)lv0);
    rtn = _dbg_send(task, noreq, &bw, &err, &len);
    if (ERR_OK != err || !_eq_text(rtn, len, want)) {
        _snip(rtn, len, snip, sizeof(snip));
        LOG_ERROR("debug test: loglv valid err %d body '%s'.", err, snip);
        binary_free(&bw);
        return ERR_FAILED;
    }
    binary_free(&bw);
    // 缺参
    binary_init(&bw, NULL, 0, 0);
    seri_append_string(&bw, "loglv", strlen("loglv"));
    rtn = _dbg_send(task, noreq, &bw, &err, &len);
    if (ERR_OK != err || !_eq_text(rtn, len, "loglv: missing level.")) {
        _snip(rtn, len, snip, sizeof(snip));
        LOG_ERROR("debug test: loglv missing err %d body '%s'.", err, snip);
        binary_free(&bw);
        return ERR_FAILED;
    }
    binary_free(&bw);
    // 越界：级别不变
    binary_init(&bw, NULL, 0, 0);
    seri_append_string(&bw, "loglv", strlen("loglv"));
    seri_append_int(&bw, (int64_t)(LOGLV_DEBUG + 1));
    rtn = _dbg_send(task, noreq, &bw, &err, &len);
    if (ERR_OK != err || !_eq_text(rtn, len, "loglv: invalid level.")) {
        _snip(rtn, len, snip, sizeof(snip));
        LOG_ERROR("debug test: loglv invalid err %d body '%s'.", err, snip);
        binary_free(&bw);
        return ERR_FAILED;
    }
    binary_free(&bw);
    if (lv0 != log_getlv()) {
        LOG_ERROR("debug test: loglv leaked level change.");
        return ERR_FAILED;
    }
    return ERR_OK;
}

// ── 测试 3：C 处理器不认的命令一律透传业务 ────────────────────────────────
// mem/gc/inject/hotfix 曾被 C 侧一张手写名字表拦下回 "not supported"；那张表已删，
// 判定移到发起方（debug_console 按目标 task 类型挡），故直发时它们与未知命令同路
static int32_t _test_passthrough(task_ctx *task, name_t noreq, name_t hasreq) {
    static const char *cmds[] = { "mem", "gc", "inject", "hotfix", "nosuchcmd" };
    binary_ctx bw;
    int32_t err;
    size_t len;
    void *rtn;
    char snip[160];
    size_t i;
    for (i = 0; i < ARRAY_SIZE(cmds); i++) {
        binary_init(&bw, NULL, 0, 0);
        seri_append_string(&bw, cmds[i], strlen(cmds[i]));
        rtn = _dbg_send(task, noreq, &bw, &err, &len);
        if (ERR_FAILED != err || !_eq_text(rtn, len, _NOREQ)) {
            _snip(rtn, len, snip, sizeof(snip));
            LOG_ERROR("debug test: '%s' expected passthrough, err %d body '%s'.",
                      cmds[i], err, snip);
            binary_free(&bw);
            return ERR_FAILED;
        }
        binary_free(&bw);
    }
    // 注册了 on_requested 的目标：拿到的是业务自己的文案，而不是框架那句
    binary_init(&bw, NULL, 0, 0);
    seri_append_string(&bw, "mem", strlen("mem"));
    rtn = _dbg_send(task, hasreq, &bw, &err, &len);
    if (ERR_FAILED != err || !_eq_text(rtn, len, _HASREQ)) {
        _snip(rtn, len, snip, sizeof(snip));
        LOG_ERROR("debug test: hasreq expected own text, err %d body '%s'.", err, snip);
        binary_free(&bw);
        return ERR_FAILED;
    }
    binary_free(&bw);
    // 非位置化载荷（首元素不是字符串）：同样透传
    binary_init(&bw, NULL, 0, 0);
    seri_append_int(&bw, 1);
    rtn = _dbg_send(task, noreq, &bw, &err, &len);
    if (ERR_FAILED != err || !_eq_text(rtn, len, _NOREQ)) {
        _snip(rtn, len, snip, sizeof(snip));
        LOG_ERROR("debug test: non-positional expected passthrough, err %d body '%s'.", err, snip);
        binary_free(&bw);
        return ERR_FAILED;
    }
    binary_free(&bw);
    return ERR_OK;
}

// ── 测试 4：debug_console 的 HTTP 面 ──────────────────────────────────────
// exact 非 0 比全串，否则只查子串（广播响应里还夹着各 task 名与分隔行）
static int32_t _http_get(task_ctx *task, uint16_t port, const char *url,
                         int32_t code, const char *want, int32_t exact) {
    SOCKET fd;
    uint64_t skid;
    if (ERR_OK != coro_connect(task, PACK_HTTP, NULL, "127.0.0.1", port, 0, NULL, &fd, &skid)) {
        LOG_ERROR("debug test: connect console %d failed for %s.", (int32_t)port, url);
        return ERR_FAILED;
    }
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    http_pack_req(&bw, "GET", url);
    http_pack_head(&bw, "Host", "127.0.0.1");
    http_pack_end(&bw);
    size_t rsize;
    // copy=0：缓冲所有权交给 ev_send，不再 binary_free
    struct http_pack_ctx *resp = coro_send(task, fd, skid, bw.data, bw.offset, &rsize, 0);
    int32_t rtn = ERR_FAILED;
    char snip[256];
    char codestr[8];
    size_t dlen = 0;
    void *body;
    buf_ctx *st;
    if (NULL == resp) {
        LOG_ERROR("debug test: no response for %s.", url);
        goto done;
    }
    st = http_status(resp);
    SNPRINTF(codestr, sizeof(codestr), "%d", code);
    if (NULL == st || !buf_compare(&st[1], codestr, strlen(codestr))) {
        LOG_ERROR("debug test: %s expected code %d.", url, code);
        goto done;
    }
    body = http_data(resp, &dlen);
    if (0 == (exact ? _eq_text(body, dlen, want) : _has_text(body, dlen, want))) {
        _snip(body, dlen, snip, sizeof(snip));
        LOG_ERROR("debug test: %s body expected '%s', got '%s'.", url, want, snip);
        goto done;
    }
    rtn = ERR_OK;
done:
    ev_close(&task->loader->netev, fd, skid);
    return rtn;
}
static int32_t _test_console(task_ctx *task, task_debug_args *arg) {
    char url[64];
    // Lua VM 专属命令打到 C task：发起方就地挡下，请求根本不发出去
    SNPRINTF(url, sizeof(url), "/%"PRIu64"/mem", (uint64_t)arg->noreq);
    if (ERR_OK != _http_get(task, arg->port, url, 200, _NOTLUA, 1)) {
        return ERR_FAILED;
    }
    // needlua=0 的命令照旧透传到目标
    SNPRINTF(url, sizeof(url), "/%"PRIu64"/stat", (uint64_t)arg->noreq);
    if (ERR_OK != _http_get(task, arg->port, url, 200, "MTYPE", 0)) {
        return ERR_FAILED;
    }
    // 广播：本二进制里全是 C task，逐个都该是同一句不支持
    if (ERR_OK != _http_get(task, arg->port, "/0/mem", 200, _NOTLUA, 0)) {
        return ERR_FAILED;
    }
    return ERR_OK;
}

static void _startup(task_ctx *task) {
    task_debug_args *arg = (task_debug_args *)coro_get_arg(task);
    if (ERR_OK != _test_builtin(task, arg->noreq)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_loglv(task, arg->noreq)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_passthrough(task, arg->noreq, arg->hasreq)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    // console 的 task_listen 异步落地，同 mqtt_test3/4 的处理：先等一会儿再连
    coro_sleep(task, 300);
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_console(task, arg)) {
        return;
    }
    *(arg->ok) = 1;
    LOG_INFO("debug tested.");
}

void task_debug_start(loader_ctx *loader, const char *name, uint16_t port, int32_t *ok) {
    if (NULL == ok) {
        return;
    }
    task_ctx *noreq = coro_task_register(loader, "debug_noreq", 0, _noreq_startup, NULL, NULL, NULL);
    task_ctx *hasreq = coro_task_register(loader, "debug_hasreq", 0, _hasreq_startup, NULL, NULL, NULL);
    if (NULL == noreq || NULL == hasreq) {
        LOG_ERROR("debug test: target task register failed.");
        return;
    }
    task_debug_args *arg;
    CALLOC(arg, 1, sizeof(task_debug_args));
    arg->port = port;
    arg->ok = ok;
    arg->noreq = noreq->handle;
    arg->hasreq = hasreq->handle;
    coro_task_register(loader, name, 0, _startup, NULL, _free, arg);
}
