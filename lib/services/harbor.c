#include "services/harbor.h"
#include "protocol/http.h"
#include "utils/router.h"
#include "event/event.h"
#include "utils/binary.h"
#include "utils/utils.h"
#include "containers/hashmap.h"
#include "crypt/hmac.h"

#define HARBOR_SIGN_WINDOW_SEC  (5 * 60)   // 请求时间戳容许的偏差窗口（秒）
#define HARBOR_NONCE_BYTES      16         // nonce 原始字节数（128 bit 防碰撞）
#define HARBOR_NONCE_HEX        (HARBOR_NONCE_BYTES * 2)  // X-Nonce header 字符长度（hex 编码）

// nonce 缓存项：以 hex 字符串形式存储，避免 hex 解码开销
typedef struct nonce_entry_ctx {
    char nonce[HARBOR_NONCE_HEX];
}nonce_entry_ctx;
// harbor 实例上下文（每 task 堆分配，存 task->arg，由 coro_get_arg 取；监听信息 + HMAC 签名上下文）
typedef struct harbor_ctx {
    uint16_t port;          // 监听端口
    struct evssl_ctx *ssl;  // SSL上下文（NULL表示不使用SSL）
    router_ctx *router;     // HTTP 路由器
    uint64_t lsnid;         // 监听ID（ev_unlisten使用）
    hmac_ctx hmac;          // HMAC签名上下文（用于验证请求合法性）
    char ip[IP_LENS];       // 监听IP
    // 双 hashmap 轮转防 nonce 重放：每 HARBOR_SIGN_WINDOW_SEC 切换，实际重放窗口最长 2×SEC
    struct hashmap *nonce_cur;
    struct hashmap *nonce_prev;
    uint64_t window_start;
}harbor_ctx;

static uint64_t _harbor_nonce_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    const nonce_entry_ctx *e = item;
    return hashmap_sip(e->nonce, HARBOR_NONCE_HEX, seed0, seed1);
}
static int _harbor_nonce_compare(const void *a, const void *b, void *udata) {
    (void)udata;
    return memcmp(((const nonce_entry_ctx *)a)->nonce,
                  ((const nonce_entry_ctx *)b)->nonce,
                  HARBOR_NONCE_HEX);
}
/* 时间恒定的十六进制字符串比较（大小写不敏感），防止时序侧信道攻击。
 * 使用 volatile 阻止编译器将循环优化为短路分支。 */
