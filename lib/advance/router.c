#include "advance/router.h"
#include "advance/router_tree.h"
#include "utils/binary.h"
#include "containers/hashmap.h"
#include "containers/sarray.h"
#include "srey/loader.h"
#include "srey/task.h"

// 三种 Content-Type 的整行常量(线格式同 http_pack_head："Key: value" + CRLF)，一次追加写入；ROUTER_CT_LINE 展开成 指针, 长度
#define ROUTER_CT_TEXT "Content-Type: text/plain; charset=utf-8" FLAG_CRLF
#define ROUTER_CT_JSON "Content-Type: application/json" FLAG_CRLF
#define ROUTER_CT_HTML "Content-Type: text/html; charset=utf-8" FLAG_CRLF
#define ROUTER_CT_LINE(lit) (lit), (sizeof(lit) - 1)
// 非 200 的错误正文。两个派发入口的正文必须一模一样, 只是发送方式不同
// (dispatch 只回响应, 流式那边还要关连接 + 丢记录), 故正文在这里定死。
// 匹配失败的码由 _router_code_body 按状态码生成; 下面两条是它生成不出来的
#define ROUTER_CODE_BODY_LENS 64
#define ROUTER_BODY_500 "Internal Server Error\n"
#define ROUTER_BODY_CHAIN "Chain too long\n"
#define ROUTER_BODY_411 "chunked request not supported\n"
// 按 method 生成 router_get / router_post / ... 等便捷包装, 内部一律转发到 router_add;
// 展开点在 router_add 之后(宏体里的类型只在展开处才需要可见)
#define DEF_ROUTE_FN(name, mask)  \
router_entry *router_##name(router_ctx *r, const router_group *g,  \
                            const char *path, router_cb h, \
                            const char *const *mws, int32_t mws_n) { \
    return router_add(r, g, mask, path, h, NULL, mws, mws_n); \
}
// 流式便捷包装只给 POST / PUT: 带请求体的方法就这两个, 其余方法要流式直接用 router_add_stream
#define DEF_STREAM_FN(name, mask)  \
router_entry *router_##name##_stream(router_ctx *r, const router_group *g,  \
                                     const char *path, router_stream_cb sh, \
                                     const char *const *mws, int32_t mws_n) { \
    return router_add_stream(r, g, mask, path, sh, NULL, mws, mws_n); \
}

// router_seg 在 advance/router_pub.h，str 由 _router_parse_seg MALLOC，
// router_free 中逐段 FREE。
// 具名中间件登记项
typedef struct named_mw {
    char *name;       // strdup 后存储, router_free 释放
    router_step step; // 回调 + 注册时传入的 arg
} named_mw;
// 路由条目
// 注册时一次性完成路径解析、中间件合并和树插入; 匹配由 router_tree 负责。
// 条目单独 MALLOC, 路由表只存指针, 故 router_add 返回的指针到 router_free 前都有效。
struct router_entry {
    int32_t id;                // 路由表下标, 也是树里的 route_id
    int32_t segs_n;            // 原始段数
    int32_t mws_n;             // 合并后的中间件数
    router_seg *segs;          // 原始段序列, router_seg_index 用
    rtree_variant *variants;   // 树展开出的变体, param_seg 用来绑参数名
    int32_t variants_n;        // 变体数
    router_step *mws;          // 已合并的中间件 (group + 路由级)
    router_cb handler;         // 普通路由; 与 on_chunk 互斥
    void *handler_arg;         // 普通路由的 arg
    router_stream_cb on_chunk; // 流式路由; 两者皆空即 router_add_index 注册的纯匹配条目
    void *on_chunk_arg;        // 流式路由的 arg
};
// 三组动态数组直接用容器层的 ARR_DECL(倍增策略归它管);
// 路由表存指针, 条目单独分配; 全局中间件 / 具名表互不影响
ARR_DECL(route_ptr_arr, router_entry *)
ARR_DECL(rmw_arr, router_step)
ARR_DECL(rnamed_arr, named_mw)
// 一条正在接收的流式请求。req / url 要跨帧活到收齐, 故整体堆分配, 表里只放下面那个小元素
// —— 表扩容搬的是指针, 交给 on_chunk 的 ctx 地址始终不变。
// 首包不留副本: req.pack 只在它活着的那段时间(准入链 + 首帧回调)有效, 之后置 NULL
typedef struct router_stream {
    router_stream_cb on_chunk;   // 命中路由的流式回调
    void *on_chunk_arg;          // 流式回调的 arg
    router_req req;              // 跨帧复用的请求上下文, 连接标识也在里面
    url_ctx url;                 // req.url 恒指向这里
} router_stream;
// 流式表的元素: 键 + 本体指针。本体近 6KB, 按值入表的话查一次就得在栈上摆一个同样大的探针
typedef struct router_st_ent {
    sock_ctx sk;              // st_map 的键
    router_stream *st;
} router_st_ent;
// 流式表的 HASHFN / CMPFN(键是 sk 的 fd + skid 两段,多行所以用函数不用宏)。
// 定义与 _router_st_free 一族放在一起,这里只前向声明,好让下面的 HASHMAP_DECL 能用
static inline uint64_t _router_st_hash(const router_st_ent *ent);
static inline int _router_st_cmp(const router_st_ent *x, const router_st_ent *y);
HASHMAP_DECL(st_map, router_st_ent, _router_st_hash, _router_st_cmp)
// 路由器
struct router_ctx {
    int32_t has_stream; // 注册过流式路由; 没有就走 _router_chunked_nostream, 不必堆分配 router_stream
    int32_t own_chain;  // 执行链由上层自己跑: 注册期不按 ROUTER_MAX_CHAIN 告警
    rtree_ctx *tree;  // 段级路由树; 叶子只存 route id / variant id
    route_ptr_arr routes;
    rmw_arr global_mw;
    rnamed_arr named;
    router_step no_route;  // 404 回退
    router_step no_method; // 405 回退
    st_map *streams;    // router_st_ent 表; 首次遇到流式请求才建
};
// 三个数组按基址下标访问:循环上界就是 *_size, *_at 每轮再跑一遍负下标归一与越界
// 断言是重复的。空数组时 *_front 返 NULL,各处的 for 上界与显式判空已挡住解引用
#define _R_ROUTES(r)   ((r)->routes.ptr)
#define _R_GMW(r)      rmw_arr_front(&(r)->global_mw)
#define _R_NAMED(r)    rnamed_arr_front(&(r)->named)
#define _R_ROUTES_N(r) ((int32_t)(r)->routes.size)
#define _R_GMW_N(r)    ((int32_t)rmw_arr_size(&(r)->global_mw))
#define _R_NAMED_N(r)  ((int32_t)rnamed_arr_size(&(r)->named))

