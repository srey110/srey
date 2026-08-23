#ifndef ROUTER_H_
#define ROUTER_H_

#include "protocol/http.h"
#include "protocol/urlparse.h"
#include "srey/spub.h"
#include "event/event.h"

// ─────────────────────────────────────────────────────────────────────────────
// HTTP 路由器 (基于 task + http_pack_ctx)
// 用途
//   task 内监听 PACK_HTTP, _net_recv 整串转 router_net_recv,
//   按方法 + 路径模板派发到 handler, 期间穿过若干中间件 (前置 / 后置都行)。
//   对标 Express / Laravel Route, 但 C 风格 + 同步执行。
//   chunked 请求只有流式路由 (router_add_stream) 收得下, 普通路由一律回 411;
//   注册了流式路由就必须把 _net_close 接到 router_closed (见该函数说明)。
// 核心概念
//   router_ctx   路由器实例, 持有路由表 + 全局中间件 + 具名中间件登记
//   router_entry 一条路由记录 (method + path + handler + 路由级中间件)
//   router_group 分组对象 (栈), prefix + 中间件; 嵌套通过 parent 指针向上找
//   router_req   单次请求上下文; dispatch 期间栈分配, handler / 中间件读写
//   router_cb    handler 和中间件的统一签名 void (*)(router_req *)
//   router_stream_cb 流式路由的数据回调 (请求体逐块到, router 不缓存)
// 路径模板
//   /foo/bar         字面量精确匹配
//   /user/{id}       {name} 必填路径参数, router_req_param("id", ...) 取
//   /file/{path?}    {name?} 可选路径参数, 缺失时取不到值但仍匹配
//                    匹配是精确的(非贪婪前瞻): 只要存在"某些 OPT 取值、其余缺失"的组合能让
//                    整条路径对齐就算命中, 能取到值的 OPT 优先取值; 单条路由最多 ROUTER_MAX_OPT 个
//   {name} 名字为空 (如 {} / {?}) 或内部含 '?' (如 {a?b}) 都不算参数, 整段退化为字面量匹配
//                    (注册照常成功, 只是该段按原文逐字比对)
//   /static/*        末尾通配, 一旦命中后续任意请求段都吃下
//   多条同 path 不同 method 算独立路由, 方法位掩码 ROUTER_M_GET|ROUTER_M_POST 也支持
// 线程约定
//   注册期 (router_add / router_use / router_define 等) 与派发期 (router_dispatch) 不可并发:
//   路由表是连续数组, 扩容会 realloc 整块, 派发方手里的 router_entry * 和正在扫的下标都会失效;
//   条目也是先入表后补 handler/中间件, 中途被读到就是半成品。按"启动期注册完再开始服务"用即可
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
//   static void _net_recv(task_ctx *task, sk_id *sk,
//                         subtype_t pktype, uint8_t client, uint8_t slice,
//                         void *data, size_t size) {
//       router_net_recv(g_router, task, sk, pktype, client, slice, data, size);
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
//   URL 解析失败      → 400 Bad Request (段数超 URL_MAX_PATH_DEPTH / URI 超长)
//   未匹配路由        → 404 Not Found
//   方法不识别        → 405 Method Not Allowed
//   chunked 打到普通路由 → 411 Length Required (router_net_recv 路径, 回完即关连接)
//   中间件 + handler 都没写响应  → 500 Internal Server Error (兜底)
//   兜底仅救援"漏写响应"; 段错误等不可恢复异常仍会崩 (C 无 setjmp 救援)
// 生命周期
//   router_ctx       router_new ~ router_free, 跨 task 整个生命周期
//   router_group     栈对象, 仅在 router_add 期间使用; prefix / mw_names 字符串
//                    生命周期需跨过所有相关 router_add 调用 (用字面量 / 静态数组即可)
//   router_req       栈对象, dispatch 调用期间有效, 不可跨 yield 持有;
//                    流式路由收 chunked 时 ctx 才是 router 持有的堆对象, 活到收齐或连接关闭,
//                    slice == 0 那次仍是栈对象
//   params[].key     指向 router_ctx 内部路径模板, 跟 router_new ~ router_free 同生命周期
//   params[].val     指向 ctx->url 内部, 跟 ctx 同生命周期
//   query 返回值     指向 ctx->url 内部, 跟 ctx 同生命周期
//   header / body    指向 pack 内部, pack 在 _net_recv 返回后失效;
//                    流式路由只有首帧有 pack, 之后两个访问器返 NULL
// ─────────────────────────────────────────────────────────────────────────────

