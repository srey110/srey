#include "advance/router_tree.h"
#include "containers/radix.h"
#include "containers/sarray.h"
#include "utils/urlparse.h"

// 节点 id 用 uint32；UINT32_MAX 表示没有该子节点。
// route id 的空值用 ROUTER_ROUTE_NONE，两个 sentinel 不混用。
typedef struct rtree_leaf rtree_leaf;
typedef struct rtree_node rtree_node;

struct rtree_leaf {
    router_method mask;      // 这条路由覆盖的方法
    router_route_id route_id; // 路由表下标
    uint32_t variant_id;     // 变体下标，指向 rtree_add_result.variants
};
RADIX_DECL(rtree_static, uint32_t)
ARR_DECL(rtree_leaf_arr, rtree_leaf)
struct rtree_node {
    uint32_t param_child;         // UINT32_MAX = 无
    uint32_t wild_child;          // UINT32_MAX = 无
    rtree_leaf_arr leaves;        // 本路径上的方法条目，按注册顺序
    rtree_static static_children; // 字面量段 -> 子节点 id
};
ARR_DECL(rtree_node_arr, rtree_node)
ARR_DECL(rtree_variant_arr, rtree_variant)
struct rtree_ctx {
    rtree_node_arr nodes; // 0 是根；节点只存 id，不存跨扩容指针
    size_t leaf_count;    // 全部叶子条目数，rtree_check 对账
};
typedef struct rtree_add_walk {
    int32_t n;               // 原始段数
    int32_t nopt;            // OPT 段数
    router_method mask;      // 路由方法掩码
    router_route_id route_id; // 调用方分配的路由 id
    int32_t inserted;        // 真正入叶子的变体数
    int32_t shadowed;        // 被已有叶子完全遮住的变体数
    int32_t skipped;         // 参数超限跳过数
    rtree_ctx *t;            // 目标树
    const router_seg *segs;  // 原始段序列，树只借用
    rtree_variant_arr variants; // 已生成的规范化变体
} rtree_add_walk;
typedef struct rtree_walk {
    int32_t nseg;            // 请求段数
    router_method method;    // 请求方法
    rtree_ctx *t;            // 目标树
    const buf_ctx *segs;     // 请求段
    uint8_t *scratch;        // 通配值拼接缓冲
    size_t cap;              // scratch 容量
    rtree_match_result *out; // 结果直接写这里：命中项、参数值、失败时的方法并集
} rtree_walk;
typedef struct rtree_check_ctx {
    const rtree_ctx *t; // 被检查的树
    uint32_t parent;    // 当前节点 id，子节点必须更大
    int32_t ok;         // 回调发现非法即置 0
} rtree_check_ctx;

