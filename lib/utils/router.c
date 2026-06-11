#include "utils/router.h"
#include "protocol/prots.h"
#include "utils/utils.h"
#include "utils/binary.h"
#include "utils/log.h"
#include "srey/loader.h"
#include "srey/task.h"

// 路径解析时单条路径的段数上限; 同时也是 _router_match_path 中请求段数组的容量上限
#define ROUTER_MAX_SEGS 64

// 路径段类型
typedef enum router_seg_type {
    ROUTER_SEG_LIT,    // 字面量
    ROUTER_SEG_PARAM,  // {name}    必填路径参数
    ROUTER_SEG_OPT,    // {name?}   可选路径参数
    ROUTER_SEG_WILD    // *         末尾通配, 出现即吞掉后续所有请求段
} router_seg_type;
// 路径段
// - LIT:    str 存字面量内容
// - PARAM:  str 存参数名 (不含花括号)
// - OPT:    str 存参数名 (不含花括号和问号)
// - WILD:   str 为 NULL
// str 由 _router_parse_seg MALLOC, router_free 中逐段 FREE
typedef struct router_seg {
    uint32_t str_len;
    router_seg_type t;
    char *str;
} router_seg;
// 具名中间件登记项
typedef struct named_mw {
    char *name;       // strdup 后存储, router_free 释放
    router_cb fn;
} named_mw;
// 路由条目
// 注册时一次性完成路径解析和中间件合并 (group → 路由级);
// dispatch 时按位掩码 + 路径段线性扫描首条匹配
struct router_entry {
    int32_t segs_n;
    int32_t mws_n;
    router_method method_mask;  // enum 4B, 后跟编译器自动补 4B padding 让指针 8 字节对齐
    router_seg *segs;
    router_cb *mws;             // 已合并的中间件函数指针 (group + 路由级, 已 _router_resolve_mw)
    router_cb handler;
};
// 路由器
// 三组动态数组共享一份 _router_grow 几何扩容逻辑; 全局中间件 / 路由表 / 具名表互不影响
struct router_ctx {
    int32_t routes_n;
    int32_t routes_cap;
    int32_t global_mw_n;
    int32_t global_mw_cap;
    int32_t named_n;
    int32_t named_cap;
    router_entry *routes;
    router_cb *global_mw;
    named_mw *named;
};