// 路径参数 / chain 数组上限
#define ROUTER_MAX_PARAMS  16
#define ROUTER_MAX_CHAIN   16
// 单条路由内 {name?} 段数上限; 匹配用的可行性表按本值定维, 超出即注册失败
#define ROUTER_MAX_OPT     8
// 流式路由的中止通知, router 自造, 协议层不会产生这个值
// (msg.slice 只可能是 0 / PROT_SLICE_START / PROT_SLICE / PROT_SLICE_END)
#define ROUTER_STREAM_ABORT 0x80

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
/// <summary>
/// 流式路由的数据回调 (router_add_stream 注册)。slice 透传协议层分片状态，按它分支；
/// data 仅本次调用内有效；ctx 在"只调这一次 / 最后一次"的那次返回后即失效。
///   0                   非 chunked，data 即完整 body（可能为空），只调这一次，之后不会再有
///                       END / ABORT。要异步就 coro_fork 拷走 fd/skid 等，并置 ctx->responded = 1
///   PROT_SLICE_START    chunked 首帧，data 恒为 NULL
///   PROT_SLICE          chunked 数据块
///   PROT_SLICE_END      chunked 终止块，data 恒为 NULL
///   ROUTER_STREAM_ABORT 流没收齐就没了（连接断 / 同连接新流式首帧 / router_free），
///                       data 恒为 NULL，与 END 只会来一个。这是 ctx->user 最后的释放机会，
///                       只做清理：不写响应（无兜底 500），不用 ctx->task 投消息或挂起；
///                       该流已先摘表，回调内调 router_closed 是安全的 no-op
/// 请求头只在 slice == 0 与首帧（及之前的准入中间件）读得到，之后 router_req_header / _body
/// 一律返 NULL，要留就首帧拷进 ctx->user；路径参数与 query 每帧都在（含 ABORT）。
/// 与中间件一样不能挂起，理由见 router_add_stream
/// </summary>
typedef void (*router_stream_cb)(router_req *ctx, uint8_t slice, void *data, size_t lens);
// 路径参数键值对 (仅用于 router_req::params);
// key 指向 router_entry::segs[].str  (router_ctx 持有, 跟 router_new ~ router_free 同生命周期)
// val 指向 ctx->url->buf 内部 (栈对象, 跟 dispatch 调用同生命周期)
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
    int32_t admitted;   // 流式路由准入标志, router 内部填写
    router_method method;     // 当前请求方法位掩码
    task_ctx *task;       // 当前 task
    struct http_pack_ctx *pack;       // 原始 http 包, 供 http_data / http_header 访问;
                                      // 流式路由过了首帧即为 NULL
    void *user;       // 中间件间传值, 用户自管
    sk_id sk;                 // 连接标识 fd+skid
    router_cb chain[ROUTER_MAX_CHAIN]; // 中间件 + handler 拼接链
    router_kv params[ROUTER_MAX_PARAMS]; // {name} / {name?} 提取结果
    // URL 解析结果 (内部使用)。存储由调用方提供并在调用 router_match_index 前赋值:
    // url_ctx 有 4KB 出头, 内嵌会让本结构每次零初始化都白清一遍; 指针化后不必预先清零(见 url_parse)
    url_ctx *url;
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
/// 字符串引用 (严格匹配, 大小写敏感)。同名再次 define 直接覆盖 named 表项;
/// 但 router_use / router_add 注册时即快照函数指针, 之后再 define 不影响已注册的
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
/// <param name="h">handler, 不可为 NULL</param>
/// <param name="mws">路由级中间件名数组, 可为 NULL</param>
/// <param name="mws_n">mws 数量</param>
/// <returns>路由条目, NULL 表示失败 (handler 为空 / 前缀或路径过长 / 段数超上限 / 可选段超 ROUTER_MAX_OPT / 通配符非末段 / 段格式非法); 返回指针仅即时有效, 下次 router_* 注册可能 realloc 路由表使其失效, 不可长期持有</returns>
router_entry *router_add(router_ctx *r, const router_group *g,
                         router_method method, const char *path,
                         router_cb h,
                         const char *const *mws, int32_t mws_n);