static inline rtree_node *_node(rtree_ctx *t, uint32_t id) {
    ASSERTAB(id < t->nodes.size, "rtree node id out of range.");
    return &t->nodes.ptr[id];
}
static inline const rtree_node *_cnode(const rtree_ctx *t, uint32_t id) {
    ASSERTAB(id < t->nodes.size, "rtree node id out of range.");
    return &t->nodes.ptr[id];
}
// 追加一个空节点，返回 id。节点数组扩容后旧指针失效，调用方只能存 id。
static uint32_t _node_new(rtree_ctx *t) {
    rtree_node n;
    ZERO(&n, sizeof(n));
    rtree_static_init(&n.static_children, NULL);
    n.param_child = UINT32_MAX;
    n.wild_child = UINT32_MAX;
    rtree_leaf_arr_init(&n.leaves, 0);
    rtree_node_arr_push_back(&t->nodes, &n);
    return (uint32_t)(t->nodes.size - 1);
}
// 取/建字面量子节点。slot 指向 radix 内部值，不随 nodes 扩容失效。
static uint32_t _lit_child(rtree_ctx *t, uint32_t node_id, const router_seg *seg) {
    rtree_node *node = _node(t, node_id);
    uint32_t def = UINT32_MAX;
    int32_t created = 0;
    uint32_t *slot = rtree_static_get_or_insert(&node->static_children, seg->str, seg->str_len, &def, &created);
    if (created) {
        uint32_t child = _node_new(t);
        *slot = child;
    }
    return *slot;
}
// 取/建参数子节点；node_new 后必须重新取节点指针再赋值。
static uint32_t _param_child(rtree_ctx *t, uint32_t node_id) {
    rtree_node *node = _node(t, node_id);
    if (UINT32_MAX == node->param_child) {
        uint32_t child = _node_new(t);
        node = _node(t, node_id);
        node->param_child = child;
    }
    return node->param_child;
}
// 取/建末尾通配子节点。
static uint32_t _wild_child(rtree_ctx *t, uint32_t node_id) {
    rtree_node *node = _node(t, node_id);
    if (UINT32_MAX == node->wild_child) {
        uint32_t child = _node_new(t);
        node = _node(t, node_id);
        node->wild_child = child;
    }
    return node->wild_child;
}
// 把一个具体变体插到叶子。返回 1=插入，0=被已有条目完全遮住，-1=内部错误。
static int32_t _insert_variant(rtree_ctx *t, const router_seg *segs, int32_t n,
                               uint32_t bits, router_method mask,
                               router_route_id route_id, uint32_t variant_id) {
    uint32_t node_id = 0;
    int32_t opt_i = 0;
    const router_seg *seg;
    // 按具体形状下降：LIT 建静态边，PARAM/出现的 OPT 建参数边，WILD 建末尾边。
    for (int32_t i = 0; i < n; i++) {
        seg = &segs[i];
        switch (seg->t) {
        case ROUTER_SEG_LIT:
            node_id = _lit_child(t, node_id, seg);
            break;
        case ROUTER_SEG_PARAM:
            node_id = _param_child(t, node_id);
            break;
        case ROUTER_SEG_OPT:
            if (0 != (bits & (1u << opt_i))) {
                node_id = _param_child(t, node_id);
            }
            opt_i++;
            break;
        case ROUTER_SEG_WILD:
            if (i != n - 1) {
                return -1;
            }
            node_id = _wild_child(t, node_id);
            break;
        default:
            return -1;
        }
    }
    rtree_node *node = _node(t, node_id);
    // 已有条目的掩码合起来包住新掩码才算遮住（新条目永远轮不到）；部分重叠允许并存。
    router_method covered = 0;
    for (uint32_t i = 0; i < node->leaves.size; i++) {
        covered |= node->leaves.ptr[i].mask;
    }
    if (0 == (mask & ~covered)) {
        return 0;
    }
    rtree_leaf leaf;
    leaf.mask = mask;
    leaf.route_id = route_id;
    leaf.variant_id = variant_id;
    rtree_leaf_arr_push_back(&node->leaves, &leaf);
    t->leaf_count++;
    return 1;
}
// 生成一个变体：统计参数、插叶子。
static void _emit_variant(rtree_add_walk *w, uint32_t bits) {
    uint16_t param_seg[ROUTER_MAX_PARAMS];
    int32_t param_n = 0;
    int32_t opt_i = 0;
    const router_seg *seg;
    int32_t present;
    // 统计本变体的参数位置；超过上限直接跳过，不生成叶子。
    for (int32_t i = 0; i < w->n; i++) {
        seg = &w->segs[i];
        if (ROUTER_SEG_LIT == seg->t) {
            continue;
        }
        if (ROUTER_SEG_OPT == seg->t) {
            present = (0 != (bits & (1u << opt_i)));
            opt_i++;
            if (!present) {
                continue;
            }
        }
        if (param_n >= ROUTER_MAX_PARAMS) {
            w->skipped++;
            return;
        }
        param_seg[param_n++] = (uint16_t)i;
        if (ROUTER_SEG_WILD == seg->t) {
            break;
        }
    }
    // 先登记变体，variant_id 就是结果数组下标；再插叶子。
    uint32_t variant_id = (uint32_t)w->variants.size;
    rtree_variant v;
    ZERO(&v, sizeof(v));
    v.opt_bits = bits;
    v.param_n = (uint8_t)param_n;
    for (int32_t i = 0; i < param_n; i++) {
        v.param_seg[i] = param_seg[i];
    }
    rtree_variant_arr_push_back(&w->variants, &v);
    int32_t rtn = _insert_variant(w->t, w->segs, w->n, bits, w->mask, w->route_id, variant_id);
    if (rtn < 0) {
        w->variants.size--;
        w->skipped++;
    } else if (0 == rtn) {
        w->shadowed++;
    } else {
        w->inserted++;
    }
}
rtree_ctx *rtree_new(void) {
    rtree_ctx *t;
    MALLOC(t, sizeof(*t));
    ZERO(t, sizeof(*t));
    rtree_node_arr_init(&t->nodes, 8);
    (void)_node_new(t);
    return t;
}
void rtree_free(rtree_ctx *t) {
    if (NULL == t) {
        return;
    }
    for (uint32_t i = 0; i < t->nodes.size; i++) {
        rtree_static_free(&t->nodes.ptr[i].static_children);
        rtree_leaf_arr_free(&t->nodes.ptr[i].leaves);
    }
    rtree_node_arr_free(&t->nodes);
    FREE(t);
}
void rtree_add_result_free(rtree_add_result *res) {
    if (NULL == res) {
        return;
    }
    FREE(res->variants);
    ZERO(res, sizeof(*res));
}
int32_t rtree_add(rtree_ctx *t, const router_seg *segs, int32_t n, router_method mask, router_route_id route_id,
                  rtree_add_result *res) {
    if (NULL == res) {
        return -1;
    }
    ZERO(res, sizeof(*res));
    if (NULL == t || n < 0 || n > URL_MAX_PATH_DEPTH
        || (n > 0 && NULL == segs)) {
        return -1;
    }
    if (0 == mask || ROUTER_ROUTE_NONE == route_id || route_id > INT32_MAX) {
        return -1;
    }
    // WILD 必须末段；OPT 只能在末尾（后面只许再有 OPT 或 WILD），数量不能超过展开上限。
    int32_t nopt = 0;
    const router_seg *seg;
    for (int32_t i = 0; i < n; i++) {
        seg = &segs[i];
        if (ROUTER_SEG_WILD == seg->t) {
            if (i != n - 1) {
                return -1;
            }
        } else if (NULL == seg->str) {
            return -1;
        }
        if (nopt > 0
            && ROUTER_SEG_OPT != seg->t
            && ROUTER_SEG_WILD != seg->t) {
            return -1;
        }
        if (ROUTER_SEG_OPT == seg->t) {
            if (nopt >= ROUTER_MAX_OPT) {
                return -1;
            }
            nopt++;
        }
    }
    rtree_add_walk w;
    ZERO(&w, sizeof(w));
    w.t = t;
    w.segs = segs;
    w.n = n;
    w.nopt = nopt;
    w.mask = mask;
    w.route_id = route_id;
    rtree_variant_arr_init(&w.variants, 0);
    // 可选段只在末尾，展开就是"前 k 个出现"：k 从 nopt 递减，靠前的 OPT 优先取值，形状互不重复
    for (int32_t k = nopt; k >= 0; k--) {
        _emit_variant(&w, (1u << k) - 1);
    }
    // variants 所有权交给 res；树只保留 route/variant id。
    res->variants = w.variants.ptr;
    res->n = (int32_t)w.variants.size;
    res->inserted = w.inserted;
    res->shadowed = w.shadowed;
    res->skipped = w.skipped;
    w.variants.ptr = NULL;
    w.variants.size = 0;
    w.variants.maxsize = 0;
    // 部分变体被遮仍成功；全部被遮才返回 -2。
    if (0 == res->inserted) {
        return (res->shadowed > 0) ? -2 : -1;
    }
    return ERR_OK;
}
static int32_t _try_leaf(rtree_walk *w, uint32_t node_id, int32_t param_n) {
    const rtree_node *node = _cnode(w->t, node_id);
    const rtree_leaf *leaf;
    // 按注册顺序找第一个覆盖请求方法的条目。
    for (uint32_t i = 0; i < node->leaves.size; i++) {
        leaf = &node->leaves.ptr[i];
        if (0 != (w->method & leaf->mask)) {
            w->out->route_id = leaf->route_id;
            w->out->variant_id = leaf->variant_id;
            w->out->params_n = param_n;
            return 1;
        }
        // 路径命中但方法不匹配：记下来供 405 / OPTIONS 使用。
        w->out->allow |= leaf->mask;
        w->out->path_hit = 1;
    }
    return 0;
}
// 把 segs[si..nseg) 用 '/' 拼到 scratch；长度不够或溢出返回 0。
static int32_t _wild_join(const buf_ctx *segs, int32_t si, int32_t nseg,
                          uint8_t *scratch, size_t cap, rtree_param *out) {
    // 先算总长，再一次性写 scratch，避免两遍拼接。
    size_t total = 0;
    for (int32_t i = si; i < nseg; i++) {
        if (segs[i].lens > SIZE_MAX - total) {
            return 0;
        }
        total += segs[i].lens;
        if (i > si) {
            total++;
        }
    }
    if (total > cap || (uint64_t)total > UINT32_MAX
        || (total > 0 && NULL == scratch)) {
        return 0;
    }
    // 段之间补 '/'；空段只是不拷字节。
    size_t off = 0;
    for (int32_t i = si; i < nseg; i++) {
        if (i > si) {
            scratch[off++] = '/';
        }
        if (segs[i].lens > 0) {
            memcpy(scratch + off, segs[i].data, segs[i].lens);
            off += segs[i].lens;
        }
    }
    out->val = scratch;
    out->len = (uint32_t)off;
    return 1;
}
// 静态优先；静态/参数分支失败后继续试参数/通配。只有方法命中才返回 1。
static int32_t _match_node(rtree_walk *w, uint32_t node_id,
                           int32_t si, int32_t param_n) {
    const rtree_node *node = _cnode(w->t, node_id);
    // 路径走完：先精确叶子，再试零段通配。
    if (si == w->nseg) {
        if (_try_leaf(w, node_id, param_n)) {
            return 1;
        }
        if (UINT32_MAX != node->wild_child
            && _try_leaf(w, node->wild_child, param_n)) {
            return 1;
        }
        return 0;
    }
    // 1. 静态优先。
    const buf_ctx *seg = &w->segs[si];
    uint32_t *slot = rtree_static_get(&node->static_children, seg->data, seg->lens);
    if (NULL != slot && UINT32_MAX != *slot
        && _match_node(w, *slot, si + 1, param_n)) {
        return 1;
    }
    // 2. 参数分支；失败继续往下试通配。
    if (UINT32_MAX != node->param_child && param_n < ROUTER_MAX_PARAMS) {
        w->out->params[param_n].val = (const uint8_t *)seg->data;
        w->out->params[param_n].len = (uint32_t)seg->lens;
        if (_match_node(w, node->param_child, si + 1, param_n + 1)) {
            return 1;
        }
    }
    // 3. 通配吃剩余段。
    if (UINT32_MAX != node->wild_child && param_n < ROUTER_MAX_PARAMS) {
        if (_wild_join(w->segs, si, w->nseg, w->scratch, w->cap, &w->out->params[param_n])) {
            if (_try_leaf(w, node->wild_child, param_n + 1)) {
                return 1;
            }
        }
    }
    return 0;
}
int32_t rtree_match(rtree_ctx *t, const buf_ctx *segs, int32_t nseg, uint8_t *scratch, size_t scratch_cap, router_method method,
                    rtree_match_result *out) {
    if (NULL == out) {
        return -1;
    }
    ZERO(out, sizeof(*out));
    out->route_id = ROUTER_ROUTE_NONE;
    if (NULL == t || nseg < 0 || nseg > URL_MAX_PATH_DEPTH
        || (nseg > 0 && NULL == segs)) {
        return -1;
    }
    if (0 == method) {
        return -3;
    }
    rtree_walk w;
    w.nseg = nseg;
    w.method = method;
    w.t = t;
    w.segs = segs;
    w.scratch = scratch;
    w.cap = scratch_cap;
    w.out = out;
    // DFS 命中即返回；否则按 path_hit/allow 区分 404/405。参数只填值，key 由 router 层用 variant->param_seg 回填
    if (_match_node(&w, 0, 0, 0)) {
        out->allow = 0;// 命中前回溯过的分支可能记过，命中时不报
        out->path_hit = 0;
        return (int32_t)out->route_id;
    }
    if (0 == out->path_hit) {
        return -1;
    }
    return (ROUTER_M_OPTIONS == method) ? -5 : -4;
}
static int32_t _check_static_cb(const uint8_t *key, size_t klen,
                                uint32_t *val, void *ud) {
    rtree_check_ctx *ctx = ud;
    (void)key;
    (void)klen;
    if (NULL == val || UINT32_MAX == *val || *val >= ctx->t->nodes.size
        || *val <= ctx->parent) {
        ctx->ok = 0;
        return 0;
    }
    return 1;
}
int32_t rtree_check(const rtree_ctx *t) {
    if (NULL == t || 0 == t->nodes.size) {
        return ERR_FAILED;
    }
    size_t leaves = 0;
    const rtree_node *node;
    rtree_check_ctx ctx;
    // 子节点 id 必须更大：节点只追加，出现回边就是环或坏 id。
    for (uint32_t i = 0; i < t->nodes.size; i++) {
        node = _cnode(t, i);
        if (UINT32_MAX != node->param_child
            && (node->param_child >= t->nodes.size || node->param_child <= i)) {
            return ERR_FAILED;
        }
        if (UINT32_MAX != node->wild_child
            && (node->wild_child >= t->nodes.size || node->wild_child <= i)) {
            return ERR_FAILED;
        }
        // radix 自己查前缀/子节点，回调只查值指向合法节点。
        if (rtree_static_check(&node->static_children) != ERR_OK) {
            return ERR_FAILED;
        }
        ctx.t = t;
        ctx.parent = i;
        ctx.ok = 1;
        (void)rtree_static_walk(&node->static_children, _check_static_cb, &ctx);
        if (0 == ctx.ok) {
            return ERR_FAILED;
        }
        // 叶子掩码不能为 0，route id 不能为空。
        for (uint32_t k = 0; k < node->leaves.size; k++) {
            if (0 == node->leaves.ptr[k].mask
                || ROUTER_ROUTE_NONE == node->leaves.ptr[k].route_id) {
                return ERR_FAILED;
            }
            leaves++;
        }
    }
    if (leaves != t->leaf_count) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
size_t rtree_node_count(const rtree_ctx *t) {
    return NULL == t ? 0 : t->nodes.size;
}
size_t rtree_leaf_count(const rtree_ctx *t) {
    return NULL == t ? 0 : t->leaf_count;
}
size_t rtree_mem(const rtree_ctx *t) {
    if (NULL == t) {
        return 0;
    }
    size_t total = sizeof(*t) + t->nodes.maxsize * sizeof(rtree_node);
    // 按容量算，不按 size；空槽也算占用。
    for (uint32_t i = 0; i < t->nodes.size; i++) {
        total += t->nodes.ptr[i].leaves.maxsize * sizeof(rtree_leaf);
        // 内嵌的 radix 结构体已算在节点数组里，扣掉 radix_mem 自带的那份
        total += rtree_static_mem(&t->nodes.ptr[i].static_children) - sizeof(rtree_static);
    }
    return total;
}
