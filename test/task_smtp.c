#include "task_smtp.h"

static int32_t _prt = 1;
const char *_smtp_sv = "smtp.163.com";
const uint16_t _smtp_port = 465;
const char *_smtp_user = "test@163.com";
const char *_smtp_from = "test@163.com";
const char *_smtp_rcpt = "test@qq.com";
const char *_smtp_psw = "CAZMcsYms";
static smtp_ctx _smtp;

static void _startup(task_ctx *task) {
    size_t n;
    dns_ip *ips = dns_lookup(task, _smtp_sv, 0, &n);
    struct evssl_ctx *ssl = evssl_qury(102);
    smtp_init(&_smtp, ips[0].ip, _smtp_port, ssl, _smtp_user, _smtp_psw);
    FREE(ips);
    if (ERR_OK != smtp_connect(task, &_smtp)) {
        LOG_WARN("smtp_connect error.");
        return;
    }
    if (ERR_OK != smtp_send(&_smtp, _smtp_from, _smtp_rcpt, "test subject", "123")) {
        LOG_WARN("smtp_send error.");
        return;
    }
    if (ERR_OK != smtp_send(&_smtp, _smtp_from, _smtp_rcpt, "test subject2", "456")) {
        LOG_WARN("smtp_send error.");
        return;
    }
    smtp_close(&_smtp);
}
static void _net_close(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client) {
    if (_prt) {
        LOG_INFO("socket %d closed", (uint32_t)fd);
    }
}
void task_smtp_start(loader_ctx *loader, name_t name, int32_t pt) {
    _prt = pt;
    task_ctx *task = task_new(loader, name, NULL, NULL, NULL);
    task_register(task, _startup, NULL);
    on_closed(task, _net_close);
}
