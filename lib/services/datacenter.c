#include "services/datacenter.h"
#include "srey/task.h"
#include "containers/hashmap.h"
#include "utils/binary.h"
#include "utils/utils.h"
#include "base/macro.h"

// klen 不能太大;栈上字节足够大多数 key,超过直接拒绝
#define DC_KEY_MAX 512
// 子命令操作码:payload 首字节,跟随特定子命令格式的剩余字节
// payload 布局:
// u8 op   + u16 key len(网络序) + key + u32 val len(网络字节序) + val
typedef enum dc_op {
    DC_OP_SET  = 0x01,
    DC_OP_GET  = 0x02,
    DC_OP_WAIT = 0x03,
    DC_OP_DEL  = 0x04,
    DC_OP_LIST = 0x05,
}dc_op;
// 内部存储 entry:key/val 都由 dc_ctx 拷贝持有,删除时统一 FREE
typedef struct dc_entry {
    size_t size;   // val 字节数
    char *key;     // strdup
    void *val;     // MALLOC + memcpy;NULL 表示软清空
} dc_entry;
// 单个 waiter:src+sess 标识一个挂起协程,set 时通过 task_response 唤醒
typedef struct dc_waiter {
    name_t src;              // 请求方 task 句柄
    uint64_t sess;           // 会话 id(task_response 唤醒用)
    struct dc_waiter *next;  // 同 key FIFO 链表 next
} dc_waiter;
// 单个 key 的 waiter FIFO 队列
typedef struct dc_pending {
    char *key;          // strdup,作为 hashmap key
    dc_waiter *head;    // 队头(最早等待者)
    dc_waiter *tail;    // 队尾(最新追加)
} dc_pending;
// DataCenter task 的 arg 上下文
typedef struct dc_ctx {
    loader_ctx *loader;       // 所属 loader
    struct hashmap *kv;       // 元素 sizeof(dc_entry),by-value 存
    struct hashmap *pending;  // 元素 sizeof(dc_pending),by-value 存
} dc_ctx;

