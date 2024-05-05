#include "service/harbor.h"
#include "service/srey.h"
#include "service/synsl.h"
#include "proto/urlparse.h"
#include "proto/http.h"
#include "algo/hmac.h"

static uint64_t lsnid = 0;
static hmac_sha256_ctx macsha256;

static void _harbor_closing(task_ctx *harbor, message_ctx *msg) {
    if (0 != lsnid) {
        ev_unlisten(&harbor->srey->netev, lsnid);
        lsnid = 0;
    }
}
static void _harbor_response(task_ctx *harbor, char *jresp, size_t lens, int32_t code, message_ctx *msg) {
    buffer_ctx buffer;
    buffer_init(&buffer);
    http_pack_resp(&buffer, code);
    http_pack_head(&buffer, "Server", "Srey");
    if (NULL != jresp) {
        http_pack_head(&buffer, "Content-Type", "application/json");
        http_pack_content(&buffer, jresp, lens);
    } else {
        http_pack_head(&buffer, "Content-Type", "text/plain");
        const char *erro = http_code_status(code);
        http_pack_content(&buffer, (void *)erro, strlen(erro));
    }
    http_response(harbor, msg->fd, msg->skid, &buffer, NULL, NULL, NULL);
    buffer_free(&buffer);
}
static int32_t _check_sign(struct http_pack_ctx *pack, buf_ctx *url, char *jreq, size_t jlens, const char *key) {
    size_t klens = strlen(key);
    if (0 == klens) {
        return ERR_OK;
    }
    size_t tlens = 0;
    char *tbuf = http_header(pack, "X-Timestamp", &tlens);
    if (NULL == tbuf
        || 0 == tlens) {
        LOG_WARN("not find X-Timestamp head.");
        return ERR_FAILED;
    }
    size_t slens = 0;
    char *sign = http_header(pack, "Authorization", &slens);
    if (NULL == sign
        || 0 == slens) {
        LOG_WARN("not find authorization head.");
        return ERR_FAILED;
    }
    uint64_t tms = (uint64_t)atoll(tbuf);
    uint64_t tnow = nowsec();
    int32_t diff;
    if (tnow >= tms) {
        diff = (int32_t)(tnow - tms);
    } else {
        diff = (int32_t)(tms - tnow);
    }
    if (diff >= 5 * 60) {
        LOG_WARN("timestamp error.");
        return ERR_FAILED;
    }

    size_t off = 0;
    size_t total = url->lens + jlens + tlens + 1;
    char *signstr;
    MALLOC(signstr, total);
    memcpy(signstr + off, url->data, url->lens);
    off += url->lens;
    memcpy(signstr + off, jreq, jlens);
    off += jlens;
    memcpy(signstr + off, tbuf, tlens);
    off += tlens;
    signstr[off] = '\0';

    unsigned char hs[SHA256_BLOCK_SIZE];
    char hexhs[HEX_ENSIZE(sizeof(hs))];
    hmac_sha256_init(&macsha256);
    hmac_sha256_update(&macsha256, (unsigned char *)signstr, off);
    hmac_sha256_final(&macsha256, hs);
    tohex(hs, sizeof(hs), hexhs);
    FREE(signstr);

    if (strlen(hexhs) != slens
        || 0 != _memicmp(sign, hexhs, slens)) {
        LOG_WARN("check sign failed.");
        return ERR_FAILED;
    }
    return ERR_OK;
}
static void _harbor_recv(task_ctx *harbor, message_ctx *msg) {
    size_t lens;
    char *jreq = http_data(msg->data, &lens);
    if (NULL == jreq
        || 0 == lens
        || 0 != http_chunked(msg->data)) {
        ev_close(&harbor->srey->netev, msg->fd, msg->skid);
        return;
    }
    buf_ctx *hstatus = http_status(msg->data);
    if (!buf_icompare(&hstatus[0], "post", strlen("post"))) {
        ev_close(&harbor->srey->netev, msg->fd, msg->skid);
        return;
    }
    url_ctx url;
    url_parse(&url, hstatus[1].data, hstatus[1].lens);
    if (buf_empty(&url.path)) {
        ev_close(&harbor->srey->netev, msg->fd, msg->skid);
        return;
    }
    buf_ctx *bdst = url_get_param(&url, "dst");
    if (buf_empty(bdst)) {
        ev_close(&harbor->srey->netev, msg->fd, msg->skid);
        return;
    }
    if (ERR_OK != _check_sign(msg->data, &hstatus[1], jreq, lens, harbor->srey->key)) {
        ev_close(&harbor->srey->netev, msg->fd, msg->skid);
        return;
    }
    name_t dst = strtol(bdst->data, NULL, 10);
    if (buf_icompare(&url.path, "rpc_call", strlen("rpc_call"))) {
        task_ctx *to = srey_task_grab(harbor->srey, dst);
        if (NULL == to){
            LOG_WARN("can't find task %d.", dst);
            return;
        }
        srey_call(to, REQ_TYPE_RPC, jreq, lens, 1);
        srey_task_ungrab(to);
    } else if (buf_icompare(&url.path, "rpc_request", strlen("rpc_request"))) {
        task_ctx *to = srey_task_grab(harbor->srey, dst);
        if (NULL == to) {
            _harbor_response(harbor, NULL, 0, 404, msg);
            LOG_WARN("can't find task %d.", dst);
            return;
        }
        int32_t erro;
        char *rpcrtn = syn_request(to, harbor, REQ_TYPE_RPC, jreq, lens, 1, &erro, &lens);
        if (ERR_OK == erro) {
            _harbor_response(harbor, rpcrtn, lens, 200, msg);
        } else {
            _harbor_response(harbor, NULL, 0, 417, msg);
        }
        srey_task_ungrab(to);
    } else {
        ev_close(&harbor->srey->netev, msg->fd, msg->skid);
    }
}
int32_t harbor_start(srey_ctx *ctx, name_t tname, name_t ssl, const char *host, uint16_t port) {
    size_t klens = strlen(ctx->key);
    if (klens > 0) {
        hmac_sha256_key(&macsha256, (unsigned char *)ctx->key, klens);
    }
    task_ctx *harbor = srey_task_new(TTYPE_C, tname, NULL, NULL);
    srey_task_regcb(harbor, MSG_TYPE_RECV, _harbor_recv);
    srey_task_regcb(harbor, MSG_TYPE_CLOSING, _harbor_closing);
    if (ERR_OK != srey_task_register(ctx, harbor)) {
        srey_task_free(harbor);
        return ERR_FAILED;
    }
#if WITH_SSL
    return srey_listen(harbor, PACK_HTTP, srey_ssl_qury(ctx, ssl), host, port, 0, &lsnid);
#else
    return srey_listen(harbor, PACK_HTTP, NULL, host, port, 0, &lsnid);
#endif
}
