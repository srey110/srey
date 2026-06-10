#include "containers/hashset.h"
#include "containers/hashmap.h"

// scan trampoline:把用户 int32_t 回调适配为 hashmap 的 bool 回调
typedef struct _hs_scan_ctx {
    int32_t (*iter)(const void *item, void *udata);
    void *udata;
} _hs_scan_ctx;
// 内部:hashset 是 hashmap 的薄包装。
// hashmap_set 语义:已存在则替换并返回旧 item 指针;不存在新插入返 NULL。
// hashset_add 上层语义:不更新已存在元素,1 新增 / 0 已存在 / -1 OOM。
struct hashset {
    struct hashmap *map;
};

hashset *hashset_new(size_t elsize, size_t cap,
                     uint64_t (*hash)(const void *item, uint64_t seed0, uint64_t seed1),
                     int (*compare)(const void *a, const void *b, void *udata),
                     void (*elfree)(void *item),
                     void *udata) {
    if (NULL == hash || NULL == compare || 0 == elsize) {
        return NULL;
    }
    hashset *s;
    MALLOC(s, sizeof(hashset));
    s->map = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                        elsize, cap, 0, 0,
                                        hash, compare, elfree, udata);
    if (NULL == s->map) {
        FREE(s);
        return NULL;
    }
    return s;
}
void hashset_free(hashset *s) {
    if (NULL == s) {
        return;
    }
    hashmap_free(s->map);
    FREE(s);
}
void hashset_clear(hashset *s, int32_t update_cap) {
    hashmap_clear(s->map, 0 != update_cap);
}
size_t hashset_count(const hashset *s) {
    return hashmap_count(s->map);
}
int32_t hashset_oom(const hashset *s) {
    // hashmap_oom 接收非 const 指针(内部访问 oom 字段),这里 cast 掉 const
    return hashmap_oom((struct hashmap *)s->map) ? 1 : 0;
}
int32_t hashset_add(hashset *s, const void *item) {
    // 先查是否已存在,避免 hashmap_set 无谓的"替换"开销与歧义
    if (NULL != hashmap_get(s->map, item)) {
        return 0;
    }
    (void)hashmap_set(s->map, item);
    if (hashmap_oom(s->map)) {
        return -1;
    }
    return 1;
}
int32_t hashset_contains(const hashset *s, const void *item) {
    return NULL != hashmap_get(s->map, item) ? 1 : 0;
}
const void *hashset_remove(hashset *s, const void *item) {
    return hashmap_delete(s->map, item);
}
// scan 适配:hashmap_scan 要求 bool 回调,转发到用户的 int32_t 回调
static bool _hashset_scan_trampoline(const void *item, void *udata) {
    _hs_scan_ctx *c = (_hs_scan_ctx *)udata;
    return 0 != c->iter(item, c->udata);
}
int32_t hashset_scan(hashset *s, int32_t (*iter)(const void *item, void *udata), void *udata) {
    _hs_scan_ctx c;
    c.iter = iter;
    c.udata = udata;
    return hashmap_scan(s->map, _hashset_scan_trampoline, &c) ? 1 : 0;
}
int32_t hashset_iter(hashset *s, size_t *i, void **item) {
    return hashmap_iter(s->map, i, item) ? 1 : 0;
}
