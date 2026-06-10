#include "services/debug_console.h"
#include "srey/spub.h"
#include "srey/loader.h"
#include "serial/seri.h"
#include "protocol/http.h"
#include "utils/router.h"
#include "event/event.h"
#include "utils/binary.h"
#include "utils/utils.h"
#include "utils/log.h"

// task 列表收集项 + 动态数组（用于 /__alive 与广播）
typedef struct dbg_task {
    name_t handle;   // task 句柄
    char name[64];   // task 名；匿名 task 为空串
}dbg_task;
typedef struct dbg_tasklist {
    int32_t n;       // 当前元素数
    int32_t cap;     // 已分配容量
    dbg_task *items; // 元素数组（REALLOC 倍增，调用方 FREE）
}dbg_tasklist;
// 广播单个 task 的 fork 参数；coro_request 响应在协程结束后失效，须复制到 resp 堆
typedef struct bcast_arg {
    size_t bsize;      // 命令字节数
    size_t resp_len;   // 输出：响应字节数
    name_t handle;     // 目标 task 句柄
    void *body;        // seri 命令（所有 fork 共享，copy=1 不转移）
    char *resp;        // 输出：响应文本（MALLOC，聚合后 FREE）；不可达为 NULL
}bcast_arg;
// debug_console 实例上下文（每 task 堆分配，存 task->arg，由 coro_get_arg 取）
typedef struct debug_console_ctx {
    uint16_t port;       // 监听端口
    size_t html_len;     // HTML 字节数
    uint64_t lsnid;      // 监听 ID（ev_unlisten 用）
    router_ctx *router;  // HTTP 路由器
    char *html;          // 调试 UI 页面（启动时 readall 一次，失败为 NULL）
    char ip[IP_LENS];    // 监听 IP
}debug_console_ctx;

// 调试命令用法说明（对齐前端 UI 与 debug_request 处理器）
static const char *_HELP =
    "GET  /{handle}/help            this help\n"
    "GET  /{handle}/mem             Lua GC memory (KB)\n"
    "GET  /{handle}/gc              force GC, show freed amount\n"
    "GET  /{handle}/stat            per-mtype dispatch stat (nmsg, dispatch_cpu_ns, avg_ns + total)\n"
    "GET  /{handle}/coros           list suspended coros clustered by stack\n"
    "GET  /{handle}/loglv/{lv}      set log level (0=FATAL 1=ERROR 2=WARN 3=INFO 4=DEBUG)\n"
    "POST /{handle}/inject          inject Lua; body=source, _U holds upvalues\n"
    "POST /{handle}/hotfix/{module} hotfix module; body=patch source\n";

