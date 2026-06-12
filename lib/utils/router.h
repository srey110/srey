#ifndef ROUTER_H_
#define ROUTER_H_

// ─────────────────────────────────────────────────────────────────────────────
// HTTP 路由器 (基于 task + http_pack_ctx)
// 用途
//   task 内监听 PACK_HTTP, _net_recv 收到完整 HTTP 包后调 router_dispatch,
//   按方法 + 路径模板派发到 handler, 期间穿过若干中间件 (前置 / 后置都行)。
//   对标 Express / Laravel Route, 但 C 风格 + 同步执行。
// 核心概念
//   router_ctx   路由器实例, 持有路由表 + 全局中间件 + 具名中间件登记
//   router_entry 一条路由记录 (method + path + handler + 路由级中间件)
//   router_group 分组对象 (栈), prefix + 中间件; 嵌套通过 parent 指针向上找
//   router_req   单次请求上下文; dispatch 期间栈分配, handler / 中间件读写
//   router_cb    handler 和中间件的统一签名 void (*)(router_req *)
// 路径模板
//   /foo/bar         字面量精确匹配
//   /user/{id}       {name} 必填路径参数, router_req_param("id", ...) 取
//   /file/{path?}    {name?} 可选路径参数, 缺失时取不到值但仍匹配
//   /static/*        末尾通配, 一旦命中后续任意请求段都吃下
//   多条同 path 不同 method 算独立路由, 方法位掩码 ROUTER_M_GET|ROUTER_M_POST 也支持
// 中间件 (洋葱模型)
//   注册顺序:        全局中间件 → 分组中间件 (父→子) → 路由级中间件 → handler
//   每个中间件主动调 router_next(ctx) 进入下一层, 不调即截断 (后续不执行)
//   router_next(ctx) 同步返回后可继续做后置处理 (打日志 / 写统计 / 改响应)
//   具名中间件 (router_define) 可在路由 / group 的 mws 数组中以字符串引用,
//   也可 router_use_fn 直传函数指针不入名表
// 完整使用流程
//   ─────────────────────────────────────────────────────────────────────────
//   static router_ctx *g_router = NULL;
//   static void mw_auth(router_req *ctx) {
//       size_t n; char *t = router_req_header(ctx, "X-Token", &n);
//       // 长度必须严格相等再 memcmp; n != 6 时 memcmp 读 "secret" 越界 + 短 token 可前缀通过
//       if (NULL == t || 6 != n || 0 != memcmp(t, "secret", 6)) {
//           router_req_text(ctx, 401, "no", 2);
//           return;          // 不调 router_next → 截断
//       }
//       router_next(ctx);    // 继续后续中间件 / handler
//   }
//   static void h_user(router_req *ctx) {
//       size_t n; const char *id = router_req_param(ctx, "id", &n);
//       router_req_text(ctx, 200, id, n);
//   }
//   static void _net_recv(task_ctx *task, SOCKET fd, uint64_t skid,
//                         uint8_t pktype, uint8_t client, uint8_t slice,
//                         void *data, size_t size) {
//       if (0 != slice) { return; }
//       router_dispatch(g_router, task, fd, skid, (struct http_pack_ctx *)data);
//   }
//   static void _startup(task_ctx *task) {
//       task_recved(task, _net_recv);
//       g_router = router_new();
//       // 1) 注册具名中间件
//       router_define(g_router, "auth", mw_auth);
//       // 2) 全局中间件 (对所有路由生效)
//       router_use(g_router, "auth");                 // 按名引用
//       // router_use_fn(g_router, mw_logger);        // 或函数指针直传
//       // 3) 路由 (无 group / 无路由级中间件)
//       router_get(g_router, NULL, "/user/{id}", h_user, NULL, 0);
//       // 4) 带路由级中间件
//       const char *mws[] = { "rate_limit" };
//       router_post(g_router, NULL, "/upload", h_upload, mws, 1);
//       // 5) 分组 (栈对象, 嵌套靠 parent 链)
//       const char *api_mws[] = { "auth" };
//       router_group api;
//       router_group_root(g_router, &api, "/api", api_mws, 1);
//       router_get(g_router, &api, "/users", h_list, NULL, 0);   // → /api/users
//       router_group admin;
//       router_group_nest(&api, &admin, "/admin", NULL, 0);
//       router_get(g_router, &admin, "/stats", h_stats, NULL, 0);// → /api/admin/stats
//       task_listen(task, PACK_HTTP, NULL, "0.0.0.0", 8080, ...);
//   }
//   static void _closing(task_ctx *task) {
//       router_free(g_router);
//   }
//   ─────────────────────────────────────────────────────────────────────────
// 请求访问 (在 handler / 中间件内)
//   router_req_header(ctx, key, &lens)   取请求头 (大小写不敏感)
//   router_req_param (ctx, key, &lens)   取路径参数 ({name} / {name?})
//   router_req_query (ctx, key, &lens)   取 URL ?key=value 参数
//   router_req_body  (ctx, &lens)        取请求体原始指针
// 响应辅助
//   router_req_text  (ctx, code, body, lens)        text/plain
//   router_req_json  (ctx, code, json, lens)        application/json (JSON 由调用方预编码)
//   router_req_html  (ctx, code, body, lens)        text/html
//   router_req_respond(ctx, code, extra, n, body, len)  自定义头 + 报文体
//   调用任一辅助即视为已响应, dispatch 末尾不再兜底 500
// 同步约束 (重要)
//   普通 task (task_new + task_register): handler / 中间件在 _net_recv 调用栈中
//   同步执行, 不能调 coro_send / coro_sleep 等会 yield 的 API —— ctx 是 dispatch
//   函数的栈对象, yield 期间 ctx 失效。
//   如需异步: handler 内 coro_fork 把 fd/skid/响应所需数据复制到堆参数,
//   立即置 ctx->responded = 1 防止兜底 500, handler 返回, 异步处理完后由
//   fork 出的协程自己用 binary_init + http_pack_resp + ev_send 写响应。
//   协程 task (coro_task_register): _net_recv 已在协程栈, router_dispatch 及栈上
//   的 ctx 跨 yield 保留, handler 可直接 coro_request / coro_fork_wait, 返回前再用
//   ctx 写响应 (见 lib/services/harbor.c、lib/services/debug_console.c)。
// 错误响应
//   未匹配路由        → 404 Not Found
//   方法不识别        → 405 Method Not Allowed
//   中间件 + handler 都没写响应  → 500 Internal Server Error (兜底)
//   兜底仅救援"漏写响应"; 段错误等不可恢复异常仍会崩 (C 无 setjmp 救援)
// 生命周期
//   router_ctx       router_new ~ router_free, 跨 task 整个生命周期
//   router_group     栈对象, 仅在 router_add 期间使用; prefix / mw_names 字符串
//                    生命周期需跨过所有相关 router_add 调用 (用字面量 / 静态数组即可)
//   router_req       栈对象, dispatch 调用期间有效, 不可跨 yield 持有
//   params[].key     指向 router_ctx 内部路径模板, 跟 router_new ~ router_free 同生命周期
//   params[].val     指向 ctx->url_storage 内部, 跟 dispatch 调用同生命周期
//   query 返回值     指向 ctx->url_storage 内部, 跟 dispatch 调用同生命周期
//   header / body    指向 pack 内部, pack 在 _net_recv 返回后失效
// ─────────────────────────────────────────────────────────────────────────────

