#include "services/subcenter.h"
#include "services/sv_pub.h"
#include "srey/task.h"
#include "containers/hashmap.h"
#include "containers/hashset.h"
#include "path/path_trie.h"
#include "containers/sarray.h"
#include "utils/binary.h"
#include "utils/tda.h"
#include "utils/utils.h"

// ── subcenter 限制(业务特定上限,可按部署需要调整后重编) ────────────
// 上限类的入参约束宏见 subcenter.h——那是公开契约，调用方要照它做前置检查
#define SC_SUB_WARN_THRESHOLD       1000 // 单 topic 订阅者超过此值 LOG_WARN

// publisher 元数据条目(挂 sc_ctx.publisher_meta hashmap)
typedef struct sc_publisher_meta {
    size_t size;          // meta 字节数
    name_t publisher;     // 作为 hashmap key
    void *meta;           // publisher 元数据(MALLOC)
}sc_publisher_meta;
// 共享订阅组(挂 sc_topic_data.shared_groups hashmap)
typedef struct sc_shared_group {
    size_t cursor;        // 轮询游标
    char *group;          // strdup,作为 hashmap key
    array_ctx members;    // 元素 name_t
}sc_shared_group;
// 订阅节点 payload(挂 path_trie 节点)
// 字段按内存对齐(指针/结构体 8B,然后小整型)
typedef struct sc_topic_data {
    struct hashmap *shared_groups;   // group_name → sc_shared_group(延迟创建)
    char *pattern;                   // 订阅 topic(strdup);删空通配节点时 path_remove 用
    array_ctx normal_subs;           // 元素 name_t
}sc_topic_data;
// 独立 retained 条目(挂 sc_ctx.retained_index hashmap)
// 字段按内存对齐
typedef struct sc_retained_entry {
    size_t retained_size;            // payload 字节数
    size_t retained_meta_size;       // meta 字节数
    name_t retained_publisher;       // 发布者 task 句柄
    char *topic;                     // strdup,作为 hashmap key
    void *retained;                  // 保留消息 payload(MALLOC)
    void *retained_meta;             // publish_retained 时 meta 快照
}sc_retained_entry;
// subcenter task 上下文
typedef struct sc_ctx {
    uint32_t pub_normal_cap;         // pub_normal 当前容量(元素数)
    size_t retained_skipped;         // 累计有多少次 retained 发布跳过了共享组
    loader_ctx *loader;              // 所属 loader
    path_trie *topics;               // 订阅关系
    const path_rules *rules;         // topic 规则(由 sc_start 传入,长生命周期持有)
    struct hashmap *publisher_meta;  // name_t → sc_publisher_meta
    struct hashmap *retained_index;  // topic → sc_retained_entry
    hashset *publish_dedup;          // publish 去重复用容器(name_t set)
    task_ctx **pub_normal;           // publish 复用:普通投递目标缓冲(按需 grow)
    tda_ctx retained_skip_tda;       // 上者的翻倍告警状态,免得每条消息打一遍
    array_ctx pub_shared;            // publish 复用:共享投递目标(sc_shared_dst)
    array_ctx pub_prune;             // publish 复用:死订阅待清理(name_t)
    array_ctx pub_empty;             // publish 复用:空节点路径(char*)
}sc_ctx;
// path_match 的 visit:收集订阅者
// 共享投递目标:挑中的成员 + 其所属组名 + 命中的订阅 pattern。
// group/pattern 是借用指针,分别指向 sc_shared_group.group 与 sc_topic_data.pattern,
// 不拷贝。两者的释放点(_sc_sg_free / _sc_topic_data_free)都在 _sc_publish_deliver 的懒清理
// 段里,故那一段必须排在共享投递之后 —— 理由与可踩中的路径见该处注释
typedef struct sc_shared_dst {
    uint16_t ptlen;       // pattern 长度,collect 阶段每节点算一次,免得每个目的地重算
    task_ctx *task;
    const char *group;
    const char *pattern;
}sc_shared_dst;
typedef struct sc_collect_ctx {
    uint16_t cur_ptlen;           // 上者的长度,同一节点下所有组共用,不必每组重算
    int32_t failed;               // 内存分配失败标志
    int32_t shared_emptied;       // 有共享组在 pick 时被清空 → 触发清理 pass
    int32_t retained;             // 本次是 publish_retained：共享订阅不收，只扇给普通订阅者
    int32_t shared_skipped;       // 上者导致真有共享组被跳过，投递后报一次
    sc_ctx *ctx;                  // subcenter 上下文
    array_ctx *shared_dsts;       // 共享组挑选结果(元素 sc_shared_dst,已 grab,投递后 ungrab)
    const char *cur_pattern;      // 当前 visit 到的节点 pattern,供 _sc_sg_pick_iter 取用
}sc_collect_ctx;
// QUERY_RETAINED 遍历 retained_index 的上下文:pattern 过滤,匹配项拼进 bw,超 BURST_MAX 截断
typedef struct sc_qr_ctx {
    int32_t pushed;            // 已写入条数(< SC_QUERY_RETAINED_BURST_MAX)
    int32_t truncated;         // 超上限截断标志
    binary_ctx *bw;            // 输出 wire 缓冲
    const path_rules *rules;   // topic 规则(path_matches_pattern 用)
    const char *pattern;       // 查询模式
}sc_qr_ctx;
// path_match prune visit:从命中节点(含通配)移除死订阅;删后变空节点的 pattern 收入 empty_nodes,
// 待 path_match 返回后统一 path_remove(DFS 遍历 trie 时不可删节点)
typedef struct sc_prune_ctx {
    array_ctx *prune;        // 死订阅 name 列表
    array_ctx *empty_nodes;  // 删后变空节点的 pattern(char*),待 path_remove
}sc_prune_ctx;
// _sc_prune_visit 内层(shared_groups scan):从单个共享组移除死成员,组变空收集 group 名待删
typedef struct sc_sg_prune_ctx {
    array_ctx *prune;        // 死订阅 name 列表(与 normal 共用)
    array_ctx *empty_groups; // 删空后待移除的 group 名(char*,指向 sc_shared_group.group)
}sc_sg_prune_ctx;
// publisher_meta 懒清理用：scan 期间只收集 grab 不到的 name，删除放到 scan 之后
typedef struct sc_pm_prune_ctx {
    sc_ctx *ctx;
    array_ctx *dead;
}sc_pm_prune_ctx;

