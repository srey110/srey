#include "path/path_trie.h"
#include "containers/hashmap.h"
#include "protocol/urlparse.h"
#include "utils/utils.h"

// 公开 API 前导:校验 t,WILDCARD/LITERAL 模式切片至局部 segs/n;失败 return fail_value
// (void 函数传空 fail_value,展开为 return ;)
#define PATH_PREP_SEGS(t, path, kind, fail_value) \
    if (NULL == (t)) { return fail_value; } \
    buf_ctx segs[URL_MAX_PATH_DEPTH]; \
    int32_t n = 0; \
    if (ERR_OK != _path_validate((t)->rules, (path), (kind), segs, URL_MAX_PATH_DEPTH, &n)) { \
        return fail_value; \
    }
    
// 内部节点
typedef struct path_node {
    char *seg;                          // 段名(MALLOC 拷贝,非 NUL 终结;根节点 NULL)
    size_t seglen;                      // 段名字节长度(根节点 0)
    struct path_node *parent;           // 父节点(根节点为 NULL)
    struct path_node *plus;             // single_wildcard 子节点
    struct path_node *hash;             // multi_wildcard 子节点(叶)
    struct hashmap *children;           // 精确子节点(by-value 存 path_node*),延迟创建
    void *payload;                      // 用户 payload
}path_node;
struct path_trie {
    size_t count;               // 持有 payload 的节点数(insert 增 / delete 减)
    const path_rules *rules;    // path 解析/匹配规则(外部持有,长生命周期)
    path_node root;             // 根节点(seg=NULL)
    free_cb _free;              // payload 释放回调(节点删除/树销毁时调)
};
// children hashmap 存储 path_node*(by-value 存指针),hash/cmp 通过解引用读 seg/seglen
static uint64_t _path_child_hash(const void *item, uint64_t s0, uint64_t s1) {
    const path_node *n = *(path_node *const *)item;
    return hashmap_xxhash3(n->seg, n->seglen, s0, s1);
}
static int _path_child_cmp(const void *a, const void *b, void *ud) {
    (void)ud;
    const path_node *na = *(path_node *const *)a;
    const path_node *nb = *(path_node *const *)b;
    if (na->seglen != nb->seglen) {
        return 1;
    }
    return memcmp(na->seg, nb->seg, na->seglen);
}
// 段 sv 是否为单字符通配 wc(wc=0 表示该通配未启用)
static inline int32_t _seg_is_wc(const buf_ctx *sv, char wc) {
    return 0 != wc && 1 == sv->lens && wc == ((char *)sv->data)[0];
}
// 通配符 wc 出现在段内时须独占整段,否则返 ERR_FAILED
static int32_t _wildcard_must_own_seg(const buf_ctx *sv, char wc) {
    if (NULL != memchr(sv->data, wc, sv->lens)
        && !_seg_is_wc(sv, wc)) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
// 段级内置校验(每段调一次)
static int32_t _path_builtin_validate_seg(const path_rules *r, const buf_ctx *sv, path_kind kind) {
    // 空段
    if (0 == sv->lens) {
        return ERR_FAILED;
    }
    // 段内含 NUL,会被 strdup 截断
    if (NULL != memchr(sv->data, '\0', sv->lens)) {
        return ERR_FAILED;
    }
    // 段内混入分隔符
    if (NULL != memchr(sv->data, r->sep, sv->lens)) {
        return ERR_FAILED;
    }
    // 通配符出现即必须独占整段,不能作为段内普通字符
    if (0 != r->single_wildcard 
        && ERR_OK != _wildcard_must_own_seg(sv, r->single_wildcard)) {
        return ERR_FAILED;
    }
    if (0 != r->multi_wildcard 
        && ERR_OK != _wildcard_must_own_seg(sv, r->multi_wildcard)) {
        return ERR_FAILED;
    }
    // 字面路径不接受纯通配段
    if (PATH_KIND_LITERAL == kind
        && (_seg_is_wc(sv, r->single_wildcard)
            || _seg_is_wc(sv, r->multi_wildcard))) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
// 路径级内置校验(切分后调一次)
static int32_t _path_builtin_validate(const path_rules *r, const buf_ctx *segs, int32_t n, path_kind kind) {
    if (0 == n) {
        return ERR_FAILED;
    }
    // multi_wildcard 必须末尾
    if (0 != r->multi_wildcard && PATH_KIND_WILDCARD == kind) {
        for (int32_t i = 0; i < n - 1; i++) {
            if (_seg_is_wc(&segs[i], r->multi_wildcard)) {
                return ERR_FAILED;
            }
        }
    }
    return ERR_OK;
}
// 统一校验流程:返回 ERR_OK + 段数;失败返 ERR_FAILED
static int32_t _path_validate(const path_rules *r, const char *path, path_kind kind,
                          buf_ctx *segs, int32_t cap, int32_t *out_n) {
    if (NULL == r || NULL == path) {
        return ERR_FAILED;
    }
    size_t plen = strlen(path);
    if (plen >= URL_BUF_LENS) {
        return ERR_FAILED;
    }
    // 业务自定义整路径校验
    if (NULL != r->validate_path
        && ERR_OK != r->validate_path(path, kind, r->udata)) {
        return ERR_FAILED;
    }
    // 按 sep 切分为段
    int32_t n = split2((char *)path, plen, r->sep, segs, cap);
    if (n < 0) {
        return ERR_FAILED;
    }
    // 路径级内置校验(段数 / multi_wildcard 必须末尾)
    if (ERR_OK != _path_builtin_validate(r, segs, n, kind)) {
        return ERR_FAILED;
    }
    // 逐段:业务自定义段校验 + 内置段校验
    for (int32_t i = 0; i < n; i++) {
        if (NULL != r->validate_segment
            && ERR_OK != r->validate_segment(segs[i].data, segs[i].lens, kind, r->udata)) {
            return ERR_FAILED;
        }
        if (ERR_OK != _path_builtin_validate_seg(r, &segs[i], kind)) {
            return ERR_FAILED;
        }
    }
    *out_n = n;
    return ERR_OK;
}
// 公开:独立校验
int32_t path_validate(const path_rules *rules, const char *path, path_kind kind) {
    buf_ctx segs[URL_MAX_PATH_DEPTH];
    int32_t n = 0;
    return _path_validate(rules, path, kind, segs, URL_MAX_PATH_DEPTH, &n);
}
// 节点 alloc:sv=NULL 表示根
static path_node *_path_node_alloc(const buf_ctx *sv, path_node *parent) {
    path_node *n;
    CALLOC(n, 1, sizeof(path_node));
    n->parent = parent;
    if (NULL != sv && sv->lens > 0) {
        MALLOC(n->seg, sv->lens);
        memcpy(n->seg, sv->data, sv->lens);
        n->seglen = sv->lens;
    }
    return n;
}
// 节点递归释放:用于 path_free / 错误回滚
static void _path_node_free_recursive(path_trie *t, path_node *n) {
    if (NULL == n) {
        return;
    }
    if (NULL != n->payload && NULL != t->_free) {
        t->_free(n->payload);
    }
    n->payload = NULL;
    if (NULL != n->plus) {
        _path_node_free_recursive(t, n->plus);
        n->plus = NULL;
    }
    if (NULL != n->hash) {
        _path_node_free_recursive(t, n->hash);
        n->hash = NULL;
    }
    if (NULL != n->children) {
        size_t i = 0;
        void *item;
        path_node *child;
        while (hashmap_iter(n->children, &i, &item)) {
            child = *(path_node **)item;
            _path_node_free_recursive(t, child);
        }
        hashmap_free(n->children);
        n->children = NULL;
    }
    FREE(n->seg);
    if (n != &t->root) {
        FREE(n);
    }
}
// 创建 trie
path_trie *path_new(const path_rules *rules, free_cb _path_free) {
    if (NULL == rules || 0 == rules->sep) {
        return NULL;
    }
    path_trie *t;
    CALLOC(t, 1, sizeof(path_trie));
    t->rules = rules;
    t->_free = _path_free;
    return t;
}
// 销毁
void path_free(path_trie *t) {
    if (NULL == t) {
        return;
    }
    _path_node_free_recursive(t, &t->root);
    FREE(t);
}
size_t path_count(const path_trie *t) {
    return NULL == t ? 0 : t->count;
}
// 在父节点 children 中查找(临时查询节点 seg/seglen 直接引用 sv,无需拷贝)
static path_node *_path_children_lookup(struct hashmap *children, const buf_ctx *sv) {
    if (NULL == children) {
        return NULL;
    }
    path_node tmp;
    tmp.seg = (char *)sv->data;
    tmp.seglen = sv->lens;
    path_node *qptr = &tmp;
    path_node *const *found = (path_node *const *)hashmap_get(children, &qptr);
    return (NULL != found) ? *found : NULL;
}
// walk insert:从 root 沿 segs 创建路径,返回最终节点(NULL 失败)
static path_node *_path_walk_insert(path_trie *t, const buf_ctx *segs, int32_t n) {
    path_node *cur = &t->root;
    const path_rules *r = t->rules;
    path_node *next, *found, *child;
    for (int32_t i = 0; i < n; i++) {
        next = NULL;
        if (_seg_is_wc(&segs[i], r->single_wildcard)) {
            // single_wildcard 段:plus 子树
            if (NULL == cur->plus) {
                cur->plus = _path_node_alloc(&segs[i], cur);
            }
            next = cur->plus;
        } else if (_seg_is_wc(&segs[i], r->multi_wildcard)) {
            // multi_wildcard 段:hash 子树(叶)
            if (NULL == cur->hash) {
                cur->hash = _path_node_alloc(&segs[i], cur);
            }
            next = cur->hash;
        } else {
            // 精确分支
            if (NULL == cur->children) {
                cur->children = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                                           sizeof(path_node *), 4, 0, 0,
                                                           _path_child_hash, _path_child_cmp, NULL, NULL);
            }
            found = _path_children_lookup(cur->children, &segs[i]);
            if (NULL != found) {
                next = found;
            } else {
                child = _path_node_alloc(&segs[i], cur);
                hashmap_set(cur->children, &child);
                next = child;
            }
        }
        cur = next;
    }
    return cur;
}
// walk 精确路径(仅查询,不创建)
static path_node *_path_walk_exact(path_trie *t, const buf_ctx *segs, int32_t n) {
    path_node *cur = &t->root;
    const path_rules *r = t->rules;
    path_node *next;
    for (int32_t i = 0; i < n; i++) {
        next = NULL;
        if (_seg_is_wc(&segs[i], r->single_wildcard)) {
            // single_wildcard 段:plus 子树
            next = cur->plus;
        } else if (_seg_is_wc(&segs[i], r->multi_wildcard)) {
            // multi_wildcard 段:hash 子树(叶)
            next = cur->hash;
        } else {
            // 精确分支
            next = _path_children_lookup(cur->children, &segs[i]);
        }
        if (NULL == next) {
            return NULL;
        }
        cur = next;
    }
    return cur;
}
// 空节点回收:从 node 向 root 回溯,沿途释放完全空的节点
static void _path_try_collect(path_trie *t, path_node *node) {
    path_node *cur = node;
    path_node *parent = NULL;
    path_node *qptr = NULL;
    while (cur != &t->root) {
        if (NULL != cur->payload) {
            return;
        }
        if (NULL != cur->plus || NULL != cur->hash) {
            return;
        }
        if (NULL != cur->children && hashmap_count(cur->children) > 0) {
            return;
        }
        parent = cur->parent;
        // 从 parent 摘除自己
        if (parent->plus == cur) {
            parent->plus = NULL;
        } else if (parent->hash == cur) {
            parent->hash = NULL;
        } else if (NULL != parent->children) {
            qptr = cur;
            hashmap_delete(parent->children, &qptr);
            if (0 == hashmap_count(parent->children)) {
                hashmap_free(parent->children);
                parent->children = NULL;
            }
        }
        if (NULL != cur->children) {
            hashmap_free(cur->children);
        }
        FREE(cur->seg);
        FREE(cur);
        cur = parent;
    }
}
int32_t path_insert(path_trie *t, const char *path, void *payload) {
    if (NULL == payload) {
        return ERR_FAILED;
    }
    // payload 含通配模式时按 WILDCARD;不含通配的精确插入也允许,这里按"有什么校什么"用 WILDCARD 兼容
    PATH_PREP_SEGS(t, path, PATH_KIND_WILDCARD, ERR_FAILED);
    path_node *target = _path_walk_insert(t, segs, n);
    if (NULL != target->payload) {
        if (NULL != t->_free) {
            t->_free(target->payload);
        }
    } else {
        t->count++;
    }
    target->payload = payload;
    return ERR_OK;
}
void *path_get(path_trie *t, const char *path) {
    return path_get_or_create(t, path, NULL);
}
void *path_get_or_create(path_trie *t, const char *path, void *init) {
    PATH_PREP_SEGS(t, path, PATH_KIND_WILDCARD, NULL);
    if (NULL == init) {
        // 仅查询
        path_node *target = _path_walk_exact(t, segs, n);
        return NULL == target ? NULL : target->payload;
    }
    path_node *target = _path_walk_insert(t, segs, n);
    if (NULL == target->payload) {
        target->payload = init;
        t->count++;
    }
    return target->payload;
}
void *path_remove(path_trie *t, const char *path) {
    PATH_PREP_SEGS(t, path, PATH_KIND_WILDCARD, NULL);
    path_node *target = _path_walk_exact(t, segs, n);
    if (NULL == target || NULL == target->payload) {
        return NULL;
    }
    void *old = target->payload;
    target->payload = NULL;
    t->count--;
    _path_try_collect(t, target);
    return old;
}
// DFS 匹配
static void _path_match_recurse(path_node *node, const path_rules *r,
                            const buf_ctx *segs, int32_t n, int32_t idx,
                            match_visit_cb cb, void *ud) {
    // multi_wildcard 终端:匹配剩余任意层(含 0 层)
    if (NULL != node->hash && NULL != node->hash->payload) {
        cb(node->hash->payload, ud);
    }
    if (idx == n) {
        if (NULL != node->payload) {
            cb(node->payload, ud);
        }
        return;
    }
    // 精确分支
    if (NULL != node->children) {
        path_node *child = _path_children_lookup(node->children, &segs[idx]);
        if (NULL != child) {
            _path_match_recurse(child, r, segs, n, idx + 1, cb, ud);
        }
    }
    // single_wildcard:消耗单层
    if (NULL != node->plus) {
        _path_match_recurse(node->plus, r, segs, n, idx + 1, cb, ud);
    }
}
void path_match(path_trie *t, const char *literal_path, match_visit_cb cb, void *udata) {
    if (NULL == cb) {
        return;
    }
    // publish 必须精确,LITERAL 校验拒绝通配
    PATH_PREP_SEGS(t, literal_path, PATH_KIND_LITERAL, );
    _path_match_recurse(&t->root, t->rules, segs, n, 0, cb, udata);
}
// 反向匹配:精确 literal 是否匹配 pattern(含通配)
// 算法:切分两边为 segs,逐段比较,处理 + / # 通配
int32_t path_matches_pattern(const path_rules *rules,
                              const char *literal_path,
                              const char *pattern) {
    if (NULL == rules || NULL == literal_path || NULL == pattern) {
        return ERR_FAILED;
    }
    buf_ctx lits[URL_MAX_PATH_DEPTH];
    buf_ctx pats[URL_MAX_PATH_DEPTH];
    int32_t ln = 0;
    int32_t pn = 0;
    if (ERR_OK != _path_validate(rules, literal_path, PATH_KIND_LITERAL, lits, URL_MAX_PATH_DEPTH, &ln)
        || ERR_OK != _path_validate(rules, pattern, PATH_KIND_WILDCARD, pats, URL_MAX_PATH_DEPTH, &pn)) {
        return ERR_FAILED;
    }
    // 逐段比较
    int32_t li = 0;
    int32_t pi = 0;
    char sw = rules->single_wildcard;
    char mw = rules->multi_wildcard;
    while (pi < pn) {
        // pattern 段是 multi_wildcard 终端 → 匹配剩余任意层
        if (_seg_is_wc(&pats[pi], mw)) {
            // 必须末尾
            if (pi != pn - 1) {
                return ERR_FAILED;
            }
            return ERR_OK;
        }
        // literal 段数用完但 pattern 还有 → 不匹配
        if (li >= ln) {
            return ERR_FAILED;
        }
        // single_wildcard:literal 段非空即可
        if (_seg_is_wc(&pats[pi], sw)) {
            if (0 == lits[li].lens) {
                return ERR_FAILED;
            }
            li++;
            pi++;
            continue;
        }
        // 精确比较
        if (lits[li].lens != pats[pi].lens
            || 0 != memcmp(lits[li].data, pats[pi].data, lits[li].lens)) {
            return ERR_FAILED;
        }
        li++;
        pi++;
    }
    return li == ln ? ERR_OK : ERR_FAILED;
}
static void _path_scan_recurse(path_trie *t, path_node *node, char *buf, size_t cap, size_t pos,
                           scan_visit_cb cb, void *udata);
// 将子节点路径拼入 buf 并递归
static void _path_scan_visit(path_trie *t, path_node *child, char *buf, size_t cap, size_t pos,
                             char sep, scan_visit_cb cb, void *udata) {
    size_t slen = child->seglen;
    size_t need = (pos > 0 ? 1 : 0) + slen + 1;
    if (pos + need > cap) {
        LOG_WARN("path_scan: path too long, skip");
        return;
    }
    size_t newpos = pos;
    if (pos > 0) {
        buf[newpos++] = sep;
    }
    memcpy(buf + newpos, child->seg, slen);
    newpos += slen;
    _path_scan_recurse(t, child, buf, cap, newpos, cb, udata);
}
// 递归遍历 path_scan:用栈缓冲拼接路径
static void _path_scan_recurse(path_trie *t, path_node *node, char *buf, size_t cap, size_t pos,
                           scan_visit_cb cb, void *udata) {
    // 本节点 payload(若有)
    if (NULL != node->payload) {
        buf[pos] = '\0';
        cb(buf, node->payload, udata);
    }
    char sep = t->rules->sep;
    // 精确子节点
    if (NULL != node->children) {
        size_t i = 0;
        void *item;
        while (hashmap_iter(node->children, &i, &item)) {
            _path_scan_visit(t, *(path_node **)item, buf, cap, pos, sep, cb, udata);
        }
    }
    if (NULL != node->plus) {
        _path_scan_visit(t, node->plus, buf, cap, pos, sep, cb, udata);
    }
    if (NULL != node->hash) {
        _path_scan_visit(t, node->hash, buf, cap, pos, sep, cb, udata);
    }
}
void path_scan(path_trie *t, scan_visit_cb cb, void *udata) {
    if (NULL == t || NULL == cb) {
        return;
    }
    char buf[URL_BUF_LENS];
    _path_scan_recurse(t, &t->root, buf, sizeof(buf), 0, cb, udata);
}