/// <summary>等价于 router_add(r, g, ROUTER_M_GET, path, h, mws, mws_n)</summary>
/// <param name="r">router_ctx</param><param name="g">分组, 可为 NULL</param>
/// <param name="path">路由路径</param><param name="h">handler</param>
/// <param name="mws">路由级中间件名数组, 可为 NULL</param><param name="mws_n">mws 数量</param>
/// <returns>路由条目, NULL 表示失败</returns>
router_entry *router_get(router_ctx *r, const router_group *g, const char *path,
                         router_cb h, const char *const *mws, int32_t mws_n);
/// <summary>等价于 router_add(r, g, ROUTER_M_POST, path, h, mws, mws_n)</summary>
/// <param name="r">router_ctx</param><param name="g">分组, 可为 NULL</param>
/// <param name="path">路由路径</param><param name="h">handler</param>
/// <param name="mws">路由级中间件名数组, 可为 NULL</param><param name="mws_n">mws 数量</param>
/// <returns>路由条目, NULL 表示失败</returns>
router_entry *router_post(router_ctx *r, const router_group *g, const char *path,
                          router_cb h, const char *const *mws, int32_t mws_n);
/// <summary>等价于 router_add(r, g, ROUTER_M_PUT, path, h, mws, mws_n)</summary>
/// <param name="r">router_ctx</param><param name="g">分组, 可为 NULL</param>
/// <param name="path">路由路径</param><param name="h">handler</param>
/// <param name="mws">路由级中间件名数组, 可为 NULL</param><param name="mws_n">mws 数量</param>
/// <returns>路由条目, NULL 表示失败</returns>
router_entry *router_put(router_ctx *r, const router_group *g, const char *path,
                         router_cb h, const char *const *mws, int32_t mws_n);
/// <summary>等价于 router_add(r, g, ROUTER_M_DELETE, path, h, mws, mws_n)</summary>
/// <param name="r">router_ctx</param><param name="g">分组, 可为 NULL</param>
/// <param name="path">路由路径</param><param name="h">handler</param>
/// <param name="mws">路由级中间件名数组, 可为 NULL</param><param name="mws_n">mws 数量</param>
/// <returns>路由条目, NULL 表示失败</returns>
router_entry *router_delete(router_ctx *r, const router_group *g, const char *path,
                            router_cb h, const char *const *mws, int32_t mws_n);
/// <summary>等价于 router_add(r, g, ROUTER_M_PATCH, path, h, mws, mws_n)</summary>
/// <param name="r">router_ctx</param><param name="g">分组, 可为 NULL</param>
/// <param name="path">路由路径</param><param name="h">handler</param>
/// <param name="mws">路由级中间件名数组, 可为 NULL</param><param name="mws_n">mws 数量</param>
/// <returns>路由条目, NULL 表示失败</returns>
router_entry *router_patch(router_ctx *r, const router_group *g, const char *path,
                           router_cb h, const char *const *mws, int32_t mws_n);