// qsort 比较器：按 name 升序
static int32_t _debug_cmp_name(const void *a, const void *b) {
    return strcmp(((const dbg_task *)a)->name, ((const dbg_task *)b)->name);
}
// qsort 比较器：按 handle 升序
static int32_t _debug_cmp_handle(const void *a, const void *b) {
    name_t ha = ((const dbg_task *)a)->handle;
    name_t hb = ((const dbg_task *)b)->handle;
    return (ha > hb) - (ha < hb);
}
// 把命令位置化打包进 bw：首元素 cmd 字符串（与 C/Lua 的 REQ_DEBUG seri 解码对齐）
static void _debug_pack_cmd(binary_ctx *bw, const char *cmd) {
    binary_init(bw, NULL, 0, 0);
    seri_append_string(bw, cmd, strlen(cmd));
}
// 广播 fork 协程：向单个 task 发 REQ_DEBUG，响应复制进 arg->resp
static void _debug_bcast_one(task_ctx *task, void *arg) {
    bcast_arg *ba = arg;
    ba->resp = NULL;
    ba->resp_len = 0;
    task_ctx *dst = task_grab(task->loader, ba->handle);
    if (NULL == dst) {
        return;
    }
    int32_t err = ERR_FAILED;
    size_t rlen = 0;
    void *rtn = coro_request(dst, task, REQ_DEBUG, ba->body, ba->bsize, 1, &err, &rlen);
    task_ungrab(dst);
    if (ERR_OK == err && NULL != rtn && rlen > 0) {
        MALLOC(ba->resp, rlen);
        memcpy(ba->resp, rtn, rlen);
        ba->resp_len = rlen;
    }
}
// loader_task_each 回调：把 {name, handle} 追加进动态数组
static void _debug_tasklist(const char *name, name_t handle, void *arg) {
    dbg_tasklist *tl = arg;
    if (tl->n >= tl->cap) {
        int32_t ncap = (0 == tl->cap) ? 16 : tl->cap * 2;
        REALLOC(tl->items, tl->items, sizeof(dbg_task) * (size_t)ncap);
        tl->cap = ncap;
    }
    dbg_task *it = &tl->items[tl->n++];
    it->handle = handle;
    if (NULL != name) {
        safe_fill_str(it->name, sizeof(it->name), name);
    } else {
        it->name[0] = '\0';
    }
}
// 广播命令到所有 task（coro_fork_wait 并发），按 name 升序聚合响应（"name:\n<resp 或 (unavailable)>\n"）
static void _debug_broadcast(router_req *ctx, void *body, size_t bsize) {
    task_ctx *task = ctx->task;
    dbg_tasklist tl = { 0, 0, NULL };
    loader_task_each(task->loader, _debug_tasklist, &tl);
    if (0 == tl.n) {
        router_req_text(ctx, 200, "(no task)\n", strlen("(no task)\n"));
        FREE(tl.items);
        return;
    }
    qsort(tl.items, (size_t)tl.n, sizeof(dbg_task), _debug_cmp_name);
    bcast_arg *bargs;
    MALLOC(bargs, sizeof(bcast_arg) * (size_t)tl.n);
    void (**funcs)(task_ctx *, void *);
    MALLOC(funcs, sizeof(void *) * (size_t)tl.n);
    void **args;
    MALLOC(args, sizeof(void *) * (size_t)tl.n);
    int32_t i;
    for (i = 0; i < tl.n; i++) {
        bargs[i].handle = tl.items[i].handle;
        bargs[i].body = body;
        bargs[i].bsize = bsize;
        bargs[i].resp = NULL;
        bargs[i].resp_len = 0;
        funcs[i] = _debug_bcast_one;
        args[i] = &bargs[i];
    }
    coro_fork_wait(task, tl.n, funcs, args);
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    for (i = 0; i < tl.n; i++) {
        const char *nm = ('\0' == tl.items[i].name[0]) ? "(anonymous)" : tl.items[i].name;
        binary_set_va(&bw, "%s:\n", nm);
        if (NULL != bargs[i].resp && bargs[i].resp_len > 0) {
            binary_set_string(&bw, bargs[i].resp, bargs[i].resp_len);
            FREE(bargs[i].resp);
        } else {
            binary_set_string(&bw, "(unavailable)", strlen("(unavailable)"));
        }
        binary_set_string(&bw, "\n", 1);
    }
    router_req_text(ctx, 200, bw.data, bw.offset);
    binary_free(&bw);
    FREE(bargs);
    FREE(funcs);
    FREE(args);
    FREE(tl.items);
}
// handler 公共转发：取 handle，单发直接 coro_request、handle=0 广播；cmd 由本函数接管，完成后 binary_free
static void _debug_forward(router_req *ctx, binary_ctx *cmd) {
    size_t n = 0;
    const char *ts = router_req_param(ctx, "task", &n);
    if (NULL == ts) {
        router_req_text(ctx, 404, "invalid task handle\n", strlen("invalid task handle\n"));
        binary_free(cmd);
        return;
    }
    char hbuf[64];
    size_t hn = (n < sizeof(hbuf) - 1) ? n : (sizeof(hbuf) - 1);
    memcpy(hbuf, ts, hn);
    hbuf[hn] = '\0';
    char *endp = NULL;
    name_t handle = (name_t)strtoull(hbuf, &endp, 10);
    if (endp == hbuf || '\0' != *endp) {
        router_req_text(ctx, 404, "invalid task handle\n", strlen("invalid task handle\n"));
        binary_free(cmd);
        return;
    }
    if (0 == handle) {//广播
        _debug_broadcast(ctx, cmd->data, cmd->offset);
        binary_free(cmd);
        return;
    }
    task_ctx *dst = task_grab(ctx->task->loader, handle);
    if (NULL == dst) {
        router_req_text(ctx, 503, "task unavailable\n", strlen("task unavailable\n"));
        binary_free(cmd);
        return;
    }
    int32_t err = ERR_FAILED;
    size_t rlen = 0;
    void *rtn = coro_request(dst, ctx->task, REQ_DEBUG, cmd->data, cmd->offset, 1, &err, &rlen);
    task_ungrab(dst);
    if (ERR_OK != err || NULL == rtn) {
        router_req_text(ctx, 503, "task unavailable or timeout\n", strlen("task unavailable or timeout\n"));
    } else {
        router_req_text(ctx, 200, rtn, rlen);
    }
    binary_free(cmd);
}
// GET / → 调试 UI 页面
static void _debug_root(router_req *ctx) {
    debug_console_ctx *dc = coro_get_arg(ctx->task);
    if (NULL != dc->html) {
        router_req_html(ctx, 200, dc->html, dc->html_len);
    } else {
        router_req_text(ctx, 503, "debug_console.html not loaded\n", strlen("debug_console.html not loaded\n"));
    }
}
// GET /__alive：每行 "handle\tname"（按 handle 升序）；供前端建 name→handle 映射 + 下拉
static void _debug_alive(router_req *ctx) {
    dbg_tasklist tl = { 0, 0, NULL };
    loader_task_each(ctx->task->loader, _debug_tasklist, &tl);
    qsort(tl.items, (size_t)tl.n, sizeof(dbg_task), _debug_cmp_handle);
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    int32_t i;
    for (i = 0; i < tl.n; i++) {
        binary_set_va(&bw, "%"PRIu64"\t%s\n",
            tl.items[i].handle, ('\0' == tl.items[i].name[0]) ? "" : tl.items[i].name);
    }
    router_req_text(ctx, 200, bw.data, bw.offset);
    binary_free(&bw);
    FREE(tl.items);
}
// GET /__cmem：C 层全局内存分配统计（MEMORY_CHECK 关闭时全为 0）
static void _debug_cmem(router_req *ctx) {
    uint64_t nalloc = 0;
    uint64_t nfree = 0;
    mem_stat(&nalloc, &nfree);
    char buf[160];
    int32_t n = SNPRINTF(buf, sizeof(buf),
        "nalloc: %"PRIu64"\nnfree:  %"PRIu64"\ninuse:  %"PRIu64"\n",
        nalloc, nfree, (nalloc >= nfree) ? (nalloc - nfree) : 0);
    router_req_text(ctx, 200, buf, (size_t)n);
}
// GET /{handle}/help：静态用法文本
static void _debug_help(router_req *ctx) {
    router_req_text(ctx, 200, _HELP, strlen(_HELP));
}
// GET /{handle}/mem：查询目标 task Lua VM 的 GC 内存占用 (KB)
static void _debug_mem(router_req *ctx) {
    binary_ctx cmd;
    _debug_pack_cmd(&cmd, "mem");
    _debug_forward(ctx, &cmd);
}
// GET /{handle}/gc：令目标 task 强制 Lua GC，返回释放量
static void _debug_gc(router_req *ctx) {
    binary_ctx cmd;
    _debug_pack_cmd(&cmd, "gc");
    _debug_forward(ctx, &cmd);
}
// GET /{handle}/stat：目标 task 各 mtype 的消息派发统计 (nmsg/cpu_ns/avg + 合计)
static void _debug_stat(router_req *ctx) {
    binary_ctx cmd;
    _debug_pack_cmd(&cmd, "stat");
    _debug_forward(ctx, &cmd);
}
// GET /{handle}/coros：列出目标 task 挂起的协程 (按栈聚类)
static void _debug_coros(router_req *ctx) {
    binary_ctx cmd;
    _debug_pack_cmd(&cmd, "coros");
    _debug_forward(ctx, &cmd);
}
// GET /{handle}/loglv/{lv}：lv 须 0-4
static void _debug_loglv(router_req *ctx) {
    size_t n = 0;
    const char *lv_s = router_req_param(ctx, "lv", &n);
    char lbuf[8];
    size_t ln = (NULL == lv_s) ? 0 : ((n < sizeof(lbuf) - 1) ? n : (sizeof(lbuf) - 1));
    memcpy(lbuf, (NULL == lv_s) ? "" : lv_s, ln);
    lbuf[ln] = '\0';
    int32_t lv = (int32_t)strtol(lbuf, NULL, 10);
    if (lv < 0 || lv > 4) {
        router_req_text(ctx, 400, "usage: /{handle}/loglv/<0-4>\n", strlen("usage: /{handle}/loglv/<0-4>\n"));
        return;
    }
    binary_ctx cmd;
    _debug_pack_cmd(&cmd, "loglv");
    seri_append_int(&cmd, lv);
    _debug_forward(ctx, &cmd);
}
// POST /{handle}/inject：body 为 Lua 源码
static void _debug_inject(router_req *ctx) {
    size_t blen = 0;
    void *body = router_req_body(ctx, &blen);
    if (NULL == body || 0 == blen) {
        router_req_text(ctx, 400, "no code provided\n", strlen("no code provided\n"));
        return;
    }
    binary_ctx cmd;
    _debug_pack_cmd(&cmd, "inject");
    seri_append_string(&cmd, (const char *)body, blen);
    _debug_forward(ctx, &cmd);
}
// POST /{handle}/hotfix/{module}：body 为补丁源码
static void _debug_hotfix(router_req *ctx) {
    size_t blen = 0;
    void *body = router_req_body(ctx, &blen);
    if (NULL == body || 0 == blen) {
        router_req_text(ctx, 400, "no patch source\n", strlen("no patch source\n"));
        return;
    }
    size_t mn = 0;
    const char *mod = router_req_param(ctx, "module", &mn);
    binary_ctx cmd;
    _debug_pack_cmd(&cmd, "hotfix");
    seri_append_string(&cmd, (NULL == mod) ? "" : mod, (NULL == mod) ? 0 : mn);
    seri_append_string(&cmd, (const char *)body, blen);
    _debug_forward(ctx, &cmd);
}
// HTTP 接收回调：完整请求到达后交 router 派发
static void _net_recv(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype,
                      uint8_t client, uint8_t slice, void *data, size_t size) {
    (void)pktype;
    (void)client;
    (void)size;
    if (0 != slice) {
        return;
    }
    debug_console_ctx *ctx = coro_get_arg(task);
    router_dispatch(ctx->router, task, fd, skid, (struct http_pack_ctx *)data);
}
// 启动回调：建路由器 + 注册路由 + 监听 HTTP
static void _debug_startup(task_ctx *task) {
    debug_console_ctx *ctx = coro_get_arg(task);
    task_recved(task, _net_recv);
    ctx->router = router_new();
    if (NULL == ctx->router) {
        LOG_ERROR("debug_console router_new failed.");
        return;
    }
    router_get(ctx->router, NULL, "/", _debug_root, NULL, 0);
    router_get(ctx->router, NULL, "/__alive", _debug_alive, NULL, 0);
    router_get(ctx->router, NULL, "/__cmem", _debug_cmem, NULL, 0);
    router_get(ctx->router, NULL, "/{task}/help", _debug_help, NULL, 0);
    router_get(ctx->router, NULL, "/{task}/mem", _debug_mem, NULL, 0);
    router_get(ctx->router, NULL, "/{task}/gc", _debug_gc, NULL, 0);
    router_get(ctx->router, NULL, "/{task}/stat", _debug_stat, NULL, 0);
    router_get(ctx->router, NULL, "/{task}/coros", _debug_coros, NULL, 0);
    router_get(ctx->router, NULL, "/{task}/loglv/{lv}", _debug_loglv, NULL, 0);
    router_post(ctx->router, NULL, "/{task}/inject", _debug_inject, NULL, 0);
    router_post(ctx->router, NULL, "/{task}/hotfix/{module}", _debug_hotfix, NULL, 0);
    if (ERR_OK != task_listen(task, PACK_HTTP, NULL, ctx->ip, ctx->port, &ctx->lsnid, 0)) {
        LOG_ERROR("debug_console task_listen %s:%d error.", ctx->ip, ctx->port);
    } else {
        LOG_INFO("debug_console on %s:%d", ctx->ip, ctx->port);
    }
}
// 关闭回调：取消监听
static void _debug_closing(task_ctx *task) {
    debug_console_ctx *ctx = coro_get_arg(task);
    if (NULL == ctx) {
        return;
    }
    if (0 != ctx->lsnid) {
        ev_unlisten(&task->loader->netev, ctx->lsnid);
        ctx->lsnid = 0;
    }
}
// 释放用户数据（argfree，由 coro 层在 task_free 时调）：释放路由器、HTML 与实例
static void _debug_free(void *arg) {
    if (NULL == arg) {
        return;
    }
    debug_console_ctx *ctx = (debug_console_ctx *)arg;
    if (NULL != ctx->router) {
        router_free(ctx->router);
    }
    FREE(ctx->html);
    FREE(ctx);
}
int32_t debug_console_start(loader_ctx *loader, const char *name, const char *ip, uint16_t port) {
    if (EMPTYSTR(name) || 0 == port) {
        return ERR_OK;
    }
    if (NULL == ip) {
        return ERR_FAILED;
    }
    size_t iplens = strlen(ip);
    if (0 == iplens || iplens >= IP_LENS) {
        return ERR_FAILED;
    }
    debug_console_ctx *ctx;
    CALLOC(ctx, 1, sizeof(debug_console_ctx));
    ctx->port = port;
    safe_fill_str(ctx->ip, sizeof(ctx->ip), ip);
    // 读取调试 UI （html/debug_console.html）；失败仅警告，/ 端点回提示
    char path[PATH_LENS];
    SNPRINTF(path, sizeof(path), "%s%shtml%sdebug_console.html",
        procpath(), PATH_SEPARATORSTR, PATH_SEPARATORSTR);
    ctx->html = readall(path, &ctx->html_len);
    if (NULL == ctx->html) {
        LOG_WARN("debug_console cannot read %s, UI page disabled.", path);
    }
    if (NULL == coro_task_register(loader, name, ONEK,
                                   _debug_startup, _debug_closing,
                                   _debug_free, ctx)) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
