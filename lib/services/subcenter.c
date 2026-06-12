#include "services/subcenter.h"
#include "srey/task.h"
#include "containers/hashmap.h"
#include "containers/hashset.h"
#include "path/path_trie.h"
#include "containers/sarray.h"
#include "utils/binary.h"
#include "utils/utils.h"

// ── subcenter 限制(业务特定上限,可按部署需要调整后重编) ────────────
#define SC_RETAINED_MAX_SIZE        (1 * 1024 * 1024)  // 单 topic retained 上限 1MB
#define SC_META_MAX_SIZE            1024                // publisher meta 上限 1KB
#define SC_TOPIC_MAX                256                 // topic 字符串最大长度
#define SC_GROUP_MAX                64                  // group 名最大长度
#define SC_SUB_WARN_THRESHOLD       1000                // 单 topic 订阅者超过此值 LOG_WARN
#define SC_QUERY_RETAINED_BURST_MAX 1000                // query_retained 单次上限,超过截断 + WARN

// 子命令操作码:payload 首字节,跟随特定子命令的剩余字节
typedef enum sc_op {
    SC_OP_SUB              = 0x01,  // u16 tlen + topic
    SC_OP_SUB_SHARED       = 0x02,  // u16 tlen + topic + u16 glen + group
    SC_OP_UNSUB            = 0x03,  // u16 tlen + topic
    SC_OP_UNSUB_SHARED     = 0x04,  // u16 tlen + topic + u16 glen + group
    SC_OP_PUB              = 0x05,  // u16 tlen + topic + u32 plen + payload
    SC_OP_PUB_RETAINED     = 0x06,  // u16 tlen + topic + u32 plen + payload
    SC_OP_LIST             = 0x07,  // no body
    SC_OP_QUERY_RETAINED   = 0x08,  // u16 plen + pattern
    SC_OP_SET_META         = 0x09,  // u16 mlen + meta
    SC_OP_RETAINED_LIST    = 0x0A,  // no body
}sc_op;
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
    loader_ctx *loader;              // 所属 loader
    path_trie *topics;               // 订阅关系
    const path_rules *rules;         // topic 规则(由 sc_start 传入,长生命周期持有)
    struct hashmap *publisher_meta;  // name_t → sc_publisher_meta
    struct hashmap *retained_index;  // topic → sc_retained_entry
    hashset *publish_dedup;          // publish 去重复用容器(name_t set)
}sc_ctx;
// path_match 的 visit:收集订阅者
// 共享投递目标:挑中的成员 + 其所属组名(打 deliver wire 需 group;指向 sc_shared_group.group,投递期有效)
typedef struct sc_shared_dst {
    task_ctx *task;
    const char *group;
}sc_shared_dst;
typedef struct sc_collect_ctx {
    int32_t failed;               // 内存分配失败标志
    int32_t shared_emptied;       // 有共享组在 pick 时被清空 → 触发清理 pass
    sc_ctx *ctx;                  // subcenter 上下文
    array_ctx *shared_dsts;       // 共享组挑选结果(元素 sc_shared_dst,已 grab,投递后 ungrab)
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
    MALLOC(d->pattern, plen + 1);
    safe_fill_str(d->pattern, plen + 1, pattern);
    array_init(&d->normal_subs, sizeof(name_t), 0);
    return d;
}
static void _sc_topic_data_free(void *p) {
    sc_topic_data *d = (sc_topic_data *)p;
    array_free(&d->normal_subs);
    if (NULL != d->shared_groups) {
        hashmap_free(d->shared_groups);
    }
    FREE(d->pattern);
    FREE(d);
}
// 失败响应:统一回 ERR_FAILED
static void _sc_resp_failed(sc_ctx *ctx, name_t src, uint64_t sess) {
    if (INVALID_TNAME == src || 0 == sess) {
        return;
    }
    task_ctx *t = task_grab(ctx->loader, src);
    if (NULL != t) {
        task_response(t, sess, ERR_FAILED, NULL, 0, 0);
        task_ungrab(t);
    }
}
// 成功响应
static void _sc_resp_ok(sc_ctx *ctx, name_t src, uint64_t sess) {
    if (INVALID_TNAME == src || 0 == sess) {
        return;
    }
    task_ctx *t = task_grab(ctx->loader, src);
    if (NULL != t) {
        task_response(t, sess, ERR_OK, NULL, 0, 0);
        task_ungrab(t);
    }
}
// 从 binary_ctx 读 | u16 len | bytes |;成功返指针 + 长度
static int32_t _sc_read_lp16(binary_ctx *br, const char **out_data, uint16_t *out_len) {
    if (br->size - br->offset < 2) {
        return ERR_FAILED;
    }
    uint16_t n = (uint16_t)binary_get_uinteger(br, 2, 0);
    if (br->size - br->offset < (size_t)n) {
        return ERR_FAILED;
    }
    *out_len = n;
    *out_data = (n > 0) ? binary_get_string(br, n) : NULL;
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
// 从 br 读取一个长度上限受限的 NUL 终结字符串(拷贝到 dst)
static int32_t _sc_read_cstr_max(binary_ctx *br, char *dst, size_t dst_cap, uint16_t max_len) {
    const char *p;
    uint16_t n;
    if (ERR_OK != _sc_read_lp16_max(br, &p, &n, max_len)) {
        return ERR_FAILED;
    }
    if (0 == n || n + 1u > dst_cap) {
        return ERR_FAILED;
    }
    memcpy(dst, p, n);
    dst[n] = '\0';
    return ERR_OK;
}
// 构造 deliver wire:| u8 kind | name_t publisher | u16 mlen | meta | u16 glen | group | u16 tlen | topic | u32 plen | payload |
// group 仅共享投递(kind=SHARED)非空;普通投递 glen=0
static char *_sc_pack_deliver(uint8_t kind, name_t publisher,
                              const void *meta, uint16_t mlen,
                              const char *group, uint16_t glen,
                              const char *topic,
                              const void *payload, uint32_t plen,
                              size_t *out_total) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    binary_set_uint8(&bw, kind);
    binary_set_uinteger(&bw, (uint64_t)publisher, sizeof(name_t), 0);
    binary_set_uinteger(&bw, (uint64_t)mlen, 2, 0);
    if (mlen > 0 && NULL != meta) {
        binary_set_string(&bw, (const char *)meta, mlen);
    }
    binary_set_uinteger(&bw, (uint64_t)glen, 2, 0);
    if (glen > 0 && NULL != group) {
        binary_set_string(&bw, group, glen);
    }
    size_t tlen = strlen(topic);
    binary_set_uinteger(&bw, (uint64_t)tlen, 2, 0);
    binary_set_string(&bw, topic, tlen);
    binary_set_uinteger(&bw, (uint64_t)plen, 4, 0);
    if (plen > 0 && NULL != payload) {
        binary_set_string(&bw, (const char *)payload, plen);
    }
    *out_total = bw.offset;
    return bw.data;
}
int32_t sc_parse_deliver(const void *data, size_t size, sc_deliver *out) {
    const char *p = (const char *)data;
    size_t off = 0;
    // kind(u8) + publisher(name_t) + mlen(u16)
    if (off + 1 + sizeof(name_t) + 2 > size) {
        return ERR_FAILED;
    }
    out->kind = (int32_t)(uint8_t)p[off];
    off += 1;
    out->publisher = (name_t)unpack_integer(p + off, (int32_t)sizeof(name_t), 0, 0);
    off += sizeof(name_t);
    out->mlen = (size_t)unpack_integer(p + off, 2, 0, 0);
    off += 2;
    // meta(mlen) + glen(u16)
    if (off + out->mlen + 2 > size) {
        return ERR_FAILED;
    }
    out->meta = out->mlen > 0 ? p + off : NULL;
    off += out->mlen;
    out->glen = (size_t)unpack_integer(p + off, 2, 0, 0);
    off += 2;
    // group(glen) + tlen(u16)
    if (off + out->glen + 2 > size) {
        return ERR_FAILED;
    }
    out->group = out->glen > 0 ? p + off : NULL;
    off += out->glen;
    out->tlen = (size_t)unpack_integer(p + off, 2, 0, 0);
    off += 2;
    // topic(tlen) + plen(u32)
    if (off + out->tlen + 4 > size) {
        return ERR_FAILED;
    }
    out->topic = out->tlen > 0 ? p + off : NULL;
    off += out->tlen;
    out->plen = (size_t)unpack_integer(p + off, 4, 0, 0);
    off += 4;
    // payload(plen)
    if (off + out->plen > size) {
        return ERR_FAILED;
    }
    out->payload = out->plen > 0 ? p + off : NULL;
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
        return 0;   // 已存在
    }
    array_push_back(subs, &src);
    return 1;       // 新增
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
    char topic[SC_TOPIC_MAX + 1];
    if (ERR_OK != _sc_read_cstr_max(br, topic, sizeof(topic), SC_TOPIC_MAX)) {
        _sc_resp_failed(ctx, src, sess);
        return;
    }
    char group[SC_GROUP_MAX + 1];
    if (shared) {
        if (ERR_OK != _sc_read_cstr_max(br, group, sizeof(group), SC_GROUP_MAX)) {
            _sc_resp_failed(ctx, src, sess);
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
            _sc_resp_failed(ctx, src, sess);
            return;
        }
    }
    if (shared) {
        if (NULL == d->shared_groups) {
            d->shared_groups = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                                           sizeof(sc_shared_group), 4, 0, 0,
                                                           _sc_sg_hash, _sc_sg_cmp, _sc_sg_free, NULL);
            if (NULL == d->shared_groups) {
                _sc_resp_failed(ctx, src, sess);
                return;
            }
        }
        sc_shared_group qg;
        qg.group = group;
        sc_shared_group *g = (sc_shared_group *)hashmap_get(d->shared_groups, &qg);
        if (NULL == g) {
            sc_shared_group ng;
            size_t glen = strlen(group);
            MALLOC(ng.group, glen + 1);
            safe_fill_str(ng.group, glen + 1, group);
            array_init(&ng.members, sizeof(name_t), 0);
            ng.cursor = 0;
            hashmap_set(d->shared_groups, &ng);
            if (hashmap_oom(d->shared_groups)) {
                FREE(ng.group);
                array_free(&ng.members);
                _sc_resp_failed(ctx, src, sess);
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
    _sc_resp_ok(ctx, src, sess);
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
    char topic[SC_TOPIC_MAX + 1];
    if (ERR_OK != _sc_read_cstr_max(br, topic, sizeof(topic), SC_TOPIC_MAX)) {
        _sc_resp_failed(ctx, src, sess);
        return;
    }
    char group[SC_GROUP_MAX + 1];
    if (shared) {
        if (ERR_OK != _sc_read_cstr_max(br, group, sizeof(group), SC_GROUP_MAX)) {
            _sc_resp_failed(ctx, src, sess);
            return;
        }
    }
    sc_topic_data *d = (sc_topic_data *)path_get(ctx->topics, topic);
    if (NULL == d) {
        _sc_resp_ok(ctx, src, sess);    // 幂等
        return;
    }
    if (shared) {
        if (NULL == d->shared_groups) {
            _sc_resp_ok(ctx, src, sess);
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
    _sc_resp_ok(ctx, src, sess);
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
        MALLOC(ne.topic, tlen + 1);
        safe_fill_str(ne.topic, tlen + 1, topic);
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
    while (g->members.size > 0) {
        g->cursor = (g->cursor + 1) % g->members.size;
        name_t cand = ((name_t *)g->members.ptr)[g->cursor];
        task_ctx *t = task_grab(ctx->loader, cand);
        if (NULL != t) {
            return t;
        }
        array_del_nomove(&g->members, (int32_t)g->cursor);
    }
    return NULL;
}
// hashmap_scan 回调(shared_groups):每组挑首个活成员并 grab push 到 shared_dsts;组全死则标记待清理
static bool _sc_sg_pick_iter(const void *item, void *udata) {
    sc_collect_ctx *cc = (sc_collect_ctx *)udata;
    sc_shared_group *g = (sc_shared_group *)item;
    task_ctx *picked = _sc_shared_pick_live(cc->ctx, g);
    if (NULL != picked) {
        sc_shared_dst sd = { picked, g->group };
        array_push_back(cc->shared_dsts, &sd);
    } else {
        cc->shared_emptied = 1;
    }
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
        if (hashset_add(cc->ctx->publish_dedup, &subs[i]) < 0) {
            cc->failed = 1;
            return;
        }
    }
    // 共享订阅:每组挑首个活成员(死成员当场剔除),允许重复:不同 group 之间不去重
    if (NULL != d->shared_groups) {
        hashmap_scan(d->shared_groups, _sc_sg_pick_iter, cc);
    }
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
        return ERR_OK;   // 跳过
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
                                const void *payload, uint32_t plen) {
    // 查 publisher 当前 meta
    sc_publisher_meta qm;
    qm.publisher = src;
    sc_publisher_meta *pm = (sc_publisher_meta *)hashmap_get(ctx->publisher_meta, &qm);
    const void *meta = (NULL != pm) ? pm->meta : NULL;
    uint16_t mlen = (NULL != pm) ? (uint16_t)pm->size : 0;
    // 收集订阅者:normal 进 dedup hashset 去重;shared 每组挑首个活成员并 grab,死成员当场剔除
    hashset_clear(ctx->publish_dedup, 0);
    array_ctx shared_dsts;
    array_init(&shared_dsts, sizeof(sc_shared_dst), 0);
    sc_collect_ctx cc;
    cc.ctx = ctx;
    cc.shared_dsts = &shared_dsts;
    cc.failed = 0;
    cc.shared_emptied = 0;
    path_match(ctx->topics, topic, _sc_collect_visit, &cc);
    uint32_t i;
    if (cc.failed) {
        // collect 中途失败:已 grab 的共享目标需 ungrab
        sc_shared_dst *sp = (sc_shared_dst *)shared_dsts.ptr;
        for (i = 0; i < shared_dsts.size; i++) {
            task_ungrab(sp[i].task);
        }
        array_free(&shared_dsts);
        return;
    }
    size_t n_normal = hashset_count(ctx->publish_dedup);
    if (0 == n_normal && 0 == shared_dsts.size && 0 == cc.shared_emptied) {
        array_free(&shared_dsts);
        return;
    }
    // 普通组(kind=0)dedup 后逐个 grab,死订阅收入 prune_normal 懒清理(共享已在 collect 内挑活并剔死)
    task_ctx **normal_dsts = NULL;
    int32_t normal_cnt = 0;
    array_ctx prune_normal;
    array_init(&prune_normal, sizeof(name_t), 0);
    if (n_normal > 0) {
        MALLOC(normal_dsts, sizeof(task_ctx *) * n_normal);
        size_t it = 0;
        void *item;
        while (hashset_iter(ctx->publish_dedup, &it, &item)) {
            _sc_resolve_one(ctx, *(name_t *)item, normal_dsts, (int32_t)n_normal, &normal_cnt, &prune_normal);
        }
    }
    // 懒清理:死 normal 订阅 + collect 清空的共享组 + 随之变空的节点。死订阅/空组挂在命中的通配
    // /字面节点上,用 path_match 遍历所有命中节点统一处理;变空节点在返回后 path_remove
    if (prune_normal.size > 0 || 0 != cc.shared_emptied) {
        array_ctx empty_nodes;
        array_init(&empty_nodes, sizeof(char *), 0);
        sc_prune_ctx pc;
        pc.prune = &prune_normal;
        pc.empty_nodes = &empty_nodes;
        path_match(ctx->topics, topic, _sc_prune_visit, &pc);
        char **paths = (char **)empty_nodes.ptr;
        void *removed;
        for (i = 0; i < empty_nodes.size; i++) {
            removed = path_remove(ctx->topics, paths[i]);
            if (NULL != removed) {
                _sc_topic_data_free(removed);
            }
        }
        array_free(&empty_nodes);
    }
    array_free(&prune_normal);
    // 普通投递:group 空(glen=0),批量群发同一 buffer(task_multi_call copy=0 转移所有权)
    if (normal_cnt > 0) {
        size_t dsize = 0;
        char *dbuf = _sc_pack_deliver(SC_DELIVER_NORMAL, src, meta, mlen, NULL, 0, topic, payload, plen, &dsize);
        task_multi_call(normal_dsts, normal_cnt, REQ_SC_DELIVER, dbuf, dsize, 0);
        int32_t k;
        for (k = 0; k < normal_cnt; k++) {
            task_ungrab(normal_dsts[k]);
        }
    }
    // 共享投递:每个挑中成员按各自 group 名单独打包单发,接收方据 group 精确路由
    sc_shared_dst *sds = (sc_shared_dst *)shared_dsts.ptr;
    for (i = 0; i < shared_dsts.size; i++) {
        size_t dsize = 0;
        char *dbuf = _sc_pack_deliver(SC_DELIVER_SHARED, src, meta, mlen,
                                      sds[i].group, (uint16_t)strlen(sds[i].group),
                                      topic, payload, plen, &dsize);
        task_multi_call(&sds[i].task, 1, REQ_SC_DELIVER, dbuf, dsize, 0);
        task_ungrab(sds[i].task);
    }
    FREE(normal_dsts);
    array_free(&shared_dsts);
}
// handler:PUB(retained=0)/ PUB_RETAINED(retained=1)。
// retained 路径:先 _sc_update_retained 更新槽位;plen=0 时清空后直接返,不 deliver。
// deliver 路径:_sc_publish_deliver 收集订阅者 + task_multi_call 投递
static void _sc_handle_pub(sc_ctx *ctx, name_t src, uint64_t sess, binary_ctx *br, int32_t retained) {
    char topic[SC_TOPIC_MAX + 1];
    if (ERR_OK != _sc_read_cstr_max(br, topic, sizeof(topic), SC_TOPIC_MAX)) {
        _sc_resp_failed(ctx, src, sess);
        return;
    }
    // publish/publish_retained topic 必须精确,拒绝含通配的 topic
    // (否则 retained 槽位可写但永远不会被 deliver,徒留垃圾)
    if (ERR_OK != path_validate(ctx->rules, topic, PATH_KIND_LITERAL)) {
        _sc_resp_failed(ctx, src, sess);
        return;
    }
    if (br->size - br->offset < 4) {
        _sc_resp_failed(ctx, src, sess);
        return;
    }
    uint32_t plen = (uint32_t)binary_get_uinteger(br, 4, 0);
    if (br->size - br->offset < (size_t)plen) {
        _sc_resp_failed(ctx, src, sess);
        return;
    }
    const void *payload = (plen > 0) ? binary_get_string(br, plen) : NULL;
    if (retained) {
        // 超长 retained 按头文件契约拒绝：返 ERR_FAILED、不 deliver 不存储，避免静默数据丢失
        if (plen > SC_RETAINED_MAX_SIZE) {
            LOG_WARN("subcenter retained too large: topic=%s size=%u", topic, plen);
            _sc_resp_failed(ctx, src, sess);
            return;
        }
        _sc_update_retained(ctx, src, topic, payload, plen);
        if (0 == plen) {
            // 清空 retained 后不 deliver
            _sc_resp_ok(ctx, src, sess);
            return;
        }
    }
    _sc_publish_deliver(ctx, src, topic, payload, plen);
    _sc_resp_ok(ctx, src, sess);
}
// handler:SET_META。mlen=0 删除 publisher_meta 条目(等价"清除");
// 否则 MALLOC + memcpy 覆盖现有 entry,或新建条目入 hashmap
static void _sc_handle_set_meta(sc_ctx *ctx, name_t src, uint64_t sess, binary_ctx *br) {
    const char *meta;
    uint16_t mlen;
    if (ERR_OK != _sc_read_lp16_max(br, &meta, &mlen, SC_META_MAX_SIZE)) {
        _sc_resp_failed(ctx, src, sess);
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
        _sc_resp_ok(ctx, src, sess);
        return;
    }
    sc_publisher_meta *e = (sc_publisher_meta *)hashmap_get(ctx->publisher_meta, &q);
    if (NULL != e) {
        FREE(e->meta);
        MALLOC(e->meta, mlen);
        memcpy(e->meta, meta, mlen);
        e->size = mlen;
    } else {
        sc_publisher_meta ne;
        ne.publisher = src;
        MALLOC(ne.meta, mlen);
        memcpy(ne.meta, meta, mlen);
        ne.size = mlen;
        hashmap_set(ctx->publisher_meta, &ne);
        if (hashmap_oom(ctx->publisher_meta)) {
            FREE(ne.meta);
            _sc_resp_failed(ctx, src, sess);
            return;
        }
    }
    _sc_resp_ok(ctx, src, sess);
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
        binary_set_string(c->bw, (const char *)e->retained_meta, e->retained_meta_size);
    }
    size_t tlen = strlen(e->topic);
    binary_set_uinteger(c->bw, (uint64_t)tlen, 2, 0);
    binary_set_string(c->bw, e->topic, tlen);
    binary_set_uinteger(c->bw, (uint64_t)e->retained_size, 4, 0);
    if (e->retained_size > 0 && NULL != e->retained) {
        binary_set_string(c->bw, (const char *)e->retained, e->retained_size);
    }
    c->pushed++;
    return true;
}
// handler:QUERY_RETAINED。pattern 走 WILDCARD 校验后,
// hashmap_iter 扫 retained_index + path_matches_pattern 过滤,匹配项拼到 wire buf 一次性返回
static void _sc_handle_query_retained(sc_ctx *ctx, name_t src, uint64_t sess, binary_ctx *br) {
    char pattern[SC_TOPIC_MAX + 1];
    if (ERR_OK != _sc_read_cstr_max(br, pattern, sizeof(pattern), SC_TOPIC_MAX)) {
        _sc_resp_failed(ctx, src, sess);
        return;
    }
    if (ERR_OK != path_validate(ctx->rules, pattern, PATH_KIND_WILDCARD)) {
        _sc_resp_failed(ctx, src, sess);
        return;
    }
    if (INVALID_TNAME == src || 0 == sess) {
        return;   // 无人接响应,跳过 scan 工作
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
    task_ctx *t = task_grab(ctx->loader, src);
    if (NULL == t) {
        binary_free(&bw);
        return;
    }
    if (bw.offset > 0) {
        task_response(t, sess, ERR_OK, bw.data, bw.offset, 0);
    } else {
        task_response(t, sess, ERR_OK, NULL, 0, 0);
        binary_free(&bw);
    }
    task_ungrab(t);
}
// path_scan 回调:把每个 topic 节点的"topic + normal_count + shared_count"写入 binary_ctx
static void _sc_list_visit(const char *path, void *payload, void *udata) {
    binary_ctx *bw = (binary_ctx *)udata;
    sc_topic_data *d = (sc_topic_data *)payload;
    size_t tlen = strlen(path);
    binary_set_uinteger(bw, (uint64_t)tlen, 2, 0);
    binary_set_string(bw, path, tlen);
    binary_set_uinteger(bw, (uint64_t)d->normal_subs.size, 4, 0);
    binary_set_uinteger(bw, (uint64_t)(NULL != d->shared_groups ? hashmap_count(d->shared_groups) : 0), 4, 0);
}
// handler:LIST。path_scan 全 trie 把每个 topic 的订阅信息(normal/shared count)写入 wire buf
static void _sc_handle_list(sc_ctx *ctx, name_t src, uint64_t sess) {
    if (INVALID_TNAME == src || 0 == sess) {
        return;
    }
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    path_scan(ctx->topics, _sc_list_visit, &bw);
    task_ctx *t = task_grab(ctx->loader, src);
    if (NULL == t) {
        binary_free(&bw);
        return;
    }
    if (bw.offset > 0) {
        task_response(t, sess, ERR_OK, bw.data, bw.offset, 0);
    } else {
        task_response(t, sess, ERR_OK, NULL, 0, 0);
        binary_free(&bw);
    }
    task_ungrab(t);
}
// hashmap_scan 回调:把每条 retained 的"topic + publisher + size + meta_size"写入 binary_ctx
// (不含 retained payload 自身,避免数据量大)
static bool _sc_retained_list_iter(const void *item, void *udata) {
    binary_ctx *bw = (binary_ctx *)udata;
    const sc_retained_entry *e = (const sc_retained_entry *)item;
    size_t tlen = strlen(e->topic);
    binary_set_uinteger(bw, (uint64_t)tlen, 2, 0);
    binary_set_string(bw, e->topic, tlen);
    binary_set_uinteger(bw, (uint64_t)e->retained_publisher, sizeof(name_t), 0);
    binary_set_uinteger(bw, (uint64_t)e->retained_size, 4, 0);
    binary_set_uinteger(bw, (uint64_t)e->retained_meta_size, 2, 0);
    return true;
}
// handler:RETAINED_LIST。hashmap_scan 把每条 retained 的元信息写入 wire buf(不含 payload)
static void _sc_handle_retained_list(sc_ctx *ctx, name_t src, uint64_t sess) {
    if (INVALID_TNAME == src || 0 == sess) {
        return;
    }
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    hashmap_scan(ctx->retained_index, _sc_retained_list_iter, &bw);
    task_ctx *t = task_grab(ctx->loader, src);
    if (NULL == t) {
        binary_free(&bw);
        return;
    }
    if (bw.offset > 0) {
        task_response(t, sess, ERR_OK, bw.data, bw.offset, 0);
    } else {
        task_response(t, sess, ERR_OK, NULL, 0, 0);
        binary_free(&bw);
    }
    task_ungrab(t);
}
// 中心 dispatch:reqtype 锁定 REQ_SC,剥 payload 首字节 op 后子分发到 10 个 handler
static void _sc_requested(task_ctx *task, uint8_t reqtype, uint64_t sess, name_t src,
                          void *data, size_t size) {
    (void)reqtype;
    sc_ctx *ctx = (sc_ctx *)task->arg;
    if (INVALID_TNAME == src) {
        return;
    }
    if (size < 1 || NULL == data) {
        _sc_resp_failed(ctx, src, sess);
        return;
    }
    binary_ctx br;
    binary_init(&br, (char *)data, size, 0);
    uint8_t op = binary_get_uint8(&br);
    switch (op) {
    case SC_OP_SUB:
        _sc_handle_sub(ctx, src, sess, &br, 0);
        break;
    case SC_OP_SUB_SHARED:
        _sc_handle_sub(ctx, src, sess, &br, 1);
        break;
    case SC_OP_UNSUB:
        _sc_handle_unsub(ctx, src, sess, &br, 0);
        break;
    case SC_OP_UNSUB_SHARED:
        _sc_handle_unsub(ctx, src, sess, &br, 1);
        break;
    case SC_OP_PUB:
        _sc_handle_pub(ctx, src, sess, &br, 0);
        break;
    case SC_OP_PUB_RETAINED:
        _sc_handle_pub(ctx, src, sess, &br, 1);
        break;
    case SC_OP_LIST:
        _sc_handle_list(ctx, src, sess);
        break;
    case SC_OP_QUERY_RETAINED:
        _sc_handle_query_retained(ctx, src, sess, &br);
        break;
    case SC_OP_SET_META:
        _sc_handle_set_meta(ctx, src, sess, &br);
        break;
    case SC_OP_RETAINED_LIST:
        _sc_handle_retained_list(ctx, src, sess);
        break;
    default:
        _sc_resp_failed(ctx, src, sess);
        break;
    }
}
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
    task_ctx *task = task_new(loader, name, 4 * ONEK, NULL, _sc_free, ctx);
    task_requested(task, _sc_requested);
    if (ERR_OK != task_register(task, NULL, NULL)) {
        // task_register 失败时 task 未进 maptasks,需手动 task_free;
        task_free(task);
        return ERR_FAILED;
    }
    return ERR_OK;
}
// SUB/UNSUB/QUERY_RETAINED:| u8 op | u16 tlen | topic |
static char *_sc_pack_topic(sc_op op, const char *topic, size_t *out_total) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    binary_set_uint8(&bw, (uint8_t)op);
    size_t tlen = strlen(topic);
    binary_set_uinteger(&bw, (uint64_t)tlen, 2, 0);
    binary_set_string(&bw, topic, tlen);
    *out_total = bw.offset;
    return bw.data;
}
// SUB_SHARED/UNSUB_SHARED:| u8 op | u16 tlen | topic | u16 glen | group |
static char *_sc_pack_topic_group(sc_op op, const char *topic, const char *group, size_t *out_total) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    binary_set_uint8(&bw, (uint8_t)op);
    size_t tlen = strlen(topic);
    binary_set_uinteger(&bw, (uint64_t)tlen, 2, 0);
    binary_set_string(&bw, topic, tlen);
    size_t glen = strlen(group);
    binary_set_uinteger(&bw, (uint64_t)glen, 2, 0);
    binary_set_string(&bw, group, glen);
    *out_total = bw.offset;
    return bw.data;
}
// PUB/PUB_RETAINED:| u8 op | u16 tlen | topic | u32 plen | payload |
static char *_sc_pack_topic_payload(sc_op op, const char *topic, const void *payload, size_t plen,
                                    size_t *out_total) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    binary_set_uint8(&bw, (uint8_t)op);
    size_t tlen = strlen(topic);
    binary_set_uinteger(&bw, (uint64_t)tlen, 2, 0);
    binary_set_string(&bw, topic, tlen);
    size_t real_plen = (NULL != payload) ? plen : 0;
    binary_set_uinteger(&bw, (uint64_t)real_plen, 4, 0);
    if (real_plen > 0) {
        binary_set_string(&bw, (const char *)payload, real_plen);
    }
    *out_total = bw.offset;
    return bw.data;
}
// SET_META:| u8 op | u16 mlen | meta |
static char *_sc_pack_meta(sc_op op, const void *meta, size_t mlen, size_t *out_total) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    binary_set_uint8(&bw, (uint8_t)op);
    size_t real_mlen = (NULL != meta) ? mlen : 0;
    binary_set_uinteger(&bw, (uint64_t)real_mlen, 2, 0);
    if (real_mlen > 0) {
        binary_set_string(&bw, (const char *)meta, real_mlen);
    }
    *out_total = bw.offset;
    return bw.data;
}
// LIST/RETAINED_LIST:| u8 op | (no body) |
static char *_sc_pack_nobody(sc_op op, size_t *out_total) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    binary_set_uint8(&bw, (uint8_t)op);
    *out_total = bw.offset;
    return bw.data;
}
int32_t coro_sc_subscribe(task_ctx *task, name_t sc_name, const char *topic) {
    if (EMPTYSTR(topic)) {
        return ERR_FAILED;
    }
    task_ctx *sc = task_grab(task->loader, sc_name);
    if (NULL == sc) {
        return ERR_FAILED;
    }
    size_t total;
    char *buf = _sc_pack_topic(SC_OP_SUB, topic, &total);
    int32_t erro = 0;
    size_t rsize = 0;
    coro_request(sc, task, REQ_SC, buf, total, 0, &erro, &rsize);
    task_ungrab(sc);
    return erro;
}
int32_t coro_sc_subscribe_shared(task_ctx *task, name_t sc_name,
                                 const char *topic, const char *group) {
    if (EMPTYSTR(topic) || EMPTYSTR(group)) {
        return ERR_FAILED;
    }
    task_ctx *sc = task_grab(task->loader, sc_name);
    if (NULL == sc) {
        return ERR_FAILED;
    }
    size_t total;
    char *buf = _sc_pack_topic_group(SC_OP_SUB_SHARED, topic, group, &total);
    int32_t erro = 0;
    size_t rsize = 0;
    coro_request(sc, task, REQ_SC, buf, total, 0, &erro, &rsize);
    task_ungrab(sc);
    return erro;
}
int32_t coro_sc_unsubscribe(task_ctx *task, name_t sc_name, const char *topic) {
    if (EMPTYSTR(topic)) {
        return ERR_FAILED;
    }
    task_ctx *sc = task_grab(task->loader, sc_name);
    if (NULL == sc) {
        return ERR_FAILED;
    }
    size_t total;
    char *buf = _sc_pack_topic(SC_OP_UNSUB, topic, &total);
    int32_t erro = 0;
    size_t rsize = 0;
    coro_request(sc, task, REQ_SC, buf, total, 0, &erro, &rsize);
    task_ungrab(sc);
    return erro;
}
int32_t coro_sc_unsubscribe_shared(task_ctx *task, name_t sc_name,
                                   const char *topic, const char *group) {
    if (EMPTYSTR(topic) || EMPTYSTR(group)) {
        return ERR_FAILED;
    }
    task_ctx *sc = task_grab(task->loader, sc_name);
    if (NULL == sc) {
        return ERR_FAILED;
    }
    size_t total;
    char *buf = _sc_pack_topic_group(SC_OP_UNSUB_SHARED, topic, group, &total);
    int32_t erro = 0;
    size_t rsize = 0;
    coro_request(sc, task, REQ_SC, buf, total, 0, &erro, &rsize);
    task_ungrab(sc);
    return erro;
}
int32_t coro_sc_publish(task_ctx *task, name_t sc_name, const char *topic,
                        void *data, size_t size) {
    if (EMPTYSTR(topic) || size > UINT32_MAX) {
        return ERR_FAILED;
    }
    task_ctx *sc = task_grab(task->loader, sc_name);
    if (NULL == sc) {
        return ERR_FAILED;
    }
    size_t total;
    char *buf = _sc_pack_topic_payload(SC_OP_PUB, topic, data, size, &total);
    int32_t erro = 0;
    size_t rsize = 0;
    coro_request(sc, task, REQ_SC, buf, total, 0, &erro, &rsize);
    task_ungrab(sc);
    return erro;
}
int32_t coro_sc_publish_retained(task_ctx *task, name_t sc_name, const char *topic,
                                 void *data, size_t size) {
    if (EMPTYSTR(topic) || size > UINT32_MAX) {
        return ERR_FAILED;
    }
    task_ctx *sc = task_grab(task->loader, sc_name);
    if (NULL == sc) {
        return ERR_FAILED;
    }
    size_t total;
    char *buf = _sc_pack_topic_payload(SC_OP_PUB_RETAINED, topic, data, size, &total);
    int32_t erro = 0;
    size_t rsize = 0;
    coro_request(sc, task, REQ_SC, buf, total, 0, &erro, &rsize);
    task_ungrab(sc);
    return erro;
}
void *coro_sc_query_retained(task_ctx *task, name_t sc_name, const char *pattern,
                             size_t *size, int32_t *erro) {
    if (EMPTYSTR(pattern)) {
        SET_PTR(size, 0);
        *erro = ERR_FAILED;
        return NULL;
    }
    task_ctx *sc = task_grab(task->loader, sc_name);
    if (NULL == sc) {
        SET_PTR(size, 0);
        *erro = ERR_FAILED;
        return NULL;
    }
    size_t total;
    char *buf = _sc_pack_topic(SC_OP_QUERY_RETAINED, pattern, &total);
    void *resp = coro_request(sc, task, REQ_SC, buf, total, 0, erro, size);
    task_ungrab(sc);
    if (ERR_OK != *erro) {
        SET_PTR(size, 0);
        return NULL;
    }
    return resp;
}
void *coro_sc_topics(task_ctx *task, name_t sc_name,
                     size_t *size, int32_t *erro) {
    task_ctx *sc = task_grab(task->loader, sc_name);
    if (NULL == sc) {
        SET_PTR(size, 0);
        *erro = ERR_FAILED;
        return NULL;
    }
    size_t total;
    char *buf = _sc_pack_nobody(SC_OP_LIST, &total);
    void *resp = coro_request(sc, task, REQ_SC, buf, total, 0, erro, size);
    task_ungrab(sc);
    if (ERR_OK != *erro) {
        SET_PTR(size, 0);
        return NULL;
    }
    return resp;
}
void *coro_sc_retained_topics(task_ctx *task, name_t sc_name,
                              size_t *size, int32_t *erro) {
    task_ctx *sc = task_grab(task->loader, sc_name);
    if (NULL == sc) {
        SET_PTR(size, 0);
        *erro = ERR_FAILED;
        return NULL;
    }
    size_t total;
    char *buf = _sc_pack_nobody(SC_OP_RETAINED_LIST, &total);
    void *resp = coro_request(sc, task, REQ_SC, buf, total, 0, erro, size);
    task_ungrab(sc);
    if (ERR_OK != *erro) {
        SET_PTR(size, 0);
        return NULL;
    }
    return resp;
}
int32_t coro_sc_set_meta(task_ctx *task, name_t sc_name,
                         const void *meta, size_t size) {
    if (size > SC_META_MAX_SIZE) {
        return ERR_FAILED;
    }
    task_ctx *sc = task_grab(task->loader, sc_name);
    if (NULL == sc) {
        return ERR_FAILED;
    }
    size_t total;
    char *buf = _sc_pack_meta(SC_OP_SET_META, meta, size, &total);
    int32_t erro = 0;
    size_t rsize = 0;
    coro_request(sc, task, REQ_SC, buf, total, 0, &erro, &rsize);
    task_ungrab(sc);
    return erro;
}
int32_t sc_subscribe(task_ctx *task, name_t sc_name, uint64_t sess, const char *topic) {
    if (EMPTYSTR(topic) || 0 == sess) {
        return ERR_FAILED;
    }
    task_ctx *sc = task_grab(task->loader, sc_name);
    if (NULL == sc) {
        return ERR_FAILED;
    }
    size_t total;
    char *buf = _sc_pack_topic(SC_OP_SUB, topic, &total);
    task_request(sc, task, REQ_SC, sess, buf, total, 0);
    task_ungrab(sc);
    return ERR_OK;
}
int32_t sc_subscribe_shared(task_ctx *task, name_t sc_name, uint64_t sess,
                            const char *topic, const char *group) {
    if (EMPTYSTR(topic) || EMPTYSTR(group) || 0 == sess) {
        return ERR_FAILED;
    }
    task_ctx *sc = task_grab(task->loader, sc_name);
    if (NULL == sc) {
        return ERR_FAILED;
    }
    size_t total;
    char *buf = _sc_pack_topic_group(SC_OP_SUB_SHARED, topic, group, &total);
    task_request(sc, task, REQ_SC, sess, buf, total, 0);
    task_ungrab(sc);
    return ERR_OK;
}
int32_t sc_unsubscribe(task_ctx *task, name_t sc_name, uint64_t sess, const char *topic) {
    if (EMPTYSTR(topic) || 0 == sess) {
        return ERR_FAILED;
    }
    task_ctx *sc = task_grab(task->loader, sc_name);
    if (NULL == sc) {
        return ERR_FAILED;
    }
    size_t total;
    char *buf = _sc_pack_topic(SC_OP_UNSUB, topic, &total);
    task_request(sc, task, REQ_SC, sess, buf, total, 0);
    task_ungrab(sc);
    return ERR_OK;
}
int32_t sc_unsubscribe_shared(task_ctx *task, name_t sc_name, uint64_t sess,
                              const char *topic, const char *group) {
    if (EMPTYSTR(topic) || EMPTYSTR(group) || 0 == sess) {
        return ERR_FAILED;
    }
    task_ctx *sc = task_grab(task->loader, sc_name);
    if (NULL == sc) {
        return ERR_FAILED;
    }
    size_t total;
    char *buf = _sc_pack_topic_group(SC_OP_UNSUB_SHARED, topic, group, &total);
    task_request(sc, task, REQ_SC, sess, buf, total, 0);
    task_ungrab(sc);
    return ERR_OK;
}
int32_t sc_publish(task_ctx *task, name_t sc_name, uint64_t sess, const char *topic,
                   void *data, size_t size) {
    if (EMPTYSTR(topic) || size > UINT32_MAX || 0 == sess) {
        return ERR_FAILED;
    }
    task_ctx *sc = task_grab(task->loader, sc_name);
    if (NULL == sc) {
        return ERR_FAILED;
    }
    size_t total;
    char *buf = _sc_pack_topic_payload(SC_OP_PUB, topic, data, size, &total);
    task_request(sc, task, REQ_SC, sess, buf, total, 0);
    task_ungrab(sc);
    return ERR_OK;
}
int32_t sc_publish_retained(task_ctx *task, name_t sc_name, uint64_t sess,
                            const char *topic, void *data, size_t size) {
    if (EMPTYSTR(topic) || size > UINT32_MAX || 0 == sess) {
        return ERR_FAILED;
    }
    task_ctx *sc = task_grab(task->loader, sc_name);
    if (NULL == sc) {
        return ERR_FAILED;
    }
    size_t total;
    char *buf = _sc_pack_topic_payload(SC_OP_PUB_RETAINED, topic, data, size, &total);
    task_request(sc, task, REQ_SC, sess, buf, total, 0);
    task_ungrab(sc);
    return ERR_OK;
}
int32_t sc_query_retained(task_ctx *task, name_t sc_name, uint64_t sess, const char *pattern) {
    if (EMPTYSTR(pattern) || 0 == sess) {
        return ERR_FAILED;
    }
    task_ctx *sc = task_grab(task->loader, sc_name);
    if (NULL == sc) {
        return ERR_FAILED;
    }
    size_t total;
    char *buf = _sc_pack_topic(SC_OP_QUERY_RETAINED, pattern, &total);
    task_request(sc, task, REQ_SC, sess, buf, total, 0);
    task_ungrab(sc);
    return ERR_OK;
}
int32_t sc_topics(task_ctx *task, name_t sc_name, uint64_t sess) {
    if (0 == sess) {
        return ERR_FAILED;
    }
    task_ctx *sc = task_grab(task->loader, sc_name);
    if (NULL == sc) {
        return ERR_FAILED;
    }
    size_t total;
    char *buf = _sc_pack_nobody(SC_OP_LIST, &total);
    task_request(sc, task, REQ_SC, sess, buf, total, 0);
    task_ungrab(sc);
    return ERR_OK;
}
int32_t sc_retained_topics(task_ctx *task, name_t sc_name, uint64_t sess) {
    if (0 == sess) {
        return ERR_FAILED;
    }
    task_ctx *sc = task_grab(task->loader, sc_name);
    if (NULL == sc) {
        return ERR_FAILED;
    }
    size_t total;
    char *buf = _sc_pack_nobody(SC_OP_RETAINED_LIST, &total);
    task_request(sc, task, REQ_SC, sess, buf, total, 0);
    task_ungrab(sc);
    return ERR_OK;
}
int32_t sc_set_meta(task_ctx *task, name_t sc_name, uint64_t sess,
                    const void *meta, size_t size) {
    if (size > SC_META_MAX_SIZE || 0 == sess) {
        return ERR_FAILED;
    }
    task_ctx *sc = task_grab(task->loader, sc_name);
    if (NULL == sc) {
        return ERR_FAILED;
    }
    size_t total;
    char *buf = _sc_pack_meta(SC_OP_SET_META, meta, size, &total);
    task_request(sc, task, REQ_SC, sess, buf, total, 0);
    task_ungrab(sc);
    return ERR_OK;
}