/// <summary>等价于 router_add(r, g, ROUTER_M_HEAD, path, h, mws, mws_n)</summary>
/// <param name="r">router_ctx</param><param name="g">分组, 可为 NULL</param>
/// <param name="path">路由路径</param><param name="h">handler</param>
/// <param name="mws">路由级中间件名数组, 可为 NULL</param><param name="mws_n">mws 数量</param>
/// <returns>路由条目, NULL 表示失败</returns>
router_entry *router_head(router_ctx *r, const router_group *g, const char *path,
                          router_cb h, const char *const *mws, int32_t mws_n);
/// <summary>等价于 router_add(r, g, ROUTER_M_OPTIONS, path, h, mws, mws_n)</summary>
/// <param name="r">router_ctx</param><param name="g">分组, 可为 NULL</param>
/// <param name="path">路由路径</param><param name="h">handler</param>
/// <param name="mws">路由级中间件名数组, 可为 NULL</param><param name="mws_n">mws 数量</param>
/// <returns>路由条目, NULL 表示失败</returns>
router_entry *router_options(router_ctx *r, const router_group *g, const char *path,
                             router_cb h, const char *const *mws, int32_t mws_n);
/// <summary>等价于 router_add(r, g, ROUTER_M_ANY, path, h, mws, mws_n)，匹配所有已知 HTTP 方法</summary>
/// <param name="r">router_ctx</param><param name="g">分组, 可为 NULL</param>
/// <param name="path">路由路径</param><param name="h">handler</param>
/// <param name="mws">路由级中间件名数组, 可为 NULL</param><param name="mws_n">mws 数量</param>
/// <returns>路由条目, NULL 表示失败</returns>
router_entry *router_any(router_ctx *r, const router_group *g, const char *path,
                         router_cb h, const char *const *mws, int32_t mws_n);
/// <summary>
/// 注册流式路由：请求体逐块交给 sh，不在 router 内缓存，请求体多大都不占额外内存。
/// 只有 router_net_recv 这条入口认流式路由；用它就必须同时接上 router_closed。
/// 与 router_add 的三点不同：
///   1. 中间件链只做准入——跑到底即放行、中途不调 router_next 即拒绝（没写响应就兜底 500
///      并关连接）。后置处理跑在请求体到达之前，拦不住已放行的流，日志 / 计时类中间件须知情
///   2. sh 与准入中间件都不能挂起：分片逐帧投递，挂起期间下一帧会在新协程上重入。
///      要异步就 coro_fork 拷走 fd/skid 等，并置 ctx->responded = 1
///   3. 响应由 sh 自己写；到 PROT_SLICE_END 仍未响应则兜底 500，ABORT 那次只能清理、没有兜底
/// </summary>
/// <param name="r">router_ctx</param>
/// <param name="g">分组, 可为 NULL</param>
/// <param name="method">方法位掩码</param>
/// <param name="path">路由路径</param>
/// <param name="sh">流式回调, 不可为 NULL</param>
/// <param name="mws">路由级中间件名数组, 可为 NULL</param>
/// <param name="mws_n">mws 数量</param>
/// <returns>路由条目, NULL 表示失败 (失败原因同 router_add)</returns>
router_entry *router_add_stream(router_ctx *r, const router_group *g,
                                router_method method, const char *path,
                                router_stream_cb sh,
                                const char *const *mws, int32_t mws_n);
/// <summary>等价于 router_add_stream(r, g, ROUTER_M_POST, path, sh, mws, mws_n)</summary>
/// <param name="r">router_ctx</param><param name="g">分组, 可为 NULL</param>
/// <param name="path">路由路径</param><param name="sh">流式回调</param>
/// <param name="mws">路由级中间件名数组, 可为 NULL</param><param name="mws_n">mws 数量</param>
/// <returns>路由条目, NULL 表示失败</returns>
router_entry *router_post_stream(router_ctx *r, const router_group *g, const char *path,
                                 router_stream_cb sh, const char *const *mws, int32_t mws_n);
