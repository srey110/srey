#include "task_coro_net.h"

static int32_t _prt = 1;
static uint8_t _pktype = PACK_CUSTZ_FLAG;
static void _test_syn_send(task_ctx *task) {
    SOCKET fd;
    uint64_t skid;
    if (ERR_OK != coro_connect(task, _pktype, NULL, "127.0.0.1", 15000, 0, NULL, &fd, &skid)) {
        LOG_ERROR("%s", "syn_connect error");
        return;
    }
    const char *msg = "this is tcp task_coro_net.";
    size_t size;
    void *pack = custz_pack(_pktype, (void*)msg, strlen(msg), &size);
    void *data = coro_send(task, fd, skid, pack, size, &size, 0);
    if (NULL == data) {
        LOG_ERROR("%s", "syn_send error");
        return;
    }
    ASSERTAB(size == strlen(msg) && 0 == _memicmp(data, msg, strlen(msg)), "syn_send error");
    ev_close(&task->loader->netev, fd, skid);
    if (_prt) {
        LOG_INFO("_test_syn_send ok.");
    }
}
static void _test_syn_ssl1_send(task_ctx *task) {
#if WITH_SSL
    SOCKET fd;
    uint64_t skid;
    if (ERR_OK != coro_connect(task, _pktype, NULL, "127.0.0.1", 15001, NETEV_AUTHSSL, NULL, &fd, &skid)) {
        LOG_ERROR("%s", "syn_connect error");
        return;
    }
    struct evssl_ctx *ssl = evssl_qury(101);
    if (ERR_OK != coro_ssl_exchange(task, fd, skid, 1, ssl)) {
        LOG_ERROR("%s", "coro_auth_ssl error");
        return;
    }
    const char *msg = "this is tcp task_coro_net.";
    size_t size;
    void *pack = custz_pack(_pktype, (void*)msg, strlen(msg), &size);
    void *data = coro_send(task, fd, skid, pack, size, &size, 0);
    if (NULL == data) {
        LOG_ERROR("%s", "syn_send error");
        return;
    }
    ASSERTAB(size == strlen(msg) && 0 == _memicmp(data, msg, strlen(msg)), "syn_send error");
    ev_close(&task->loader->netev, fd, skid);
    if (_prt) {
        LOG_INFO("_test_syn_ssl1_send ok.");
    }
#endif
}
static void _test_syn_ssl2_send(task_ctx *task) {
#if WITH_SSL
    SOCKET fd;
    uint64_t skid;
    struct evssl_ctx *ssl = evssl_qury(101);
    if (ERR_OK != coro_connect(task, _pktype, ssl, "127.0.0.1", 15001, NETEV_AUTHSSL, NULL, &fd, &skid)) {
        LOG_ERROR("%s", "syn_connect error");
        return;
    }
    const char *msg = "this is tcp task_coro_net.";
    size_t size;
    void *pack = custz_pack(_pktype, (void*)msg, strlen(msg), &size);
    void *data = coro_send(task, fd, skid, pack, size, &size, 0);
    if (NULL == data) {
        LOG_ERROR("%s", "syn_send error");
        return;
    }
    ASSERTAB(size == strlen(msg) && 0 == _memicmp(data, msg, strlen(msg)), "syn_send error");
    ev_close(&task->loader->netev, fd, skid);
    if (_prt) {
        LOG_INFO("_test_syn_ssl2_send ok.");
    }
#endif
}
static void _test_syn_sendto(task_ctx *task) {
    SOCKET fd;
    uint64_t skid;
    if (ERR_OK != task_udp(task, "0.0.0.0", 0, &fd, &skid)) {
        LOG_ERROR("%s", "task_udp error");
        return;
    }
    const char *msg = "this is udp task_coro_net.";
    size_t size;
    void *data = coro_sendto(task, fd, skid, "127.0.0.1", 15002, (void*)msg, strlen(msg), &size);
    if (NULL == data) {
        LOG_ERROR("%s", "syn_sendto error");
        ev_close(&task->loader->netev, fd, skid);
        return;
    }
    ASSERTAB(0 == _memicmp(data, msg, strlen(msg)), "syn_sendto error");
    ev_close(&task->loader->netev, fd, skid);
    if (_prt) {
        LOG_INFO("_test_syn_sendto ok.");
    }
}
static void _net_close(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype,
    uint8_t client) { }
static void _timeout(task_ctx *task, uint64_t sess) {
    _test_syn_send(task);
    _test_syn_sendto(task);
    _test_syn_ssl1_send(task);
    _test_syn_ssl2_send(task);
    uint64_t skid;
    SOCKET fd = wbsock_connect(task, NULL, "ws://127.0.0.1:15004", NULL, &skid, 0);
    if (INVALID_SOCK != fd) {
        ev_close(&task->loader->netev, fd, skid);
    }
    task_timeout(task, 0, 1000, _timeout);
}
static void _startup(task_ctx *task) {
    on_closed(task, _net_close);
    task_timeout(task, 0, 1000, _timeout);
}
void task_coro_net_start(loader_ctx *loader, name_t name, int32_t pt) {
    _prt = pt;
    task_ctx *task = task_new(loader, name, NULL, NULL, NULL);
    task_register(task, _startup, NULL);
}
