#include "test_synsl.h"

struct _send_ck_arg {
    int32_t n;
    char data[128];
};
static void *_send_huncked(size_t *lens, void *arg) {
    struct _send_ck_arg *ckarg = arg;
    ckarg->n++;
    if (ckarg->n <= 3) {
        ZERO(ckarg->data, sizeof(ckarg->data));
        SNPRINTF(ckarg->data, sizeof(ckarg->data) - 1, "1234567890 %d.", ckarg->n);
        *lens = strlen(ckarg->data);
        return ckarg->data;
    } else {
        return NULL;
    }
}
static void recv_ck(void *data, size_t lens, int32_t end, void *arg) {
}
static void _timeout_free(task_ctx *task, void *arg) {
    //LOG_INFO("test_synsl release");
    srey_task_close(task);
}
#if WITH_CORO
static void _syn_wbsk_conn(task_ctx *task) {
    uint64_t skid;
    SOCKET fd = syn_websock_connect(task, "127.0.0.1", 15003, NULL, &skid);
    if (INVALID_SOCK != fd) {
        ev_close(&task->srey->netev, fd, skid);
    } else {
        LOG_WARN("syn_websock_connect error.");
    }
}
static void _syn_http(task_ctx *task) {
    uint64_t skid;
    SOCKET fd = syn_connect(task, PACK_HTTP, NULL, "127.0.0.1", 15004, 0, &skid);
    if (INVALID_SOCK == fd) {
        LOG_WARN("syn_connect error.");
        return;
    }
    buffer_ctx buf;
    buffer_init(&buf);
    http_pack_req(&buf, "GET", "/get");
    http_pack_head(&buf, "Host", "127.0.0.1:15004");
    http_pack_end(&buf);
    struct http_pack_ctx *hpack = http_get(task, fd, skid, &buf, NULL, NULL);
    if (NULL == hpack) {
        LOG_WARN("http_get error.");
    }
    struct _send_ck_arg arg;
    arg.n = 0;
    http_pack_req(&buf, "Post", "/post");
    http_pack_head(&buf, "Host", "127.0.0.1:15004");
    hpack = http_post(task, fd, skid, &buf, _send_huncked, recv_ck, NULL, &arg);
    if (NULL == hpack) {
        LOG_WARN("http_post error.");
    }
    ev_close(&task->srey->netev, fd, skid);
    buffer_free(&buf);
}
static void _call_request(task_ctx *task) {
    task_ctx *test = srey_task_grab(task->srey, TEST_TIMEOUT);
    if (NULL == test) {
        return;
    }
    const char *call = "this is srey_call.";
    srey_call(test, (void *)call, strlen(call), 1);
    const char *req = "this is syn_request, test_synsl->test_timeout.";
    int32_t err;
    size_t lens;
    char *rtn = syn_request(test, task, (void *)req, strlen(req), 1, &err, &lens);
    if (lens != strlen(req)
        || 0 != memcmp(rtn, req, lens)) {
        LOG_WARN("syn_request error.");
    }
    srey_task_ungrab(test);
}
#endif
static void _timeout_loop(task_ctx *task, void *arg) {
#if WITH_CORO
    _syn_wbsk_conn(task);
    _syn_http(task);
    _call_request(task);
    syn_timeout(task, 50, _timeout_loop, NULL, NULL);
#endif
}
static void _print_ips(dns_ip *ips, size_t n) {
    for (size_t i = 0; i < n; i++) {
        //LOG_INFO("%s", ips[i].ip);
    }
}
static void _startup(task_ctx *task, message_ctx *msg) {
#if WITH_CORO
    size_t n;
    dns_ip *ips = syn_dns_lookup(task, "8.8.8.8", "www.google.com", 0, &n);
    if (NULL != ips) {
        _print_ips(ips, n);
        FREE(ips);
    }
    ips = syn_dns_lookup(task, "8.8.8.8", "www.google.com", 1, &n);
    if (NULL != ips) {
        _print_ips(ips, n);
        FREE(ips);
    }
#endif
}
void test_synsl(void) {
    task_ctx *task = srey_task_new(TTYPE_C, TEST_SYN, 0, 0, NULL, NULL);
    srey_task_regcb(task, MSG_TYPE_STARTUP, _startup);
    srey_task_register(srey, task);
    //LOG_INFO("test_synsl after 5s close.");
#if WITH_CORO
    syn_timeout(task, 50, _timeout_loop, NULL, NULL);
    syn_timeout(task, 100, _timeout_free, NULL, NULL);
#else
    srey_timeout(task, 0, 100, _timeout_free, NULL, NULL);
#endif
}