/// <summary>等价于 router_add_stream(r, g, ROUTER_M_PUT, path, sh, mws, mws_n)</summary>
/// <param name="r">router_ctx</param><param name="g">分组, 可为 NULL</param>
/// <param name="path">路由路径</param><param name="sh">流式回调</param>
/// <param name="mws">路由级中间件名数组, 可为 NULL</param><param name="mws_n">mws 数量</param>
/// <returns>路由条目, NULL 表示失败</returns>
router_entry *router_put_stream(router_ctx *r, const router_group *g, const char *path,
                                router_stream_cb sh, const char *const *mws, int32_t mws_n);
/// <summary>
/// 注册路由；不经 group/mw 解析，handler 置 NULL，只能配 router_match_index 使用
/// （调用方自己按索引派发），再交给 router_dispatch / router_net_recv 命中即回 500 拒绝。
/// 与已注册条目等价时拒绝注册（见返回值）：dispatch 取首条命中，后注册的那条永远够不着。
/// method 支持 "GET"/"POST"/"PUT"/"DELETE"/"PATCH"/"HEAD"/"OPTIONS"/"ANY"
/// </summary>
/// <param name="r">router_ctx</param>
/// <param name="method">HTTP 方法字符串</param>
/// <param name="method_len">method 长度</param>
/// <param name="path">路由完整路径（调用方已拼好前缀）</param>
/// <param name="path_len">path 长度</param>
/// <returns>路由索引（≥0）；-1 路径非法或方法未知；-2 已有一条等价路由把它遮住
/// （方法掩码有交集且段序列在匹配意义上相同——参数名不参与匹配，如 /u/{id} 之于 /u/{uid}）</returns>
int32_t router_add_index(router_ctx *r, const char *method, size_t method_len,
                         const char *path, size_t path_len);
/// <summary>
/// 路径匹配（不执行 handler/中间件）；调用方提供已零初始化的 ctx 与 url 存储。
/// 成功后 ctx->params/params_n 已填充，ctx->url 为 backing store
/// </summary>
/// <param name="r">router_ctx</param>
/// <param name="method">HTTP 方法字符串</param>
/// <param name="method_len">method 长度</param>
/// <param name="url">原始请求 URI（含查询字符串）</param>
/// <param name="url_len">url 长度</param>
/// <param name="ctx">调用方提供的 router_req，**必须已整体零初始化**（匹配失败不写 params_n，
/// 脏值会让 router_req_param 读到未初始化指针），同一个 ctx 不可跨请求复用。
/// ctx->url 须指向一块调用方持有的 url_ctx，不必预先清零；仅返回 ≥0 或 -1 时其内容可用，
/// 且是**规范化后**的：空段一律剔除（"/a//b" 读出来是 "/a/b"），"/" 与 "//" 的 npath 与
/// pathlens 均为 0（url_reorg_path 只吐得出空串，需要 "/" 由调用方补）</param>
/// <returns>路由索引（≥0）；-1 无匹配路由；-2 URL 解析失败；-3 方法不在已知列表(对应 405)</returns>
int32_t router_match_index(router_ctx *r, const char *method, size_t method_len,
                           const char *url, size_t url_len, router_req *ctx);