// 对存放 router_entry / router_cb / named_mw 三种结构体 (含指针成员) 的数组
// 这里直接 REALLOC 几何扩容, 由调用方自己写 [size++] 入位置
static void _router_grow(void **arr, int32_t *cap, int32_t need, size_t elem_size) {
    if (need <= *cap) {
        return;
    }
    // 起始 8, 之后翻倍直到满足 need; REALLOC 在 *arr=NULL 时等价 malloc
    int32_t newcap = 0 == *cap ? 8 : *cap;
    while (newcap < need) {
        newcap *= 2;
    }
    REALLOC(*arr, *arr, (size_t)newcap * elem_size);
    *cap = newcap;
}
// 解析单段, 写入 out; 成功 ERR_OK, 失败 out->str 保持 NULL
// src 不要求以 \0 结尾, 仅按 len 读取
static int32_t _router_parse_seg(const char *src, size_t len, router_seg *out) {
    out->str = NULL;
    out->str_len = 0;
    // 单字符 '*' → 末尾通配
    if (1 == len && '*' == src[0]) {
        out->t = ROUTER_SEG_WILD;
        return ERR_OK;
    }
    // {name} 或 {name?}; 至少 "{x}" 三字符
    if (len >= 3 && '{' == src[0] && '}' == src[len - 1]) {
        size_t name_len = len - 2;        // 去掉首尾花括号
        const char *name_src = src + 1;
        int32_t is_opt = 0;
        // 末尾 '?' 表示可选段
        if ('?' == name_src[name_len - 1]) {
            is_opt = 1;
            name_len--;
        }
        // {} / {?} 等空名拒绝
        if (0 == name_len) {
            return ERR_FAILED;
        }
        // +1 字节存 \0, router_req_param 内可直接 memcmp 不必再带长度
        MALLOC(out->str, name_len + 1);
        memcpy(out->str, name_src, name_len);
        out->str[name_len] = '\0';
        out->str_len = (uint32_t)name_len;
        out->t = is_opt ? ROUTER_SEG_OPT : ROUTER_SEG_PARAM;
        return ERR_OK;
    }
    // 其他: 字面量段, 整体拷贝
    MALLOC(out->str, len + 1);
    memcpy(out->str, src, len);
    out->str[len] = '\0';
    out->str_len = (uint32_t)len;
    out->t = ROUTER_SEG_LIT;
    return ERR_OK;
}
// 按 '/' 拆分 path, 调用 _router_parse_seg 逐段解析, 写入新分配的 *out_segs
// 全部成功才落堆, 任一段失败时回滚已 MALLOC 的 str
static int32_t _router_parse_path(const char *path, size_t path_len, router_seg **out_segs, int32_t *out_n) {
    int32_t n = 0;
    size_t i = 0;
    size_t start;
    // 栈缓存: 先填这里, 全部成功后一次性 MALLOC + memcpy, 失败路径无需 realloc 回滚
    router_seg buf[ROUTER_MAX_SEGS];
    while (i < path_len) {
        // 跳过连续 '/': 兼容 "//foo" 或前导 '/' 多次
        while (i < path_len && '/' == path[i]) {
            i++;
        }
        if (i >= path_len) {
            break;
        }
        start = i;
        // 推到下个 '/' 或末尾, [start, i) 为一段
        while (i < path_len && '/' != path[i]) {
            i++;
        }
        if (n >= ROUTER_MAX_SEGS) {
            LOG_WARN("router: path segments exceed %d, rejected.", ROUTER_MAX_SEGS);
            // 已申请 str 释放; buf 本身是栈缓冲无需 free
            for (int32_t k = 0; k < n; k++) {
                FREE(buf[k].str);
            }
            return ERR_FAILED;
        }
        // WILD 段必须是最末段; 此时若上一段已是 WILD 却还有当前段, 说明 WILD 后还有内容, 拒绝
        // (router 默认会在 _router_match_path 命中 WILD 时立即返成功, 中间 WILD 会让后续段静默失效, 易掉坑)
        if (n > 0 && ROUTER_SEG_WILD == buf[n - 1].t) {
            LOG_WARN("router: wildcard '*' must be the last segment.");
            for (int32_t k = 0; k < n; k++) {
                FREE(buf[k].str);
            }
            return ERR_FAILED;
        }
        if (ERR_OK != _router_parse_seg(path + start, i - start, &buf[n])) {
            LOG_WARN("router: invalid path segment.");
            for (int32_t k = 0; k < n; k++) {
                FREE(buf[k].str);
            }
            return ERR_FAILED;
        }
        n++;
    }
    if (0 == n) {
        // 根路径 "/" 拆出 0 段; segs_n=0 同样能匹配请求 path="/"
        *out_segs = NULL;
        *out_n = 0;
        return ERR_OK;
    }
    // 结构体浅拷贝, str 指针所有权随之转移到堆上的 *out_segs
    MALLOC(*out_segs, sizeof(router_seg) * (size_t)n);
    memcpy(*out_segs, buf, sizeof(router_seg) * (size_t)n);
    *out_n = n;
    return ERR_OK;
}
// 把请求 path 拆段后与 rsegs 对照, 成功填 ctx->params 并返回 1
// 不修改 path; ctx->params[i].key 指向 rsegs[].str (router_ctx 持有),
// val 指向 path 内部 (ctx 持有, 即 ctx->url_storage.buf)
static int32_t _router_match_path(const router_seg *rsegs, int32_t rn,
                                  const char *path, size_t path_len,
                                  router_req *ctx) {
    // 把请求 path 拆成 (起址, 长度) 二元组数组, 不复制字符串
    struct { const char *s; size_t n; } qsegs[ROUTER_MAX_SEGS];
    int32_t qn = 0;
    size_t i = 0;
    size_t start;
    while (i < path_len && qn < ROUTER_MAX_SEGS) {
        while (i < path_len && '/' == path[i]) {
            i++;
        }
        if (i >= path_len) {
            break;
        }
        start = i;
        while (i < path_len && '/' != path[i]) {
            i++;
        }
        qsegs[qn].s = path + start;
        qsegs[qn].n = i - start;
        qn++;
    }
    // 请求段超 ROUTER_MAX_SEGS:跳末尾 '/' 后仍有内容即存在第 65 段,标记溢出供下方判不匹配
    int32_t overflow = 0;
    if (ROUTER_MAX_SEGS == qn) {
        while (i < path_len && '/' == path[i]) {
            i++;
        }
        overflow = (i < path_len) ? 1 : 0;
    }
    // ri = 路由段游标, qi = 请求段游标, pn = 已填参数计数
    int32_t ri = 0;
    int32_t qi = 0;
    int32_t pn = 0;
    while (ri < rn) {
        const router_seg *seg = &rsegs[ri];
        if (ROUTER_SEG_WILD == seg->t) {
            // '*' 一旦出现, 后续请求段任意, 直接匹配成功
            ctx->params_n = pn;
            return 1;
        } else if (ROUTER_SEG_LIT == seg->t) {
            // 字面量必须长度相同且内容全等
            if (qi >= qn || seg->str_len != (uint32_t)qsegs[qi].n
                || 0 != memcmp(seg->str, qsegs[qi].s, qsegs[qi].n)) {
                return 0;
            }
            ri++;
            qi++;
        } else if (ROUTER_SEG_PARAM == seg->t) {
            // {name} 必须有对应请求段
            if (qi >= qn) {
                return 0;
            }
            if (pn >= ROUTER_MAX_PARAMS) {
                LOG_WARN("router: params exceed %d, rejected.", ROUTER_MAX_PARAMS);
                return 0;
            }
            ctx->params[pn].key = seg->str;
            ctx->params[pn].key_len = seg->str_len;
            ctx->params[pn].val = qsegs[qi].s;
            ctx->params[pn].val_len = (uint32_t)qsegs[qi].n;
            pn++;
            ri++;
            qi++;
        } else /* ROUTER_SEG_OPT */ {
            // {name?} 有就吃, 没就跳, 不算匹配失败
            if (qi < qn) {
                if (pn >= ROUTER_MAX_PARAMS) {
                    LOG_WARN("router: params exceed %d, rejected.", ROUTER_MAX_PARAMS);
                    return 0;
                }
                ctx->params[pn].key = seg->str;
                ctx->params[pn].key_len = seg->str_len;
                ctx->params[pn].val = qsegs[qi].s;
                ctx->params[pn].val_len = (uint32_t)qsegs[qi].n;
                pn++;
                qi++;
            }
            ri++;
        }
    }
    // 路由段消耗完但请求段还有剩或溢出 ROUTER_MAX_SEGS, 不匹配 (避免 /a 命中 /a/b)
    if (qi != qn || overflow) {
        return 0;
    }
    ctx->params_n = pn;
    return 1;
}
// HTTP 方法字符串 → 位掩码; 未识别返回 0, dispatch 处响应 405
static router_method _router_method_str_to_mask(const char *m, size_t n) {
    if (3 == n && 0 == memcmp(m, "GET", 3))    return ROUTER_M_GET;
    if (4 == n && 0 == memcmp(m, "POST", 4))   return ROUTER_M_POST;
    if (3 == n && 0 == memcmp(m, "PUT", 3))    return ROUTER_M_PUT;
    if (6 == n && 0 == memcmp(m, "DELETE", 6)) return ROUTER_M_DELETE;
    if (5 == n && 0 == memcmp(m, "PATCH", 5))  return ROUTER_M_PATCH;
    if (4 == n && 0 == memcmp(m, "HEAD", 4))   return ROUTER_M_HEAD;
    if (7 == n && 0 == memcmp(m, "OPTIONS", 7))return ROUTER_M_OPTIONS;
    return 0;
}
router_ctx *router_new(void) {
    router_ctx *r;
    MALLOC(r, sizeof(router_ctx));
    ZERO(r, sizeof(router_ctx));
    return r;
}
void router_free(router_ctx *r) {
    if (NULL == r) {
        return;
    }
    router_entry *e;
    // 逐 entry 释放其内嵌的字符串和数组
    for (int32_t i = 0; i < r->routes_n; i++) {
        e = &r->routes[i];
        // segs 内每个 str 都是 _router_parse_seg MALLOC 的
        for (int32_t k = 0; k < e->segs_n; k++) {
            FREE(e->segs[k].str);
        }
        FREE(e->segs);
        FREE(e->mws);
    }
    FREE(r->routes);
    FREE(r->global_mw);
    // 具名表 name 是 router_define 中 MALLOC + memcpy 的副本
    for (int32_t i = 0; i < r->named_n; i++) {
        FREE(r->named[i].name);
    }
    FREE(r->named);
    FREE(r);
}
// 按名查具名中间件; 数量小, 线性扫描即可, 未注册视为 router_use / 路由 mws 引用错误
static router_cb _router_resolve_mw(router_ctx *r, const char *name) {
    for (int32_t i = 0; i < r->named_n; i++) {
        if (0 == strcmp(r->named[i].name, name)) {
            return r->named[i].fn;
        }
    }
    LOG_WARN("router: middleware '%s' not defined.", name);
    return NULL;
}
void router_define(router_ctx *r, const char *name, router_cb fn) {
    // 同名直接覆盖 named 表项; 注意 router_add / router_use 在被调时已把函数指针快照
    // 存进 entry->mws / global_mw, 覆盖只影响其后注册的路由 / 全局中间件
    for (int32_t i = 0; i < r->named_n; i++) {
        if (0 == strcmp(r->named[i].name, name)) {
            r->named[i].fn = fn;
            return;
        }
    }
    _router_grow((void **)&r->named, &r->named_cap, r->named_n + 1, sizeof(named_mw));
    // strdup 一份, 调用方栈上 / 常量区字符串都能用
    size_t len = strlen(name);
    char *dup;
    MALLOC(dup, len + 1);
    memcpy(dup, name, len + 1);
    r->named[r->named_n].name = dup;
    r->named[r->named_n].fn = fn;
    r->named_n++;
}
void router_use(router_ctx *r, const char *name) {
    // 走 _router_resolve_mw 把名字转成函数指针, 再委托给 _use_fn 统一入数组
    router_cb fn = _router_resolve_mw(r, name);
    if (NULL == fn) {
        return;
    }
    router_use_fn(r, fn);
}
void router_use_fn(router_ctx *r, router_cb fn) {
    _router_grow((void **)&r->global_mw, &r->global_mw_cap, r->global_mw_n + 1, sizeof(router_cb));
    r->global_mw[r->global_mw_n++] = fn;
}
// group 是纯栈对象, 字段全部按值/指针存; 嵌套靠 parent 指针链向上找祖先节点。
// 调用方必须保证 prefix / mw_names 在所有 router_* 注册调用期间生命周期有效
// (一般用字符串字面量 / 静态数组即可)
void router_group_root(router_ctx *r, router_group *g, const char *prefix,
                       const char *const *mw_names, int32_t n) {
    g->parent = NULL;
    g->router = r;
    g->prefix = NULL == prefix ? "" : prefix;
    g->prefix_len = (uint32_t)strlen(g->prefix);
    g->mw_names = mw_names;
    g->mw_names_n = n;
}
void router_group_nest(const router_group *parent, router_group *g, const char *prefix,
                       const char *const *mw_names, int32_t n) {
    g->parent = parent;
    g->router = parent->router;
    g->prefix = NULL == prefix ? "" : prefix;
    g->prefix_len = (uint32_t)strlen(g->prefix);
    g->mw_names = mw_names;
    g->mw_names_n = n;
}
// 沿父链按 root→leaf 顺序把各级 prefix 拼到 out, *out_len 写已用字节数。
// 递归先到根再回溯写; 累积长度 > cap 时提前 ERR_FAILED, 避免 memcpy 越界写栈
static int32_t _router_group_build_prefix(const router_group *g, char *out, size_t cap, size_t *out_len) {
    *out_len = 0;
    if (NULL == g) {
        return ERR_OK;
    }
    if (NULL != g->parent) {
        if (ERR_OK != _router_group_build_prefix(g->parent, out, cap, out_len)) {
            return ERR_FAILED;
        }
    }
    if (*out_len + g->prefix_len > cap) {
        return ERR_FAILED;
    }
    memcpy(out + *out_len, g->prefix, g->prefix_len);
    *out_len += g->prefix_len;
    return ERR_OK;
}
// 沿父链累加中间件数量, router_add 用来一次性 MALLOC 合并数组
static int32_t _router_group_count_mws(const router_group *g) {
    int32_t total = 0;
    while (NULL != g) {
        total += g->mw_names_n;
        g = g->parent;
    }
    return total;
}
// 沿父链按 root→leaf 顺序把所有 mw_names 平铺到 out, 返回写入数量;
// 递归保证先写父再写子, 与中间件执行顺序 (祖先先于子孙) 一致
static int32_t _router_group_collect_mws(const router_group *g, char **out) {
    if (NULL == g) {
        return 0;
    }
    int32_t k = _router_group_collect_mws(g->parent, out);
    for (int32_t i = 0; i < g->mw_names_n; i++) {
        out[k + i] = (char *)g->mw_names[i];
    }
    return k + g->mw_names_n;
}
router_entry *router_add(router_ctx *r, const router_group *g,
                         router_method method, const char *path,
                         router_cb h,
                         const char *const *mws, int32_t mws_n) {
    // 1) 沿 group 父链拼接 prefix + path → full_buf (栈, 仅用于段解析, 解析完即可丢弃)
    size_t prefix_len = 0;
    char full_buf[ONEK];
    if (NULL != g) {
        if (ERR_OK != _router_group_build_prefix(g, full_buf, sizeof(full_buf), &prefix_len)) {
            LOG_WARN("router: group prefix too long.");
            return NULL;
        }
    }
    size_t path_len = strlen(path);
    size_t full_len = prefix_len + path_len;
    if (full_len >= sizeof(full_buf)) {
        LOG_WARN("router: full path too long (%zu).", full_len);
        return NULL;
    }
    memcpy(full_buf + prefix_len, path, path_len);
    // full_buf 不要求 \0 结尾; _router_parse_path 按 full_len 处理
    // 2) 把 full_buf 解析为段数组 (LIT/PARAM/OPT/WILD), 解析完段数组进堆, full_buf 出栈丢弃
    router_seg *segs = NULL;
    int32_t segs_n = 0;
    if (ERR_OK != _router_parse_path(full_buf, full_len, &segs, &segs_n)) {
        return NULL;
    }
    // 3) 合并中间件: group (root→leaf) → 路由级; 未注册的名字 _router_resolve_mw 已 LOG_WARN, 跳过
    int32_t g_mws_n = _router_group_count_mws(g);
    int32_t total_mws = g_mws_n + mws_n;
    router_cb *mws_arr = NULL;
    if (total_mws > 0) {
        MALLOC(mws_arr, sizeof(router_cb) * (size_t)total_mws);
        int32_t k = 0;
        // 3a) 先收集 group 上下文的 mw_names, 查表落实为函数指针
        if (g_mws_n > 0) {
            char **gnames;
            MALLOC(gnames, sizeof(const char *) * (size_t)g_mws_n);
            _router_group_collect_mws(g, gnames);
            for (int32_t i = 0; i < g_mws_n; i++) {
                router_cb fn = _router_resolve_mw(r, (const char*)gnames[i]);
                if (NULL != fn) {
                    mws_arr[k++] = fn;
                }
            }
            FREE(gnames);
        }
        // 3b) 再追加路由级 mws
        for (int32_t i = 0; i < mws_n; i++) {
            router_cb fn = _router_resolve_mw(r, mws[i]);
            if (NULL != fn) {
                mws_arr[k++] = fn;
            }
        }
        // 实际有效条数 (跳过未注册的) 可能 < 预分配, 更新 mws_n; 全部失败时释放空数组
        total_mws = k;
        if (0 == total_mws) {
            FREE(mws_arr);
        }
    }
    // 4) 入路由表 (尾插, dispatch 时按注册顺序线性扫描)
    _router_grow((void **)&r->routes, &r->routes_cap, r->routes_n + 1, sizeof(router_entry));
    router_entry *e = &r->routes[r->routes_n];
    ZERO(e, sizeof(*e));
    e->segs = segs;
    e->segs_n = segs_n;
    e->mws = mws_arr;
    e->mws_n = total_mws;
    e->handler = h;
    e->method_mask = method;
    r->routes_n++;
    return e;
}
// 按 method 生成 router_get / router_post / ... 等便捷包装, 内部一律转发到 router_add
#define DEF_ROUTE_FN(name, mask)  \
router_entry *router_##name(router_ctx *r, const router_group *g,  \
                            const char *path, router_cb h, \
                            const char *const *mws, int32_t mws_n) { \
    return router_add(r, g, mask, path, h, mws, mws_n); \
}
DEF_ROUTE_FN(get,     ROUTER_M_GET)
DEF_ROUTE_FN(post,    ROUTER_M_POST)
DEF_ROUTE_FN(put,     ROUTER_M_PUT)
DEF_ROUTE_FN(delete,  ROUTER_M_DELETE)
DEF_ROUTE_FN(patch,   ROUTER_M_PATCH)
DEF_ROUTE_FN(head,    ROUTER_M_HEAD)
DEF_ROUTE_FN(options, ROUTER_M_OPTIONS)
DEF_ROUTE_FN(any,     ROUTER_M_ANY)
#undef DEF_ROUTE_FN