// publisher_meta hashmap
static uint64_t _sc_pm_hash(const void *item, uint64_t s0, uint64_t s1) {
    const sc_publisher_meta *e = (const sc_publisher_meta *)item;
    return hashmap_xxhash3(&e->publisher, sizeof(name_t), s0, s1);
}
static int _sc_pm_cmp(const void *a, const void *b, void *ud) {
    (void)ud;
    name_t na = ((const sc_publisher_meta *)a)->publisher;
    name_t nb = ((const sc_publisher_meta *)b)->publisher;
    return (na > nb) - (na < nb);
}
static void _sc_pm_free(void *item) {
    sc_publisher_meta *e = (sc_publisher_meta *)item;
    FREE(e->meta);
}
static bool _sc_pm_prune_iter(const void *item, void *udata) {
    const sc_publisher_meta *e = (const sc_publisher_meta *)item;
    sc_pm_prune_ctx *pp = (sc_pm_prune_ctx *)udata;
    task_ctx *t = task_grab(pp->ctx->loader, e->publisher);
    if (NULL == t) {
        array_push_back(pp->dead, &e->publisher);
        return true;
    }
    task_ungrab(t);
    return true;
}
// shared_groups hashmap
static uint64_t _sc_sg_hash(const void *item, uint64_t s0, uint64_t s1) {
    const sc_shared_group *e = (const sc_shared_group *)item;
    return hashmap_xxhash3(e->group, strlen(e->group), s0, s1);
}
static int _sc_sg_cmp(const void *a, const void *b, void *ud) {
    (void)ud;
    return strcmp(((const sc_shared_group *)a)->group, ((const sc_shared_group *)b)->group);
}
static void _sc_sg_free(void *item) {
    sc_shared_group *g = (sc_shared_group *)item;
    FREE(g->group);
    array_free(&g->members);
}
// retained_index hashmap
static uint64_t _sc_re_hash(const void *item, uint64_t s0, uint64_t s1) {
    const sc_retained_entry *e = (const sc_retained_entry *)item;
    return hashmap_xxhash3(e->topic, strlen(e->topic), s0, s1);
}
static int _sc_re_cmp(const void *a, const void *b, void *ud) {
    (void)ud;
    return strcmp(((const sc_retained_entry *)a)->topic, ((const sc_retained_entry *)b)->topic);
}
static void _sc_re_free(void *item) {
    sc_retained_entry *e = (sc_retained_entry *)item;
    FREE(e->topic);
    FREE(e->retained);
    FREE(e->retained_meta);
}
// publish_dedup hashset(name_t)
static uint64_t _sc_name_hash(const void *item, uint64_t s0, uint64_t s1) {
    return hashmap_xxhash3(item, sizeof(name_t), s0, s1);
}
static int _sc_name_cmp(const void *a, const void *b, void *ud) {
    (void)ud;
    name_t na = *(const name_t *)a;
    name_t nb = *(const name_t *)b;
    return (na > nb) - (na < nb);
}
// sc_topic_data alloc / free
static sc_topic_data *_sc_alloc_topic_data(const char *pattern) {
    sc_topic_data *d;
    CALLOC(d, 1, sizeof(sc_topic_data));
    size_t plen = strlen(pattern);
    d->pattern = dup_zero(pattern, plen);
    array_init(&d->normal_subs, sizeof(name_t), 0);
    return d;
}
// 释放 sc_topic_data（trie 节点移除或销毁时调用）
static void _sc_topic_data_free(void *p) {
    sc_topic_data *d = (sc_topic_data *)p;
    array_free(&d->normal_subs);
    if (NULL != d->shared_groups) {
        hashmap_free(d->shared_groups);
    }
    FREE(d->pattern);
    FREE(d);
}
// 从 binary_ctx 读 | u16 len | bytes |;成功返指针 + 长度
static int32_t _sc_read_lp16(binary_ctx *br, const char **out_data, uint16_t *out_len) {
    if (!binary_have(br, 2)) {
        return ERR_FAILED;
    }
    uint16_t n = (uint16_t)binary_get_uinteger(br, 2, 0);
    if (!binary_have(br, (size_t)n)) {
        return ERR_FAILED;
    }
    *out_len = n;
    *out_data = (n > 0) ? binary_get_binary(br, n) : NULL;
    return ERR_OK;
}
// 同上,长度上限校验
static int32_t _sc_read_lp16_max(binary_ctx *br, const char **out_data, uint16_t *out_len, uint16_t max_len) {
    if (ERR_OK != _sc_read_lp16(br, out_data, out_len)) {
        return ERR_FAILED;
    }
    if (*out_len > max_len) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
// 从 br 读取一个长度上限受限的 NUL 终结字符串(拷贝到 dst)。
// 内嵌 NUL 一律拒绝：它会让 dst 这个 C 串在 NUL 处截断，实际用到的是另一个更短的 topic /
// group，与 datacenter 的 _dc_key_to_cstr 同口径
static int32_t _sc_read_cstr_max(binary_ctx *br, char *dst, size_t dst_cap, uint16_t max_len) {
    const char *p;
    uint16_t n;
    if (ERR_OK != _sc_read_lp16_max(br, &p, &n, max_len)) {
        return ERR_FAILED;
    }
    if (0 == n || n + 1u > dst_cap
        || NULL != memchr(p, 0, n)) {
        return ERR_FAILED;
    }
    memcpy(dst, p, n);
    dst[n] = '\0';
    return ERR_OK;
}
// 构造 deliver wire:| u8 kind | name_t publisher | u16 mlen | meta | u16 glen | group |
//                   | u16 ptlen | pattern | u16 tlen | topic | u32 plen | payload |
// group/pattern 仅共享投递(kind=SHARED)非空;普通投递 glen=ptlen=0。
// pattern 是命中的订阅模式:接收方据 (pattern, group) 精确定位 handler,不带它就只能对所有
// 匹配模式扇出,而 C 侧本就按(节点,组)逐条单发,两次扇出相乘即平方级重复调用
static char *_sc_pack_deliver(uint8_t kind, name_t publisher,
                              const void *meta, uint16_t mlen,
                              const char *group, uint16_t glen,
                              const char *pattern, uint16_t ptlen,
                              const char *topic,
                              const void *payload, uint32_t plen,
                              size_t *out_total) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    binary_set_uint8(&bw, kind);
    binary_set_uinteger(&bw, (uint64_t)publisher, sizeof(name_t), 0);
    binary_set_uinteger(&bw, (uint64_t)mlen, 2, 0);
    if (mlen > 0 && NULL != meta) {
        binary_set_binary(&bw, (const char *)meta, mlen);
    }
    binary_set_uinteger(&bw, (uint64_t)glen, 2, 0);
    if (glen > 0 && NULL != group) {
        binary_set_binary(&bw, group, glen);
    }
    binary_set_uinteger(&bw, (uint64_t)ptlen, 2, 0);
    if (ptlen > 0 && NULL != pattern) {
        binary_set_binary(&bw, pattern, ptlen);
    }
    size_t tlen = strlen(topic);
    binary_set_uinteger(&bw, (uint64_t)tlen, 2, 0);
    binary_set_binary(&bw, topic, tlen);
    binary_set_uinteger(&bw, (uint64_t)plen, 4, 0);
    if (plen > 0 && NULL != payload) {
        binary_set_binary(&bw, (const char *)payload, plen);
    }
    *out_total = bw.offset;
    return bw.data;
}
int32_t sc_parse_deliver(const void *data, size_t size, sc_deliver *out) {
    binary_ctx br;
    binary_init(&br, (char *)data, size, 0);
    // kind(u8) + publisher(name_t) + mlen(u16)
    if (!binary_have(&br, 1 + sizeof(name_t) + 2)) {
        return ERR_FAILED;
    }
    out->kind = (int32_t)binary_get_uint8(&br);
    out->publisher = (name_t)binary_get_uinteger(&br, sizeof(name_t), 0);
    out->mlen = (size_t)binary_get_uinteger(&br, 2, 0);
    // meta(mlen) + glen(u16)
    if (!binary_have(&br, out->mlen + 2)) {
        return ERR_FAILED;
    }
    out->meta = out->mlen > 0 ? binary_get_binary(&br, out->mlen) : NULL;
    out->glen = (size_t)binary_get_uinteger(&br, 2, 0);
    // group(glen) + tlen(u16)
    if (!binary_have(&br, out->glen + 2)) {
        return ERR_FAILED;
    }
    out->group = out->glen > 0 ? binary_get_binary(&br, out->glen) : NULL;
    out->ptlen = (size_t)binary_get_uinteger(&br, 2, 0);
    // pattern(ptlen) + tlen(u16)
    if (!binary_have(&br, out->ptlen + 2)) {
        return ERR_FAILED;
    }
    out->pattern = out->ptlen > 0 ? binary_get_binary(&br, out->ptlen) : NULL;
    out->tlen = (size_t)binary_get_uinteger(&br, 2, 0);
    // topic(tlen) + plen(u32)
    if (!binary_have(&br, out->tlen + 4)) {
        return ERR_FAILED;
    }
    out->topic = out->tlen > 0 ? binary_get_binary(&br, out->tlen) : NULL;
    out->plen = (size_t)binary_get_uinteger(&br, 4, 0);
    // payload(plen)
    if (!binary_have(&br, out->plen)) {
        return ERR_FAILED;
    }
    out->payload = out->plen > 0 ? binary_get_binary(&br, out->plen) : NULL;
    return ERR_OK;
}
// 游标式解析 query_retained 响应:| name_t publisher | u16 mlen | meta | u16 tlen | topic | u32 plen | payload |
int32_t sc_parse_retained(binary_ctx *br, sc_retained *out) {
    // publisher(name_t) + mlen(u16)
    if (!binary_have(br, sizeof(name_t) + 2)) {
        return ERR_FAILED;
    }
    out->publisher = (name_t)binary_get_uinteger(br, sizeof(name_t), 0);
    out->mlen = (size_t)binary_get_uinteger(br, 2, 0);
    // meta(mlen) + tlen(u16)
    if (!binary_have(br, out->mlen + 2)) {
        return ERR_FAILED;
    }
    out->meta = out->mlen > 0 ? binary_get_binary(br, out->mlen) : NULL;
    out->tlen = (size_t)binary_get_uinteger(br, 2, 0);
    // topic(tlen) + plen(u32)
    if (!binary_have(br, out->tlen + 4)) {
        return ERR_FAILED;
    }
    out->topic = out->tlen > 0 ? binary_get_binary(br, out->tlen) : NULL;
    out->plen = (size_t)binary_get_uinteger(br, 4, 0);
    // payload(plen)
    if (!binary_have(br, out->plen)) {
        return ERR_FAILED;
    }
    out->payload = out->plen > 0 ? binary_get_binary(br, out->plen) : NULL;
    return ERR_OK;
}
// 游标式解析 topics 响应:| u16 tlen | topic | u32 normal | u32 shared |
int32_t sc_parse_topics(binary_ctx *br, sc_topic *out) {
    // tlen(u16)
    if (!binary_have(br, 2)) {
        return ERR_FAILED;
    }
    out->tlen = (size_t)binary_get_uinteger(br, 2, 0);
    // topic(tlen) + normal(u32) + shared(u32)
    if (!binary_have(br, out->tlen + 4 + 4)) {
        return ERR_FAILED;
    }
    out->topic = out->tlen > 0 ? binary_get_binary(br, out->tlen) : NULL;
    out->normal = (uint32_t)binary_get_uinteger(br, 4, 0);
    out->shared = (uint32_t)binary_get_uinteger(br, 4, 0);
    return ERR_OK;
}
// 游标式解析 retained_topics 响应:| u16 tlen | topic | name_t publisher | u32 size | u16 meta_size |
int32_t sc_parse_retained_topics(binary_ctx *br, sc_retained_topic *out) {
    // tlen(u16)
    if (!binary_have(br, 2)) {
        return ERR_FAILED;
    }
    out->tlen = (size_t)binary_get_uinteger(br, 2, 0);
    // topic(tlen) + publisher(name_t) + size(u32) + meta_size(u16)
    if (!binary_have(br, out->tlen + sizeof(name_t) + 4 + 2)) {
        return ERR_FAILED;
    }
    out->topic = out->tlen > 0 ? binary_get_binary(br, out->tlen) : NULL;
    out->publisher = (name_t)binary_get_uinteger(br, sizeof(name_t), 0);
    out->size = (uint32_t)binary_get_uinteger(br, 4, 0);
    out->meta_size = (uint16_t)binary_get_uinteger(br, 2, 0);
    return ERR_OK;
}
// 在 array_ctx 中线性查找 name,找到返回索引,否则返 -1
static int32_t _sc_name_find(array_ctx *arr, name_t n) {
    name_t *p = (name_t *)arr->ptr;
    uint32_t i;
    for (i = 0; i < arr->size; i++) {
        if (p[i] == n) {
            return (int32_t)i;
        }
    }
    return -1;
}
// 普通订阅:加入 normal_subs(幂等)
static int32_t _sc_normal_subs_add(array_ctx *subs, name_t src) {
    if (_sc_name_find(subs, src) >= 0) {
        return 0;// 已存在
    }
    array_push_back(subs, &src);
    return 1;// 新增
}
// 普通订阅:从 normal_subs 移除(找不到幂等返 0)
static int32_t _sc_normal_subs_remove(array_ctx *subs, name_t src) {
    int32_t idx = _sc_name_find(subs, src);
    if (idx < 0) {
        return 0;
    }
    array_del_nomove(subs, idx);
    return 1;
}
// handler:SUB(shared=0)/ SUB_SHARED(shared=1)。shared 时多解析 group;
// 路径含通配由 path_get_or_create 内部 WILDCARD 校验拒绝
static void _sc_handle_sub(sc_ctx *ctx, name_t src, uint64_t sess, binary_ctx *br, int32_t shared) {
    subtype_t reqtype = shared ? REQ_SC_SUB_SHARED : REQ_SC_SUB;
    char topic[SC_TOPIC_MAX + 1];
    if (ERR_OK != _sc_read_cstr_max(br, topic, sizeof(topic), SC_TOPIC_MAX)) {
        _svpub_respond(ctx->loader, src, reqtype, sess, ERR_FAILED);
        return;
    }
    char group[SC_GROUP_MAX + 1];
    if (shared) {
        if (ERR_OK != _sc_read_cstr_max(br, group, sizeof(group), SC_GROUP_MAX)) {
            _svpub_respond(ctx->loader, src, reqtype, sess, ERR_FAILED);
            return;
        }
    }
    // path_get_or_create 在 topic 已存在或 path 校验失败时不会接管 init,
    // 这里 get 失败时再独立 alloc + insert,避免重复订阅泄漏 sc_topic_data
    sc_topic_data *d = (sc_topic_data *)path_get(ctx->topics, topic);
    if (NULL == d) {
        d = _sc_alloc_topic_data(topic);
        if (ERR_OK != path_insert(ctx->topics, topic, d)) {
            _sc_topic_data_free(d);
            _svpub_respond(ctx->loader, src, reqtype, sess, ERR_FAILED);
            return;
        }
    }
    if (shared) {
        if (NULL == d->shared_groups) {
            d->shared_groups = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                                           sizeof(sc_shared_group), 4, 0, 0,
                                                           _sc_sg_hash, _sc_sg_cmp, _sc_sg_free, NULL);
            if (NULL == d->shared_groups) {
                _svpub_respond(ctx->loader, src, reqtype, sess, ERR_FAILED);
                return;
            }
        }
        sc_shared_group qg;
        qg.group = group;
        sc_shared_group *g = (sc_shared_group *)hashmap_get(d->shared_groups, &qg);
        if (NULL == g) {
            sc_shared_group ng;
            size_t glen = strlen(group);
            ng.group = dup_zero(group, glen);
            array_init(&ng.members, sizeof(name_t), 0);
            ng.cursor = 0;
            hashmap_set(d->shared_groups, &ng);
            if (hashmap_oom(d->shared_groups)) {
                FREE(ng.group);
                array_free(&ng.members);
                _svpub_respond(ctx->loader, src, reqtype, sess, ERR_FAILED);
                return;
            }
            g = (sc_shared_group *)hashmap_get(d->shared_groups, &qg);
        }
        // 加入 members(幂等)
        if (_sc_name_find(&g->members, src) < 0) {
            array_push_back(&g->members, &src);
        }
    } else {
        if (1 == _sc_normal_subs_add(&d->normal_subs, src)
            && d->normal_subs.size > SC_SUB_WARN_THRESHOLD) {
            LOG_WARN("subcenter topic '%s' has %u subscribers", topic, d->normal_subs.size);
        }
    }
    _svpub_respond(ctx->loader, src, reqtype, sess, ERR_OK);
}
// 节点回收检查:若节点完全空(无普通订阅、无共享组),从 trie 移除
static void _sc_try_remove_empty_topic(sc_ctx *ctx, const char *topic, sc_topic_data *d) {
    if (d->normal_subs.size > 0) {
        return;
    }
    if (NULL != d->shared_groups && hashmap_count(d->shared_groups) > 0) {
        return;
    }
    void *removed = path_remove(ctx->topics, topic);
    if (NULL != removed) {
        _sc_topic_data_free(removed);
    }
}
// handler:UNSUB(shared=0)/ UNSUB_SHARED(shared=1)。未订阅过的 topic 幂等返 OK;
// 节点完全空(无 normal_subs、无 shared_groups)时从 trie 移除
static void _sc_handle_unsub(sc_ctx *ctx, name_t src, uint64_t sess, binary_ctx *br, int32_t shared) {
    subtype_t reqtype = shared ? REQ_SC_UNSUB_SHARED : REQ_SC_UNSUB;
    char topic[SC_TOPIC_MAX + 1];
    if (ERR_OK != _sc_read_cstr_max(br, topic, sizeof(topic), SC_TOPIC_MAX)) {
        _svpub_respond(ctx->loader, src, reqtype, sess, ERR_FAILED);
        return;
    }
    char group[SC_GROUP_MAX + 1];
    if (shared) {
        if (ERR_OK != _sc_read_cstr_max(br, group, sizeof(group), SC_GROUP_MAX)) {
            _svpub_respond(ctx->loader, src, reqtype, sess, ERR_FAILED);
            return;
        }
    }
    sc_topic_data *d = (sc_topic_data *)path_get(ctx->topics, topic);
    if (NULL == d) {
        _svpub_respond(ctx->loader, src, reqtype, sess, ERR_OK);// 幂等
        return;
    }
    if (shared) {
        if (NULL == d->shared_groups) {
            _svpub_respond(ctx->loader, src, reqtype, sess, ERR_OK);
            return;
        }
        sc_shared_group qg;
        qg.group = group;
        sc_shared_group *g = (sc_shared_group *)hashmap_get(d->shared_groups, &qg);
        if (NULL != g) {
            int32_t idx = _sc_name_find(&g->members, src);
            if (idx >= 0) {
                array_del_nomove(&g->members, idx);
            }
            if (0 == g->members.size) {
                // hashmap_delete 返回 spare 副本但不会自动调 elfree,需手动调 _sc_sg_free 释放 group/members
                sc_shared_group *removed = (sc_shared_group *)hashmap_delete(d->shared_groups, &qg);
                if (NULL != removed) {
                    _sc_sg_free(removed);
                }
            }
        }
        if (0 == hashmap_count(d->shared_groups)) {
            hashmap_free(d->shared_groups);
            d->shared_groups = NULL;
        }
    } else {
        (void)_sc_normal_subs_remove(&d->normal_subs, src);
    }
    _sc_try_remove_empty_topic(ctx, topic, d);
    _svpub_respond(ctx->loader, src, reqtype, sess, ERR_OK);
}
// 更新 retained_index 槽位(publish_retained 第一步,独立于普通 deliver 路径)。
// plen=0 → 删除条目;否则 MALLOC + memcpy + 取 publisher 当前 meta 做快照存进 entry
static void _sc_update_retained(sc_ctx *ctx, name_t src, const char *topic,
                                const void *payload, uint32_t plen) {
    sc_retained_entry q;
    q.topic = (char *)topic;
    sc_retained_entry *e = (sc_retained_entry *)hashmap_get(ctx->retained_index, &q);
    if (0 == plen) {
        // 清空 retained 槽位:hashmap_delete 返回 spare 副本但不自动调 elfree,需手动 _sc_re_free
        if (NULL != e) {
            sc_retained_entry *removed = (sc_retained_entry *)hashmap_delete(ctx->retained_index, &q);
            if (NULL != removed) {
                _sc_re_free(removed);
            }
        }
        return;
    }
    // 查 publisher 当前 meta 做快照
    sc_publisher_meta qm;
    qm.publisher = src;
    sc_publisher_meta *pm = (sc_publisher_meta *)hashmap_get(ctx->publisher_meta, &qm);
    const void *meta = (NULL != pm) ? pm->meta : NULL;
    size_t mlen = (NULL != pm) ? pm->size : 0;
    if (NULL != e) {
        // 更新现有 entry
        FREE(e->retained);
        FREE(e->retained_meta);
        MALLOC(e->retained, plen);
        memcpy(e->retained, payload, plen);
        e->retained_size = plen;
        if (mlen > 0) {
            MALLOC(e->retained_meta, mlen);
            memcpy(e->retained_meta, meta, mlen);
            e->retained_meta_size = mlen;
        } else {
            e->retained_meta = NULL;
            e->retained_meta_size = 0;
        }
        e->retained_publisher = src;
    } else {
        // 新建 entry
        sc_retained_entry ne;
        size_t tlen = strlen(topic);
        ne.topic = dup_zero(topic, tlen);
        MALLOC(ne.retained, plen);
        memcpy(ne.retained, payload, plen);
        ne.retained_size = plen;
        if (mlen > 0) {
            MALLOC(ne.retained_meta, mlen);
            memcpy(ne.retained_meta, meta, mlen);
            ne.retained_meta_size = mlen;
        } else {
            ne.retained_meta = NULL;
            ne.retained_meta_size = 0;
        }
        ne.retained_publisher = src;
        hashmap_set(ctx->retained_index, &ne);
        if (hashmap_oom(ctx->retained_index)) {
            FREE(ne.topic);
            FREE(ne.retained);
            FREE(ne.retained_meta);
            LOG_WARN("subcenter retained_index OOM");
        }
    }
}
// 从 cursor 起轮询挑一个活成员;死成员当场从 members 剔除,返回首个活成员(grab 保留,投递后由调用方 ungrab);
// 组内全死(members 清空)返 NULL。每轮非返回即剔一员,至多 members.size 次,必终止
static task_ctx *_sc_shared_pick_live(sc_ctx *ctx, sc_shared_group *g) {
    name_t cand;
    task_ctx *t;
    while (g->members.size > 0) {
        g->cursor = (g->cursor + 1) % g->members.size;
        cand = ((name_t *)g->members.ptr)[g->cursor];
        t = task_grab(ctx->loader, cand);
        if (NULL != t) {
            return t;
        }
        array_del_nomove(&g->members, (int32_t)g->cursor);
        if (g->cursor > 0) {
            g->cursor--;
        } else if (g->members.size > 0) {
            g->cursor = g->members.size - 1;
        }
    }
    return NULL;
}
// hashmap_scan 回调(shared_groups):每组挑首个活成员并 grab push 到 shared_dsts;组全死则标记待清理
static bool _sc_sg_pick_iter(const void *item, void *udata) {
    sc_collect_ctx *cc = (sc_collect_ctx *)udata;
    sc_shared_group *g = (sc_shared_group *)item;
    task_ctx *picked = _sc_shared_pick_live(cc->ctx, g);
    if (NULL == picked) {
        cc->shared_emptied = 1;
        return true;
    }
    // retained 不投共享组:组是工作队列语义,快照重复派发等于让 worker 多消费一次。
    // 但挑活这趟仍要走完——死成员剔除是它的副作用,只靠 retained 驱动的 topic 全指望这里
    if (0 != cc->retained) {
        cc->shared_skipped = 1;
        task_ungrab(picked);
        return true;
    }
    // 指定初始化:字段按对齐规则排过序,位置初始化会随重排静默错位
    sc_shared_dst sd = { .task = picked, .group = g->group,
                         .pattern = cc->cur_pattern, .ptlen = cc->cur_ptlen };
    array_push_back(cc->shared_dsts, &sd);
    return true;
}
// path_match visit 回调:normal_subs 全收到 publish_dedup hashset 去重,
// shared_groups 每个组挑首个活成员 grab 进 shared_dsts(组间允许重复)
static void _sc_collect_visit(void *payload, void *udata) {
    sc_collect_ctx *cc = (sc_collect_ctx *)udata;
    sc_topic_data *d = (sc_topic_data *)payload;
    // 普通订阅者:加入 hashset 去重
    name_t *subs = (name_t *)d->normal_subs.ptr;
    uint32_t i;
    for (i = 0; i < d->normal_subs.size; i++) {
        (void)hashset_add(cc->ctx->publish_dedup, &subs[i]);
        if (hashset_oom(cc->ctx->publish_dedup)) {
            cc->failed = 1;
            return;
        }
    }
    // 共享订阅:每组挑首个活成员(死成员当场剔除),允许重复:不同 group 之间不去重。
    // retained 收不收由 _sc_sg_pick_iter 判,这里不短路——短路会连死成员剔除一起跳过
    if (NULL == d->shared_groups
        || 0 == hashmap_count(d->shared_groups)) {
        return;
    }
    cc->cur_pattern = d->pattern;
    cc->cur_ptlen = (uint16_t)strlen(d->pattern);
    hashmap_scan(d->shared_groups, _sc_sg_pick_iter, cc);
}
// 把 name 经 task_grab 拿到 task_ctx 后塞入 dsts(已满返 FAILED);
// task_grab 失败的 name 收入 prune(后续懒清理 normal_subs);prune=NULL 表示不收集
static int32_t _sc_resolve_one(sc_ctx *ctx, name_t n, task_ctx **dsts, int32_t cap, int32_t *cnt,
                               array_ctx *prune) {
    if (*cnt >= cap) {
        return ERR_FAILED;
    }
    task_ctx *t = task_grab(ctx->loader, n);
    if (NULL == t) {
        if (NULL != prune) {
            array_push_back(prune, &n);
        }
        return ERR_OK;// 跳过
    }
    dsts[*cnt] = t;
    (*cnt)++;
    return ERR_OK;
}
// hashmap_scan 回调(shared_groups):从组移除 prune 中的死成员;组变空收集 group 名待删
static bool _sc_sg_prune_member_iter(const void *item, void *udata) {
    sc_shared_group *g = (sc_shared_group *)item;
    sc_sg_prune_ctx *sp = (sc_sg_prune_ctx *)udata;
    name_t *pn = (name_t *)sp->prune->ptr;
    int32_t idx;
    uint32_t i;
    for (i = 0; i < sp->prune->size; i++) {
        idx = _sc_name_find(&g->members, pn[i]);
        if (idx >= 0) {
            array_del_nomove(&g->members, idx);
        }
    }
    if (0 == g->members.size) {
        array_push_back(sp->empty_groups, &g->group);
    }
    return true;
}
// path_match visit：移除死订阅，空节点收入 empty_nodes 待后续 path_remove
static void _sc_prune_visit(void *payload, void *udata) {
    sc_topic_data *d = (sc_topic_data *)payload;
    sc_prune_ctx *pc = (sc_prune_ctx *)udata;
    name_t *pn = (name_t *)pc->prune->ptr;
    uint32_t i;
    for (i = 0; i < pc->prune->size; i++) {
        (void)_sc_normal_subs_remove(&d->normal_subs, pn[i]);
    }
    // 共享组:移除 prune 中的死成员,删空组,shared_groups 空则释放
    if (NULL != d->shared_groups) {
        array_ctx empty_groups;
        array_init(&empty_groups, sizeof(char *), 0);
        sc_sg_prune_ctx sp;
        sp.prune = pc->prune;
        sp.empty_groups = &empty_groups;
        hashmap_scan(d->shared_groups, _sc_sg_prune_member_iter, &sp);
        char **gnames = (char **)empty_groups.ptr;
        sc_shared_group qg;
        sc_shared_group *removed;
        for (i = 0; i < empty_groups.size; i++) {
            qg.group = gnames[i];
            removed = (sc_shared_group *)hashmap_delete(d->shared_groups, &qg);
            if (NULL != removed) {
                _sc_sg_free(removed);
            }
        }
        array_free(&empty_groups);
        if (0 == hashmap_count(d->shared_groups)) {
            hashmap_free(d->shared_groups);
            d->shared_groups = NULL;
        }
    }
    if (0 == d->normal_subs.size
        && (NULL == d->shared_groups || 0 == hashmap_count(d->shared_groups))) {
        array_push_back(pc->empty_nodes, &d->pattern);
    }
}
// publish 投递:fire-and-forget 投递到所有匹配订阅者
static void _sc_publish_deliver(sc_ctx *ctx, name_t src, const char *topic,
                                const void *payload, uint32_t plen, int32_t retained) {
    // 查 publisher 当前 meta
    sc_publisher_meta qm;
    qm.publisher = src;
    sc_publisher_meta *pm = (sc_publisher_meta *)hashmap_get(ctx->publisher_meta, &qm);
    const void *meta = (NULL != pm) ? pm->meta : NULL;
    uint16_t mlen = (NULL != pm) ? (uint16_t)pm->size : 0;
    // 收集订阅者:normal 进 dedup hashset 去重;shared 每组挑首个活成员并 grab,死成员当场剔除
    hashset_clear(ctx->publish_dedup, 1);
    array_ctx *shared_dsts = &ctx->pub_shared;
    array_clear(shared_dsts);
    sc_collect_ctx cc;
    cc.ctx = ctx;
    cc.shared_dsts = shared_dsts;
    cc.failed = 0;
    cc.shared_emptied = 0;
    cc.retained = retained;
    cc.shared_skipped = 0;
    cc.cur_pattern = NULL;
    cc.cur_ptlen = 0;
    path_match(ctx->topics, topic, _sc_collect_visit, &cc);
    uint32_t i;
    if (cc.failed) {
        // collect 中途失败:已 grab 的共享目标需 ungrab
        sc_shared_dst *sp = (sc_shared_dst *)shared_dsts->ptr;
        for (i = 0; i < shared_dsts->size; i++) {
            task_ungrab(sp[i].task);
        }
        return;
    }
    // 跳过的 retained 得让运维看得见；宽 pattern 的共享组会让每条 retained 都命中，
    // 故走翻倍告警：1/2/4/8… 次各报一遍，不刷屏也不哑掉
    if (0 != cc.shared_skipped) {
        ctx->retained_skipped++;
        if (tda_check(&ctx->retained_skip_tda, ctx->retained_skipped)) {
            LOG_WARN("subcenter: retained publish on '%s' skipped its shared subscription group(s), "
                     "%zu time(s) so far (shared subscribers never receive retained, see subcenter.h).",
                     topic, ctx->retained_skipped);
        }
    }
    size_t n_normal = hashset_count(ctx->publish_dedup);
    if (0 == n_normal && 0 == shared_dsts->size && 0 == cc.shared_emptied) {
        return;
    }
    // 普通组(kind=0)dedup 后逐个 grab,死订阅收入 prune_normal 懒清理(共享已在 collect 内挑活并剔死)
    task_ctx **normal_dsts = NULL;
    int32_t normal_cnt = 0;
    array_ctx *prune_normal = &ctx->pub_prune;
    array_clear(prune_normal);
    if (n_normal > 0) {
        if (ctx->pub_normal_cap < n_normal) {
            REALLOC(ctx->pub_normal, ctx->pub_normal, sizeof(task_ctx *) * n_normal);
            ctx->pub_normal_cap = (uint32_t)n_normal;
        }
        normal_dsts = ctx->pub_normal;
        size_t it = 0;
        void *item;
        while (hashset_iter(ctx->publish_dedup, &it, &item)) {
            _sc_resolve_one(ctx, *(name_t *)item, normal_dsts, (int32_t)n_normal, &normal_cnt, prune_normal);
        }
    }
    // 普通投递:group 空(glen=0),批量群发同一 buffer(task_multi_call copy=0 转移所有权)
    if (normal_cnt > 0) {
        size_t dsize = 0;
        char *dbuf = _sc_pack_deliver(SC_DELIVER_NORMAL, src, meta, mlen, NULL, 0, NULL, 0, topic, payload, plen, &dsize);
        task_multi_call(normal_dsts, normal_cnt, REQ_SC_DELIVER, dbuf, dsize, 0);
        int32_t k;
        for (k = 0; k < normal_cnt; k++) {
            task_ungrab(normal_dsts[k]);
        }
    }
    // 共享投递:每个挑中成员按各自 group 名单独打包单发,接收方据 group 精确路由
    sc_shared_dst *sds = (sc_shared_dst *)shared_dsts->ptr;
    size_t dsize = 0;
    char *dbuf;
    for (i = 0; i < shared_dsts->size; i++) {
        dbuf = _sc_pack_deliver(SC_DELIVER_SHARED, src, meta, mlen,
                                sds[i].group, (uint16_t)strlen(sds[i].group),
                                sds[i].pattern, sds[i].ptlen,
                                topic, payload, plen, &dsize);
        task_multi_call(&sds[i].task, 1, REQ_SC_DELIVER, dbuf, dsize, 0);
        task_ungrab(sds[i].task);
    }
    // 懒清理:死 normal 订阅 + collect 清空的共享组 + 随之变空的节点。死订阅/空组挂在命中的通配
    // /字面节点上,用 path_match 遍历所有命中节点统一处理;变空节点在返回后 path_remove。
    // 必须排在共享投递之后:sds[i].group / sds[i].pattern 分别指向 sc_shared_group.group 与
    // sc_topic_data.pattern,而这里的 _sc_sg_free / _sc_topic_data_free 正是释放它们的地方。
    // 放在前面的话有一条真实路径能踩中——同一个 task 既是某组唯一活成员又是普通订阅者时,
    // collect 的 task_grab 成功(记进 sds)、resolve 的第二次 task_grab 却因它正在关闭而失败,
    // 于是被当死订阅 prune 掉、组空、节点被判空移除,投递循环再去读那两个已释放的指针。
    // 清理本就是懒的,推迟到投递之后无任何副作用
    if (prune_normal->size > 0 || 0 != cc.shared_emptied) {
        array_ctx *empty_nodes = &ctx->pub_empty;
        array_clear(empty_nodes);
        sc_prune_ctx pc;
        pc.prune = prune_normal;
        pc.empty_nodes = empty_nodes;
        path_match(ctx->topics, topic, _sc_prune_visit, &pc);
        char **paths = (char **)empty_nodes->ptr;
        void *removed;
        for (i = 0; i < empty_nodes->size; i++) {
            removed = path_remove(ctx->topics, paths[i]);
            if (NULL != removed) {
                _sc_topic_data_free(removed);
            }
        }
    }
}
// handler:PUB(retained=0)/ PUB_RETAINED(retained=1)。
// retained 路径:先 _sc_update_retained 更新槽位;plen=0 时清空后直接返,不 deliver。
// deliver 路径:_sc_publish_deliver 收集订阅者 + task_multi_call 投递
static void _sc_handle_pub(sc_ctx *ctx, name_t src, uint64_t sess, binary_ctx *br, int32_t retained) {
    subtype_t reqtype = retained ? REQ_SC_PUB_RETAINED : REQ_SC_PUB;
    char topic[SC_TOPIC_MAX + 1];
    if (ERR_OK != _sc_read_cstr_max(br, topic, sizeof(topic), SC_TOPIC_MAX)) {
        _svpub_respond(ctx->loader, src, reqtype, sess, ERR_FAILED);
        return;
    }
    // publish/publish_retained topic 必须精确,拒绝含通配的 topic
    // (否则 retained 槽位可写但永远不会被 deliver,徒留垃圾)
    if (ERR_OK != path_validate(ctx->rules, topic, PATH_KIND_LITERAL)) {
        _svpub_respond(ctx->loader, src, reqtype, sess, ERR_FAILED);
        return;
    }
    if (!binary_have(br, 4)) {
        _svpub_respond(ctx->loader, src, reqtype, sess, ERR_FAILED);
        return;
    }
    uint32_t plen = (uint32_t)binary_get_uinteger(br, 4, 0);
    if (!binary_have(br, (size_t)plen)) {
        _svpub_respond(ctx->loader, src, reqtype, sess, ERR_FAILED);
        return;
    }
    const void *payload = (plen > 0) ? binary_get_binary(br, plen) : NULL;
    if (retained) {
        // 超长 retained 按头文件契约拒绝：返 ERR_FAILED、不 deliver 不存储，避免静默数据丢失
        if (plen > SC_RETAINED_MAX_SIZE) {
            LOG_WARN("subcenter retained too large: topic=%s size=%u", topic, plen);
            _svpub_respond(ctx->loader, src, reqtype, sess, ERR_FAILED);
            return;
        }
        _sc_update_retained(ctx, src, topic, payload, plen);
        if (0 == plen) {
            // 清空 retained 后不 deliver
            _svpub_respond(ctx->loader, src, reqtype, sess, ERR_OK);
            return;
        }
    }
    _sc_publish_deliver(ctx, src, topic, payload, plen, retained);
    _svpub_respond(ctx->loader, src, reqtype, sess, ERR_OK);
}
// handler:SET_META。mlen=0 删除 publisher_meta 条目(等价"清除");
// 否则 MALLOC + memcpy 覆盖现有 entry,或新建条目入 hashmap
// 摘掉所有已经死掉的 publisher 的 meta。key 是 name_t，publisher 若没在 _closing 里调
// set_meta(NULL, 0) 就死了，这条 entry 从此再没人碰得到（unsub 不碰它，投递侧只读不删）。
// 只在"新 publisher 首次登记 meta"那一步调用：那是这张表唯一的增长点，在增长点清理就能保证
// 表大小不会随时间单调上涨；publish 路径一次都不付（那是热路径，每次全扫 + 逐个 task_grab 太贵）
static void _sc_pm_prune(sc_ctx *ctx) {
    if (0 == hashmap_count(ctx->publisher_meta)) {
        return;
    }
    array_ctx dead;
    array_init(&dead, sizeof(name_t), 0);
    sc_pm_prune_ctx pp;
    pp.ctx = ctx;
    pp.dead = &dead;
    hashmap_scan(ctx->publisher_meta, _sc_pm_prune_iter, &pp);
    name_t *dn = (name_t *)dead.ptr;
    sc_publisher_meta q;
    sc_publisher_meta *removed;
    for (uint32_t i = 0; i < dead.size; i++) {
        q.publisher = dn[i];
        // hashmap_delete 只摘不释放，meta 缓冲要自己 free
        removed = (sc_publisher_meta *)hashmap_delete(ctx->publisher_meta, &q);
        if (NULL != removed) {
            _sc_pm_free(removed);
        }
    }
    array_free(&dead);
}
static void _sc_handle_set_meta(sc_ctx *ctx, name_t src, uint64_t sess, binary_ctx *br) {
    const char *meta;
    uint16_t mlen;
    if (ERR_OK != _sc_read_lp16_max(br, &meta, &mlen, SC_META_MAX_SIZE)) {
        _svpub_respond(ctx->loader, src, REQ_SC_SET_META, sess, ERR_FAILED);
        return;
    }
    sc_publisher_meta q;
    q.publisher = src;
    if (0 == mlen) {
        // hashmap_delete 返回 spare 副本但不自动调 elfree,需手动 _sc_pm_free 释放 meta
        sc_publisher_meta *removed = (sc_publisher_meta *)hashmap_delete(ctx->publisher_meta, &q);
        if (NULL != removed) {
            _sc_pm_free(removed);
        }
        _svpub_respond(ctx->loader, src, REQ_SC_SET_META, sess, ERR_OK);
        return;
    }
    sc_publisher_meta *e = (sc_publisher_meta *)hashmap_get(ctx->publisher_meta, &q);
    if (NULL != e) {
        FREE(e->meta);
        MALLOC(e->meta, mlen);
        memcpy(e->meta, meta, mlen);
        e->size = mlen;
    } else {
        // 新增条目 = 这张表唯一的增长点，先把死掉的摘掉再插
        _sc_pm_prune(ctx);
        sc_publisher_meta ne;
        ne.publisher = src;
        MALLOC(ne.meta, mlen);
        memcpy(ne.meta, meta, mlen);
        ne.size = mlen;
        hashmap_set(ctx->publisher_meta, &ne);
        if (hashmap_oom(ctx->publisher_meta)) {
            FREE(ne.meta);
            _svpub_respond(ctx->loader, src, REQ_SC_SET_META, sess, ERR_FAILED);
            return;
        }
    }
    _svpub_respond(ctx->loader, src, REQ_SC_SET_META, sess, ERR_OK);
}
// hashmap_iter 回调(retained_index):对匹配 pattern 的每条 retained 写入 wire buf,
// 达到 SC_QUERY_RETAINED_BURST_MAX 后 truncated=1 + 返 false 终止 scan
static bool _sc_qr_iter(const void *item, void *udata) {
    sc_qr_ctx *c = (sc_qr_ctx *)udata;
    if (c->pushed >= SC_QUERY_RETAINED_BURST_MAX) {
        c->truncated = 1;
        return false;
    }
    const sc_retained_entry *e = (const sc_retained_entry *)item;
    if (ERR_OK != path_matches_pattern(c->rules, e->topic, c->pattern)) {
        return true;
    }
    binary_set_uinteger(c->bw, (uint64_t)e->retained_publisher, sizeof(name_t), 0);
    binary_set_uinteger(c->bw, (uint64_t)e->retained_meta_size, 2, 0);
    if (e->retained_meta_size > 0 && NULL != e->retained_meta) {
        binary_set_binary(c->bw, (const char *)e->retained_meta, e->retained_meta_size);
    }
    size_t tlen = strlen(e->topic);
    binary_set_uinteger(c->bw, (uint64_t)tlen, 2, 0);
    binary_set_binary(c->bw, e->topic, tlen);
    binary_set_uinteger(c->bw, (uint64_t)e->retained_size, 4, 0);
    if (e->retained_size > 0 && NULL != e->retained) {
        binary_set_binary(c->bw, (const char *)e->retained, e->retained_size);
    }
    c->pushed++;
    return true;
}
// 把 scan 填好的 bw 回给 src:非空转移所有权(copy=0),空则回 NULL 并释放 bw
static void _sc_grab_respond(sc_ctx *ctx, name_t src, uint64_t sess, subtype_t reqtype, binary_ctx *bw) {
    task_ctx *t = task_grab(ctx->loader, src);
    if (NULL == t) {
        binary_free(bw);
        return;
    }
    if (bw->offset > 0) {
        task_response(t, reqtype, sess, ERR_OK, bw->data, bw->offset, 0);
    } else {
        task_response(t, reqtype, sess, ERR_OK, NULL, 0, 0);
        binary_free(bw);
    }
    task_ungrab(t);
}
// handler:QUERY_RETAINED。pattern 走 WILDCARD 校验后,
// hashmap_iter 扫 retained_index + path_matches_pattern 过滤,匹配项拼到 wire buf 一次性返回
static void _sc_handle_query_retained(sc_ctx *ctx, name_t src, uint64_t sess, binary_ctx *br) {
    char pattern[SC_TOPIC_MAX + 1];
    if (ERR_OK != _sc_read_cstr_max(br, pattern, sizeof(pattern), SC_TOPIC_MAX)) {
        _svpub_respond(ctx->loader, src, REQ_SC_QUERY_RETAINED, sess, ERR_FAILED);
        return;
    }
    if (ERR_OK != path_validate(ctx->rules, pattern, PATH_KIND_WILDCARD)) {
        _svpub_respond(ctx->loader, src, REQ_SC_QUERY_RETAINED, sess, ERR_FAILED);
        return;
    }
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    sc_qr_ctx qc;
    qc.bw = &bw;
    qc.rules = ctx->rules;
    qc.pattern = pattern;
    qc.pushed = 0;
    qc.truncated = 0;
    hashmap_scan(ctx->retained_index, _sc_qr_iter, &qc);
    if (qc.truncated) {
        LOG_WARN("query_retained pattern '%s' truncated at %d entries", pattern, qc.pushed);
    }
    _sc_grab_respond(ctx, src, sess, REQ_SC_QUERY_RETAINED, &bw);
}
// path_scan 回调:把每个 topic 节点的"topic + normal_count + shared_count"写入 binary_ctx
static void _sc_list_visit(const char *path, void *payload, void *udata) {
    binary_ctx *bw = (binary_ctx *)udata;
    sc_topic_data *d = (sc_topic_data *)payload;
    size_t tlen = strlen(path);
    binary_set_uinteger(bw, (uint64_t)tlen, 2, 0);
    binary_set_binary(bw, path, tlen);
    binary_set_uinteger(bw, (uint64_t)d->normal_subs.size, 4, 0);
    binary_set_uinteger(bw, (uint64_t)(NULL != d->shared_groups ? hashmap_count(d->shared_groups) : 0), 4, 0);
}
// handler:LIST。path_scan 全 trie 把每个 topic 的订阅信息(normal/shared count)写入 wire buf
static void _sc_handle_list(sc_ctx *ctx, name_t src, uint64_t sess) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    path_scan(ctx->topics, _sc_list_visit, &bw);
    _sc_grab_respond(ctx, src, sess, REQ_SC_LIST, &bw);
}
// hashmap_scan 回调:把每条 retained 的"topic + publisher + size + meta_size"写入 binary_ctx
// (不含 retained payload 自身,避免数据量大)
static bool _sc_retained_list_iter(const void *item, void *udata) {
    binary_ctx *bw = (binary_ctx *)udata;
    const sc_retained_entry *e = (const sc_retained_entry *)item;
    size_t tlen = strlen(e->topic);
    binary_set_uinteger(bw, (uint64_t)tlen, 2, 0);
    binary_set_binary(bw, e->topic, tlen);
    binary_set_uinteger(bw, (uint64_t)e->retained_publisher, sizeof(name_t), 0);
    binary_set_uinteger(bw, (uint64_t)e->retained_size, 4, 0);
    binary_set_uinteger(bw, (uint64_t)e->retained_meta_size, 2, 0);
    return true;
}
// handler:RETAINED_LIST。hashmap_scan 把每条 retained 的元信息写入 wire buf(不含 payload)
static void _sc_handle_retained_list(sc_ctx *ctx, name_t src, uint64_t sess) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    hashmap_scan(ctx->retained_index, _sc_retained_list_iter, &bw);
    _sc_grab_respond(ctx, src, sess, REQ_SC_RETAINED_LIST, &bw);
}
// 中心 dispatch:reqtype 直接标识子命令(请求不再带 op 字节),子分发到 10 个 handler
static void _sc_requested(task_ctx *task, subtype_t reqtype, uint64_t sess, name_t src,
                          void *data, size_t size) {
    sc_ctx *ctx = (sc_ctx *)task->arg;
    if (INVALID_TNAME == src) {
        return;
    }
    binary_ctx br;
    switch (reqtype) {
    case REQ_SC_SUB:
        if (NULL == data) {
            _svpub_respond(ctx->loader, src, reqtype, sess, ERR_FAILED);
            break;
        }
        binary_init(&br, (char *)data, size, 0);
        _sc_handle_sub(ctx, src, sess, &br, 0);
        break;
    case REQ_SC_SUB_SHARED:
        if (NULL == data) {
            _svpub_respond(ctx->loader, src, reqtype, sess, ERR_FAILED);
            break;
        }
        binary_init(&br, (char *)data, size, 0);
        _sc_handle_sub(ctx, src, sess, &br, 1);
        break;
    case REQ_SC_UNSUB:
        if (NULL == data) {
            _svpub_respond(ctx->loader, src, reqtype, sess, ERR_FAILED);
            break;
        }
        binary_init(&br, (char *)data, size, 0);
        _sc_handle_unsub(ctx, src, sess, &br, 0);
        break;
    case REQ_SC_UNSUB_SHARED:
        if (NULL == data) {
            _svpub_respond(ctx->loader, src, reqtype, sess, ERR_FAILED);
            break;
        }
        binary_init(&br, (char *)data, size, 0);
        _sc_handle_unsub(ctx, src, sess, &br, 1);
        break;
    case REQ_SC_PUB:
        if (NULL == data) {
            _svpub_respond(ctx->loader, src, reqtype, sess, ERR_FAILED);
            break;
        }
        binary_init(&br, (char *)data, size, 0);
        _sc_handle_pub(ctx, src, sess, &br, 0);
        break;
    case REQ_SC_PUB_RETAINED:
        if (NULL == data) {
            _svpub_respond(ctx->loader, src, reqtype, sess, ERR_FAILED);
            break;
        }
        binary_init(&br, (char *)data, size, 0);
        _sc_handle_pub(ctx, src, sess, &br, 1);
        break;
    case REQ_SC_LIST:
        _sc_handle_list(ctx, src, sess);
        break;
    case REQ_SC_QUERY_RETAINED:
        if (NULL == data) {
            _svpub_respond(ctx->loader, src, reqtype, sess, ERR_FAILED);
            break;
        }
        binary_init(&br, (char *)data, size, 0);
        _sc_handle_query_retained(ctx, src, sess, &br);
        break;
    case REQ_SC_SET_META:
        if (NULL == data) {
            _svpub_respond(ctx->loader, src, reqtype, sess, ERR_FAILED);
            break;
        }
        binary_init(&br, (char *)data, size, 0);
        _sc_handle_set_meta(ctx, src, sess, &br);
        break;
    case REQ_SC_RETAINED_LIST:
        _sc_handle_retained_list(ctx, src, sess);
        break;
    default:
        _svpub_respond(ctx->loader, src, reqtype, sess, ERR_FAILED);
        break;
    }
}
// 释放 subcenter task 关联资源
static void _sc_free(void *arg) {
    if (NULL == arg) {
        return;
    }
    sc_ctx *ctx = (sc_ctx *)arg;
    if (NULL != ctx->publish_dedup) {
        hashset_free(ctx->publish_dedup);
    }
    if (NULL != ctx->retained_index) {
        hashmap_free(ctx->retained_index);
    }
    if (NULL != ctx->publisher_meta) {
        hashmap_free(ctx->publisher_meta);
    }
    if (NULL != ctx->topics) {
        path_free(ctx->topics);
    }
    array_free(&ctx->pub_shared);
    array_free(&ctx->pub_prune);
    array_free(&ctx->pub_empty);
    FREE(ctx->pub_normal);
    FREE(ctx);
}
int32_t sc_start(loader_ctx *loader, const char *name, const path_rules *rules) {
    if (EMPTYSTR(name)) {
        return ERR_OK;
    }
    if (NULL == rules) {
        return ERR_FAILED;
    }
    sc_ctx *ctx;
    CALLOC(ctx, 1, sizeof(sc_ctx));
    ctx->loader = loader;
    ctx->rules = rules;
    tda_init(&ctx->retained_skip_tda, 1);// 首次就报，之后 2/4/8… 次各报一遍
    ctx->topics = path_new(rules, _sc_topic_data_free);
    if (NULL == ctx->topics) {
        FREE(ctx);
        return ERR_FAILED;
    }
    ctx->publisher_meta = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                                     sizeof(sc_publisher_meta), ONEK, 0, 0,
                                                     _sc_pm_hash, _sc_pm_cmp, _sc_pm_free, NULL);
    ctx->retained_index = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                                     sizeof(sc_retained_entry), ONEK, 0, 0,
                                                     _sc_re_hash, _sc_re_cmp, _sc_re_free, NULL);
    ctx->publish_dedup = hashset_new(sizeof(name_t), 64, _sc_name_hash, _sc_name_cmp, NULL, NULL);
    if (NULL == ctx->publisher_meta
        || NULL == ctx->retained_index
        || NULL == ctx->publish_dedup) {
        _sc_free(ctx);
        return ERR_FAILED;
    }
    array_init(&ctx->pub_shared, sizeof(sc_shared_dst), 0);
    array_init(&ctx->pub_prune, sizeof(name_t), 0);
    array_init(&ctx->pub_empty, sizeof(char *), 0);
    task_ctx *task = task_new(loader, name, 4 * ONEK, NULL, _sc_free, ctx);
    task_requested(task, _sc_requested);
    if (ERR_OK != task_register(task, NULL, NULL)) {
        // task_register 失败时 task 未进 maptasks,需手动 task_free;
        task_free(task);
        return ERR_FAILED;
    }
    return ERR_OK;
}
// 客户端入口的 topic / group 校验。线格式的长度前缀只有 2 字节，binary_set_uinteger 走
// pack_integer 逐字节写、超出部分静默丢掉：65541 字节的 topic 前缀会写成 5，服务端照着建了个
// 5 字符的节点还回 ERR_OK——调用方订阅到了另一个 topic，却被告知成功。上界取值与服务端
// _sc_read_lp16_max 一致：SC_TOPIC_MAX / SC_GROUP_MAX 是长度上限本身，等于它仍合法。
// 与 datacenter 的 DC_KEY_MAX 相反——那个是缓冲容量(含 NUL)，所以它那边判的是 <
static int32_t _sc_check_topic(const char *topic) {
    return (!EMPTYSTR(topic) && strlen(topic) <= SC_TOPIC_MAX) ? ERR_OK : ERR_FAILED;
}
static int32_t _sc_check_group(const char *group) {
    return (!EMPTYSTR(group) && strlen(group) <= SC_GROUP_MAX) ? ERR_OK : ERR_FAILED;
}
// SUB/UNSUB/QUERY_RETAINED body(不带 op): u16 tlen | topic
static char *_sc_pack_topic(const char *topic, size_t *out_total) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    size_t tlen = strlen(topic);
    binary_set_uinteger(&bw, (uint64_t)tlen, 2, 0);
    binary_set_binary(&bw, topic, tlen);
    *out_total = bw.offset;
    return bw.data;
}
// SUB_SHARED/UNSUB_SHARED body: u16 tlen | topic | u16 glen | group
static char *_sc_pack_topic_group(const char *topic, const char *group, size_t *out_total) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    size_t tlen = strlen(topic);
    binary_set_uinteger(&bw, (uint64_t)tlen, 2, 0);
    binary_set_binary(&bw, topic, tlen);
    size_t glen = strlen(group);
    binary_set_uinteger(&bw, (uint64_t)glen, 2, 0);
    binary_set_binary(&bw, group, glen);
    *out_total = bw.offset;
    return bw.data;
}
// PUB/PUB_RETAINED body: u16 tlen | topic | u32 plen | payload
static char *_sc_pack_topic_payload(const char *topic, const void *payload, size_t plen,
                                    size_t *out_total) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    size_t tlen = strlen(topic);
    binary_set_uinteger(&bw, (uint64_t)tlen, 2, 0);
    binary_set_binary(&bw, topic, tlen);
    size_t real_plen = (NULL != payload) ? plen : 0;
    binary_set_uinteger(&bw, (uint64_t)real_plen, 4, 0);
    if (real_plen > 0) {
        binary_set_binary(&bw, (const char *)payload, real_plen);
    }
    *out_total = bw.offset;
    return bw.data;
}
// SET_META body: u16 mlen | meta
static char *_sc_pack_meta(const void *meta, size_t mlen, size_t *out_total) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    size_t real_mlen = (NULL != meta) ? mlen : 0;
    binary_set_uinteger(&bw, (uint64_t)real_mlen, 2, 0);
    if (real_mlen > 0) {
        binary_set_binary(&bw, (const char *)meta, real_mlen);
    }
    *out_total = bw.offset;
    return bw.data;
}
// ── 业务侧 helper:协程版,内部包 coro_request 挂起等响应 ──
// 每条命令的协程版与非协程版只差投递方式:前者挂起等响应,后者由业务按 sess 自管配对。
// 校验、组包、REQ 常量三者的搭配放在这四个按"入参形状"分的实现里,免得两个入口各写一遍——
// 漏改一边就成了"一条路径拒、另一条放行"。
// coro 非 0 走 _svpub_call(不用 sess);为 0 走 _svpub_send,且要求 sess 非 0:
// subcenter 每条命令都要回执,不接受 sess=0 那种 fire-and-forget
static int32_t _sc_cmd_topic(task_ctx *task, name_t sc_name, subtype_t req,
                             uint64_t sess, int32_t coro, const char *topic) {
    if (ERR_OK != _sc_check_topic(topic)
        || (0 == coro && 0 == sess)) {
        return ERR_FAILED;
    }
    size_t total;
    char *buf = _sc_pack_topic(topic, &total);
    return (0 != coro) ? _svpub_call(task, sc_name, req, buf, total)
                       : _svpub_send(task, sc_name, req, sess, buf, total);
}
static int32_t _sc_cmd_topic_group(task_ctx *task, name_t sc_name, subtype_t req,
                                   uint64_t sess, int32_t coro,
                                   const char *topic, const char *group) {
    if (ERR_OK != _sc_check_topic(topic)
        || ERR_OK != _sc_check_group(group)
        || (0 == coro && 0 == sess)) {
        return ERR_FAILED;
    }
    size_t total;
    char *buf = _sc_pack_topic_group(topic, group, &total);
    return (0 != coro) ? _svpub_call(task, sc_name, req, buf, total)
                       : _svpub_send(task, sc_name, req, sess, buf, total);
}
// 与另外三个不同,这里先 grab 再组包:publish 的载荷可以很大,组包就是整块拷一遍,
// 目标不在时那份拷贝纯属白做(理由见 sv_pub.h 的 _svpub_*_dst)
static int32_t _sc_cmd_topic_payload(task_ctx *task, name_t sc_name, subtype_t req,
                                     uint64_t sess, int32_t coro,
                                     const char *topic, void *data, size_t size) {
    if (ERR_OK != _sc_check_topic(topic)
        || size > UINT32_MAX
        || (0 == coro && 0 == sess)) {
        return ERR_FAILED;
    }
    task_ctx *dst = task_grab(task->loader, sc_name);
    if (NULL == dst) {
        return ERR_FAILED;
    }
    size_t total;
    char *buf = _sc_pack_topic_payload(topic, data, size, &total);
    return (0 != coro) ? _svpub_call_dst(dst, task, req, buf, total)
                       : _svpub_send_dst(dst, task, req, sess, buf, total);
}
static int32_t _sc_cmd_meta(task_ctx *task, name_t sc_name, subtype_t req,
                            uint64_t sess, int32_t coro,
                            const void *meta, size_t size) {
    if (size > SC_META_MAX_SIZE
        || (0 == coro && 0 == sess)) {
        return ERR_FAILED;
    }
    size_t total;
    char *buf = _sc_pack_meta(meta, size, &total);
    return (0 != coro) ? _svpub_call(task, sc_name, req, buf, total)
                       : _svpub_send(task, sc_name, req, sess, buf, total);
}
int32_t coro_sc_subscribe(task_ctx *task, name_t sc_name, const char *topic) {
    return _sc_cmd_topic(task, sc_name, REQ_SC_SUB, 0, 1, topic);
}
int32_t coro_sc_subscribe_shared(task_ctx *task, name_t sc_name,
                                 const char *topic, const char *group) {
    return _sc_cmd_topic_group(task, sc_name, REQ_SC_SUB_SHARED, 0, 1, topic, group);
}
int32_t coro_sc_unsubscribe(task_ctx *task, name_t sc_name, const char *topic) {
    return _sc_cmd_topic(task, sc_name, REQ_SC_UNSUB, 0, 1, topic);
}
int32_t coro_sc_unsubscribe_shared(task_ctx *task, name_t sc_name,
                                   const char *topic, const char *group) {
    return _sc_cmd_topic_group(task, sc_name, REQ_SC_UNSUB_SHARED, 0, 1, topic, group);
}
int32_t coro_sc_publish(task_ctx *task, name_t sc_name, const char *topic,
                        void *data, size_t size) {
    return _sc_cmd_topic_payload(task, sc_name, REQ_SC_PUB, 0, 1, topic, data, size);
}
int32_t coro_sc_publish_retained(task_ctx *task, name_t sc_name, const char *topic,
                                 void *data, size_t size) {
    return _sc_cmd_topic_payload(task, sc_name, REQ_SC_PUB_RETAINED, 0, 1, topic, data, size);
}
// 协程版比非协程版多取一段响应,返回类型也不同,无法与 _sc_cmd_topic 共用
void *coro_sc_query_retained(task_ctx *task, name_t sc_name, const char *pattern,
                             size_t *size, int32_t *erro) {
    if (ERR_OK != _sc_check_topic(pattern)) {
        SET_PTR(size, 0);
        *erro = ERR_FAILED;
        return NULL;
    }
    size_t total;
    char *buf = _sc_pack_topic(pattern, &total);
    return _svpub_call_resp(task, sc_name, REQ_SC_QUERY_RETAINED, buf, total, size, erro);
}
void *coro_sc_topics(task_ctx *task, name_t sc_name,
                     size_t *size, int32_t *erro) {
    return _svpub_call_resp(task, sc_name, REQ_SC_LIST, NULL, 0, size, erro);
}
void *coro_sc_retained_topics(task_ctx *task, name_t sc_name,
                              size_t *size, int32_t *erro) {
    return _svpub_call_resp(task, sc_name, REQ_SC_RETAINED_LIST, NULL, 0, size, erro);
}
int32_t coro_sc_set_meta(task_ctx *task, name_t sc_name,
                         const void *meta, size_t size) {
    return _sc_cmd_meta(task, sc_name, REQ_SC_SET_META, 0, 1, meta, size);
}
// ── 无协程版:task_request 不挂起,sess 由业务自管配对 ──
int32_t sc_subscribe(task_ctx *task, name_t sc_name, uint64_t sess, const char *topic) {
    return _sc_cmd_topic(task, sc_name, REQ_SC_SUB, sess, 0, topic);
}
int32_t sc_subscribe_shared(task_ctx *task, name_t sc_name, uint64_t sess,
                            const char *topic, const char *group) {
    return _sc_cmd_topic_group(task, sc_name, REQ_SC_SUB_SHARED, sess, 0, topic, group);
}
int32_t sc_unsubscribe(task_ctx *task, name_t sc_name, uint64_t sess, const char *topic) {
    return _sc_cmd_topic(task, sc_name, REQ_SC_UNSUB, sess, 0, topic);
}
int32_t sc_unsubscribe_shared(task_ctx *task, name_t sc_name, uint64_t sess,
                              const char *topic, const char *group) {
    return _sc_cmd_topic_group(task, sc_name, REQ_SC_UNSUB_SHARED, sess, 0, topic, group);
}
int32_t sc_publish(task_ctx *task, name_t sc_name, uint64_t sess, const char *topic,
                   void *data, size_t size) {
    return _sc_cmd_topic_payload(task, sc_name, REQ_SC_PUB, sess, 0, topic, data, size);
}
int32_t sc_publish_retained(task_ctx *task, name_t sc_name, uint64_t sess,
                            const char *topic, void *data, size_t size) {
    return _sc_cmd_topic_payload(task, sc_name, REQ_SC_PUB_RETAINED, sess, 0, topic, data, size);
}
int32_t sc_query_retained(task_ctx *task, name_t sc_name, uint64_t sess, const char *pattern) {
    return _sc_cmd_topic(task, sc_name, REQ_SC_QUERY_RETAINED, sess, 0, pattern);
}
int32_t sc_topics(task_ctx *task, name_t sc_name, uint64_t sess) {
    if (0 == sess) {
        return ERR_FAILED;
    }
    return _svpub_send(task, sc_name, REQ_SC_LIST, sess, NULL, 0);
}
int32_t sc_retained_topics(task_ctx *task, name_t sc_name, uint64_t sess) {
    if (0 == sess) {
        return ERR_FAILED;
    }
    return _svpub_send(task, sc_name, REQ_SC_RETAINED_LIST, sess, NULL, 0);
}
int32_t sc_set_meta(task_ctx *task, name_t sc_name, uint64_t sess,
                    const void *meta, size_t size) {
    return _sc_cmd_meta(task, sc_name, REQ_SC_SET_META, sess, 0, meta, size);
}