/// <summary>
/// router_match_index 的返回值 → HTTP 状态码。所有派发面共用本映射，
/// 新增失败哨兵时只需改这一处，不同派发面不会对同一请求给出不同状态码
/// </summary>
/// <param name="idx">router_match_index 的返回值</param>
/// <returns>200 命中；405 方法未知；400 URL 解析失败；404 无匹配路由</returns>
int32_t router_match_code(int32_t idx);
/// <summary>
/// 派发 HTTP 请求 —— 在 _net_recv 中 slice == 0 分支调用; 内部完成方法 / 路径
/// 匹配, 拼接 chain, 启动中间件链。URL 解析失败 → 400; 方法不识别 → 405; 路径未匹配 → 404;
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
/// 拒绝 chunked 请求 —— 回 HTTP 411 后立即关闭连接。router_net_recv 命中非流式路由时
/// 内部即调本函数; 自己写 _net_recv 而不打算支持 chunked 的, 在 slice == PROT_SLICE_START
/// 分支调它
/// </summary>
/// <param name="task">task</param>
/// <param name="fd">socket fd</param>
/// <param name="skid">连接 skid</param>
void router_reject_chunked(task_ctx *task, SOCKET fd, uint64_t skid);
/// <summary>
/// 连接关闭时清理该连接尚未收齐的流式请求 —— 在 _net_close_cb 中调用。
/// 用 router_net_recv 且注册了流式路由就**必须**接上本函数(task_closed 注册):
/// 未收齐的流在 router 内占约 5KB 上下文, 只有这里能回收, 回收前投一次
/// ROUTER_STREAM_ABORT 让 on_chunk 清 ctx->user; 漏接则两者都泄漏, 无上限兜底
/// </summary>
/// <param name="r">router_ctx</param>
/// <param name="fd">socket fd</param>
/// <param name="skid">连接 skid</param>
void router_closed(router_ctx *r, SOCKET fd, uint64_t skid);
/// <summary>
/// _net_recv 回调的标准实现 —— slice == 0 的完整请求直接转 router_dispatch;
/// chunked 命中流式路由则逐帧交给它, 命中普通路由则回 411 并关连接。匹配不上仍是
/// 404/400/405 —— 411 只表示"路由在, 但它接不住 chunked"。
/// 参数与 _net_recv_cb 一一对应, 只在最前面多一个 router_ctx
/// </summary>
/// <param name="r">router_ctx</param>
/// <param name="task">task</param>
/// <param name="sk">连接标识 (fd + skid)</param>
/// <param name="pktype">协议类型 (未使用)</param>
/// <param name="client">是否客户端连接 (未使用)</param>
/// <param name="slice">分片标志; 0 表示完整消息</param>
/// <param name="data">http_pack_ctx 指针</param>
/// <param name="size">数据字节数 (未使用)</param>
void router_net_recv(router_ctx *r, task_ctx *task, sk_id *sk,
                     subtype_t pktype, uint8_t client, uint8_t slice, void *data, size_t size);
/// <summary>
/// 中间件链推进; 中间件内调用即执行下一节点 (handler 或下一个中间件),
/// next 返回后可继续做后置处理。handler 不应调用
/// </summary>
/// <param name="ctx">router_req</param>
void router_next(router_req *ctx);
/// <summary>
/// 取请求头; 大小写不敏感匹配。流式路由过了首帧就取不到了, 见 router_stream_cb
/// </summary>
/// <param name="ctx">router_req</param>
/// <param name="key">header 名</param>
/// <param name="lens">输出值长度</param>
/// <returns>值指针 (pack 内部, 不复制); 未找到、或流式路由已过首帧, 返回 NULL</returns>
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
/// <returns>值指针; 键不存在返回 NULL, 键存在但值空(?a=)返回非 NULL 零长指针</returns>
const char *router_req_query(router_req *ctx, const char *key, size_t *lens);
/// <summary>
/// 取请求 body。流式路由不要用它: chunked 时这里恒为空,
/// 请求体一律从 router_stream_cb 的 data 取
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
/// 自定义响应; extra 为附加头。Content-Type 就经 extra 传(本函数自己不写, 与 router_req_text /
/// _json / _html 写死类型不同)。附加头逐条校验, 不合规者整条丢弃(仅 LOG_WARN):
/// 头名为 Content-Length / Transfer-Encoding (前者按 body_len 自动写, 再叠一条对端会判为
/// 请求走私), 头名为空、>= 128 字节或不是 RFC 7230 token, 头值为 NULL 或含 NUL/CR/LF,
/// 以及该条会让头部块累计越过 http.c 的 MAX_HEADLENS。头名一律不截断——截断等于改名发上线缆
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
