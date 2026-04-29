#include "services/harbor.h"
#include "protocol/urlparse.h"
#include "protocol/http.h"
#include "utils/binary.h"
#include "crypt/hmac.h"

// harbor全局上下文（单例，监听信息+HMAC签名上下文）
typedef struct harbor_ctx {
    uint16_t port;          // 监听端口
    struct evssl_ctx *ssl;  // SSL上下文（NULL表示不使用SSL）
    uint64_t lsnid;         // 监听ID（ev_unlisten使用）
    hmac_ctx hmac;          // HMAC签名上下文（用于验证请求合法性）
    char ip[IP_LENS];       // 监听IP
}harbor_ctx;

static harbor_ctx _harbor; // harbor全局实例

// 构造并发送HTTP响应（有数据时Content-Type为octet-stream，否则为text/plain状态文本）
static void _harbor_response(task_ctx *harbor, SOCKET fd, uint64_t skid, char *respdata, size_t lens, int32_t code) {
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    http_pack_resp(&bwriter, code);
    http_pack_head(&bwriter, "Server", "Srey");
    if (NULL != respdata
        && lens > 0) {
        http_pack_head(&bwriter, "Content-Type", "application/octet-stream");
        http_pack_content(&bwriter, respdata, lens);
    } else {
        http_pack_head(&bwriter, "Content-Type", "text/plain");
        const char *erro = http_code_status(code);
        http_pack_content(&bwriter, (void *)erro, strlen(erro));
    }
    ev_send(&harbor->loader->netev, fd, skid, bwriter.data, bwriter.offset, 0);
}
// 验证请求签名：检查X-Timestamp时间窗口（5分钟内）及Authorization HMAC-SHA256签名
static int32_t _check_sign(struct http_pack_ctx *pack, buf_ctx *url, char *reqdata, size_t reqlens) {
    size_t tlens = 0;
    char *tbuf = http_header(pack, "X-Timestamp", &tlens);
    if (NULL == tbuf
        || 0 == tlens) {
        LOG_WARN("not find X-Timestamp.");
        return ERR_FAILED;
    }
    size_t slens = 0;
    char *sign = http_header(pack, "Authorization", &slens);
    if (NULL == sign
        || 0 == slens) {
        LOG_WARN("not find Authorization.");
        return ERR_FAILED;
    }
    uint64_t tms = (uint64_t)atoll(tbuf);
    uint64_t tnow = nowsec();
    uint64_t diff = tnow >= tms ? (tnow - tms) : (tms - tnow);
    if (diff >= 5 * 60) {
        LOG_WARN("timestamp error.");
        return ERR_FAILED;
    }
    char *signstr;
    size_t total = url->lens + reqlens + tlens;
    MALLOC(signstr, total);
    memcpy(signstr, url->data, url->lens);
    if (0 != reqlens) {
        memcpy(signstr + url->lens, reqdata, reqlens);
    }
    memcpy(signstr + url->lens + reqlens, tbuf, tlens);
    char hs[SHA256_BLOCK_SIZE];
    char hexhs[HEX_ENSIZE(sizeof(hs))];
    hmac_update(&_harbor.hmac, signstr, total);
    hmac_final(&_harbor.hmac, hs);
    hmac_reset(&_harbor.hmac);
    tohex(hs, sizeof(hs), hexhs);
    FREE(signstr);
    if (strlen(hexhs) != slens
        || 0 != _memicmp(sign, hexhs, slens)) {
        LOG_WARN("check sign failed.");
        return ERR_FAILED;
    }
    return ERR_OK;
}
// harbor HTTP接收回调：解析HTTP请求，验证签名，路由到call或request处理
static void _harbor_net_recv(task_ctx *harbor, SOCKET fd, uint64_t skid, uint8_t pktype,
    uint8_t client, uint8_t slice, void *data, size_t size) {
    (void)pktype;
    (void)client;
    (void)slice;
    (void)size;
    size_t lens;
    char *reqdata = http_data(data, &lens);
    if (NULL == reqdata
        || 0 == lens
        || 0 != http_chunked(data)) {
        ev_close(&harbor->loader->netev, fd, skid);
        return;
    }
    buf_ctx *hstatus = http_status(data);
    if (!buf_icompare(&hstatus[0], "post", strlen("post"))) {
        ev_close(&harbor->loader->netev, fd, skid);
        return;
    }
    url_ctx url;
    url_parse(&url, hstatus[1].data, hstatus[1].lens);
    if (buf_empty(&url.path)) {
        ev_close(&harbor->loader->netev, fd, skid);
        return;
    }
    if (ERR_OK != _check_sign(data, &hstatus[1], reqdata, lens)) {
        ev_close(&harbor->loader->netev, fd, skid);
        return;
    }
    buf_ctx *bdst = url_get_param(&url, "dst");
    if (buf_empty(bdst)) {
        ev_close(&harbor->loader->netev, fd, skid);
        return;
    }
    buf_ctx *reqtype = url_get_param(&url, "type");
    if (buf_empty(reqtype)) {
        ev_close(&harbor->loader->netev, fd, skid);
        return;
    }
    name_t dst = (name_t)strtol(bdst->data, NULL, 10);
    uint8_t type = (uint8_t)strtol(reqtype->data, NULL, 10);
    if (buf_icompare(&url.path, "call", strlen("call"))) {
        task_ctx *to = task_grab(harbor->loader, dst);
        if (NULL == to) {
            _harbor_response(harbor, fd, skid, NULL, 0, 404);
            return;
        }
        task_call(to, type, reqdata, lens, 1);
        task_ungrab(to);
        _harbor_response(harbor, fd, skid, NULL, 0, 200);
    } else if (buf_icompare(&url.path, "request", strlen("request"))) {
        task_ctx *to = task_grab(harbor->loader, dst);
        if (NULL == to) {
            _harbor_response(harbor, fd, skid, NULL, 0, 404);
            return;
        }
        int32_t err;
        void *rtn = coro_request(to, harbor, type, (void *)reqdata, lens, 1, &err, &lens);
        task_ungrab(to);
        if (ERR_OK != err) {
            _harbor_response(harbor, fd, skid, rtn, lens, 400);
        } else {
            _harbor_response(harbor, fd, skid, rtn, lens, 200);
        }
    } else {
        ev_close(&harbor->loader->netev, fd, skid);
    }
}
// harbor任务启动回调：注册接收函数并开始监听
static void _harbor_startup(task_ctx *harbor) {
    task_recved(harbor, _harbor_net_recv);
    if (ERR_OK != task_listen(harbor, PACK_HTTP, _harbor.ssl, _harbor.ip, _harbor.port, &_harbor.lsnid, 0)) {
        LOG_ERROR("task_listen %s:%d error", _harbor.ip, _harbor.port);
    }
}
// harbor任务关闭回调：取消监听
static void _harbor_closing(task_ctx *harbor) {
    (void)harbor;
    if (0 != _harbor.lsnid) {
        ev_unlisten(&harbor->loader->netev, _harbor.lsnid);
        _harbor.lsnid = 0;
    }
}
int32_t harbor_start(loader_ctx *loader, name_t tname, name_t ssl, const char *ip, uint16_t port, const char *key) {
    if (INVALID_TNAME == tname
        || 0 == port) {
        return ERR_OK;
    }
    size_t klens = strlen(key);
    if (0 == klens) {
        return ERR_FAILED;
    }
    _harbor.port = port;
#if WITH_SSL
    _harbor.ssl = evssl_qury(ssl);
#else
    (void)ssl;
#endif
    _harbor.lsnid = 0;
    hmac_init(&_harbor.hmac, DG_SHA256, key, klens);
    strcpy(_harbor.ip, ip);
    if (NULL == coro_task_register(loader, tname, _harbor_startup, _harbor_closing)) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
// 为HTTP请求头添加X-Timestamp和Authorization签名（HMAC-SHA256，签名内容=url+data+timestamp）
static void _harbor_sign(binary_ctx *bwriter, const char *key, const char *url, void *data, size_t size) {
    size_t klens = strlen(key);
    if (0 == klens) {
        return;
    }
    char tms[64];
    SNPRINTF(tms, sizeof(tms), "%"PRIu64, nowsec());
    size_t ulens = strlen(url);
    size_t tslens = strlen(tms);
    size_t lens = ulens + size + tslens;
    char *sbuf;
    MALLOC(sbuf, lens);
    memcpy(sbuf, url, ulens);
    if (0 != size) {
        memcpy(sbuf + ulens, data, size);
    }
    memcpy(sbuf + ulens + size, tms, tslens);
    char hs[SHA256_BLOCK_SIZE];
    char hexhs[HEX_ENSIZE(sizeof(hs))];
    hmac_ctx hmac;
    hmac_init(&hmac, DG_SHA256, key, klens);
    hmac_update(&hmac, sbuf, lens);
    hmac_final(&hmac, hs);
    tohex(hs, sizeof(hs), hexhs);
    FREE(sbuf);
    http_pack_head(bwriter, "X-Timestamp", tms);
    http_pack_head(bwriter, "Authorization", hexhs);
}
void *harbor_pack(name_t task, int32_t call, uint8_t reqtype, const char *key, void *data, size_t size, size_t *lens) {
    char url[512];
    if (0 != call) {
        SNPRINTF(url, sizeof(url), "/call?dst=%d&type=%d", task, reqtype);
    } else {
        SNPRINTF(url, sizeof(url), "/request?dst=%d&type=%d", task, reqtype);
    }
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    http_pack_req(&bwriter, "POST", url);
    http_pack_head(&bwriter, "Connection", "Keep-Alive");
    http_pack_head(&bwriter, "Content-Type", "application/octet-stream");
    _harbor_sign(&bwriter, key, url, data, size);
    http_pack_content(&bwriter, data, size);
    *lens = bwriter.offset;
    return bwriter.data;
}