#include "protocol/http.h"
#include "protocol/urlparse.h"
#include "srey/spub.h"
#include "event/event.h"

// 路径参数 / chain 数组上限
#define ROUTER_MAX_PARAMS  16
#define ROUTER_MAX_CHAIN   16

// HTTP 方法位掩码; 单个路由可通过按位或组合 (ROUTER_M_ANY 匹配所有方法)
typedef enum router_method {
    ROUTER_M_GET     = 1 << 0,
    ROUTER_M_POST    = 1 << 1,
    ROUTER_M_PUT     = 1 << 2,
    ROUTER_M_DELETE  = 1 << 3,
    ROUTER_M_PATCH   = 1 << 4,
    ROUTER_M_HEAD    = 1 << 5,
    ROUTER_M_OPTIONS = 1 << 6,
    ROUTER_M_ANY     = 0xFF
} router_method;

typedef struct router_ctx router_ctx;
typedef struct router_entry router_entry;
typedef struct router_req router_req;
// 路由 handler / 中间件统一签名;handler 不调 next, 中间件主动调 router_next(ctx) 推进链路, 不调即截断
typedef void (*router_cb)(router_req *ctx);
// 路径参数键值对 (仅用于 router_req::params);
// key 指向 router_entry::segs[].str  (router_ctx 持有, 跟 router_new ~ router_free 同生命周期)
// val 指向 ctx->url_storage.buf 内部 (栈对象, 跟 dispatch 调用同生命周期)
// 调用方不得释放
typedef struct router_kv {
    uint32_t key_len;
    uint32_t val_len;
    const char *key;
    const char *val;
} router_kv;
// 请求上下文; 字段在 router_dispatch 中填充, handler / 中间件读取并通过
// router_req_* 辅助函数写响应。栈分配, 生命周期与 dispatch 调用一致 —— handler
// 内若需异步处理 (如 coro_send), 须自行 coro_fork 并把 fd/skid 等拷贝到堆,
// 同时置 ctx->responded = 1 防止兜底 500
struct router_req {
    int32_t chain_n;    // 链元素数量
    int32_t chain_i;    // next 推进游标
    int32_t params_n;   // 路径参数数量
    int32_t responded;  // 响应已写出标志 (用于兜底 500)
    uint32_t path_len;
    router_method method;     // 当前请求方法位掩码
    SOCKET fd;
    uint64_t skid;
    task_ctx *task;       // 当前 task
    struct http_pack_ctx *pack;       // 原始 http 包, 供 http_data / http_header 访问
    void *user;       // 中间件间传值, 用户自管
    const char *path;       // 解析后的路径 (指向 url_storage 内部)
    router_cb chain[ROUTER_MAX_CHAIN]; // 中间件 + handler 拼接链
    router_kv params[ROUTER_MAX_PARAMS]; // {name} / {name?} 提取结果
    url_ctx  url_storage; // URL 解析结果 (内部使用)
};
// 分组对象 (栈分配, 调用方持有);  prefix / mws 仅持引用, 调用方需保证生命周期
// 跨过所有 router_* 注册调用。嵌套通过 router_group_nest 派生, 父对象不可变
typedef struct router_group {
    int32_t mw_names_n;
    uint32_t prefix_len;
    const struct router_group *parent;
    router_ctx *router;
    const char *prefix;
    const char *const *mw_names; // 中间件名数组, 由 _resolve 在 dispatch 前查表
} router_group;

