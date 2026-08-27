#include "advance/router.h"
#include "utils/utils.h"
#include "utils/binary.h"
#include "containers/hashmap.h"
#include "srey/loader.h"
#include "srey/task.h"

// 非 200 的错误正文。两个派发入口的正文必须一模一样, 只是发送方式不同
// (dispatch 只回响应, 流式那边还要关连接 + 丢记录), 故正文在这里定死。
// 匹配失败的码由 _router_code_body 按状态码生成; 下面两条是它生成不出来的
#define ROUTER_CODE_BODY_LENS 64
#define ROUTER_BODY_500 "Internal Server Error\n"
#define ROUTER_BODY_CHAIN "Chain too long\n"
// 按 method 生成 router_get / router_post / ... 等便捷包装, 内部一律转发到 router_add;
// 展开点在 router_add 之后(宏体里的类型只在展开处才需要可见)
#define DEF_ROUTE_FN(name, mask)  \
router_entry *router_##name(router_ctx *r, const router_group *g,  \
                            const char *path, router_cb h, \
                            const char *const *mws, int32_t mws_n) { \
    return router_add(r, g, mask, path, h, mws, mws_n); \
}
// 流式便捷包装只给 POST / PUT: 带请求体的方法就这两个, 其余方法要流式直接用 router_add_stream
#define DEF_STREAM_FN(name, mask)  \
router_entry *router_##name##_stream(router_ctx *r, const router_group *g,  \
                                     const char *path, router_stream_cb sh, \
                                     const char *const *mws, int32_t mws_n) { \
    return router_add_stream(r, g, mask, path, sh, mws, mws_n); \
}

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
    router_method method_mask;  // 方法位掩码, enum 占 4B
    int32_t segs_nopt;          // segs 中 OPT 段数, 注册期算好; 恰好占掉 method_mask 后的 4B padding
    router_seg *segs;
    router_cb *mws;             // 已合并的中间件函数指针 (group + 路由级, 已 _router_resolve_mw)
    router_cb handler;          // 普通路由; 与 on_chunk 互斥
    router_stream_cb on_chunk;  // 流式路由; 两者皆空即 router_add_index 注册的纯匹配条目
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
    int32_t has_stream;      // 注册过流式路由; 没有就走 _router_chunked_nostream, 不必堆分配 router_stream
    router_entry *routes;
    router_cb *global_mw;
    named_mw *named;
    struct hashmap *streams; // router_st_ent 表; 首次遇到流式请求才建
};
// 一条正在接收的流式请求。req / url 要跨帧活到收齐, 故整体堆分配, 表里只放下面那个小元素
// —— 表扩容搬的是指针, 交给 on_chunk 的 ctx 地址始终不变。
// 首包不留副本: req.pack 只在它活着的那段时间(准入链 + 首帧回调)有效, 之后置 NULL
typedef struct router_stream {
    router_stream_cb on_chunk;   // 命中路由的流式回调
    router_req req;              // 跨帧复用的请求上下文, 连接标识也在里面
    url_ctx url;                 // req.url 恒指向这里
} router_stream;
// 流式表的元素: 键 + 本体指针。本体近 5KB, 按值入表的话查一次就得在栈上摆一个同样大的探针
typedef struct router_st_ent {
    sk_id sk;              // hashmap 的键
    router_stream *st;
} router_st_ent;

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
// 解析单段, 写入 out。src 不要求以 \0 结尾, 仅按 len 读取。任何输入都能解析出一段:
// 认不出占位符形态(名字为空 / 名字里有 '?')就当字面量, 故没有失败返回
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
        // 名字为空({?}; {} 长度不够, 压根进不来)或名字内部含 '?'({a?b})都不认作占位符,
        // 落字面量而不是让整条路由注册失败
        if (0 != name_len
            && NULL == memchr(name_src, '?', name_len)) {
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
// 按 '/' 拆分 path, 调用 _router_parse_seg 逐段解析, 写入新分配的 *out_segs
// 全部成功才落堆, 任一段失败时回滚已 MALLOC 的 str
static int32_t _router_parse_path(const char *path, size_t path_len, router_seg **out_segs,
                                  int32_t *out_n, int32_t *out_nopt) {
    int32_t n = 0;
    size_t i = 0;
    size_t start;
    // 栈缓存: 先填这里, 全部成功后一次性 MALLOC + memcpy, 失败路径无需 realloc 回滚
    router_seg buf[URL_MAX_PATH_DEPTH];
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
        if (n >= URL_MAX_PATH_DEPTH) {
            LOG_WARN("router: path segments exceed %d, rejected.", URL_MAX_PATH_DEPTH);
            _router_segs_free_str(buf, n);
            return ERR_FAILED;
        }
        // WILD 段必须是最末段; 此时若上一段已是 WILD 却还有当前段, 说明 WILD 后还有内容, 拒绝
        // (两条匹配路径命中 WILD 都立即返成功, 中间 WILD 会让后续段静默失效, 易掉坑)
        if (n > 0 && ROUTER_SEG_WILD == buf[n - 1].t) {
            LOG_WARN("router: wildcard '*' must be the last segment.");
            _router_segs_free_str(buf, n);
            return ERR_FAILED;
        }
        _router_parse_seg(path + start, i - start, &buf[n]);
        n++;
    }
    // 可选段数决定 _router_match_path 可行性表的第二维, 超出即拒绝注册
    int32_t nopt = 0, nparam = 0;
    for (int32_t k = 0; k < n; k++) {
        if (ROUTER_SEG_OPT == buf[k].t) {
            nopt++;
        } else if (ROUTER_SEG_PARAM == buf[k].t) {
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
    // 只按必填段判上限: OPT 段可以不取值, 把它算进来会误杀那些跳过可选段后恰好不超限的路由
    if (nparam > ROUTER_MAX_PARAMS) {
        LOG_ERROR("router: required path params %d exceed %d, route rejected.", nparam, ROUTER_MAX_PARAMS);
        _router_segs_free_str(buf, n);
        return ERR_FAILED;
    }
    *out_nopt = nopt;
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
// 把请求段 qsegs[*qi] 作为 seg 命名的参数填入 ctx->params, 并推进 *pn / *qi。
// 成功返 1; 超出 ROUTER_MAX_PARAMS 返 0, 此时不改动任何计数。
// {name} 与 {name?} 共用本函数, 保证两者填参形状与上限判定始终一致
static int32_t _router_param_take(router_req *ctx, const router_seg *seg,
                                  const buf_ctx *qsegs, int32_t *pn, int32_t *qi) {
    if (*pn >= ROUTER_MAX_PARAMS) {
        LOG_WARN("router: params exceed %d, rejected.", ROUTER_MAX_PARAMS);
        return 0;
    }
    ctx->params[*pn].key = seg->str;
    ctx->params[*pn].key_len = seg->str_len;
    ctx->params[*pn].val = qsegs[*qi].data;
    ctx->params[*pn].val_len = (uint32_t)qsegs[*qi].lens;
    (*pn)++;
    (*qi)++;
    return 1;
}
// 无 OPT 段时每段恒吃一个请求段, 对齐唯一, 一趟线性扫描即精确匹配 —— 首个字面量不符就返回,
// 不建表也不预扫。路由表里绝大多数是这个形状, 故与 DP 分开走。
// 前提是调用方已保证 nopt == 0: 末尾那个 else 分支把 PARAM 与 OPT 一并当必填段吃
static int32_t _router_match_linear(const router_seg *rsegs, int32_t rn,
                                    const buf_ctx *qsegs, int32_t qn,
                                    router_req *ctx) {
    const router_seg *seg;
    int32_t pn = 0;
    int32_t qi = 0;
    int32_t ri;
    for (ri = 0; ri < rn; ri++) {
        seg = &rsegs[ri];
        if (ROUTER_SEG_WILD == seg->t) {
            ctx->params_n = pn;
            return 1;
        }
        if (qi >= qn) {
            return 0;
        }
        if (ROUTER_SEG_LIT == seg->t) {
            if (seg->str_len != (uint32_t)qsegs[qi].lens
                || 0 != memcmp(seg->str, qsegs[qi].data, qsegs[qi].lens)) {
                return 0;
            }
            qi++;
        } else if (!_router_param_take(ctx, seg, qsegs, &pn, &qi)) {
            return 0;
        }
    }
    if (qi != qn) {
        return 0;
    }
    ctx->params_n = pn;
    return 1;
}
// 把 url_parse 拆好的请求段 qsegs 与 rsegs 对照, 成功填 ctx->params 并返回 1。
// params[i].key 指向 rsegs[].str(router_ctx 持有), val 指向 qsegs 内部。
// nopt 恒 > 0 —— nopt == 0 由 _router_find 分给 _router_match_linear, 不进这里。
// "哪些 OPT 取值"是组合选择, 贪婪前瞻会漏解, 故先反向推可行性表再正向重建。
// 状态 (ri, s): s 为 rsegs[0,ri) 内已跳过的 OPT 数, 于是 qi 恒等于 ri - s 不必单独进状态;
// ok[ri][s] 表示 rsegs[ri,rn) 能否匹配 qsegs[qi,qn)。重建时 OPT 能取值就取, 取不到才跳过
static int32_t _router_match_path(const router_seg *rsegs, int32_t rn, int32_t nopt,
                                  const buf_ctx *qsegs, int32_t qn,
                                  router_req *ctx) {
    const router_seg *seg;
    int32_t qi;
    int32_t s;
    int32_t ri;
    int32_t any;
    // s 的可达上界是 rsegs[0,ri) 内的 OPT 数, 反向走时随 ri 递减; 用它替代固定的 nopt
    // 能砍掉大量结构上不可达的状态(尾部带可选段的常见形状里, 死状态占绝大多数)
    int32_t nopt_pref = nopt;
    ASSERTAB(nopt > 0 && nopt <= ROUTER_MAX_OPT, "nopt out of table range.");
    // O(1) 必要条件: 无 WILD 时消耗的请求段数恒落在 [rn-nopt, rn]（即终态行 rn-s==qn 的取值域），
    // 不满足直接否掉, 免去建表。WILD 会提前返回成功, 段数不受此约束故跳过
    if ((0 == rn || ROUTER_SEG_WILD != rsegs[rn - 1].t)
        && (qn > rn || qn < rn - nopt)) {
        return 0;
    }
    uint8_t ok[URL_MAX_PATH_DEPTH + 1][ROUTER_MAX_OPT + 1];
    // 终态: 路由段走完且请求段恰好吃干净
    for (s = 0; s <= nopt; s++) {
        ok[rn][s] = (rn - s == qn) ? 1 : 0;
    }
    for (ri = rn - 1; ri >= 0; ri--) {
        seg = &rsegs[ri];
        if (ROUTER_SEG_OPT == seg->t) {
            nopt_pref--;
        }
        any = 0;
        for (s = 0; s <= nopt_pref; s++) {
            qi = ri - s;
            if (qi < 0 || qi > qn) {
                ok[ri][s] = 0;
            } else if (ROUTER_SEG_WILD == seg->t) {
                // '*' 一旦出现, 后续请求段任意(含零个), 恒可行
                ok[ri][s] = 1;
            } else if (ROUTER_SEG_OPT == seg->t) {
                // 跳过转移落在 (ri+1, s+1): 本段是 OPT 故 nopt_pref 刚减过 1,
                // s+1 必在上一行已填范围内, 无需再判上界
                ok[ri][s] = (ok[ri + 1][s + 1]
                    || (qi < qn && ok[ri + 1][s])) ? 1 : 0;
            } else if (qi >= qn) {
                ok[ri][s] = 0;
            } else if (ROUTER_SEG_LIT == seg->t) {
                // 后缀先判: 已不可行就不必再 memcmp(等价于 cond ? x : 0 写成 x && cond)
                ok[ri][s] = (ok[ri + 1][s]
                    && seg->str_len == (uint32_t)qsegs[qi].lens
                    && 0 == memcmp(seg->str, qsegs[qi].data, qsegs[qi].lens)) ? 1 : 0;
            } else {
                ok[ri][s] = ok[ri + 1][s];
            }
            any |= ok[ri][s];
        }
        // 整行不可行即可收工: WILD 是唯一能脱离下一行独立成立的规则, 而它被注册期
        // 强制为末段(ri == rn-1), 故更靠前的行只会全零传导下去, ok[0][0] 必为 0
        if (!any) {
            return 0;
        }
    }
    if (!ok[0][0]) {
        return 0;
    }
    // 沿可行转移正向重建并填参; 不变式 qi == ri - s 且 ok[ri][s] 恒为 1,
    // 故除参数条数超限外不会再失败, 终态 ok[rn][s] 已蕴含 qi == qn
    int32_t pn = 0;
    s = 0;
    qi = 0;
    for (ri = 0; ri < rn; ri++) {
        seg = &rsegs[ri];
        if (ROUTER_SEG_WILD == seg->t) {
            break;
        }
        if (ROUTER_SEG_OPT == seg->t) {
            if (qi < qn
                && ok[ri + 1][s]) {
                if (!_router_param_take(ctx, seg, qsegs, &pn, &qi)) {
                    return 0;
                }
            } else {
                s++;
            }
        } else if (ROUTER_SEG_PARAM == seg->t) {
            if (!_router_param_take(ctx, seg, qsegs, &pn, &qi)) {
                return 0;
            }
        } else {
            qi++;
        }
    }
    ctx->params_n = pn;
    return 1;
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
    return r;
}
// 流式表的 hash / compare / elfree。key 是 sk_id，按字段逐个喂而不是整体 memhash：
// sk_id 里 fd 与 skid 之间有对齐填充，填充字节是未初始化的
static uint64_t _router_st_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    (void)seed0;
    (void)seed1;
    const router_st_ent *ent = (const router_st_ent *)item;
    uint64_t key[2];
    key[0] = (uint64_t)ent->sk.fd;
    key[1] = ent->sk.skid;
    return hash((const char *)key, sizeof(key));
}
static int _router_st_cmp(const void *a, const void *b, void *ud) {
    (void)ud;
    const router_st_ent *x = (const router_st_ent *)a;
    const router_st_ent *y = (const router_st_ent *)b;
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
    st->on_chunk(&st->req, ROUTER_STREAM_ABORT, NULL, 0);
    FREE(st);
}
// 摘掉一条流式记录并释放它。hashmap_delete 只返回元素副本、不会自动调 elfree, 得在这里补上
static void _router_st_drop(router_ctx *r, sk_id *sk) {
    if (NULL == r->streams) {
        return;
    }
    router_st_ent probe;
    probe.sk = *sk;
    probe.st = NULL;
    router_st_ent *removed = (router_st_ent *)hashmap_delete(r->streams, &probe);
    if (NULL != removed) {
        _router_st_free(removed);
    }
}
// 排空流式表。每轮都从 i = 0 重新起步, 不复用被 hashmap_delete 作废的游标
static void _router_st_drain(router_ctx *r) {
    size_t i;
    void *item;
    sk_id sk;
    while (0 != hashmap_count(r->streams)) {
        i = 0;
        if (!hashmap_iter(r->streams, &i, &item)) {
            break;
        }
        sk = ((router_st_ent *)item)->sk;
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
        hashmap_free(r->streams);
    }
    router_entry *e;
    // 逐 entry 释放其内嵌的字符串和数组
    for (int32_t i = 0; i < r->routes_n; i++) {
        e = &r->routes[i];
        _router_segs_free_str(e->segs, e->segs_n);
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
    char *dup = dup_zero(name, len);
    r->named[r->named_n].name = dup;
    r->named[r->named_n].fn = fn;
    r->named_n++;
}
// 执行链长度 = 全局中间件 + 路由级中间件 + 末位一格(普通路由的 handler / 流式路由的准入哨兵)。
// 注册时、后加全局中间件时、派发前各判一次, 三处共用这一个谓词
static inline int32_t _router_chain_over(const router_ctx *r, int32_t mws_n) {
    return r->global_mw_n + mws_n + 1 > ROUTER_MAX_CHAIN;
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
    // 已注册的路由是按当时的 global_mw_n 判过链长的; 全局中间件后加就得回头再判一遍,
    // 否则超限只在跑起来后表现为每请求 500 "Chain too long"
    for (int32_t i = 0; i < r->routes_n; i++) {
        if (0 != _router_chain_over(r, r->routes[i].mws_n)) {
            LOG_WARN("router: route %d chain now exceeds %d (global=%d, route=%d) after router_use.",
                     i, ROUTER_MAX_CHAIN, r->global_mw_n, r->routes[i].mws_n);
        }
    }
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
void router_group_root(router_ctx *r, router_group *g, const char *prefix,
                       const char *const *mw_names, int32_t n) {
    g->parent = NULL;
    g->router = r;
    _router_group_fill(g, prefix, mw_names, n);
}
void router_group_nest(const router_group *parent, router_group *g, const char *prefix,
                       const char *const *mw_names, int32_t n) {
    g->parent = parent;
    g->router = parent->router;
    _router_group_fill(g, prefix, mw_names, n);
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
// 新条目会不会被已注册的某条永远遮住：方法掩码有交集 + 段序列在"匹配意义上"完全相同
// (段数、逐段类型、LIT 文本都相同；参数名不参与匹配，故 /u/{id} 与 /u/{uid} 是同一条)。
// 被遮住的那条静默不可达、极难查，故在注册期就拒掉。
// 只判"完全相同"，不判更一般的"谁比谁宽泛"——那在 OPT 与 WILD 组合下是偏序问题，会误杀
static int32_t _router_shadowed(router_ctx *r, router_method m,
                                const router_seg *segs, int32_t segs_n) {
    router_entry *e;
    int32_t k;
    for (int32_t i = 0; i < r->routes_n; i++) {
        e = &r->routes[i];
        if (0 == (e->method_mask & m)
            || e->segs_n != segs_n) {
            continue;
        }
        for (k = 0; k < segs_n; k++) {
            if (e->segs[k].t != segs[k].t) {
                break;
            }
            if (ROUTER_SEG_LIT == segs[k].t
                && (e->segs[k].str_len != segs[k].str_len
                    || 0 != memcmp(e->segs[k].str, segs[k].str, segs[k].str_len))) {
                break;
            }
        }
        if (k == segs_n) {
            return i;
        }
    }
    return -1;
}
// 解析路径并做影子检查。成功返 ERR_OK，段数组所有权转给调用方；
// 路径非法返 -1，被已注册路由遮蔽返 -2（此时段数组已就地释放并置空）
static int32_t _router_segs_prepare(router_ctx *r, router_method m, const char *path, size_t path_len,
                                    router_seg **out_segs, int32_t *out_n, int32_t *out_nopt) {
    *out_segs = NULL;
    *out_n = 0;
    *out_nopt = 0;
    if (ERR_OK != _router_parse_path(path, path_len, out_segs, out_n, out_nopt)) {
        return -1;
    }
    int32_t shadow = _router_shadowed(r, m, *out_segs, *out_n);
    if (shadow >= 0) {
        LOG_WARN("router: route shadowed by the one registered at index %d, this registration is ignored.", shadow);
        _router_segs_free_str(*out_segs, *out_n);
        FREE(*out_segs);
        return -2;
    }
    return ERR_OK;
}
// 段数组入路由表尾。填 method_mask 与三个 segs 字段，其余清零留给调用方补
static int32_t _router_entry_push(router_ctx *r, router_method m,
                                  router_seg *segs, int32_t segs_n, int32_t segs_nopt) {
    _router_grow((void **)&r->routes, &r->routes_cap, r->routes_n + 1, sizeof(router_entry));
    router_entry *e = &r->routes[r->routes_n];
    ZERO(e, sizeof(*e));
    e->method_mask = m;
    e->segs = segs;
    e->segs_n = segs_n;
    e->segs_nopt = segs_nopt;
    return r->routes_n++;//后自增：返回值即新条目的稳定下标
}
// router_add / router_add_stream 的共同实现: h 与 sh 恰有一个非空,
// 决定这条路由是普通派发还是流式接收
static router_entry *_router_add_common(router_ctx *r, const router_group *g,
                                        router_method method, const char *path,
                                        router_cb h, router_stream_cb sh,
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
    int32_t segs_nopt = 0;
    if (ERR_OK != _router_segs_prepare(r, method, full_buf, full_len, &segs, &segs_n, &segs_nopt)) {
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
        router_cb fn;
        if (g_mws_n > 0) {
            char **gnames;
            MALLOC(gnames, sizeof(const char *) * (size_t)g_mws_n);
            _router_group_collect_mws(g, gnames);
            for (int32_t i = 0; i < g_mws_n; i++) {
                fn = _router_resolve_mw(r, (const char*)gnames[i]);
                if (NULL != fn) {
                    mws_arr[k++] = fn;
                }
            }
            FREE(gnames);
        }
        // 3b) 再追加路由级 mws
        for (int32_t i = 0; i < mws_n; i++) {
            fn = _router_resolve_mw(r, mws[i]);
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
    if (0 != _router_chain_over(r, total_mws)) {
        LOG_WARN("router: chain will exceed %d (global=%d, route=%d) at dispatch.",
                 ROUTER_MAX_CHAIN, r->global_mw_n, total_mws);
    }
    // 下标与取址必须分成两条语句：&r->routes[f()] 里 r->routes 与 f() 的求值顺序未定义，
    // 而 _router_entry_push 内部的 _router_grow 可能 REALLOC 掉 r->routes
    int32_t idx = _router_entry_push(r, method, segs, segs_n, segs_nopt);
    router_entry *e = &r->routes[idx];
    e->mws = mws_arr;
    e->mws_n = total_mws;
    e->handler = h;
    e->on_chunk = sh;
    return e;
}
router_entry *router_add(router_ctx *r, const router_group *g,
                         router_method method, const char *path,
                         router_cb h,
                         const char *const *mws, int32_t mws_n) {
    if (NULL == h) {
        LOG_WARN("router: router_add needs a handler.");
        return NULL;
    }
    return _router_add_common(r, g, method, path, h, NULL, mws, mws_n);
}
router_entry *router_add_stream(router_ctx *r, const router_group *g,
                                router_method method, const char *path,
                                router_stream_cb sh,
                                const char *const *mws, int32_t mws_n) {
    if (NULL == sh) {
        LOG_WARN("router: router_add_stream needs a stream callback.");
        return NULL;
    }
    router_entry *e = _router_add_common(r, g, method, path, NULL, sh, mws, mws_n);
    if (NULL != e) {
        r->has_stream = 1;
    }
    return e;
}
// 便捷包装的展开点; 两个生成宏定义在文件头
DEF_ROUTE_FN(get,     ROUTER_M_GET)
DEF_ROUTE_FN(post,    ROUTER_M_POST)
DEF_ROUTE_FN(put,     ROUTER_M_PUT)
DEF_ROUTE_FN(delete,  ROUTER_M_DELETE)
DEF_ROUTE_FN(patch,   ROUTER_M_PATCH)
DEF_ROUTE_FN(head,    ROUTER_M_HEAD)
DEF_ROUTE_FN(options, ROUTER_M_OPTIONS)
DEF_ROUTE_FN(any,     ROUTER_M_ANY)
DEF_STREAM_FN(post, ROUTER_M_POST)
DEF_STREAM_FN(put,  ROUTER_M_PUT)
int32_t router_add_index(router_ctx *r, const char *method, size_t method_len,
                       const char *path, size_t path_len) {
    router_method m = _router_method_str_to_mask(method, method_len);
    if (0 == m) {
        //_router_method_str_to_mask 不含 ANY，此处特判后映射 ROUTER_M_ANY（全方法通配掩码）
        if (3 != method_len || 0 != memcmp(method, "ANY", 3)) {
            return -1;
        }
        m = ROUTER_M_ANY;
    }
    router_seg *segs = NULL;
    int32_t segs_n = 0;
    int32_t segs_nopt = 0;
    int32_t rtn = _router_segs_prepare(r, m, path, path_len, &segs, &segs_n, &segs_nopt);
    if (ERR_OK != rtn) {
        return rtn;//-1 路径非法 / -2 被已注册路由遮蔽
    }
    return _router_entry_push(r, m, segs, segs_n, segs_nopt);
}
// 在已 url_parse 的 ctx->url 上匹配：就地剔除空段（RFC 允许 /a//b）后线性
// 扫描路由表，方法掩码命中 + 路径匹配，返回首条命中索引，无命中返回 -1
static int32_t _router_find(router_ctx *r, router_method m, router_req *ctx) {
    int32_t qn = 0;
    size_t plens = 0;
    router_entry *e;
    for (int32_t i = 0; i < ctx->url->npath; i++) {
        if (ctx->url->segs[i].lens > 0) {
            plens += (ctx->url->segs[i].lens + 1);
            ctx->url->segs[qn++] = ctx->url->segs[i];
        }
    }
    ctx->url->npath = qn;
    // pathlens 得跟着段数一起收,url_ctx 声明的是 pathlens == Σ(segs[i].lens + 1),
    // 下游按它预分配重组缓冲
    ctx->url->pathlens = plens;
    for (int32_t i = 0; i < r->routes_n; i++) {
        e = &r->routes[i];
        if (0 == (e->method_mask & m)) {
            continue;
        }
        // 无可选段的路由(绝大多数)走线性匹配, 不进 _router_match_path,
        // 它那张 (URL_MAX_PATH_DEPTH+1)*(ROUTER_MAX_OPT+1) 的可行性表就不会压上协程栈
        if (0 == e->segs_nopt) {
            if (_router_match_linear(e->segs, e->segs_n, ctx->url->segs, qn, ctx)) {
                return i;
            }
            continue;
        }
        if (_router_match_path(e->segs, e->segs_n, e->segs_nopt, ctx->url->segs, qn, ctx)) {
            return i;
        }
    }
    return -1;
}
int32_t router_match_index(router_ctx *r, const char *method, size_t method_len,
                           const char *url, size_t url_len, router_req *ctx) {
    router_method m = _router_method_str_to_mask(method, method_len);
    if (0 == m) {
        return -3;
    }
    ctx->method = m;
    //url_parse 解码后段写入 ctx->url；params.val 指向该缓冲，ctx 生命周期须覆盖 params 使用
    if (ERR_OK != url_parse(ctx->url, url, url_len, '/', 1)) {
        return -2;
    }
    return _router_find(r, m, ctx);
}
int32_t router_match_code(int32_t idx) {
    if (idx >= 0) {
        return 200;
    }
    if (-3 == idx) {
        return 405;
    }
    if (-2 == idx) {
        return 400;
    }
    return 404;
}
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
    // 流式路由过了首帧就没有 pack 了, 见 router_stream_cb 的说明
    if (NULL == ctx->pack) {
        return NULL;
    }
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
// 组装完整 HTTP 响应并通过 ev_send 推出去; 自动写 Content-Length, content_type 非 NULL 时
// 自动写 Content-Type, extra 由调用方追加 (不可重复 CL / CT / Transfer-Encoding)
// bw 内部托管 (binary_init(NULL,...) 模式), ev_send copy=0 转移 bw.data 所有权给框架,
// 函数返回后无需 binary_free。不接 router_req, 供无 ctx 的错误路径 (拒 chunked 的 411、
// 流式首帧的 400 / 500 等) 共用同一条响应管线; 有 ctx 的入口走 _router_send_resp 包一层置 responded
static void _router_send_core(task_ctx *task, SOCKET fd, uint64_t skid, int32_t code,
                              const char *content_type,
                              const http_header_ctx *extra, int32_t extra_n,
                              const char *body, size_t body_len) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    http_pack_resp(&bw, code);
    if (NULL != content_type) {
        http_pack_head(&bw, "Content-Type", content_type);
    }
    // 头值按长度直传 http_pack_head2 无长度限制; 头名没有按长度取值的重载, 仍需 \0 结尾副本。
    // 非法头一律整条丢弃而不截断、更不 abort: 截断头名等于把它改成另一个名字发上线缆, 比不发更糟;
    // 而 http_pack_head2 对 CR/LF 是断言退进程, 让业务数据能打死服务端不可接受, 故在此先筛掉
    char k[128];
    // 尾部 http_pack_content 还要写 "Content-Length: %zu" 加两个 CRLF, 判定时先扣掉这段。
    // 20 是 %zu 的最长十进制位数(UINT64_MAX 有 20 位), 32 位平台上只会多留不会少留
    const size_t tail = sizeof("Content-Length: ") - 1 + 20 + CRLF_SIZE * 2;
    for (int32_t i = 0; i < extra_n; i++) {
        if (NULL == extra[i].key.data
            || 0 == extra[i].key.lens
            || extra[i].key.lens >= sizeof(k)) {
            LOG_WARN("router: header key length %zu invalid, dropped.", extra[i].key.lens);
            continue;
        }
        if (!is_token((const char *)extra[i].key.data, extra[i].key.lens)) {
            LOG_WARN("router: header key is not a valid token, dropped.");
            continue;
        }
        // 帧长头归 http_pack_content 独占: 再叠一条 Content-Length 或补一条 Transfer-Encoding,
        // 对端(含 srey 自己的解析器)判为请求走私、整包丢弃并断连。
        // 去 const 是因为 buf_icompare 收非 const 指针, 它只读不写
        if (buf_icompare((buf_ctx *)&extra[i].key, "Content-Length", sizeof("Content-Length") - 1)
            || buf_icompare((buf_ctx *)&extra[i].key, "Transfer-Encoding", sizeof("Transfer-Encoding") - 1)) {
            LOG_WARN("router: framing header must not come from extra, dropped.");
            continue;
        }
        // MAX_HEADLENS 管的是整个头部块, 故按已写入的 bw.offset 累计判而非逐条判。
        // 超长值先单独挡一道: 直接相加会在 lens 接近 SIZE_MAX 时回绕成小值放行
        if (extra[i].value.lens > MAX_HEADLENS
            || bw.offset + tail + extra[i].key.lens + extra[i].value.lens
               + sizeof(": \r\n") - 1 > MAX_HEADLENS) {
            LOG_WARN("router: header would push head block past MAX_HEADLENS, dropped.");
            continue;
        }
        // 值为 NULL 一律丢: 调用方传 NULL 是"这条别发", 空值头要发就传 {"", 0}
        if (NULL == extra[i].value.data
            || (extra[i].value.lens > 0
                && (NULL != memchr(extra[i].value.data, '\0', extra[i].value.lens)
                    || NULL != memchr(extra[i].value.data, '\r', extra[i].value.lens)
                    || NULL != memchr(extra[i].value.data, '\n', extra[i].value.lens)))) {
            LOG_WARN("router: header value is NULL or contains NUL/CRLF, dropped.");
            continue;
        }
        memcpy(k, extra[i].key.data, extra[i].key.lens);
        k[extra[i].key.lens] = '\0';
        http_pack_head2(&bw, k, (const char *)extra[i].value.data, extra[i].value.lens);
    }
    // 1xx/204/304 禁带 Content-Length 与报文体, 只收尾不写 body(给了也丢);
    // 其余走 http_pack_content, 它写 \r\n\r\n + body 完成整包, 空 body 也统一收敛成
    // Content-Length: 0, 各入口不必自己归一
    if (http_code_nobody(code)) {
        http_pack_end(&bw);
    } else {
        http_pack_content(&bw, (void *)body, body_len);
    }
    ev_send(&task->loader->netev, fd, skid, bw.data, bw.offset, 0);
}
// _router_send_core 的 router_req 版: 发完置 responded 避免 dispatch 末尾兜底 500 又发一遍
static void _router_send_resp(router_req *ctx, int32_t code, const char *content_type,
                              const http_header_ctx *extra, int32_t extra_n,
                              const char *body, size_t body_len) {
    _router_send_core(ctx->task, ctx->sk.fd, ctx->sk.skid, code, content_type,
                      extra, extra_n, body, body_len);
    ctx->responded = 1;
}
void router_req_text(router_req *ctx, int32_t code, const char *body, size_t lens) {
    _router_send_resp(ctx, code, "text/plain; charset=utf-8", NULL, 0, body, lens);
}
void router_req_json(router_req *ctx, int32_t code, const char *json, size_t lens) {
    _router_send_resp(ctx, code, "application/json", NULL, 0, json, lens);
}
void router_req_html(router_req *ctx, int32_t code, const char *body, size_t lens) {
    _router_send_resp(ctx, code, "text/html; charset=utf-8", NULL, 0, body, lens);
}
void router_req_respond(router_req *ctx, int32_t code,
                      const http_header_ctx *extra, int32_t extra_n,
                      const char *body, size_t body_len) {
    _router_send_resp(ctx, code, NULL, extra, extra_n, body, body_len);
}
// 兜底响应 (404 / 405 / 500); body 走 strlen 的纯文本简写, 适合 dispatch 未匹配 /
// 未识别方法 / 中间件链溢出等错误路径。部分调用方 (router_reject_chunked) 无 router_req
// 可用, 故不接 ctx —— 有 ctx 的调用方需自行在调用后置 ctx->responded = 1 防止兜底 500 重发
static void _router_send_simple(task_ctx *task, SOCKET fd, uint64_t skid, int32_t code, const char *body) {
    _router_send_core(task, fd, skid, code, "text/plain; charset=utf-8", NULL, 0,
                      body, (NULL == body) ? 0 : strlen(body));
}
// 拒绝 chunked 请求：回 411 后立即关闭连接
void router_reject_chunked(task_ctx *task, SOCKET fd, uint64_t skid) {
    _router_send_simple(task, fd, skid, 411, "chunked request not supported\n");
    ev_close(&task->loader->netev, fd, skid);
}
// 流式路由的链尾哨兵: 跑到这里说明每个中间件都调了 router_next。不能拿 chain_i == chain_n 判,
// 最后一个中间件调不调 next 留下的游标完全一样
static void _router_admit(router_req *ctx) {
    ctx->admitted = 1;
}
// 拼接执行链: 全局中间件 → 路由级中间件 → 末位 (普通路由是 handler, 流式路由是准入哨兵)。
// 超出 ROUTER_MAX_CHAIN 视为配置错误, 由调用方回 500
static int32_t _router_chain_build(router_ctx *r, router_entry *e, router_req *ctx) {
    if (0 != _router_chain_over(r, e->mws_n)) {
        LOG_WARN("router: chain exceeds %d (global=%d, route=%d), rejected.",
                 ROUTER_MAX_CHAIN, r->global_mw_n, e->mws_n);
        return ERR_FAILED;
    }
    int32_t k = 0;
    for (int32_t i = 0; i < r->global_mw_n; i++) {
        ctx->chain[k++] = r->global_mw[i];
    }
    for (int32_t i = 0; i < e->mws_n; i++) {
        ctx->chain[k++] = e->mws[i];
    }
    ctx->chain[k++] = (NULL != e->handler) ? e->handler : _router_admit;
    ctx->chain_n = k;
    ctx->chain_i = 0;
    return ERR_OK;
}
// 按 code 生成错误正文; 为什么两个入口必须共用见 ROUTER_CODE_BODY_LENS 处的说明
static void _router_code_body(int32_t code, char body[ROUTER_CODE_BODY_LENS]) {
    SNPRINTF(body, ROUTER_CODE_BODY_LENS, "%s\n", http_code_status(code));
}
// 按 code 生成正文并回给客户端。chunked 首帧那面不走这里(它要的是 _router_st_reject
// 的关连接收尾), 自己另有一份同样的栈缓冲
static void _router_send_code(task_ctx *task, SOCKET fd, uint64_t skid, int32_t code) {
    char body[ROUTER_CODE_BODY_LENS];
    _router_code_body(code, body);
    _router_send_simple(task, fd, skid, code, body);
}
// status[0] = 方法, status[1] = 请求 URI; pack 为空或任一段为空都算无效 HTTP。
// 三个派发入口共用: 返 NULL 即静默丢, 连响应都不发——对面发的不是 HTTP, 回什么都没意义
static buf_ctx *_router_http_status(struct http_pack_ctx *pack) {
    if (NULL == pack) {
        return NULL;
    }
    buf_ctx *status = http_status(pack);
    if (NULL == status
        || 0 == status[0].lens
        || 0 == status[1].lens) {
        return NULL;
    }
    return status;
}
// 栈上请求上下文的装配。url 由调用方持有: 它得和 ctx 活得一样久, 且有意不清零
// (url_parse 自己清该清的, 那三个大数组白清就是每请求 4KB 死写)
static void _router_req_init(router_req *ctx, url_ctx *url, task_ctx *task,
                             SOCKET fd, uint64_t skid, struct http_pack_ctx *pack) {
    ZERO(ctx, sizeof(router_req));
    ctx->url = url;
    ctx->task = task;
    ctx->sk.fd = fd;
    ctx->sk.skid = skid;
    ctx->pack = pack;
}
// 解方法 + URL parse + 扫表 + 错误码映射, 两个派发入口共用这一份。
// status 由调用方先取好: 流式入口要赶在分配请求上下文之前把无效 HTTP 挡掉。
// 返回 200 表示 *out_idx 有效, 其余为应回给客户端的应答码
static int32_t _router_match_entry(router_ctx *r, router_req *req,
                                   const buf_ctx *status, int32_t *out_idx) {
    // 所有派发面走同一个 router_match_index / router_match_code,
    // 避免对同一请求给出不同码
    int32_t idx = router_match_index(r, status[0].data, status[0].lens,
                                     status[1].data, status[1].lens, req);
    // 无条件回填: 失败时 idx 为负, 调用方漏判 code 会当场拿到非法下标而不是 0 号路由
    *out_idx = idx;
    return router_match_code(idx);
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
// 派发流程: 解方法 → URL parse → 线性扫表 → 拼 chain → 推进 → 兜底 500
void router_dispatch(router_ctx *r, task_ctx *task,
                     SOCKET fd, uint64_t skid,
                     struct http_pack_ctx *pack) {
    if (NULL == r) {
        return;
    }
    buf_ctx *status = _router_http_status(pack);
    if (NULL == status) {
        return;
    }
    url_ctx url;
    router_req ctx;
    _router_req_init(&ctx, &url, task, fd, skid, pack);
    int32_t idx;
    int32_t code = _router_match_entry(r, &ctx, status, &idx);
    if (200 != code) {
        _router_send_code(task, fd, skid, code);
        return;
    }
    router_entry *matched = &r->routes[idx];
    if (0 != _router_entry_misconfigured(matched, idx)) {
        _router_send_simple(task, fd, skid, 500, ROUTER_BODY_500);
        return;
    }
    if (ERR_OK != _router_chain_build(r, matched, &ctx)) {
        _router_send_simple(task, fd, skid, 500, ROUTER_BODY_CHAIN);
        return;
    }
    // 启动链路, 第一个中间件 / handler 通过 router_next 递归推进
    router_next(&ctx);
    // 流式路由命中一次到齐的请求: 准入通过才把请求体按 slice == 0 一次交出去
    if (NULL != matched->on_chunk
        && ctx.admitted) {
        size_t dlens = 0;
        void *data = http_data(pack, &dlens);
        matched->on_chunk(&ctx, 0, data, dlens);
    }
    // 中间件主动 return 不调 router_next 是合法截断; 但都没写响应 (handler 漏发 + 中间件
    // 也没截断) 时, 客户端会卡死, 这里兜底 500 让它别等
    if (!ctx.responded) {
        _router_send_simple(task, fd, skid, 500, ROUTER_BODY_500);
    }
}
void router_closed(router_ctx *r, SOCKET fd, uint64_t skid) {
    if (NULL == r
        || NULL == r->streams) {
        return;
    }
    sk_id sk;
    sk.fd = fd;
    sk.skid = skid;
    _router_st_drop(r, &sk);
}
// 首帧不通过时的统一收尾: 回响应 → 关连接 → 丢记录。请求体还在后面, 连接留着也收不了。
// code 传 0 表示调用方已经写过响应, 只关连接
static void _router_st_reject(router_stream *st, task_ctx *task, int32_t code, const char *body) {
    if (code > 0) {
        _router_send_simple(task, st->req.sk.fd, st->req.sk.skid, code, body);
    }
    ev_close(&task->loader->netev, st->req.sk.fd, st->req.sk.skid);
    FREE(st);
}
// 流式首帧: 匹配路由 → 跑准入链 → 进表 → 回调 PROT_SLICE_START
static void _router_st_begin(router_ctx *r, task_ctx *task, sk_id *sk, struct http_pack_ctx *pack) {
    buf_ctx *status = _router_http_status(pack);
    if (NULL == status) {
        return;// 无效 HTTP, 静默丢, 同 router_dispatch
    }
    // 同连接已有记录说明上一条流式请求没收尾, 丢旧的重开
    _router_st_drop(r, sk);
    router_stream *st;
    MALLOC(st, sizeof(router_stream));
    // MALLOC 不清零, 靠逐字段写满: 往 router_stream 加字段必须同时进这里(同 _router_group_fill)。
    // req 交给 _router_req_init, url 有意不清(理由见那里)
    st->on_chunk = NULL;
    _router_req_init(&st->req, &st->url, task, sk->fd, sk->skid, pack);
    int32_t idx;
    int32_t code = _router_match_entry(r, &st->req, status, &idx);
    if (200 != code) {
        char body[ROUTER_CODE_BODY_LENS];
        _router_code_body(code, body);
        _router_st_reject(st, task, code, body);
        return;
    }
    router_entry *matched = &r->routes[idx];
    // 配置错误要与 dispatch 给同一个码; 排在下面的 411 之前, 否则 index 条目会被
    // 当成"普通路由收到 chunked"而回 411, 同一个错两个码
    if (0 != _router_entry_misconfigured(matched, idx)) {
        _router_st_reject(st, task, 500, ROUTER_BODY_500);
        return;
    }
    // 命中的不是流式路由: 请求体正一块块往这边来, 普通 handler 接不住
    if (NULL == matched->on_chunk) {
        FREE(st);
        router_reject_chunked(task, sk->fd, sk->skid);
        return;
    }
    st->on_chunk = matched->on_chunk;
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
    // 流式表懒建: 多数 router 一辈子见不到一个流式请求, 不必都摊这份内存
    if (NULL == r->streams) {
        r->streams = hashmap_new(sizeof(router_st_ent), 8, 0, 0,
                                 _router_st_hash, _router_st_cmp, NULL, NULL);
        if (NULL == r->streams) {
            _router_st_reject(st, task, 500, ROUTER_BODY_500);
            return;
        }
    }
    // 进表要排在回调之前: 表满 OOM 得先有结论, 回调里若关连接也才找得到这条记录
    router_st_ent ent;
    ent.sk = *sk;
    ent.st = st;
    hashmap_set(r->streams, &ent);
    if (hashmap_oom(r->streams)) {
        _router_st_reject(st, task, 500, ROUTER_BODY_500);
        return;
    }
    st->on_chunk(&st->req, PROT_SLICE_START, NULL, 0);
    // 首包随本次回调结束即被协议层回收, 后面几帧头部一律读不到。
    // 回调里可能把这条流关掉(router_closed), 那时 st 已经释放, 先确认表里还是它才能写
    const router_st_ent *cur = (const router_st_ent *)hashmap_get(r->streams, &ent);
    if (NULL != cur
        && cur->st == st) {
        st->req.pack = NULL;
    }
}
// 流式中间/结束帧: 原样把 slice 与数据交给 on_chunk
static void _router_st_feed(router_ctx *r, task_ctx *task, sk_id *sk,
                            uint8_t slice, struct http_pack_ctx *pack) {
    if (NULL == r->streams) {
        return;
    }
    router_st_ent probe;
    probe.sk = *sk;
    probe.st = NULL;
    const router_st_ent *found = (const router_st_ent *)hashmap_get(r->streams, &probe);
    if (NULL == found) {
        return;// 首帧被拒过, 连接那时就关了, 后续帧静默丢
    }
    router_stream *st = found->st;
    size_t dlens = 0;
    void *data = http_data(pack, &dlens);
    if (0 == (PROT_SLICE_END & slice)) {
        st->on_chunk(&st->req, slice, data, dlens);
        return;
    }
    // 结束帧: 先摘表项再回调, 回调返回后连同 req 一起释放
    hashmap_delete(r->streams, &probe);
    st->on_chunk(&st->req, slice, data, dlens);
    if (!st->req.responded) {
        _router_send_simple(task, sk->fd, sk->skid, 500, ROUTER_BODY_500);
    }
    FREE(st);
}
// 没注册过流式路由时的 chunked 首帧: 结局只能是"匹配不上"/"命中普通路由"/"命中 index 条目",
// 三种都能用栈上 req 算出来, 不必先堆分配 router_stream。给的码与一次到齐的同一请求完全一致
static void _router_chunked_nostream(router_ctx *r, task_ctx *task, sk_id *sk,
                                     struct http_pack_ctx *pack) {
    buf_ctx *status = _router_http_status(pack);
    if (NULL == status) {
        return;// 无效 HTTP, 静默丢, 同 router_dispatch
    }
    url_ctx url;
    router_req ctx;
    _router_req_init(&ctx, &url, task, sk->fd, sk->skid, pack);
    int32_t idx;
    int32_t code = _router_match_entry(r, &ctx, status, &idx);
    if (200 == code
        && 0 == _router_entry_misconfigured(&r->routes[idx], idx)) {
        router_reject_chunked(task, sk->fd, sk->skid);// 命中普通路由, 请求体接不住
        return;
    }
    if (200 == code) {
        _router_send_simple(task, sk->fd, sk->skid, 500, ROUTER_BODY_500);
    } else {
        _router_send_code(task, sk->fd, sk->skid, code);
    }
    ev_close(&task->loader->netev, sk->fd, sk->skid);
}
// _net_recv 回调的标准实现: 一次到齐的请求直接派发; chunked 命中流式路由则逐帧交给它,
// 命中普通路由回 411, 匹配不上按普通请求的码走(404/400/405)。
// harbor / debug_console 各自的回调只负责从 task 参数里取出自己的 router 再转到这里
void router_net_recv(router_ctx *r, task_ctx *task, sk_id *sk,
                     subtype_t pktype, uint8_t client, uint8_t slice, void *data, size_t size) {
    (void)pktype;
    (void)client;
    (void)size;
    if (0 == slice) {
        router_dispatch(r, task, sk->fd, sk->skid, (struct http_pack_ctx *)data);
        return;
    }
    if (NULL == r) {
        return;
    }
    if (PROT_SLICE_START & slice) {
        // 一条流式路由都没注册: 结局不可能是"开一条流", 走 nostream 那条省掉近 5KB 堆分配
        if (0 == r->has_stream) {
            _router_chunked_nostream(r, task, sk, (struct http_pack_ctx *)data);
            return;
        }
        _router_st_begin(r, task, sk, (struct http_pack_ctx *)data);
        return;
    }
    _router_st_feed(r, task, sk, slice, (struct http_pack_ctx *)data);
}