// 中间件需主动调本函数推进链路; 不调即截断, 后续 mw / handler 不执行。
// 因为是同步递归调用栈, 中间件可在 router_next 返回后做后置处理 (打日志 / 统计耗时)
void router_next(router_req *ctx) {
    if (ctx->chain_i >= ctx->chain_n) {
        return;
    }
    int32_t i = ctx->chain_i++;
    ctx->chain[i](ctx);
}
char *router_req_header(router_req *ctx, const char *key, size_t *lens) {
    // 先把 lens 清 0, http_header 未找到时不写 lens, 调用方读到旧值会误判
    *lens = 0;
    return http_header(ctx->pack, key, lens);
}
const char *router_req_param(router_req *ctx, const char *key, size_t *lens) {
    // params 由 _router_match_path 填; key 指向 segs[].str (\0 结尾), 这里按长度比对兼容外部传入串
    size_t klen = strlen(key);
    for (int32_t i = 0; i < ctx->params_n; i++) {
        if (klen == ctx->params[i].key_len
            && 0 == memcmp(ctx->params[i].key, key, klen)) {
            *lens = ctx->params[i].val_len;
            return ctx->params[i].val;
        }
    }
    *lens = 0;
    return NULL;
}
const char *router_req_query(router_req *ctx, const char *key, size_t *lens) {
    // url_parse 已完成 url_decode, 这里直接返底层 buf_ctx
    buf_ctx *v = url_get_param(&ctx->url_storage, key);
    if (NULL == v) {
        *lens = 0;
        return NULL;
    }
    *lens = v->lens;
    return (const char *)v->data;
}
void *router_req_body(router_req *ctx, size_t *lens) {
    return http_data(ctx->pack, lens);
}
// 组装完整 HTTP 响应并通过 ev_send 推出去; 自动写 Content-Length, content_type 非 NULL 时
// 自动写 Content-Type, extra 由调用方追加 (不可重复 CL / CT / Transfer-Encoding)
// bw 内部托管 (binary_init(NULL,...) 模式), ev_send copy=0 转移 bw.data 所有权给框架,
// 函数返回后无需 binary_free
static void _router_send_resp(router_req *ctx, int32_t code, const char *content_type,
                              const http_header_ctx *extra, int32_t extra_n,
                              const char *body, size_t body_len) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    http_pack_resp(&bw, code);
    if (NULL != content_type) {
        http_pack_head(&bw, "Content-Type", content_type);
    }
    // extra 的 key/value 是 buf_ctx (不以 \0 结尾), http_pack_head 要 C 串, 复制 + 加 \0
    char k[128];
    char v[256];
    size_t klen, vlen;
    for (int32_t i = 0; i < extra_n; i++) {
        klen = extra[i].key.lens < sizeof(k) - 1 ? extra[i].key.lens : sizeof(k) - 1;
        vlen = extra[i].value.lens < sizeof(v) - 1 ? extra[i].value.lens : sizeof(v) - 1;
        memcpy(k, extra[i].key.data, klen); k[klen] = '\0';
        memcpy(v, extra[i].value.data, vlen); v[vlen] = '\0';
        http_pack_head(&bw, k, v);
    }
    // http_pack_content 内部会写 \r\n\r\n + body, 完成完整响应包
    http_pack_content(&bw, (void *)body, body_len);
    ev_send(&ctx->task->loader->netev, ctx->fd, ctx->skid, bw.data, bw.offset, 0);
    // 置位避免 dispatch 末尾兜底 500 又发一遍
    ctx->responded = 1;
}
void router_req_text(router_req *ctx, int32_t code, const char *body, size_t lens) {
    _router_send_resp(ctx, code, "text/plain; charset=utf-8", NULL, 0,
                      NULL == body ? "" : body, NULL == body ? 0 : lens);
}
void router_req_json(router_req *ctx, int32_t code, const char *json, size_t lens) {
    _router_send_resp(ctx, code, "application/json", NULL, 0,
                      NULL == json ? "" : json, NULL == json ? 0 : lens);
}
void router_req_html(router_req *ctx, int32_t code, const char *body, size_t lens) {
    _router_send_resp(ctx, code, "text/html; charset=utf-8", NULL, 0,
                      NULL == body ? "" : body, NULL == body ? 0 : lens);
}
void router_req_respond(router_req *ctx, int32_t code,
                      const http_header_ctx *extra, int32_t extra_n,
                      const char *body, size_t body_len) {
    _router_send_resp(ctx, code, NULL, extra, extra_n,
                      NULL == body ? "" : body, NULL == body ? 0 : body_len);
}
// 兜底响应 (404 / 405 / 500); 跟 _router_send_resp 相比省去 extra 头数组处理, body 走 strlen,
// 适合 dispatch 未匹配 / 未识别方法 / 中间件链溢出等错误路径快速发出短文本响应
static void _router_send_simple(router_req *ctx, int32_t code, const char *body) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    http_pack_resp(&bw, code);
    http_pack_head(&bw, "Content-Type", "text/plain; charset=utf-8");
    size_t blen = NULL == body ? 0 : strlen(body);
    http_pack_content(&bw, (void *)(NULL == body ? "" : body), blen);
    ev_send(&ctx->task->loader->netev, ctx->fd, ctx->skid, bw.data, bw.offset, 0);
    ctx->responded = 1;
}
// 派发流程: 解方法 → URL parse → 线性扫表 → 拼 chain → 推进 → 兜底 500
void router_dispatch(router_ctx *r, task_ctx *task,
                     SOCKET fd, uint64_t skid,
                     struct http_pack_ctx *pack) {
    if (NULL == r || NULL == pack) {
        return;
    }
    // status[0] = 方法, status[1] = 请求 URI; 任一为空视为无效 HTTP, 静默丢
    buf_ctx *st = http_status(pack);
    if (NULL == st || 0 == st[0].lens || 0 == st[1].lens) {
        return;
    }
    router_method m = _router_method_str_to_mask(st[0].data, st[0].lens);
    if (0 == m) {
        // 方法不在我们已知列表 → 405; 因为 ctx 尚未完整初始化, 用临时 ctx 仅承载发响应必需字段
        router_req tmpctx = { 0 };
        tmpctx.task = task;
        tmpctx.fd = fd;
        tmpctx.skid = skid;
        tmpctx.pack = pack;
        _router_send_simple(&tmpctx, 405, "Method Not Allowed\n");
        return;
    }
    // 解析 URL: path 用于段匹配, query 落 ctx->url_storage 供 router_req_query 取
    router_req ctx = { 0 };
    ctx.task = task;
    ctx.fd = fd;
    ctx.skid = skid;
    ctx.pack = pack;
    ctx.method = m;
    url_parse(&ctx.url_storage, st[1].data, st[1].lens);
    ctx.path = ctx.url_storage.path.data;
    ctx.path_len = (uint32_t)ctx.url_storage.path.lens;
    // 线性扫描路由表: 方法位掩码 & 命中 + 路径匹配, 首条命中即用
    router_entry *matched = NULL;
    for (int32_t i = 0; i < r->routes_n; i++) {
        router_entry *e = &r->routes[i];
        if (0 == (e->method_mask & m)) {
            continue;
        }
        ctx.params_n = 0;
        if (_router_match_path(e->segs, e->segs_n, ctx.path, ctx.path_len, &ctx)) {
            matched = e;
            break;
        }
    }
    if (NULL == matched) {
        _router_send_simple(&ctx, 404, "Not Found\n");
        return;
    }
    // 拼接执行链 chain: 全局中间件 → 路由级中间件 → handler
    int32_t need = r->global_mw_n + matched->mws_n + 1;
    if (need > ROUTER_MAX_CHAIN) {
        // chain 数组容量固定 (ROUTER_MAX_CHAIN), 超额视为配置错误直接 500
        LOG_WARN("router: chain exceeds %d (global=%d, route=%d), rejected.",
                 ROUTER_MAX_CHAIN, r->global_mw_n, matched->mws_n);
        _router_send_simple(&ctx, 500, "Chain too long\n");
        return;
    }
    int32_t k = 0;
    for (int32_t i = 0; i < r->global_mw_n; i++) {
        ctx.chain[k++] = r->global_mw[i];
    }
    for (int32_t i = 0; i < matched->mws_n; i++) {
        ctx.chain[k++] = matched->mws[i];
    }
    ctx.chain[k++] = matched->handler;
    ctx.chain_n = k;
    ctx.chain_i = 0;
    // 启动链路, 第一个中间件 / handler 通过 router_next 递归推进
    router_next(&ctx);
    // 中间件主动 return 不调 router_next 是合法截断; 但都没写响应 (handler 漏发 + 中间件
    // 也没截断) 时, 客户端会卡死, 这里兜底 500 让连接尽快关闭
    if (!ctx.responded) {
        _router_send_simple(&ctx, 500, "Internal Server Error\n");
    }
}