/// <summary>
/// 创建路由器
/// </summary>
/// <returns>router_ctx 指针</returns>
router_ctx *router_new(void);
/// <summary>
/// 释放路由器及全部已注册路由 / 中间件资源
/// </summary>
/// <param name="r">router_ctx</param>
void router_free(router_ctx *r);
/// <summary>
/// 注册具名中间件; 后续可在 router_use / 路由 mws / group mw_names 中以
/// 字符串引用 (字符串严格匹配, 大小写敏感)。同名再次 define 直接覆盖 named 表项;
/// 但 router_use / router_add 是"注册时快照": 调用瞬间 _resolve_mw 把函数指针
/// 存进 global_mw / entry->mws, 之后再 define 不会影响已注册的路由 / 全局中间件
/// </summary>
/// <param name="r">router_ctx</param>
/// <param name="name">中间件名</param>
/// <param name="fn">中间件函数</param>
void router_define(router_ctx *r, const char *name, router_cb fn);
/// <summary>
/// 注册全局中间件 (具名查表); 对所有路由生效, 按注册顺序追加到链头
/// </summary>
/// <param name="r">router_ctx</param>
/// <param name="name">具名中间件名, 必须已 define</param>
void router_use(router_ctx *r, const char *name);
/// <summary>
/// 注册全局中间件 (函数指针直传, 不入命名表)
/// </summary>
/// <param name="r">router_ctx</param>
/// <param name="fn">中间件函数</param>
void router_use_fn(router_ctx *r, router_cb fn);
/// <summary>
/// 初始化根分组; 后续 router_* 注册时传该 group 即可继承 prefix 和 mws
/// </summary>
/// <param name="r">router_ctx</param>
/// <param name="g">待初始化的 router_group (调用方栈分配)</param>
/// <param name="prefix">路径前缀, 例 "/api"; 调用方持有生命周期</param>
/// <param name="mw_names">中间件名数组; NULL 表示无中间件</param>
/// <param name="n">mw_names 数量</param>
void router_group_root(router_ctx *r, router_group *g, const char *prefix,
                       const char *const *mw_names, int32_t n);