static int32_t _harbor_ct_hexcmp(const char *a, const char *b, size_t n) {
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t ca = (uint8_t)a[i];
        uint8_t cb = (uint8_t)b[i];
        if (ca >= 'a' && ca <= 'f') ca -= 0x20;
        if (cb >= 'a' && cb <= 'f') cb -= 0x20;
        diff |= ca ^ cb;
    }
    return 0 != diff;
}
// 验证请求签名：X-Timestamp 时间窗口 + X-Nonce 防重放 + Authorization HMAC-SHA256 签名
static int32_t _harbor_check_sign(task_ctx *harbor, struct http_pack_ctx *pack, buf_ctx *url, char *reqdata, size_t reqlens) {
    size_t tlens = 0;
    harbor_ctx *ctx = (harbor_ctx *)coro_get_arg(harbor);
    char *tbuf = http_header(pack, "X-Timestamp", &tlens);
    if (EMPTYPTR(tbuf, tlens)) {
        LOG_WARN("not find X-Timestamp.");
        return ERR_FAILED;
    }
    size_t nlens = 0;
    char *nonce_hex = http_header(pack, "X-Nonce", &nlens);
    if (NULL == nonce_hex || HARBOR_NONCE_HEX != nlens) {
        LOG_WARN("not find X-Nonce or invalid length.");
        return ERR_FAILED;
    }
    size_t slens = 0;
    char *sign = http_header(pack, "Authorization", &slens);
    if (EMPTYPTR(sign, slens)) {
        LOG_WARN("not find Authorization.");
        return ERR_FAILED;
    }
    uint64_t tms = strtoull(tbuf, NULL, 10);
    uint64_t tnow = nowsec();
    uint64_t diff = tnow >= tms ? (tnow - tms) : (tms - tnow);
    if (diff >= HARBOR_SIGN_WINDOW_SEC) {
        LOG_WARN("timestamp error.");
        return ERR_FAILED;
    }
    char hs[SHA256_BLOCK_SIZE];
    char hexhs[HEX_ENSIZE(sizeof(hs))];
    hmac_update(&ctx->hmac, url->data, url->lens);
    if (0 != reqlens) {
        hmac_update(&ctx->hmac, reqdata, reqlens);
    }
    hmac_update(&ctx->hmac, tbuf, tlens);
    hmac_update(&ctx->hmac, nonce_hex, nlens);
    hmac_final(&ctx->hmac, hs);
    hmac_reset(&ctx->hmac);
    tohex(hs, sizeof(hs), hexhs);
    int32_t sign_ok = (sizeof(hs) * 2 == slens && 0 == _harbor_ct_hexcmp(sign, hexhs, slens));
    secure_zero(hs, sizeof(hs));
    secure_zero(hexhs, sizeof(hexhs));
    if (!sign_ok) {
        LOG_WARN("check sign failed.");
        return ERR_FAILED;
    }
    // 签名通过后做 nonce 重放检查；窗口轮转：每 HARBOR_SIGN_WINDOW_SEC 切换 cur→prev
    if (tnow - ctx->window_start >= HARBOR_SIGN_WINDOW_SEC) {
        if (NULL != ctx->nonce_prev) {
            hashmap_free(ctx->nonce_prev);
        }
        ctx->nonce_prev = ctx->nonce_cur;
        ctx->nonce_cur = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                                    sizeof(nonce_entry_ctx), 0, 0, 0,
                                                    _harbor_nonce_hash, _harbor_nonce_compare, NULL, NULL);
        ctx->window_start = tnow;
    }
    nonce_entry_ctx key;
    memcpy(key.nonce, nonce_hex, HARBOR_NONCE_HEX);
    if (NULL != hashmap_get(ctx->nonce_cur, &key)
        || (NULL != ctx->nonce_prev && NULL != hashmap_get(ctx->nonce_prev, &key))) {
        LOG_WARN("nonce replay detected.");
        return ERR_FAILED;
    }
    hashmap_set(ctx->nonce_cur, &key);
    return ERR_OK;
}
// 构造并发送 HTTP 响应（有数据时 Content-Type 为 octet-stream，否则 text/plain 状态文本）
// router handler 内自行响应后置 ctx->responded = 1，防 router_dispatch 兜底 500
static void _harbor_respond(router_req *ctx, int32_t code, void *body, size_t lens) {
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    http_pack_resp(&bwriter, code);
    http_pack_head(&bwriter, "Server", "Srey");
    if (NULL != body && lens > 0) {
        http_pack_head(&bwriter, "Content-Type", "application/octet-stream");
        http_pack_content(&bwriter, body, lens);
    } else {
        http_pack_head(&bwriter, "Content-Type", "text/plain");
        const char *erro = http_code_status(code);
        http_pack_content(&bwriter, (void *)erro, strlen(erro));
    }
    ev_send(&ctx->task->loader->netev, ctx->fd, ctx->skid, bwriter.data, bwriter.offset, 0);
    ctx->responded = 1;
}
// 签名 + 参数校验中间件（router_group 承载）：非法请求静默关连接（不暴露），合法则 router_next
static void _harbor_check(router_req *ctx) {
    struct http_pack_ctx *pack = ctx->pack;
    size_t blen = 0;
    char *body = http_data(pack, &blen);
    if (NULL == body
        || 0 == blen
        || 0 != http_chunked(pack)) {
        ev_close(&ctx->task->loader->netev, ctx->fd, ctx->skid, 1);
        ctx->responded = 1;
        return;
    }
    // 签名内容含请求行 url（含 query），从原始状态行取
    buf_ctx *st = http_status(pack);
    if (ERR_OK != _harbor_check_sign(ctx->task, pack, &st[1], body, blen)) {
        ev_close(&ctx->task->loader->netev, ctx->fd, ctx->skid, 1);
        ctx->responded = 1;
        return;
    }
    size_t dn = 0;
    size_t tn = 0;
    if (NULL == router_req_query(ctx, "dst", &dn)
        || NULL == router_req_query(ctx, "type", &tn)) {
        ev_close(&ctx->task->loader->netev, ctx->fd, ctx->skid, 1);
        ctx->responded = 1;
        return;
    }
    router_next(ctx);
}
// POST /call：单向投递（task_call），不等响应
static void _harbor_call(router_req *ctx) {
    size_t dn = 0;
    size_t tn = 0;
    size_t blen = 0;
    const char *ds = router_req_query(ctx, "dst", &dn);
    const char *tp = router_req_query(ctx, "type", &tn);
    void *body = http_data(ctx->pack, &blen);
    name_t dst = (name_t)strtoull(ds, NULL, 10);
    subtype_t type = (subtype_t)strtoul(tp, NULL, 10);
    task_ctx *to = task_grab(ctx->task->loader, dst);
    if (NULL == to) {
        _harbor_respond(ctx, 404, NULL, 0);
        return;
    }
    task_call(to, type, body, blen, 1);
    task_ungrab(to);
    _harbor_respond(ctx, 200, NULL, 0);
}
// POST /request：请求-响应（coro_request，handler 处于协程栈可 yield），回带目标 task 响应
static void _harbor_request(router_req *ctx) {
    size_t dn = 0;
    size_t tn = 0;
    size_t blen = 0;
    const char *ds = router_req_query(ctx, "dst", &dn);
    const char *tp = router_req_query(ctx, "type", &tn);
    void *body = http_data(ctx->pack, &blen);
    name_t dst = (name_t)strtoull(ds, NULL, 10);
    subtype_t type = (subtype_t)strtoul(tp, NULL, 10);
    task_ctx *to = task_grab(ctx->task->loader, dst);
    if (NULL == to) {
        _harbor_respond(ctx, 404, NULL, 0);
        return;
    }
    int32_t err = ERR_FAILED;
    size_t rlen = 0;
    void *rtn = coro_request(to, ctx->task, type, body, blen, 1, &err, &rlen);
    task_ungrab(to);
    if (ERR_OK != err) {
        _harbor_respond(ctx, 400, rtn, rlen);
    } else {
        _harbor_respond(ctx, 200, rtn, rlen);
    }
}
// HTTP 接收回调：完整请求到达后交 router 派发（签名/参数校验在 group 中间件，转发在 handler）
static void _net_recv(task_ctx *task, SOCKET fd, uint64_t skid, subtype_t pktype,
    uint8_t client, uint8_t slice, void *data, size_t size) {
    (void)pktype;
    (void)client;
    (void)size;
    if (0 != slice) {
        return;
    }
    harbor_ctx *ctx = (harbor_ctx *)coro_get_arg(task);
    router_dispatch(ctx->router, task, fd, skid, (struct http_pack_ctx *)data);
}
// harbor任务启动回调：建路由器 + 注册带签名中间件的 group + 监听
static void _harbor_startup(task_ctx *harbor) {
    harbor_ctx *ctx = (harbor_ctx *)coro_get_arg(harbor);
    task_recved(harbor, _net_recv);
    ctx->router = router_new();
    // 参数 + 签名校验统一放 group 中间件，/call 与 /request 共享
    router_define(ctx->router, "sign", _harbor_check);
    const char *mws[] = { "sign" };
    router_group g;
    router_group_root(ctx->router, &g, "", mws, 1);
    router_post(ctx->router, &g, "/call", _harbor_call, NULL, 0);
    router_post(ctx->router, &g, "/request", _harbor_request, NULL, 0);
    if (ERR_OK != task_listen(harbor, PACK_HTTP, ctx->ssl, ctx->ip, ctx->port, &ctx->lsnid, 0)) {
        LOG_ERROR("task_listen %s:%d error", ctx->ip, ctx->port);
    }
}
static void _harbor_free(void *arg) {
    if (NULL == arg) {
        return;
    }
    harbor_ctx *ctx = (harbor_ctx *)arg;
    if (NULL != ctx->router) {
        router_free(ctx->router);
    }
    hmac_free(&ctx->hmac);
    if (NULL != ctx->nonce_cur) {
        hashmap_free(ctx->nonce_cur);
        ctx->nonce_cur = NULL;
    }
    if (NULL != ctx->nonce_prev) {
        hashmap_free(ctx->nonce_prev);
        ctx->nonce_prev = NULL;
    }
    FREE(ctx);
}
// harbor任务关闭回调：取消监听
static void _harbor_closing(task_ctx *harbor) {
    harbor_ctx *ctx = (harbor_ctx *)coro_get_arg(harbor);
    if (NULL == ctx) {
        return;
    }
    if (0 != ctx->lsnid) {
        ev_unlisten(&harbor->loader->netev, ctx->lsnid);
        ctx->lsnid = 0;
    }
}
int32_t harbor_start(loader_ctx *loader, const char *tname, const char *ssl, const char *ip, uint16_t port, const char *key) {
    if (EMPTYSTR(tname) || 0 == port) {
        return ERR_OK;
    }
    if (NULL == ip || NULL == key) {
        return ERR_FAILED;
    }
    size_t klens = strlen(key);
    size_t iplens = strlen(ip);
    if (0 == klens || iplens >= IP_LENS) {
        return ERR_FAILED;
    }
    harbor_ctx *ctx;
    CALLOC(ctx, 1, sizeof(harbor_ctx));
    ctx->port = port;
#if WITH_SSL
    ctx->ssl = evssl_qury(ssl);
#else
    (void)ssl;
#endif
    hmac_init(&ctx->hmac, DG_SHA256, key, klens);
    safe_fill_str(ctx->ip, sizeof(ctx->ip), ip);
    ctx->nonce_cur = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                                sizeof(nonce_entry_ctx), 0, 0, 0,
                                                _harbor_nonce_hash, _harbor_nonce_compare, NULL, NULL);
    ctx->window_start = nowsec();
    if (NULL == coro_task_register(loader, tname, 4 * ONEK,
                                   _harbor_startup, _harbor_closing,
                                   _harbor_free, ctx)) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