// ── hashmap hash / compare / free 回调 ─────────────────────────────────────
static uint64_t _dc_kv_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    (void)seed0;
    (void)seed1;
    const dc_entry *e = (const dc_entry *)item;
    return hash(e->key, strlen(e->key));
}
static int _dc_kv_compare(const void *a, const void *b, void *ud) {
    (void)ud;
    return strcmp(((const dc_entry *)a)->key, ((const dc_entry *)b)->key);
}
static void _dc_kv_free(void *item) {
    dc_entry *e = (dc_entry *)item;
    FREE(e->key);
    FREE(e->val);
}
static uint64_t _dc_pending_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    (void)seed0;
    (void)seed1;
    const dc_pending *p = (const dc_pending *)item;
    return hash(p->key, strlen(p->key));
}
static int _dc_pending_compare(const void *a, const void *b, void *ud) {
    (void)ud;
    return strcmp(((const dc_pending *)a)->key, ((const dc_pending *)b)->key);
}
static void _dc_pending_free(void *item) {
    dc_pending *p = (dc_pending *)item;
    FREE(p->key);
    // waiter 链表由调用方在 take 时摘走;走到这里说明 hashmap_free/clear 强制清理,
    // 此时 waiter 协程已无人唤醒(loader 关闭),释放节点不发 response
    dc_waiter *w = p->head;
    while (w) {
        dc_waiter *next = w->next;
        FREE(w);
        w = next;
    }
}
// 写 kv:已存在 → 释放旧 val + 替换新 val;不存在 → 新建。key 内部 strdup,val MALLOC 拷贝
static void _dc_kv_set(dc_ctx *ctx, const char *key, void *val, size_t size) {
    dc_entry query;
    query.key = (char *)key;
    dc_entry *found = (dc_entry *)hashmap_get(ctx->kv, &query);
    void *new_val = NULL;
    if (val && size > 0) {
        MALLOC(new_val, size);
        memcpy(new_val, val, size);
    }
    if (found) {
        // 已存在:释放旧 val,原地改 val/size(key 复用)
        FREE(found->val);
        found->val = new_val;
        found->size = size;
        return;
    }
    // 新建:key 独立 strdup
    dc_entry e;
    size_t klen = strlen(key);
    MALLOC(e.key, klen + 1);
    safe_fill_str(e.key, klen + 1, key);
    e.val = new_val;
    e.size = size;
    hashmap_set(ctx->kv, &e);
}
// 读 kv:返回 hashmap 内部 dc_entry 指针(不要修改),不存在返回 NULL
static dc_entry *_dc_kv_get(dc_ctx *ctx, const char *key) {
    dc_entry query;
    query.key = (char *)key;
    return (dc_entry *)hashmap_get(ctx->kv, &query);
}
// 删 kv:hashmap_delete 返回 spare 副本但不会自动调 elfree,需手动 _dc_kv_free
static void _dc_kv_del(dc_ctx *ctx, const char *key) {
    dc_entry query;
    query.key = (char *)key;
    dc_entry *removed = (dc_entry *)hashmap_delete(ctx->kv, &query);
    if (NULL != removed) {
        _dc_kv_free(removed);
    }
}
// push waiter 到 pending[key];key 不存在则新建 dc_pending 节点
static void _dc_pending_push(dc_ctx *ctx, const char *key, dc_waiter *w) {
    dc_pending query;
    query.key = (char *)key;
    dc_pending *found = (dc_pending *)hashmap_get(ctx->pending, &query);
    if (found) {
        if (found->tail) {
            found->tail->next = w;
            found->tail = w;
        } else {
            found->head = found->tail = w;
        }
        return;
    }
    // 新建
    dc_pending p;
    size_t klen = strlen(key);
    MALLOC(p.key, klen + 1);
    safe_fill_str(p.key, klen + 1, key);
    p.head = p.tail = w;
    hashmap_set(ctx->pending, &p);
}
// 摘下 pending[key] 整条 FIFO 队列并从 hashmap 删除节点,返回头指针(调用方负责 FREE 链表)
static dc_waiter *_dc_pending_take(dc_ctx *ctx, const char *key) {
    dc_pending query;
    query.key = (char *)key;
    dc_pending *found = (dc_pending *)hashmap_get(ctx->pending, &query);
    if (!found) {
        return NULL;
    }
    dc_waiter *head = found->head;
    found->head = found->tail = NULL;  // 摘空,避免 _dc_pending_free 再次释放链表
    // hashmap_delete 不自动调 elfree,需手动 _dc_pending_free 释放 key
    dc_pending *removed = (dc_pending *)hashmap_delete(ctx->pending, &query);
    if (NULL != removed) {
        _dc_pending_free(removed);
    }
    return head;
}
// 把 key bytes 复制为 NUL 结尾 string(hashmap 比较走 strcmp,要求 NUL 终止)
static int _dc_key_to_cstr(const void *src, size_t len, char *dst, size_t dst_cap) {
    if (0 == len || len + 1 > dst_cap) {
        return ERR_FAILED;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
    return ERR_OK;
}
// 失败响应 helper:统一回 ERR_FAILED 给 src,无数据
static void _dc_resp_failed(dc_ctx *ctx, name_t src, uint64_t sess) {
    if (INVALID_TNAME == src) {
        return;
    }
    task_ctx *src_task = task_grab(ctx->loader, src);
    if (NULL != src_task) {
        task_response(src_task, sess, ERR_FAILED, NULL, 0, 0);
        task_ungrab(src_task);
    }
}
// 从 binary_ctx 读 | u16 klen | key bytes |,复制到 NUL 结尾 keybuf。SET/GET/WAIT/DEL 共用
static int _dc_read_key(binary_ctx *br, char *keybuf, size_t cap) {
    if (br->size - br->offset < 2) {
        return ERR_FAILED;
    }
    uint16_t klen = (uint16_t)binary_get_uinteger(br, 2, 0);  // u16 klen 网络序
    if (br->size - br->offset < (size_t)klen) {
        return ERR_FAILED;
    }
    char *raw = binary_get_string(br, klen);
    return _dc_key_to_cstr(raw, klen, keybuf, cap);
}
// SET body 格式:| u16 klen | key | u32 vlen | val |
static void _dc_handle_set(dc_ctx *ctx, name_t src, uint64_t sess, binary_ctx *br) {
    char keybuf[DC_KEY_MAX];
    if (ERR_OK != _dc_read_key(br, keybuf, sizeof(keybuf))) {
        _dc_resp_failed(ctx, src, sess);
        return;
    }
    if (br->size - br->offset < 4) {
        _dc_resp_failed(ctx, src, sess);
        return;
    }
    uint32_t vlen = (uint32_t)binary_get_uinteger(br, 4, 0);  // u32 vlen 网络序
    if (br->size - br->offset < (size_t)vlen) {
        _dc_resp_failed(ctx, src, sess);
        return;
    }
    size_t vsize = vlen;
    void *val = (vsize > 0) ? (void *)binary_get_string(br, vsize) : NULL;
    // 1. 写 kv
    _dc_kv_set(ctx, keybuf, val, vsize);
    // 2. 回 setter:OK(fire-and-forget 跳过)
    if (INVALID_TNAME != src) {
        task_ctx *src_task = task_grab(ctx->loader, src);
        if (NULL != src_task) {
            task_response(src_task, sess, ERR_OK, NULL, 0, 0);
            task_ungrab(src_task);
        }
    }
    // 3. 摘下 pending[key] 并唤醒所有 waiter(waiter 来自其它协程的 wait,有独立 src/sess,不受本请求 src 影响)
    dc_waiter *w = _dc_pending_take(ctx, keybuf);
    while (w) {
        dc_waiter *next = w->next;
        task_ctx *wt = task_grab(ctx->loader, w->src);
        if (wt) {
            task_response(wt, w->sess, ERR_OK, val, vsize, 1);  // copy=1
            task_ungrab(wt);
        }
        FREE(w);
        w = next;
    }
}
// GET body 格式:| u16 klen | key |
static void _dc_handle_get(dc_ctx *ctx, name_t src, uint64_t sess, binary_ctx *br) {
    if (INVALID_TNAME == src) {
        return;  // fire-and-forget:无人接响应,纯读操作直接早返
    }
    char keybuf[DC_KEY_MAX];
    if (ERR_OK != _dc_read_key(br, keybuf, sizeof(keybuf))) {
        _dc_resp_failed(ctx, src, sess);
        return;
    }
    dc_entry *e = _dc_kv_get(ctx, keybuf);
    task_ctx *src_task = task_grab(ctx->loader, src);
    if (NULL != src_task) {
        if (e && e->val) {
            task_response(src_task, sess, ERR_OK, e->val, e->size, 1);
        } else {
            task_response(src_task, sess, ERR_OK, NULL, 0, 0);
        }
        task_ungrab(src_task);
    }
}
// WAIT body 格式:| u16 klen | key |;命中立即返回,未命中挂 pending
static void _dc_handle_wait(dc_ctx *ctx, name_t src, uint64_t sess, binary_ctx *br) {
    if (INVALID_TNAME == src) {
        return;  // fire-and-forget:命中也无人接,未命中也不能挂 pending(永远无法唤醒,内存泄漏)
    }
    char keybuf[DC_KEY_MAX];
    if (ERR_OK != _dc_read_key(br, keybuf, sizeof(keybuf))) {
        _dc_resp_failed(ctx, src, sess);
        return;
    }
    dc_entry *e = _dc_kv_get(ctx, keybuf);
    if (e && e->val) {
        // 命中,立即返回
        task_ctx *src_task = task_grab(ctx->loader, src);
        if (NULL != src_task) {
            task_response(src_task, sess, ERR_OK, e->val, e->size, 1);
            task_ungrab(src_task);
        }
        return;
    }
    // 未命中:塞入 pending,不发 response → src 业务协程继续 yield,超时由其自身 coro_request 超时机制兜底
    dc_waiter *w;
    MALLOC(w, sizeof(dc_waiter));
    w->src = src;
    w->sess = sess;
    w->next = NULL;
    _dc_pending_push(ctx, keybuf, w);
}
// DEL body 格式:| u16 klen | key |
static void _dc_handle_del(dc_ctx *ctx, name_t src, uint64_t sess, binary_ctx *br) {
    char keybuf[DC_KEY_MAX];
    if (ERR_OK != _dc_read_key(br, keybuf, sizeof(keybuf))) {
        _dc_resp_failed(ctx, src, sess);
        return;
    }
    _dc_kv_del(ctx, keybuf);  // 不影响 _pending,语义上"撤回真值"
    // 回 ack:OK(fire-and-forget 跳过)
    if (INVALID_TNAME != src) {
        task_ctx *src_task = task_grab(ctx->loader, src);
        if (NULL != src_task) {
            task_response(src_task, sess, ERR_OK, NULL, 0, 0);
            task_ungrab(src_task);
        }
    }
}
static bool _dc_iter(const void *item, void *udata) {
    const dc_entry *e = (const dc_entry *)item;
    binary_ctx *bw = (binary_ctx *)udata;
    size_t klen = strlen(e->key);
    binary_set_uinteger(bw, (uint64_t)klen, 2, 0);
    binary_set_string(bw, e->key, klen);
    return true;
}
static void _dc_handle_list(dc_ctx *ctx, name_t src, uint64_t sess) {
    if (INVALID_TNAME == src) {
        return;  // fire-and-forget:跳过整个 scan,无人接响应
    }
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    hashmap_scan(ctx->kv, _dc_iter, &bw);
    task_ctx *src_task = task_grab(ctx->loader, src);
    if (NULL == src_task) {
        binary_free(&bw);
        return;
    }
    if (bw.offset > 0) {
        // copy=0 转移所有权,response 投递完成后由 _message_clean → FREE 释放 bw.data
        task_response(src_task, sess, ERR_OK, bw.data, bw.offset, 0);
    } else {
        task_response(src_task, sess, ERR_OK, NULL, 0, 0);
        binary_free(&bw);
    }
    task_ungrab(src_task);
}
// 中心 dispatch:reqtype 锁定 REQ_DC,剥 payload 首字节 op 后子分发到 5 个 handler
static void _dc_requested(task_ctx *task, uint8_t reqtype, uint64_t sess, name_t src,
                          void *data, size_t size) {
    (void)reqtype;
    dc_ctx *ctx = (dc_ctx *)task->arg;
    // payload 至少 1 字节 op;前置自检避免 binary_get_* 内部 ASSERTAB abort 远端构造的非法包
    if (size < 1 || NULL == data) {
        _dc_resp_failed(ctx, src, sess);
        return;
    }
    binary_ctx br;
    binary_init(&br, (char *)data, size, 0);  // 外部托管,只读
    uint8_t op = binary_get_uint8(&br);
    switch (op) {
    case DC_OP_SET:
        _dc_handle_set(ctx, src, sess, &br);
        break;
    case DC_OP_GET:
        _dc_handle_get(ctx, src, sess, &br);
        break;
    case DC_OP_WAIT:
        _dc_handle_wait(ctx, src, sess, &br);
        break;
    case DC_OP_DEL:
        _dc_handle_del(ctx, src, sess, &br);
        break;
    case DC_OP_LIST:
        _dc_handle_list(ctx, src, sess);
        break;
    default:
        // 未知 op:回 ERR_FAILED
        _dc_resp_failed(ctx, src, sess);
        break;
    }
}
// task 关闭回调:唤醒所有 pending waiter(ERR_FAILED) + 释放 hashmap
static bool _dc_pending_close_iter(const void *item, void *udata) {
    const dc_pending *p = (const dc_pending *)item;
    dc_ctx *ctx = (dc_ctx *)udata;
    dc_waiter *w = p->head;
    while (w) {
        task_ctx *wt = task_grab(ctx->loader, w->src);
        if (wt) {
            task_response(wt, w->sess, ERR_FAILED, NULL, 0, 0);
            task_ungrab(wt);
        }
        w = w->next;
    }
    return true;
}
static void _dc_closing(task_ctx *task) {
    dc_ctx *ctx = (dc_ctx *)task->arg;
    // 唤醒所有 pending waiter(ERR_FAILED),避免业务协程死等
    hashmap_scan(ctx->pending, _dc_pending_close_iter, ctx);
}
static void _dc_free(void *arg) {
    if (NULL == arg) {
        return;
    }
    dc_ctx *ctx = (dc_ctx *)arg;
    hashmap_free(ctx->kv);
    hashmap_free(ctx->pending);
    FREE(ctx);
}
// 公开 API:注册 DataCenter task service。name 为 NULL/空串视为"不启动 DataCenter",返回 ERR_OK 跳过
int32_t dc_start(loader_ctx *loader, const char *name) {
    if (EMPTYSTR(name)) {
        return ERR_OK;
    }
    dc_ctx *ctx;
    CALLOC(ctx, 1, sizeof(dc_ctx));
    ctx->loader = loader;
    ctx->kv = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                         sizeof(dc_entry), ONEK, 0, 0,
                                         _dc_kv_hash, _dc_kv_compare, _dc_kv_free, NULL);
    ctx->pending = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                              sizeof(dc_pending), ONEK, 0, 0,
                                              _dc_pending_hash, _dc_pending_compare, _dc_pending_free, NULL);
    task_ctx *task = task_new(loader, name, 4 * ONEK, NULL, _dc_free, ctx);
    task_requested(task, _dc_requested);
    if (ERR_OK != task_register(task, NULL, _dc_closing)) {
        // task_register 失败时 task 未进 maptasks,需手动 task_free;
        task_free(task);
        return ERR_FAILED;
    }
    return ERR_OK;
}
// 统一 payload 构造:| u8 op | <子命令字段> |
//   SET:           u16 keylen(网络序) + key + val(可选)
//   GET/WAIT/DEL:  key
//   LIST:          (无 body)
// 返回 binary 内部 MALLOC 的 buf,所有权转给调用方;调用方 copy=0 传给 task_request,
// 由 _message_clean → FREE 释放(binary 内部用 MALLOC,与 _message_clean 的 FREE 配对)。
static char *_dc_pack(dc_op op, const char *key, void *val, size_t vsize, size_t *out_total) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    binary_set_uint8(&bw, (uint8_t)op);
    switch (op) {
    case DC_OP_SET: {
        size_t klen = strlen(key);
        // val=NULL 时强制 vlen=0,避免"vlen 声称 N 但实际不写 val 字节"造成协议不一致
        size_t real_vsize = (NULL != val) ? vsize : 0;
        binary_set_uinteger(&bw, (uint64_t)klen, 2, 0);  // u16 klen 网络序
        binary_set_string(&bw, key, klen);
        binary_set_uinteger(&bw, (uint64_t)real_vsize, 4, 0);  // u32 vlen 网络序(总写,空 val 也写 0)
        if (real_vsize > 0) {
            binary_set_string(&bw, (const char *)val, real_vsize);
        }
        break;
    }
    case DC_OP_GET:
    case DC_OP_WAIT:
    case DC_OP_DEL: {
        size_t klen = strlen(key);
        binary_set_uinteger(&bw, (uint64_t)klen, 2, 0);  // u16 klen 网络序
        binary_set_string(&bw, key, klen);
        break;
    }
    case DC_OP_LIST:
        // 只写 op,无 body
        break;
    }
    *out_total = bw.offset;
    return bw.data;
}
// ── 业务侧 helper:C 端业务通过这些函数调 DataCenter,内部包 coro_request ──
// 所有 reqtype 统一为 REQ_DC,子命令由 _dc_pack 写入 payload 首字节
int32_t coro_dc_set(task_ctx *task, name_t dc_name, const char *key, void *val, size_t size) {
    ASSERTAB(!EMPTYSTR(key), "datacenter key empty");
    ASSERTAB(size <= UINT32_MAX, "datacenter val size exceeds UINT32_MAX");
    task_ctx *dc = task_grab(task->loader, dc_name);
    if (NULL == dc) {
        return ERR_FAILED;
    }
    size_t total;
    char *buf = _dc_pack(DC_OP_SET, key, val, size, &total);
    int32_t erro = 0;
    size_t rsize = 0;
    coro_request(dc, task, REQ_DC, buf, total, 0, &erro, &rsize);  // copy=0 转移所有权
    task_ungrab(dc);
    return erro;
}
void *coro_dc_get(task_ctx *task, name_t dc_name, const char *key, size_t *size) {
    ASSERTAB(!EMPTYSTR(key), "datacenter key empty");
    task_ctx *dc = task_grab(task->loader, dc_name);
    if (NULL == dc) {
        SET_PTR(size, 0);
        return NULL;
    }
    size_t total;
    char *buf = _dc_pack(DC_OP_GET, key, NULL, 0, &total);
    int32_t erro = 0;
    void *resp = coro_request(dc, task, REQ_DC, buf, total, 0, &erro, size);  // copy=0 转移所有权
    task_ungrab(dc);
    if (ERR_OK != erro) {
        SET_PTR(size, 0);
        return NULL;
    }
    return resp;
}
void *coro_dc_wait(task_ctx *task, name_t dc_name, const char *key, size_t *size) {
    ASSERTAB(!EMPTYSTR(key), "datacenter key empty");
    task_ctx *dc = task_grab(task->loader, dc_name);
    if (NULL == dc) {
        SET_PTR(size, 0);
        return NULL;
    }
    size_t total;
    char *buf = _dc_pack(DC_OP_WAIT, key, NULL, 0, &total);
    int32_t erro = 0;
    void *resp = coro_request(dc, task, REQ_DC, buf, total, 0, &erro, size);  // copy=0 转移所有权
    task_ungrab(dc);
    if (ERR_OK != erro) {
        SET_PTR(size, 0);
        return NULL;
    }
    return resp;
}
int32_t coro_dc_del(task_ctx *task, name_t dc_name, const char *key) {
    ASSERTAB(!EMPTYSTR(key), "datacenter key empty");
    task_ctx *dc = task_grab(task->loader, dc_name);
    if (NULL == dc) {
        return ERR_FAILED;
    }
    size_t total;
    char *buf = _dc_pack(DC_OP_DEL, key, NULL, 0, &total);
    int32_t erro = 0;
    size_t rsize = 0;
    coro_request(dc, task, REQ_DC, buf, total, 0, &erro, &rsize);  // copy=0 转移所有权
    task_ungrab(dc);
    return erro;
}
void *coro_dc_keys(task_ctx *task, name_t dc_name, size_t *size) {
    task_ctx *dc = task_grab(task->loader, dc_name);
    if (NULL == dc) {
        SET_PTR(size, 0);
        return NULL;
    }
    size_t total;
    char *buf = _dc_pack(DC_OP_LIST, NULL, NULL, 0, &total);
    int32_t erro = 0;
    void *resp = coro_request(dc, task, REQ_DC, buf, total, 0, &erro, size);  // copy=0 转移所有权
    task_ungrab(dc);
    if (ERR_OK != erro) {
        SET_PTR(size, 0);
        return NULL;
    }
    return resp;
}
// ── 无协程版本:直接 task_request 不挂起;sess=0 走 fire-and-forget(src=NULL),sess!=0 业务自管响应配对 ──
// 所有 reqtype 统一为 REQ_DC,子命令由 _dc_pack 写入 payload 首字节;copy=0 转移所有权
int32_t dc_set(task_ctx *task, name_t dc_name, uint64_t sess, const char *key, void *val, size_t size) {
    ASSERTAB(!EMPTYSTR(key), "datacenter key empty");
    ASSERTAB(size <= UINT32_MAX, "datacenter val size exceeds UINT32_MAX");
    task_ctx *dc = task_grab(task->loader, dc_name);
    if (NULL == dc) {
        return ERR_FAILED;
    }
    size_t total;
    char *buf = _dc_pack(DC_OP_SET, key, val, size, &total);
    if (0 == sess) {
        task_call(dc, REQ_DC, buf, total, 0);
    } else {
        task_request(dc, task, REQ_DC, sess, buf, total, 0);
    }
    task_ungrab(dc);
    return ERR_OK;
}
int32_t dc_del(task_ctx *task, name_t dc_name, uint64_t sess, const char *key) {
    ASSERTAB(!EMPTYSTR(key), "datacenter key empty");
    task_ctx *dc = task_grab(task->loader, dc_name);
    if (NULL == dc) {
        return ERR_FAILED;
    }
    size_t total;
    char *buf = _dc_pack(DC_OP_DEL, key, NULL, 0, &total);
    if (0 == sess) {
        task_call(dc, REQ_DC, buf, total, 0);
    } else {
        task_request(dc, task, REQ_DC, sess, buf, total, 0);
    }
    task_ungrab(dc);
    return ERR_OK;
}
int32_t dc_get(task_ctx *task, name_t dc_name, uint64_t sess, const char *key) {
    ASSERTAB(!EMPTYSTR(key), "datacenter key empty");
    task_ctx *dc = task_grab(task->loader, dc_name);
    if (NULL == dc) {
        return ERR_FAILED;
    }
    size_t total;
    char *buf = _dc_pack(DC_OP_GET, key, NULL, 0, &total);
    if (0 == sess) {
        task_call(dc, REQ_DC, buf, total, 0);
    } else {
        task_request(dc, task, REQ_DC, sess, buf, total, 0);
    }
    task_ungrab(dc);
    return ERR_OK;
}
int32_t dc_wait(task_ctx *task, name_t dc_name, uint64_t sess, const char *key) {
    ASSERTAB(!EMPTYSTR(key), "datacenter key empty");
    task_ctx *dc = task_grab(task->loader, dc_name);
    if (NULL == dc) {
        return ERR_FAILED;
    }
    size_t total;
    char *buf = _dc_pack(DC_OP_WAIT, key, NULL, 0, &total);
    if (0 == sess) {
        task_call(dc, REQ_DC, buf, total, 0);
    } else {
        task_request(dc, task, REQ_DC, sess, buf, total, 0);
    }
    task_ungrab(dc);
    return ERR_OK;
}
int32_t dc_keys(task_ctx *task, name_t dc_name, uint64_t sess) {
    task_ctx *dc = task_grab(task->loader, dc_name);
    if (NULL == dc) {
        return ERR_FAILED;
    }
    size_t total;
    char *buf = _dc_pack(DC_OP_LIST, NULL, NULL, 0, &total);
    if (0 == sess) {
        task_call(dc, REQ_DC, buf, total, 0);
    } else {
        task_request(dc, task, REQ_DC, sess, buf, total, 0);
    }
    task_ungrab(dc);
    return ERR_OK;
}