/// <summary>
/// 在父分组基础上嵌套初始化; 子分组继承父 prefix 和 mws, 自身追加
/// </summary>
/// <param name="parent">父 group</param>
/// <param name="g">待初始化的 router_group (调用方栈分配)</param>
/// <param name="prefix">追加路径前缀</param>
/// <param name="mw_names">追加中间件名数组</param>
/// <param name="n">mw_names 数量</param>
void router_group_nest(const router_group *parent, router_group *g, const char *prefix,
                       const char *const *mw_names, int32_t n);
/// <summary>
/// 注册路由; method 为 router_method 位掩码, group=NULL 表示注册到根
/// path 支持 {name} 字面参数、{name?} 可选参数、* 末尾通配
/// </summary>
/// <param name="r">router_ctx</param>
/// <param name="g">分组, 可为 NULL</param>
/// <param name="method">方法位掩码</param>
/// <param name="path">路由路径</param>
/// <param name="h">handler</param>
/// <param name="mws">路由级中间件名数组, 可为 NULL</param>
/// <param name="mws_n">mws 数量</param>
/// <returns>路由条目, NULL 表示失败 (前缀或路径过长 / 段数超上限 / 通配符非末段 / 段格式非法); 返回指针仅即时有效, 下次 router_* 注册可能 realloc 路由表使其失效, 不可长期持有</returns>
router_entry *router_add(router_ctx *r, const router_group *g,
                         router_method method, const char *path,
                         router_cb h,
                         const char *const *mws, int32_t mws_n);
// 便捷注册接口: 单一方法
router_entry *router_get(router_ctx *r, const router_group *g, const char *path,
                         router_cb h, const char *const *mws, int32_t mws_n);
router_entry *router_post(router_ctx *r, const router_group *g, const char *path,
                          router_cb h, const char *const *mws, int32_t mws_n);
router_entry *router_put(router_ctx *r, const router_group *g, const char *path,
                         router_cb h, const char *const *mws, int32_t mws_n);
router_entry *router_delete(router_ctx *r, const router_group *g, const char *path,
                            router_cb h, const char *const *mws, int32_t mws_n);
router_entry *router_patch(router_ctx *r, const router_group *g, const char *path,
                           router_cb h, const char *const *mws, int32_t mws_n);
router_entry *router_head(router_ctx *r, const router_group *g, const char *path,
                          router_cb h, const char *const *mws, int32_t mws_n);
router_entry *router_options(router_ctx *r, const router_group *g, const char *path,
                             router_cb h, const char *const *mws, int32_t mws_n);
router_entry *router_any(router_ctx *r, const router_group *g, const char *path,
                         router_cb h, const char *const *mws, int32_t mws_n);
