#include "service/harbor.h"
#include "service/srey.h"
#include "service/synsl.h"
#include "proto/urlparse.h"
#include "proto/http.h"

static uint64_t lsnid = 0;

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
    task_ctx *harbor = srey_task_new(TTYPE_C, tname, 0, 0, NULL, NULL);
    srey_task_regcb(harbor, MSG_TYPE_RECV, _harbor_recv);
    srey_task_regcb(harbor, MSG_TYPE_CLOSING, _harbor_closing);
    if (ERR_OK != srey_task_register(ctx, harbor)) {
        srey_task_free(harbor);
        return ERR_FAILED;
    }
    return srey_listen(harbor, PACK_HTTP, srey_ssl_qury(ctx, ssl), host, port, 0, &lsnid);
}