// 路由表里存指针，越界即注册/树 id 不一致，直接断言
static inline router_entry *_route(router_ctx *r, int32_t id) {
    ASSERTAB(id >= 0 && id < _R_ROUTES_N(r), "router route id out of range.");
    return _R_ROUTES(r)[id];
}
static inline const router_entry *_croute(const router_ctx *r, int32_t id) {
    ASSERTAB(id >= 0 && id < _R_ROUTES_N(r), "router route id out of range.");
    return _R_ROUTES(r)[id];
}
// 占位符名字只认 [A-Za-z0-9_]。不用 isalnum: 它跟 locale 走, 同一条路由换个 locale 会变意思
static int32_t _router_seg_name_ok(const char *s, size_t len) {
    char c;
    for (size_t i = 0; i < len; i++) {
        c = s[i];
        if (!((c >= 'A' && c <= 'Z')
            || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9')
            || '_' == c)) {
            return 0;
        }
    }
    return 1;
}
// 解析单段, 写入 out。src 不要求以 \0 结尾, 仅按 len 读取。任何输入都能解析出一段:
// 认不出占位符形态(名字为空 / 名字含非 \w 字符)就当字面量, 故没有失败返回
static void _router_parse_seg(const char *src, size_t len, router_seg *out) {
    out->str = NULL;
    out->str_len = 0;
    // 单字符 '*' → 末尾通配
    if (1 == len && '*' == src[0]) {
        out->t = ROUTER_SEG_WILD;
        return;
    }
    // {name} 或 {name?}; 至少 "{x}" 三字符
    if (len >= 3 && '{' == src[0] && '}' == src[len - 1]) {
        size_t name_len = len - 2;// 去掉首尾花括号
        const char *name_src = src + 1;
        int32_t is_opt = 0;
        // 末尾 '?' 表示可选段
        if ('?' == name_src[name_len - 1]) {
            is_opt = 1;
            name_len--;
        }
        // 名字为空({?}; {} 长度不够, 压根进不来)或含非 \w 字符({a-b} / {a?b} / {*})都不认作
        // 占位符, 落字面量而不是让整条路由注册失败。名字字符集同 Laravel 的 \{(\w+?)\??\}:
        // {*} 若也当占位符, 段序列里它与末尾通配的 key 都是 "*", 反向生成 URL 时分不开
        if (0 != name_len
            && _router_seg_name_ok(name_src, name_len)) {
            // +1 字节存 \0, router_req_param 内可直接 memcmp 不必再带长度
            out->str = dup_zero(name_src, name_len);
            out->str_len = (uint32_t)name_len;
            out->t = is_opt ? ROUTER_SEG_OPT : ROUTER_SEG_PARAM;
            return;
        }
    }
    // 其他: 字面量段, 整体拷贝
    out->str = dup_zero(src, len);
    out->str_len = (uint32_t)len;
    out->t = ROUTER_SEG_LIT;
}
// 释放段数组内每个 str(均由 _router_parse_seg MALLOC); 数组本身的所有权归调用方处置
static void _router_segs_free_str(router_seg *segs, int32_t n) {
    for (int32_t k = 0; k < n; k++) {
        FREE(segs[k].str);
    }
}
// 按 '/' 拆分 path(空段丢掉, 同请求侧), 调用 _router_parse_seg 逐段解析, 写入新分配的 *out_segs
// 全部成功才落堆, 任一段失败时回滚已 MALLOC 的 str
static int32_t _router_parse_path(const char *path, size_t path_len, router_seg **out_segs,
                                  int32_t *out_n) {
    // 栈缓存: 先填这里, 全部成功后一次性 MALLOC + memcpy, 失败路径无需 realloc 回滚
    router_seg buf[URL_MAX_PATH_DEPTH];
    buf_ctx parts[URL_MAX_PATH_DEPTH];
    buf_ctx *pparts = parts;
    // split 只读不写, 去 const 只为合它的形参
    int32_t n = split((char *)path, path_len, "/", 1, &pparts, URL_MAX_PATH_DEPTH, SPLIT_SKIPEMPTY);
    if (ERR_FAILED == n) {
        LOG_WARN("router: path segments exceed %d, rejected.", URL_MAX_PATH_DEPTH);
        return ERR_FAILED;
    }
    for (int32_t k = 0; k < n; k++) {
        // WILD 段必须是最末段; 此时若上一段已是 WILD 却还有当前段, 说明 WILD 后还有内容, 拒绝
        // (两条匹配路径命中 WILD 都立即返成功, 中间 WILD 会让后续段静默失效, 易掉坑)
        if (k > 0 && ROUTER_SEG_WILD == buf[k - 1].t) {
            LOG_WARN("router: wildcard '*' must be the last segment.");
            _router_segs_free_str(buf, k);
            return ERR_FAILED;
        }
        _router_parse_seg(parts[k].data, parts[k].lens, &buf[k]);
    }
    // 可选段数决定 router_tree 的变体展开上限, 超出即拒绝注册
    int32_t nopt = 0, nparam = 0;
    for (int32_t k = 0; k < n; k++) {
        if (ROUTER_SEG_OPT == buf[k].t) {
            nopt++;
            continue;
        }
        // 可选段只能在末尾 (后面只许再跟可选段或 *): 中间的可选段让同一请求能对上几种展开, 取值没有唯一答案
        if (nopt > 0
            && ROUTER_SEG_WILD != buf[k].t) {
            LOG_ERROR("router: optional segment must be trailing (only '{x?}' or '*' may follow), route rejected.");
            _router_segs_free_str(buf, n);
            return ERR_FAILED;
        }
        if (ROUTER_SEG_PARAM == buf[k].t
            || ROUTER_SEG_WILD == buf[k].t) {
            nparam++;
        }
    }
    // 注册失败只能靠返回值反映, 而 router_get/post 这类包装的返回值调用方普遍不看,
    // 路由会就此静默消失成 404, 故下面两条都用 ERROR 而非 WARN
    if (nopt > ROUTER_MAX_OPT) {
        LOG_ERROR("router: optional segments %d exceed %d, route rejected.", nopt, ROUTER_MAX_OPT);
        _router_segs_free_str(buf, n);
        return ERR_FAILED;
    }
    // 必填段和末尾通配都占 params 槽；OPT 段可以整段不出现，不能把它们算进来误杀
    if (nparam > ROUTER_MAX_PARAMS) {
        LOG_ERROR("router: path params %d exceed %d, route rejected.", nparam, ROUTER_MAX_PARAMS);
        _router_segs_free_str(buf, n);
        return ERR_FAILED;
    }
    // 可选段出现得多的那几种形状参数会超限，路由树不收它们，这类请求会落 404
    if (nparam + nopt > ROUTER_MAX_PARAMS) {
        LOG_WARN("router: params %d with optional segments exceed %d, the longer shapes are not registered.",
                 nparam + nopt, ROUTER_MAX_PARAMS);
    }
    // 重名占位符注册期就拒: 匹配期两份都会填进 params, 而取值方按首个还是末个取
    // 并无统一答案, 同一条路由能给出不同的参数值
    for (int32_t k = 0; k < n; k++) {
        if (ROUTER_SEG_PARAM != buf[k].t
            && ROUTER_SEG_OPT != buf[k].t) {
            continue;
        }
        for (int32_t j = k + 1; j < n; j++) {
            if ((ROUTER_SEG_PARAM == buf[j].t || ROUTER_SEG_OPT == buf[j].t)
                && buf[k].str_len == buf[j].str_len
                && 0 == memcmp(buf[k].str, buf[j].str, buf[k].str_len)) {
                LOG_ERROR("router: duplicate path parameter '%s', route rejected.", buf[k].str);
                _router_segs_free_str(buf, n);
                return ERR_FAILED;
            }
        }
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
// HTTP 方法字符串 → 位掩码; 未识别返回 0, dispatch 处响应 405
static router_method _router_method_str_to_mask(const char *m, size_t n) {
    if (3 == n && 0 == memcmp(m, "GET", 3)) {
        return ROUTER_M_GET;
    }
    if (4 == n && 0 == memcmp(m, "POST", 4)) {
        return ROUTER_M_POST;
    }
    if (3 == n && 0 == memcmp(m, "PUT", 3)) {
        return ROUTER_M_PUT;
    }
    if (6 == n && 0 == memcmp(m, "DELETE", 6)) {
        return ROUTER_M_DELETE;
    }
    if (5 == n && 0 == memcmp(m, "PATCH", 5)) {
        return ROUTER_M_PATCH;
    }
    if (4 == n && 0 == memcmp(m, "HEAD", 4)) {
        return ROUTER_M_HEAD;
    }
    if (7 == n && 0 == memcmp(m, "OPTIONS", 7)) {
        return ROUTER_M_OPTIONS;
    }
    return 0;
}
router_ctx *router_new(void) {
    router_ctx *r;
    MALLOC(r, sizeof(router_ctx));
    ZERO(r, sizeof(router_ctx));
    route_ptr_arr_init(&r->routes, 8);
    rmw_arr_init(&r->global_mw, 8);
    rnamed_arr_init(&r->named, 8);
    r->tree = rtree_new();
    return r;
}
void router_set_own_chain(router_ctx *r, int32_t on) {
    if (NULL != r) {
        r->own_chain = (0 != on);
    }
}
// 流式表的 hash / compare / elfree。hash 只取 skid(键相等则 skid 必相等);compare 比 fd+skid,
// 不能比 index:它由 fd 派生,同一连接的两份标识可能一份填 INVALID_INDEX、一份填真值
static inline uint64_t _router_st_hash(const router_st_ent *ent) {
    return hash_u64(ent->sk.skid);
}
static inline int _router_st_cmp(const router_st_ent *x, const router_st_ent *y) {
    if (x->sk.fd != y->sk.fd) {
        return (x->sk.fd < y->sk.fd) ? -1 : 1;
    }
    if (x->sk.skid != y->sk.skid) {
        return (x->sk.skid < y->sk.skid) ? -1 : 1;
    }
    return 0;
}
// 表里的元素只是键 + 指针, 摘表项不会连带释放本体。
// 能走到这里的都是流没收齐就没了(连接断 / 被同连接新首帧顶掉 / router_free),
// 给 on_chunk 最后一次机会清 ctx->user —— 正常收尾走 feed 里的 END 分支, 不经过这里。
// 调用方必须先把表项摘掉再调本函数: on_chunk 里调 router_closed 是允许的, 那时表还留着这条就会二次释放
static void _router_st_free(void *item) {
    router_stream *st = ((router_st_ent *)item)->st;
    st->on_chunk(&st->req, ROUTER_STREAM_ABORT, NULL, 0, st->on_chunk_arg);
    FREE(st);
}
// 摘掉一条流式记录并释放它。st_map_delete 只返回元素副本、不会自动调 elfree, 得在这里补上
static void _router_st_drop(router_ctx *r, sock_ctx *sk) {
    if (NULL == r->streams) {
        return;
    }
    router_st_ent probe;
    probe.sk = *sk;
    probe.st = NULL;
    router_st_ent *removed = st_map_delete(r->streams, &probe);
    if (NULL != removed) {
        _router_st_free(removed);
    }
}
// 排空流式表。每轮都从 i = 0 重新起步, 不复用被 st_map_delete 作废的游标
static void _router_st_drain(router_ctx *r) {
    size_t i;
    router_st_ent *item;
    sock_ctx sk;
    while (0 != st_map_size(r->streams)) {
        i = 0;
        if (!st_map_iter(r->streams, &i, &item)) {
            break;
        }
        sk = item->sk;
        _router_st_drop(r, &sk);
    }
}
void router_free(router_ctx *r) {
    if (NULL == r) {
        return;
    }
    // 必须排在路由表之前释放: 每条流都要投一次 ROUTER_STREAM_ABORT,
    // 那次回调还能读路径参数, 而 params[].key 指向下面就要被释放的 segs[].str
    if (NULL != r->streams) {
        _router_st_drain(r);
        st_map_free(r->streams);
    }
    // 逐 entry 释放其内嵌的字符串、变体和中间件数组
    router_entry *e;
    for (int32_t i = 0; i < _R_ROUTES_N(r); i++) {
        e = _R_ROUTES(r)[i];
        _router_segs_free_str(e->segs, e->segs_n);
        FREE(e->segs);
        FREE(e->variants);
        FREE(e->mws);
        FREE(e);
    }
    route_ptr_arr_free(&r->routes);
    rtree_free(r->tree);
    rmw_arr_free(&r->global_mw);
    // 具名表 name 是 router_define 中 MALLOC + memcpy 的副本
    named_mw *nms = _R_NAMED(r);
    for (int32_t i = 0; i < _R_NAMED_N(r); i++) {
        FREE(nms[i].name);
    }
    rnamed_arr_free(&r->named);
    FREE(r);
}
// 按名查具名中间件; 数量小, 线性扫描即可。未注册即引用方注册失败, 返回值常没人看, 故打 ERROR
static int32_t _router_resolve_mw(router_ctx *r, const char *name, router_step *out) {
    named_mw *nms = _R_NAMED(r);
    for (int32_t i = 0; i < _R_NAMED_N(r); i++) {
        if (0 == strcmp(nms[i].name, name)) {
            *out = nms[i].step;
            return ERR_OK;
        }
    }
    LOG_ERROR("router: middleware '%s' not defined, registration rejected.", name);
    return ERR_FAILED;
}
int32_t router_define(router_ctx *r, const char *name, router_cb fn, void *arg) {
    if (NULL == fn) {
        LOG_ERROR("router: router_define '%s' needs a function.", name);
        return ERR_FAILED;
    }
    // 同名直接覆盖 named 表项; 注意 router_add / router_use 在被调时已把回调快照
    // 存进 entry->mws / global_mw, 覆盖只影响其后注册的路由 / 全局中间件
    named_mw *nms = _R_NAMED(r);
    for (int32_t i = 0; i < _R_NAMED_N(r); i++) {
        if (0 == strcmp(nms[i].name, name)) {
            nms[i].step.cb = fn;
            nms[i].step.arg = arg;
            return ERR_OK;
        }
    }
    // strdup 一份, 调用方栈上 / 常量区字符串都能用
    size_t len = strlen(name);
    char *dup = dup_zero(name, len);
    named_mw item;
    item.name = dup;
    item.step.cb = fn;
    item.step.arg = arg;
    rnamed_arr_push_back(&r->named, &item);
    return ERR_OK;
}
// 执行链长度 = 全局中间件 + 路由级中间件 + 末位一格(普通路由的 handler / 流式路由的准入哨兵)。
// 注册时、后加全局中间件时、派发前各判一次, 三处共用这一个谓词
static inline int32_t _router_chain_over(const router_ctx *r, int32_t mws_n) {
    return _R_GMW_N(r) + mws_n + 1 > ROUTER_MAX_CHAIN;
}
int32_t router_use(router_ctx *r, const char *name) {
    // 走 _router_resolve_mw 把名字转成回调+arg, 再委托给 _use_fn 统一入数组
    router_step step;
    if (ERR_OK != _router_resolve_mw(r, name, &step)) {
        return ERR_FAILED;
    }
    return router_use_fn(r, step.cb, step.arg);
}
int32_t router_use_fn(router_ctx *r, router_cb fn, void *arg) {
    if (NULL == fn) {
        LOG_ERROR("router: router_use_fn needs a function.");
        return ERR_FAILED;
    }
    router_step step;
    step.cb = fn;
    step.arg = arg;
    rmw_arr_push_back(&r->global_mw, &step);
    // 已注册的路由是按当时的全局中间件数判过链长的; 全局中间件后加就得回头再判一遍,
    // 否则超限只在跑起来后表现为每请求 500 "Chain too long"
    if (0 != r->own_chain) {
        return ERR_OK;
    }
    router_entry *e;
    for (int32_t i = 0; i < _R_ROUTES_N(r); i++) {
        e = _route(r, i);
        if (0 != _router_chain_over(r, e->mws_n)) {
            LOG_WARN("router: route %d chain now exceeds %d (global=%d, route=%d) after router_use.",
                     i, ROUTER_MAX_CHAIN, _R_GMW_N(r), e->mws_n);
        }
    }
    return ERR_OK;
}
int32_t router_set_no_route(router_ctx *r, router_cb cb, void *arg) {
    if (NULL == r) {
        return ERR_FAILED;
    }
    r->no_route.cb = cb;
    r->no_route.arg = arg;
    return ERR_OK;
}
int32_t router_set_no_method(router_ctx *r, router_cb cb, void *arg) {
    if (NULL == r) {
        return ERR_FAILED;
    }
    r->no_method.cb = cb;
    r->no_method.arg = arg;
    return ERR_OK;
}
// group 是纯栈对象, 字段全部按值/指针存; 嵌套靠 parent 指针链向上找祖先节点。
// 调用方必须保证 prefix / mw_names 在所有 router_* 注册调用期间生命周期有效
// (一般用字符串字面量 / 静态数组即可)
// root 与 nest 的公共字段。g 是调用方栈上对象且两个入口都不 ZERO, 靠逐字段写满,
// 所以往 router_group 加字段必须同时进这里, 否则漏掉的那个字段是未初始化栈值
static void _router_group_fill(router_group *g, const char *prefix,
                               const char *const *mw_names, int32_t n) {
    g->prefix = NULL == prefix ? "" : prefix;
    g->prefix_len = (uint32_t)strlen(g->prefix);
    g->mw_names = mw_names;
    g->mw_names_n = n;
}
void router_group_root(router_group *g, const char *prefix,
                       const char *const *mw_names, int32_t n) {
    g->parent = NULL;
    _router_group_fill(g, prefix, mw_names, n);
}
void router_group_nest(const router_group *parent, router_group *g, const char *prefix,
                       const char *const *mw_names, int32_t n) {
    g->parent = parent;
    _router_group_fill(g, prefix, mw_names, n);
}
// 沿父链按 root→leaf 顺序把各级 prefix 拼到 out, 每级后面补一个 '/'(同 Laravel; 多出来的斜杠
// 切段时当空段丢掉), *out_len 写已用字节数。
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
    if (*out_len + g->prefix_len + 1 > cap) {
        return ERR_FAILED;
    }
    memcpy(out + *out_len, g->prefix, g->prefix_len);
    *out_len += g->prefix_len;
    out[(*out_len)++] = '/';
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
// 按名解析 n 个中间件追加到 out[*k] 起; 已在 out 里的跳过, 同一个只留第一次出现的位置(同 Laravel)。
// 有名字没 define 返回 ERR_FAILED
static int32_t _router_mws_add(router_ctx *r, const char *const *names, int32_t n,
                               router_step *out, int32_t *k) {
    router_step step;
    int32_t j;
    for (int32_t i = 0; i < n; i++) {
        if (ERR_OK != _router_resolve_mw(r, names[i], &step)) {
            return ERR_FAILED;
        }
        for (j = 0; j < *k; j++) {
            if (out[j].cb == step.cb && out[j].arg == step.arg) {
                break;
            }
        }
        if (j == *k) {
            out[(*k)++] = step;
        }
    }
    return ERR_OK;
}
// 已解析的 step 数组追加到 out[*k], 同样按 (cb,arg) 去重
static int32_t _router_mws_add_steps(const router_step *in, int32_t n,
                                     router_step *out, int32_t *k) {
    int32_t j;
    for (int32_t i = 0; i < n; i++) {
        if (NULL == in[i].cb) {
            LOG_ERROR("%s", "router: middleware step without callback, route rejected.");
            return ERR_FAILED;
        }
        for (j = 0; j < *k; j++) {
            if (out[j].cb == in[i].cb && out[j].arg == in[i].arg) {
                break;
            }
        }
        if (j == *k) {
            out[(*k)++] = in[i];
        }
    }
    return ERR_OK;
}
// 沿父链按 root→leaf 顺序把各级中间件解析后追加到 out; 递归保证先写父再写子,
// 与中间件执行顺序 (祖先先于子孙) 一致
static int32_t _router_group_mws(router_ctx *r, const router_group *g, router_step *out, int32_t *k) {
    if (NULL == g) {
        return ERR_OK;
    }
    if (ERR_OK != _router_group_mws(r, g->parent, out, k)) {
        return ERR_FAILED;
    }
    return _router_mws_add(r, g->mw_names, g->mw_names_n, out, k);
}
// 树插入成功后的路由提交：条目单独 MALLOC，接管 ar->variants。
// route_id 必须等于当前路由表尾下标；树叶子已经用这个 id 指向本路由。
static router_entry *_router_route_commit(router_ctx *r,
                                          router_seg *segs, int32_t segs_n,
                                          router_cb h, void *h_arg,
                                          router_stream_cb sh, void *sh_arg,
                                          router_step *mws, int32_t mws_n,
                                          int32_t route_id, rtree_add_result *ar) {
    int32_t shadowed = ar->shadowed;
    ASSERTAB(route_id == _R_ROUTES_N(r), "router route id out of order.");
    router_entry *e;
    MALLOC(e, sizeof(*e));
    ZERO(e, sizeof(*e));
    e->id = route_id;
    e->segs = segs;
    e->segs_n = segs_n;
    e->variants = ar->variants;
    e->variants_n = ar->n;
    ar->variants = NULL;
    rtree_add_result_free(ar);
    e->mws = mws;
    e->mws_n = mws_n;
    e->handler = h;
    e->handler_arg = h_arg;
    e->on_chunk = sh;
    e->on_chunk_arg = sh_arg;
    route_ptr_arr_push_back(&r->routes, &e);
    if (shadowed > 0) {
        LOG_WARN("router: route %d has %d shadowed variant(s); the rest are registered.", route_id, shadowed);
    }
    return e;
}
// router_add / router_add_stream 的共同实现: h 与 sh 恰有一个非空,
// 决定这条路由是普通派发还是流式接收
static router_entry *_router_add_common(router_ctx *r, const router_group *g,
                                        router_method method, const char *path,
                                        router_cb h, void *h_arg,
                                        router_stream_cb sh, void *sh_arg,
                                        const char *const *mws, int32_t mws_n,
                                        const router_step *steps, int32_t steps_n) {
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
    // 2) 合并中间件: group (root→leaf) → 路由级, 去重; 有名字没 define 即整条注册失败, 不留缺了中间件的路由
    int32_t total_mws = _router_group_count_mws(g) + mws_n + steps_n;
    router_step *mws_arr = NULL;
    if (total_mws > 0) {
        MALLOC(mws_arr, sizeof(router_step) * (size_t)total_mws);
        int32_t k = 0;
        if (ERR_OK != _router_group_mws(r, g, mws_arr, &k)
            || ERR_OK != _router_mws_add(r, mws, mws_n, mws_arr, &k)
            || ERR_OK != _router_mws_add_steps(steps, steps_n, mws_arr, &k)) {
            FREE(mws_arr);
            return NULL;
        }
        total_mws = k;// 去重后的条数
    }
    // 3) 把 full_buf 解析为段数组 (LIT/PARAM/OPT/WILD), 解析完段数组进堆, full_buf 出栈丢弃
    router_seg *segs = NULL;
    int32_t segs_n = 0;
    if (ERR_OK != _router_parse_path(full_buf, full_len, &segs, &segs_n)) {
        FREE(mws_arr);
        return NULL;
    }
    if (0 == r->own_chain
        && 0 != _router_chain_over(r, total_mws)) {
        LOG_WARN("router: chain will exceed %d (global=%d, route=%d) at dispatch.",
                 ROUTER_MAX_CHAIN, _R_GMW_N(r), total_mws);
    }
    // 4) 先插树，成功后再提交路由记录；失败时树里可能留下无叶子的中间节点，不影响匹配
    int32_t route_id = _R_ROUTES_N(r);
    rtree_add_result ar;
    int32_t trtn = rtree_add(r->tree, segs, segs_n, method,
                             (router_route_id)route_id, &ar);
    if (ERR_OK != trtn) {
        if (-2 == trtn) {
            LOG_WARN("router: route '%.*s' shadowed by existing routes, registration ignored.",
                     (int)full_len, full_buf);
        } else {
            LOG_ERROR("router: route '%.*s' rejected by route tree (method mask 0x%x).",
                      (int)full_len, full_buf, (unsigned)method);
        }
        _router_segs_free_str(segs, segs_n);
        FREE(segs);
        FREE(mws_arr);
        rtree_add_result_free(&ar);
        return NULL;
    }
    return _router_route_commit(r, segs, segs_n,
                                h, h_arg, sh, sh_arg, mws_arr, total_mws, route_id, &ar);
}
router_entry *router_add(router_ctx *r, const router_group *g,
                         router_method method, const char *path,
                         router_cb h, void *arg,
                         const char *const *mws, int32_t mws_n) {
    if (NULL == h) {
        LOG_WARN("router: router_add needs a handler.");
        return NULL;
    }
    return _router_add_common(r, g, method, path, h, arg, NULL, NULL,
                              mws, mws_n, NULL, 0);
}
router_entry *router_add_steps(router_ctx *r, const router_group *g,
                               router_method method, const char *path,
                               router_cb h, void *arg,
                               const router_step *mws, int32_t mws_n) {
    if (NULL == h) {
        LOG_WARN("router: router_add_steps needs a handler.");
        return NULL;
    }
    return _router_add_common(r, g, method, path, h, arg, NULL, NULL,
                              NULL, 0, mws, mws_n);
}
router_entry *router_add_stream(router_ctx *r, const router_group *g,
                                router_method method, const char *path,
                                router_stream_cb sh, void *arg,
                                const char *const *mws, int32_t mws_n) {
    if (NULL == sh) {
        LOG_WARN("router: router_add_stream needs a stream callback.");
        return NULL;
    }
    router_entry *e = _router_add_common(r, g, method, path, NULL, NULL, sh, arg,
                                          mws, mws_n, NULL, 0);
    if (NULL != e) {
        r->has_stream = 1;
    }
    return e;
}
router_entry *router_add_stream_steps(router_ctx *r, const router_group *g,
                                      router_method method, const char *path,
                                      router_stream_cb sh, void *arg,
                                      const router_step *mws, int32_t mws_n) {
    if (NULL == sh) {
        LOG_WARN("router: router_add_stream_steps needs a stream callback.");
        return NULL;
    }
    router_entry *e = _router_add_common(r, g, method, path, NULL, NULL, sh, arg,
                                          NULL, 0, mws, mws_n);
    if (NULL != e) {
        r->has_stream = 1;
    }
    return e;
}
// 便捷包装的展开点; 两个生成宏定义在文件头
// GET 连带 HEAD: HEAD 语义就是"要 GET 的头不要体", 分开注册会让 HEAD 落 404。
// 响应侧由 _router_send_core 按 ctx->method 抑制报文体
DEF_ROUTE_FN(get,     ROUTER_M_GET | ROUTER_M_HEAD)
DEF_ROUTE_FN(post,    ROUTER_M_POST)
DEF_ROUTE_FN(put,     ROUTER_M_PUT)
DEF_ROUTE_FN(delete,  ROUTER_M_DELETE)
DEF_ROUTE_FN(patch,   ROUTER_M_PATCH)
DEF_ROUTE_FN(head,    ROUTER_M_HEAD)
DEF_ROUTE_FN(options, ROUTER_M_OPTIONS)
DEF_ROUTE_FN(any,     ROUTER_M_ANY)
DEF_STREAM_FN(post, ROUTER_M_POST)
DEF_STREAM_FN(put,  ROUTER_M_PUT)
// 注册用的方法串 → 掩码。支持 '|' 分隔的组合("GET|HEAD"): router_add 本就收掩码。
// ANY 也在这里映射(_router_method_str_to_mask 不含它);
// 任一段不认识、或出现空段("GET||HEAD" / 首尾竖线)一律整体失败, 不做部分接受
router_method router_method_mask(const char *m, size_t n) {
    router_method mask = 0;
    router_method one;
    size_t start = 0;
    size_t i;
    for (i = 0; i <= n; i++) {
        if (i != n
            && '|' != m[i]) {
            continue;
        }
        if (i == start) {
            return 0;
        }
        one = _router_method_str_to_mask(m + start, i - start);
        if (0 == one) {
            if (3 != i - start
                || 0 != memcmp(m + start, "ANY", 3)) {
                return 0;
            }
            one = ROUTER_M_ANY;
        }
        mask |= one;
        start = i + 1;
    }
    return mask;
}
// 只插树、不建 handler；命中后交给普通派发会被判成配置错误
int32_t router_add_index(router_ctx *r, const char *method, size_t method_len,
                       const char *path, size_t path_len) {
    router_method m = router_method_mask(method, method_len);
    if (0 == m) {
        return -1;
    }
    router_seg *segs = NULL;
    int32_t segs_n = 0;
    if (ERR_OK != _router_parse_path(path, path_len, &segs, &segs_n)) {
        return -1;
    }
    int32_t route_id = _R_ROUTES_N(r);
    rtree_add_result ar;
    int32_t trtn = rtree_add(r->tree, segs, segs_n, m,
                             (router_route_id)route_id, &ar);
    if (ERR_OK != trtn) {
        _router_segs_free_str(segs, segs_n);
        FREE(segs);
        rtree_add_result_free(&ar);
        return trtn;
    }
    return _router_route_commit(r, segs, segs_n,
                                NULL, NULL, NULL, NULL, NULL, 0, route_id, &ar)->id;
}
int32_t router_seg_index(router_ctx *r, int32_t idx, int32_t k, router_seg_type *t,
                         const char **str, uint32_t *str_len) {
    if (idx < 0 || idx >= _R_ROUTES_N(r) || k < 0) {
        return ERR_FAILED;
    }
    const router_entry *e = _croute(r, idx);
    if (k >= e->segs_n) {
        return ERR_FAILED;
    }
    router_seg *seg = &e->segs[k];
    *t = seg->t;
    *str = seg->str;
    *str_len = seg->str_len;
    return ERR_OK;
}
int32_t router_match_index(router_ctx *r, const char *method, size_t method_len,
                           const char *url, size_t url_len, router_req *ctx) {
    ctx->method = 0;
    ctx->allow = 0;
    ctx->params_n = 0;
    router_method m = _router_method_str_to_mask(method, method_len);
    if (0 == m) {
        return -3;
    }
    ctx->method = m;
    // url_parse 解码后段写入 ctx->url；params.val 指向该缓冲或 ctx->wildbuf。
    // url_parse 失败时 ctx->url 内容不可用，直接按 400 处理。
    if (ERR_OK != url_parse(ctx->url, url, url_len, '/', URL_DECODE | URL_SKIP_EMPTY)) {
        return -2;
    }
    // 只收 "/..." 与 "http://host/..." 两种目标: 别的形状第一段会被 url_parse 当成 host 吃掉
    if ((0 == url_len || '/' != url[0])
        && 0 == ctx->url->scheme.lens) {
        return -2;
    }
    rtree_match_result mr;
    int32_t rtn = rtree_match(r->tree, ctx->url->segs, ctx->url->npath,
                              ctx->wildbuf, sizeof(ctx->wildbuf), m, &mr);
    if (rtn < 0) {
        ctx->allow = mr.allow;
        return rtn;
    }
    const router_entry *e = _croute(r, rtn);
    if ((uint32_t)mr.variant_id >= (uint32_t)e->variants_n) {
        LOG_ERROR("router: route %d variant %u out of range.", rtn, mr.variant_id);
        return -1;
    }
    const rtree_variant *v = &e->variants[mr.variant_id];
    // 末尾通配可以零段命中，这时树不会给 "*" 填值：variant 的最后一个参数是 WILD，
    // params_n 恰好比 variant->param_n 少 1。其余数量不一致都算内部错误。
    if (mr.params_n != v->param_n) {
        if (mr.params_n + 1 != v->param_n || 0 == v->param_n) {
            LOG_ERROR("router: route %d variant %u param count mismatch.", rtn, mr.variant_id);
            return -1;
        }
        int32_t last_seg = v->param_seg[v->param_n - 1];
        if (last_seg >= e->segs_n
            || ROUTER_SEG_WILD != e->segs[last_seg].t) {
            LOG_ERROR("router: route %d variant %u zero wildcard metadata bad.", rtn, mr.variant_id);
            return -1;
        }
    }
    int32_t seg_idx;
    const router_seg *seg;
    for (int32_t k = 0; k < mr.params_n; k++) {
        seg_idx = v->param_seg[k];
        if (seg_idx >= e->segs_n) {
            LOG_ERROR("router: route %d variant %u param segment out of range.", rtn, mr.variant_id);
            return -1;
        }
        seg = &e->segs[seg_idx];
        if (ROUTER_SEG_WILD == seg->t) {
            ctx->params[k].key = "*";
            ctx->params[k].key_len = 1;
        } else {
            ctx->params[k].key = seg->str;
            ctx->params[k].key_len = seg->str_len;
        }
        ctx->params[k].val = (const char *)mr.params[k].val;
        ctx->params[k].val_len = mr.params[k].len;
    }
    ctx->params_n = mr.params_n;
    return rtn;
}
int32_t router_match_code(int32_t idx) {
    if (idx >= 0
        || -5 == idx) {
        return 200;
    }
    if (-3 == idx
        || -4 == idx) {
        return 405;
    }
    if (-2 == idx) {
        return 400;
    }
    return 404;
}
size_t router_allow_value(router_method allow, char out[ROUTER_ALLOW_LENS]) {
    static const router_method ms[] = { ROUTER_M_GET, ROUTER_M_HEAD, ROUTER_M_POST, ROUTER_M_PUT,
                                        ROUTER_M_PATCH, ROUTER_M_DELETE, ROUTER_M_OPTIONS };
    static const char *const names[] = { "GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS" };
    size_t n = 0;
    size_t lens;
    for (size_t i = 0; i < ARRAY_SIZE(ms); i++) {
        if (0 == (allow & ms[i])) {
            continue;
        }
        if (n > 0) {
            out[n++] = ',';
            out[n++] = ' ';
        }
        lens = strlen(names[i]);
        memcpy(out + n, names[i], lens);
        n += lens;
    }
    return n;
}
// 中间件需主动调本函数推进链路; 不调即截断, 后续 mw / handler 不执行。
// 因为是同步递归调用栈, 中间件可在 router_next 返回后做后置处理 (打日志 / 统计耗时)。
// 游标进入时推进、返回时恢复: 同一层再调一次 next 重跑的是它自己的下游, 不会越过下游里截断的那层
void router_next(router_req *ctx) {
    int32_t i = ctx->chain_i;
    if (i >= ctx->chain_n) {
        return;
    }
    ctx->chain_i = i + 1;
    ctx->chain[i].cb(ctx, ctx->chain[i].arg);
    ctx->chain_i = i;
}
char *router_req_header(router_req *ctx, const char *key, size_t *lens) {
    // 先把 lens 清 0, http_header 未找到时不写 lens, 调用方读到旧值会误判
    *lens = 0;
    // 流式路由过了首帧就没有 pack 了, 见 router_stream_cb 的说明
    if (NULL == ctx->pack) {
        return NULL;
    }
    return http_header(ctx->pack, key, lens);
}
const char *router_req_param(router_req *ctx, const char *key, size_t *lens) {
    // params 由 router_match_index 填; key 指向 segs[].str (\0 结尾), 这里按长度比对兼容外部传入串
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
    // url_parse 已完成 url_decode, 这里直接返底层 buf_ctx; 同名参数取末值的规则在 url_get_param
    buf_ctx *v = url_get_param(ctx->url, key);
    if (NULL == v) {
        *lens = 0;
        return NULL;
    }
    *lens = v->lens;
    // 值空(?a=)返非 NULL 零长指针, 区别于键不存在的 NULL
    return (NULL != v->data) ? (const char *)v->data : "";
}
void *router_req_body(router_req *ctx, size_t *lens) {
    if (NULL == ctx->pack) {
        *lens = 0;
        return NULL;
    }
    return http_data(ctx->pack, lens);
}
// 组装完整 HTTP 响应并 ev_send 推出去; 自动写 Content-Length, ctline 非 NULL 时原样追加这一整行 Content-Type
// (ROUTER_CT_* 常量, ctlens 为其长度), extra 由调用方追加 (不可重复 CL / CT / Transfer-Encoding)。
// bw 内部托管, ev_send copy=0 已转移所有权, 返回后无需 binary_free。
// 不接 router_req, 供无 ctx 的错误路径共用; 有 ctx 的入口走 _router_send_resp 包一层置 responded 并按需关连接。
// head_only 非 0 时只回头不回体, 但 Content-Length 仍写 body 的真实长度 —— RFC 7231 §4.3.2
// 要求 HEAD 的响应头与同一资源的 GET 一致, 多发的字节会被当成下一条响应而让 keep-alive 错位
static void _router_send_core(task_ctx *task, sock_ctx *sk, int32_t code,
                              int32_t head_only, const char *ctline, size_t ctlens,
                              const http_header_ctx *extra, int32_t extra_n,
                              const char *body, size_t body_len) {
    size_t head_reserve = 256;
    for (int32_t i = 0; i < extra_n; i++) {
        // 值为 NULL 的条目下面会丢, 它的长度可能没初始化, 不算进预留
        if (NULL != extra[i].value.data) {
            head_reserve += extra[i].key.lens + extra[i].value.lens + 4;
        }
    }
    int32_t withbody = !http_code_nobody(code) && 0 == head_only && !EMPTYPTR(body, body_len);
    binary_ctx bw;
    binary_init_write(&bw, (withbody ? body_len : 0) + head_reserve, 0);
    http_pack_resp(&bw, code);
    if (NULL != ctline) {
        binary_set_binary(&bw, ctline, ctlens);
    }
    // 非法头一律整条丢弃而不截断、更不 abort: 截断头名等于把它改成另一个名字发上线缆, 比不发更糟。
    // 筛过的头按长度直写, 线格式同 http_pack_head2(那边对 CR/LF 是断言退进程, 业务数据不能走那条)
    http_key_rc krc;
    for (int32_t i = 0; i < extra_n; i++) {
        // 帧长头归 http_pack_content 独占: 再叠一条 Content-Length 或补一条 Transfer-Encoding,
        // 对端(含 srey 自己的解析器)判为请求走私、整包丢弃并断连
        krc = http_head_key_check((const char *)extra[i].key.data, extra[i].key.lens, HTTP_AUTO_FRAME);
        if (HTTP_KEY_BAD == krc) {
            LOG_WARN("router: header key is not a valid token or is too long, dropped.");
            continue;
        }
        if (HTTP_KEY_AUTO == krc) {
            LOG_WARN("router: framing header must not come from extra, dropped.");
            continue;
        }
        // 值为 NULL 一律丢: 调用方传 NULL 是"这条别发", 空值头要发就传 {"", 0}
        if (NULL == extra[i].value.data
            || !http_head_val_ok((const char *)extra[i].value.data, extra[i].value.lens)) {
            LOG_WARN("router: header value is NULL or contains NUL/CRLF, dropped.");
            continue;
        }
        binary_set_binary(&bw, extra[i].key.data, extra[i].key.lens);
        binary_set_binary(&bw, ": ", sizeof(": ") - 1);
        binary_set_binary(&bw, extra[i].value.data, extra[i].value.lens);
        binary_set_binary(&bw, FLAG_CRLF, CRLF_SIZE);
    }
    // 1xx/204/304 禁带 Content-Length 与报文体, 只收尾不写 body(给了也丢);
    // 其余走 http_pack_content, 它写 \r\n\r\n + body 完成整包, 空 body 也统一收敛成
    // Content-Length: 0, 各入口不必自己归一
    if (http_code_nobody(code)) {
        http_pack_end(&bw);
    } else if (0 != head_only) {
        // 空判照抄 http_pack_content 的 EMPTYPTR: 同一资源的 GET 走那条会写 0,
        // 这里算出别的数就是两个方法自报的长度不一致。线格式同 http_pack_head
        binary_set_binary(&bw, "Content-Length: ", sizeof("Content-Length: ") - 1);
        binary_set_uint(&bw, EMPTYPTR(body, body_len) ? 0 : (uint64_t)body_len, 10);
        binary_set_binary(&bw, FLAG_CRLF, CRLF_SIZE);
        http_pack_end(&bw);
    } else {
        http_pack_content(&bw, (void *)body, body_len);
    }
    ev_send(&task->loader->netev, sk, bw.data, bw.offset, 0);
}
// _router_send_core 的 router_req 版: 发完置 responded 避免 dispatch 末尾兜底 500 又发一遍;
// 请求要求响应后关连接的(wantclose)随即关
static void _router_send_resp(router_req *ctx, int32_t code, const char *ctline, size_t ctlens,
                              const http_header_ctx *extra, int32_t extra_n,
                              const char *body, size_t body_len) {
    _router_send_core(ctx->task, &ctx->sk, code,
                      ROUTER_M_HEAD == ctx->method, ctline, ctlens,
                      extra, extra_n, body, body_len);
    ctx->responded = 1;
    if (0 != ctx->wantclose) {
        ev_close(&ctx->task->loader->netev, &ctx->sk);
    }
}
void router_req_text(router_req *ctx, int32_t code, const char *body, size_t lens) {
    _router_send_resp(ctx, code, ROUTER_CT_LINE(ROUTER_CT_TEXT), NULL, 0, body, lens);
}
void router_req_json(router_req *ctx, int32_t code, const char *json, size_t lens) {
    _router_send_resp(ctx, code, ROUTER_CT_LINE(ROUTER_CT_JSON), NULL, 0, json, lens);
}
void router_req_html(router_req *ctx, int32_t code, const char *body, size_t lens) {
    _router_send_resp(ctx, code, ROUTER_CT_LINE(ROUTER_CT_HTML), NULL, 0, body, lens);
}
void router_req_respond(router_req *ctx, int32_t code,
                      const http_header_ctx *extra, int32_t extra_n,
                      const char *body, size_t body_len) {
    _router_send_resp(ctx, code, NULL, 0, extra, extra_n, body, body_len);
}
// 纯文本的错误响应, body 走 strlen; 只发不关、不置 responded。给没有 router_req 的
// (router_reject_chunked) 与回完另行关连接的错误路径用; 有 ctx 的兜底走 router_req_text
static void _router_send_simple(task_ctx *task, sock_ctx *sk, int32_t code,
                                int32_t head_only, const char *body) {
    _router_send_core(task, sk, code, head_only, ROUTER_CT_LINE(ROUTER_CT_TEXT), NULL, 0,
                      body, (NULL == body) ? 0 : strlen(body));
}
// 拒绝 chunked 请求：回 411 后立即关闭连接
void router_reject_chunked(task_ctx *task, sock_ctx *sk, int32_t head_only) {
    _router_send_simple(task, sk, 411, head_only, ROUTER_BODY_411);
    ev_close(&task->loader->netev, sk);
}
// 流式路由的链尾哨兵: 跑到这里说明每个中间件都调了 router_next。不能拿游标判:
// router_next 返回时游标已恢复, 从链外看不出在哪层截断
static void _router_admit(router_req *ctx, void *arg) {
    (void)arg;
    ctx->admitted = 1;
}
// 拼接执行链: 全局中间件 → 路由级中间件 → 末位 (普通路由是 handler, 流式路由是准入哨兵)。
// 超出 ROUTER_MAX_CHAIN 视为配置错误, 由调用方回 500
static int32_t _router_chain_build(router_ctx *r, router_entry *e, router_req *ctx) {
    if (0 != _router_chain_over(r, e->mws_n)) {
        LOG_WARN("router: chain exceeds %d (global=%d, route=%d), rejected.",
                 ROUTER_MAX_CHAIN, _R_GMW_N(r), e->mws_n);
        return ERR_FAILED;
    }
    int32_t k = 0;
    router_step *gmw = _R_GMW(r);
    for (int32_t i = 0; i < _R_GMW_N(r); i++) {
        ctx->chain[k++] = gmw[i];
    }
    for (int32_t i = 0; i < e->mws_n; i++) {
        ctx->chain[k++] = e->mws[i];
    }
    if (NULL != e->handler) {
        ctx->chain[k].cb = e->handler;
        ctx->chain[k].arg = e->handler_arg;
    } else {
        ctx->chain[k].cb = _router_admit;
        ctx->chain[k].arg = NULL;
    }
    k++;
    ctx->chain_n = k;
    ctx->chain_i = 0;
    return ERR_OK;
}
// -1 / -4 对应的回退 handler; 未设置返回 NULL。OPTIONS 自动应答(-5)不在这条路上。
static const router_step *_router_fallback_step(const router_ctx *r, int32_t idx) {
    if (-1 == idx && NULL != r->no_route.cb) {
        return &r->no_route;
    }
    if (-4 == idx && NULL != r->no_method.cb) {
        return &r->no_method;
    }
    return NULL;
}
static void _router_run_step(router_req *ctx, const router_step *step) {
    ctx->chain[0] = *step;
    ctx->chain_n = 1;
    ctx->chain_i = 0;
    router_next(ctx);
}
int32_t router_exec(router_ctx *r, int32_t route_id, router_req *ctx) {
    if (NULL == r || NULL == ctx) {
        return ERR_FAILED;
    }
    const router_step *fb = _router_fallback_step(r, route_id);
    if (NULL != fb) {
        _router_run_step(ctx, fb);
        return ERR_OK;
    }
    if (route_id < 0 || route_id >= _R_ROUTES_N(r)) {
        return ERR_FAILED;
    }
    router_entry *e = _route(r, route_id);
    if (NULL == e->handler) {
        return ERR_FAILED;// 纯匹配条目或流式路由不走普通执行链
    }
    if (ERR_OK != _router_chain_build(r, e, ctx)) {
        return ERR_FAILED;
    }
    router_next(ctx);
    return ERR_OK;
}
int32_t router_entry_id(const router_entry *e) {
    return NULL == e ? -1 : e->id;
}
int32_t router_chain_count(const router_ctx *r, int32_t route_id) {
    if (NULL == r) {
        return -1;
    }
    const router_step *fb = _router_fallback_step(r, route_id);
    if (NULL != fb) {
        return 1;
    }
    if (route_id < 0 || route_id >= _R_ROUTES_N(r)) {
        return -1;
    }
    const router_entry *e = _croute(r, route_id);
    return (int32_t)r->global_mw.size + e->mws_n + (NULL != e->handler ? 1 : 0);
}
int32_t router_chain_at(const router_ctx *r, int32_t route_id, int32_t k, router_step *out) {
    if (NULL == r
        || NULL == out) {
        return ERR_FAILED;
    }
    const router_step *fb = _router_fallback_step(r, route_id);
    if (NULL != fb) {
        if (0 != k) {
            return ERR_FAILED;
        }
        *out = *fb;
        return ERR_OK;
    }
    int32_t n = router_chain_count(r, route_id);
    if (k < 0 || k >= n) {
        return ERR_FAILED;
    }
    const router_entry *e = _croute(r, route_id);
    int32_t gn = (int32_t)r->global_mw.size;
    if (k < gn) {
        *out = r->global_mw.ptr[k];
        return ERR_OK;
    }
    k -= gn;
    if (k < e->mws_n) {
        *out = e->mws[k];
        return ERR_OK;
    }
    k -= e->mws_n;
    if (0 == k && NULL != e->handler) {
        out->cb = e->handler;
        out->arg = e->handler_arg;
        return ERR_OK;
    }
    return ERR_FAILED;
}
// 按 code 生成错误正文; 为什么两个入口必须共用见 ROUTER_CODE_BODY_LENS 处的说明
static void _router_code_body(int32_t code, char body[ROUTER_CODE_BODY_LENS]) {
    const char *txt = http_code_status(code);
    size_t n = strlen(txt);
    if (n > ROUTER_CODE_BODY_LENS - 2) {
        n = ROUTER_CODE_BODY_LENS - 2;
    }
    memcpy(body, txt, n);
    body[n] = '\n';
    body[n + 1] = '\0';
}
// 匹配失败(router_match_index 返回负值)的应答, 只发不关, 关不关由调用方定。
// ctx->allow 非 0 时带 Allow 头; 码为 200 即 OPTIONS 自动应答, 不带正文
static void _router_send_fail(router_req *ctx, int32_t idx) {
    int32_t code = router_match_code(idx);
    int32_t head_only = ROUTER_M_HEAD == ctx->method;
    char body[ROUTER_CODE_BODY_LENS];
    char allow[ROUTER_ALLOW_LENS];
    http_header_ctx hd;
    if (0 == ctx->allow) {
        _router_code_body(code, body);
        _router_send_simple(ctx->task, &ctx->sk, code, head_only, body);
        return;
    }
    hd.key.data = "Allow";
    hd.key.lens = sizeof("Allow") - 1;
    hd.value.data = allow;
    hd.value.lens = router_allow_value(ctx->allow, allow);
    if (200 == code) {
        _router_send_core(ctx->task, &ctx->sk, code, head_only, NULL, 0, &hd, 1, NULL, 0);
        return;
    }
    _router_code_body(code, body);
    _router_send_core(ctx->task, &ctx->sk, code, head_only, ROUTER_CT_LINE(ROUTER_CT_TEXT),
                      &hd, 1, body, strlen(body));
}
// 三个派发入口匹配失败时共用：跑回退 handler，没配回退或它没回应就回失败码。返回 1 表示失败码是这里回的
static int32_t _router_fallback(router_ctx *r, router_req *ctx, int32_t idx) {
    const router_step *fb = _router_fallback_step(r, idx);
    if (NULL != fb) {
        _router_run_step(ctx, fb);
        if (ctx->responded) {
            return 0;
        }
    }
    _router_send_fail(ctx, idx);
    return 1;
}
// status[0] = 方法, status[1] = 请求 URI(两者为空的请求行解析器已拒); pack 为空或不带请求行算无效 HTTP。
// 三个派发入口共用: 返 NULL 即静默丢, 连响应都不发——对面发的不是 HTTP, 回什么都没意义
static buf_ctx *_router_http_status(struct http_pack_ctx *pack) {
    if (NULL == pack) {
        return NULL;
    }
    return http_status(pack);
}
// 请求上下文的装配, 并按请求行匹配路由, 返回 router_match_index 的结果; 三个派发入口共用。
// status 由调用方先取好: 流式入口要赶在分配请求上下文之前把无效 HTTP 挡掉。
// url 由调用方持有: 它得和 ctx 活得一样久, 且有意不清零(url_parse 自己清该清的, 那三个大数组白清就是每请求 4KB 死写)
static int32_t _router_req_match(router_ctx *r, router_req *ctx, url_ctx *url, task_ctx *task,
                                sock_ctx *sk, struct http_pack_ctx *pack, const buf_ctx *status) {
    ZERO(ctx, offsetof(router_req, chain));
    ctx->url = url;
    ctx->task = task;
    ctx->sk = *sk;
    ctx->pack = pack;
    ctx->wantclose = http_want_close(pack);
    return router_match_index(r, status[0].data, status[0].lens,
                              status[1].data, status[1].lens, ctx);
}
// router_add_index 注册的条目两个回调恒为 NULL(只配 router_match_index 用), 混进派发
// 会直接调空指针; 视为配置错误。两个入口对它给出同一个码, 不要退化成"chunked 不支持"
static int32_t _router_entry_misconfigured(const router_entry *e, int32_t idx) {
    if (NULL != e->handler
        || NULL != e->on_chunk) {
        return 0;
    }
    LOG_WARN("router: route %d has no handler (registered by router_add_index), rejected.", idx);
    return 1;
}
// 派发流程: 解方法 → URL parse → 树匹配 → 拼 chain → 推进 → 兜底 500
void router_dispatch(router_ctx *r, task_ctx *task, sock_ctx *sk, struct http_pack_ctx *pack) {
    if (NULL == r) {
        return;
    }
    buf_ctx *status = _router_http_status(pack);
    if (NULL == status) {
        return;
    }
    url_ctx url;
    router_req ctx;
    int32_t idx = _router_req_match(r, &ctx, &url, task, sk, pack, status);
    if (idx < 0) {
        // 回退自己回包时已按 wantclose 关过（异步回包的也由它自己关），只在失败码由这里回时补关
        if (0 != _router_fallback(r, &ctx, idx)
            && 0 != ctx.wantclose) {
            ev_close(&task->loader->netev, sk);
        }
        return;
    }
    router_entry *matched = _route(r, idx);
    if (0 != _router_entry_misconfigured(matched, idx)) {
        router_req_text(&ctx, 500, ROUTER_BODY_500, sizeof(ROUTER_BODY_500) - 1);
        return;
    }
    if (ERR_OK != _router_chain_build(r, matched, &ctx)) {
        router_req_text(&ctx, 500, ROUTER_BODY_CHAIN, sizeof(ROUTER_BODY_CHAIN) - 1);
        return;
    }
    // 启动链路, 第一个中间件 / handler 通过 router_next 递归推进
    router_next(&ctx);
    // 流式路由命中一次到齐的请求: 准入通过才把请求体按 slice == 0 一次交出去
    if (NULL != matched->on_chunk
        && ctx.admitted) {
        size_t dlens = 0;
        void *data = http_data(pack, &dlens);
        matched->on_chunk(&ctx, 0, data, dlens, matched->on_chunk_arg);
    }
    // 中间件主动 return 不调 router_next 是合法截断; 但都没写响应 (handler 漏发 + 中间件
    // 也没截断) 时, 客户端会卡死, 这里兜底 500 让它别等
    if (!ctx.responded) {
        router_req_text(&ctx, 500, ROUTER_BODY_500, sizeof(ROUTER_BODY_500) - 1);
    }
}
void router_closed(router_ctx *r, sock_ctx *sk) {
    if (NULL == r
        || NULL == r->streams) {
        return;
    }
    _router_st_drop(r, sk);
}
void router_closed_watch(task_ctx *task, sock_ctx *sk, subtype_t pktype, uint8_t client, int32_t erro,
                         uint64_t sess, void *arg) {
    (void)task;
    (void)pktype;
    (void)client;
    (void)erro;
    (void)sess;
    router_closed((router_ctx *)arg, sk);
}
// 首帧不通过时的统一收尾: 回响应 → 关连接 → 丢记录。请求体还在后面, 连接留着也收不了。
// code 传 0 表示调用方已经写过响应, 只关连接
static void _router_st_reject(router_stream *st, task_ctx *task, int32_t code, const char *body) {
    if (code > 0) {
        _router_send_simple(task, &st->req.sk, code,
                            ROUTER_M_HEAD == st->req.method, body);
    }
    ev_close(&task->loader->netev, &st->req.sk);
    FREE(st);
}
// 流式首帧: 匹配路由 → 跑准入链 → 进表 → 回调 PROT_SLICE_START
static void _router_st_begin(router_ctx *r, task_ctx *task, sock_ctx *sk, struct http_pack_ctx *pack) {
    buf_ctx *status = _router_http_status(pack);
    if (NULL == status) {
        return;// 无效 HTTP, 静默丢, 同 router_dispatch
    }
    // 同连接已有记录说明上一条流式请求没收尾, 丢旧的重开
    _router_st_drop(r, sk);
    router_stream *st;
    MALLOC(st, sizeof(router_stream));
    // MALLOC 不清零, 靠逐字段写满: 往 router_stream 加字段必须同时进这里(同 _router_group_fill)。
    // req 交给 _router_req_match, url 有意不清(理由见那里)
    st->on_chunk = NULL;
    st->on_chunk_arg = NULL;
    int32_t idx = _router_req_match(r, &st->req, &st->url, task, sk, pack, status);
    if (idx < 0) {
        st->req.wantclose = 0;// 请求体还在路上，回完由 _router_st_reject 统一关，别让回退回包时先关一次
        (void)_router_fallback(r, &st->req, idx);
        _router_st_reject(st, task, 0, NULL);
        return;
    }
    router_entry *matched = _route(r, idx);
    // 配置错误要与 dispatch 给同一个码; 排在下面的 411 之前, 否则 index 条目会被
    // 当成"普通路由收到 chunked"而回 411, 同一个错两个码
    if (0 != _router_entry_misconfigured(matched, idx)) {
        _router_st_reject(st, task, 500, ROUTER_BODY_500);
        return;
    }
    // 命中的不是流式路由: 请求体正一块块往这边来, 普通 handler 接不住
    if (NULL == matched->on_chunk) {
        _router_st_reject(st, task, 411, ROUTER_BODY_411);
        return;
    }
    st->on_chunk = matched->on_chunk;
    st->on_chunk_arg = matched->on_chunk_arg;
    if (ERR_OK != _router_chain_build(r, matched, &st->req)) {
        _router_st_reject(st, task, 500, ROUTER_BODY_CHAIN);
        return;
    }
    // 链尾是准入哨兵而非 handler; 中间件截断即拒绝, 它没写响应就兜底 500
    router_next(&st->req);
    if (!st->req.admitted) {
        _router_st_reject(st, task, st->req.responded ? 0 : 500, ROUTER_BODY_500);
        return;
    }
    // 准入通过才让等 100 Continue 的客户端发请求体; 被拒的上面已回最终响应并关连接
    if (http_expect_continue(pack)) {
        ev_send(&task->loader->netev, sk, (void *)HTTP_CONTINUE_RESP, sizeof(HTTP_CONTINUE_RESP) - 1, 1);
    }
    // 流式表懒建: 多数 router 一辈子见不到一个流式请求, 不必都摊这份内存
    if (NULL == r->streams) {
        r->streams = st_map_new(16, NULL);
    }
    // 进表要排在回调之前: 回调里若关连接才找得到这条记录
    router_st_ent ent;
    ent.sk = *sk;
    ent.st = st;
    st_map_set(r->streams, &ent);
    st->on_chunk(&st->req, PROT_SLICE_START, NULL, 0, st->on_chunk_arg);
    // 首包随本次回调结束即被协议层回收, 后面几帧头部一律读不到。
    // 回调里可能把这条流关掉(router_closed), 那时 st 已经释放, 先确认表里还是它才能写
    const router_st_ent *cur = st_map_get(r->streams, &ent);
    if (NULL != cur
        && cur->st == st) {
        st->req.pack = NULL;
    }
}
// 流式中间/结束帧: 原样把 slice 与数据交给 on_chunk
static void _router_st_feed(router_ctx *r, sock_ctx *sk, uint8_t slice, struct http_pack_ctx *pack) {
    if (NULL == r->streams) {
        return;
    }
    router_st_ent probe;
    probe.sk = *sk;
    probe.st = NULL;
    const router_st_ent *found = st_map_get(r->streams, &probe);
    if (NULL == found) {
        return;// 首帧被拒过, 连接那时就关了, 后续帧静默丢
    }
    router_stream *st = found->st;
    size_t dlens = 0;
    void *data = http_data(pack, &dlens);
    if (0 == (PROT_SLICE_END & slice)) {
        st->on_chunk(&st->req, slice, data, dlens, st->on_chunk_arg);
        return;
    }
    // 结束帧: 先摘表项再回调, 回调返回后连同 req 一起释放
    st_map_delete(r->streams, &probe);
    st->on_chunk(&st->req, slice, data, dlens, st->on_chunk_arg);
    if (!st->req.responded) {
        router_req_text(&st->req, 500, ROUTER_BODY_500, sizeof(ROUTER_BODY_500) - 1);
    }
    FREE(st);
}
// 没注册过流式路由时的 chunked 首帧: 结局只能是"匹配不上"/"命中普通路由"/"命中 index 条目",
// 三种都能用栈上 req 算出来, 不必先堆分配 router_stream。给的码与一次到齐的同一请求完全一致
static void _router_chunked_nostream(router_ctx *r, task_ctx *task, sock_ctx *sk,
                                     struct http_pack_ctx *pack) {
    buf_ctx *status = _router_http_status(pack);
    if (NULL == status) {
        return;// 无效 HTTP, 静默丢, 同 router_dispatch
    }
    url_ctx url;
    router_req ctx;
    int32_t idx = _router_req_match(r, &ctx, &url, task, sk, pack, status);
    if (idx >= 0
        && 0 == _router_entry_misconfigured(_route(r, idx), idx)) {
        router_reject_chunked(task, sk, ROUTER_M_HEAD == ctx.method);// 命中普通路由, 请求体接不住
        return;
    }
    if (idx >= 0) {
        _router_send_simple(task, sk, 500,
                            ROUTER_M_HEAD == ctx.method, ROUTER_BODY_500);
    } else {
        ctx.wantclose = 0;// 回完下面统一关，理由同 _router_st_begin
        (void)_router_fallback(r, &ctx, idx);
    }
    ev_close(&task->loader->netev, sk);
}
// _net_recv 回调的标准实现: 一次到齐的请求直接派发; chunked 命中流式路由则逐帧交给它,
// 命中普通路由回 411, 匹配不上按普通请求的码走(404/400/405)。
// harbor / debug_console 各自的回调只负责从 task 参数里取出自己的 router 再转到这里
void router_net_recv(router_ctx *r, task_ctx *task, sock_ctx *sk,
                     subtype_t pktype, uint8_t client, uint8_t slice, void *data, size_t size) {
    (void)pktype;
    (void)client;
    (void)size;
    if (0 == slice) {
        router_dispatch(r, task, sk, (struct http_pack_ctx *)data);
        return;
    }
    if (NULL == r) {
        return;
    }
    if (PROT_SLICE_START & slice) {
        // 一条流式路由都没注册: 结局不可能是"开一条流", 走 nostream 那条省掉近 6KB 堆分配
        if (0 == r->has_stream) {
            _router_chunked_nostream(r, task, sk, (struct http_pack_ctx *)data);
            return;
        }
        _router_st_begin(r, task, sk, (struct http_pack_ctx *)data);
        return;
    }
    _router_st_feed(r, sk, slice, (struct http_pack_ctx *)data);
}