// 为 HTTP 请求头添加 X-Timestamp / X-Nonce / Authorization；
// 签名内容 = url + data + timestamp + nonce_hex；nonce 由 CSPRNG 生成防重放
static int32_t _harbor_sign(binary_ctx *bwriter, const char *key, const char *url, void *data, size_t size) {
    size_t klens = strlen(key);
    if (0 == klens) {
        return ERR_OK;
    }
    uint8_t nonce_raw[HARBOR_NONCE_BYTES];
    char nonce_hex[HARBOR_NONCE_HEX + 1] = { 0 };
    if (ERR_OK != csprng_rand(nonce_raw, sizeof(nonce_raw))) {
        LOG_ERROR("csprng_rand failed, abort harbor sign.");
        return ERR_FAILED;
    }
    tohex(nonce_raw, sizeof(nonce_raw), nonce_hex);
    secure_zero(nonce_raw, sizeof(nonce_raw));
    char tms[64];
    SNPRINTF(tms, sizeof(tms), "%"PRIu64, nowsec());
    size_t ulens = strlen(url);
    size_t tslens = strlen(tms);
    char hs[SHA256_BLOCK_SIZE];
    char hexhs[HEX_ENSIZE(sizeof(hs))];
    hmac_ctx hmac;
    hmac_init(&hmac, DG_SHA256, key, klens);
    hmac_update(&hmac, url, ulens);
    if (0 != size) {
        hmac_update(&hmac, data, size);
    }
    hmac_update(&hmac, tms, tslens);
    hmac_update(&hmac, nonce_hex, HARBOR_NONCE_HEX);
    hmac_final(&hmac, hs);
    hmac_free(&hmac);
    tohex(hs, sizeof(hs), hexhs);
    secure_zero(hs, sizeof(hs));
    http_pack_head(bwriter, "X-Timestamp", tms);
    http_pack_head(bwriter, "X-Nonce", nonce_hex);
    http_pack_head(bwriter, "Authorization", hexhs);
    secure_zero(hexhs, sizeof(hexhs));
    secure_zero(nonce_hex, sizeof(nonce_hex));
    return ERR_OK;
}
void *harbor_pack(name_t task, int32_t call, subtype_t reqtype, const char *key, void *data, size_t size, size_t *lens) {
    char url[512];
    if (0 != call) {
        SNPRINTF(url, sizeof(url), "/call?dst=%"PRIu64"&type=%u", task, reqtype);
    } else {
        SNPRINTF(url, sizeof(url), "/request?dst=%"PRIu64"&type=%u", task, reqtype);
    }
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    http_pack_req(&bwriter, "POST", url);
    http_pack_head(&bwriter, "Connection", "Keep-Alive");
    http_pack_head(&bwriter, "Content-Type", "application/octet-stream");
    if (ERR_OK != _harbor_sign(&bwriter, key, url, data, size)) {
        binary_free(&bwriter);
        *lens = 0;
        return NULL;
    }
    http_pack_content(&bwriter, data, size);
    *lens = bwriter.offset;
    return bwriter.data;
}