/// <summary>
/// 派发 HTTP 请求 —— 在 _net_recv 中 slice == 0 分支调用; 内部完成方法 / 路径
/// 匹配, 拼接 chain, 启动中间件链。方法不识别 → 405; 路径未匹配 → 404;
/// 中间件 / handler 漏写响应 → 兜底 500 (C 无 setjmp 救援, 段错误等不可恢复异常仍会崩溃)
/// </summary>
/// <param name="r">router_ctx</param>
/// <param name="task">task</param>
/// <param name="fd">socket fd</param>
/// <param name="skid">连接 skid</param>
/// <param name="pack">http_pack_ctx 指针 (即 _net_recv 的 data 参数)</param>
void router_dispatch(router_ctx *r, task_ctx *task,
                     SOCKET fd, uint64_t skid,
                     struct http_pack_ctx *pack);
/// <summary>
/// 中间件链推进; 中间件内调用即执行下一节点 (handler 或下一个中间件),
/// next 返回后可继续做后置处理。handler 不应调用
/// </summary>
/// <param name="ctx">router_req</param>
void router_next(router_req *ctx);
/// <summary>
/// 取请求头; 大小写不敏感匹配
/// </summary>
/// <param name="ctx">router_req</param>
/// <param name="key">header 名</param>
/// <param name="lens">输出值长度</param>
/// <returns>值指针 (pack 内部, 不复制); 未找到返回 NULL</returns>
char *router_req_header(router_req *ctx, const char *key, size_t *lens);
/// <summary>
/// 取路径参数 (来自 {name} / {name?})
/// </summary>
/// <param name="ctx">router_req</param>
/// <param name="key">参数名</param>
/// <param name="lens">输出值长度</param>
/// <returns>值指针; 未找到返回 NULL</returns>
const char *router_req_param(router_req *ctx, const char *key, size_t *lens);
/// <summary>
/// 取 URL query 参数 (?a=1&amp;b=2)
/// </summary>
/// <param name="ctx">router_req</param>
/// <param name="key">参数名</param>
/// <param name="lens">输出值长度</param>
/// <returns>值指针; 未找到返回 NULL</returns>
const char *router_req_query(router_req *ctx, const char *key, size_t *lens);
/// <summary>
/// 取请求 body
/// </summary>
/// <param name="ctx">router_req</param>
/// <param name="lens">输出 body 长度</param>
/// <returns>body 指针; 无 body 返回 NULL</returns>
void *router_req_body(router_req *ctx, size_t *lens);
/// <summary>
/// text/plain 响应
/// </summary>
/// <param name="ctx">router_req</param>
/// <param name="code">状态码</param>
/// <param name="body">报文体, NULL 表示空</param>
/// <param name="lens">报文体长度</param>
void router_req_text(router_req *ctx, int32_t code, const char *body, size_t lens);
/// <summary>
/// application/json 响应; 调用方传预编码 JSON 字符串, 本函数仅包 Content-Type / Length
/// </summary>
/// <param name="ctx">router_req</param>
/// <param name="code">状态码</param>
/// <param name="json">JSON 字符串</param>
/// <param name="lens">JSON 长度</param>
void router_req_json(router_req *ctx, int32_t code, const char *json, size_t lens);
/// <summary>
/// text/html 响应
/// </summary>
/// <param name="ctx">router_req</param>
/// <param name="code">状态码</param>
/// <param name="body">HTML</param>
/// <param name="lens">长度</param>
void router_req_html(router_req *ctx, int32_t code, const char *body, size_t lens);
/// <summary>
/// 自定义响应; extra 为附加头, 不得包含 Content-Length / Content-Type / Transfer-Encoding
/// (本函数按 body_len 自动写 Content-Length)
/// </summary>
/// <param name="ctx">router_req</param>
/// <param name="code">状态码</param>
/// <param name="extra">附加头数组, NULL 表示无</param>
/// <param name="extra_n">附加头数量</param>
/// <param name="body">报文体</param>
/// <param name="body_len">报文体长度</param>
void router_req_respond(router_req *ctx, int32_t code,
                        const http_header_ctx *extra, int32_t extra_n,
                        const char *body, size_t body_len);

#endif // ROUTER_H_
