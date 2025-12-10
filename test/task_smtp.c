#include "task_smtp.h"

static int32_t _prt = 1;
const char *_smtp_sv = "smtp.163.com";
const uint16_t _smtp_port = 465;
const char *_smtp_user = "test@163.com";
const char *_smtp_psw = "FCAZMcsYms";
static smtp_ctx _smtp;
static mail_ctx _mail;

static void _startup(task_ctx *task) {
    mail_init(&_mail);
    mail_from(&_mail, "srey", "test@163.com");
    size_t n;
    dns_ip *ips = dns_lookup(task, _smtp_sv, 0, &n);
    struct evssl_ctx *ssl = evssl_qury(102);
    smtp_init(&_smtp, ips[0].ip, _smtp_port, ssl, _smtp_user, _smtp_psw);
    FREE(ips);
    if (ERR_OK != smtp_connect(task, &_smtp)) {
        LOG_WARN("smtp_connect error.");
        return;
    }
    mail_addrs_add(&_mail, "test@qq.com", TO);
    mail_addrs_add(&_mail, "test@gmail.com", TO);
    mail_subject(&_mail, "subject1");
    mail_msg(&_mail, "this is message");
    if (ERR_OK != smtp_send(&_smtp, &_mail)) {
        LOG_WARN("smtp_send error.");
        return;
    }
    const char *html = "<!DOCTYPE html><html><title>HTML Tutorial</title><body><h1>This is a heading</h1><p>This is a paragraph.</p></body></html>";
    mail_html(&_mail, html, strlen(html));
    mail_attach_add(&_mail, "D:\\....\\panda.jpg");
    if (ERR_OK != smtp_send(&_smtp, &_mail)) {
        LOG_WARN("smtp_send error.");
        return;
    }
    smtp_quit(&_smtp);
}
static void _net_close(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client) {
    if (_prt) {
        LOG_INFO("socket %d closed", (uint32_t)fd);
    }
    mail_free(&_mail);
}
void task_smtp_start(loader_ctx *loader, name_t name, int32_t pt) {
    _prt = pt;
    task_ctx *task = task_new(loader, name, NULL, NULL, NULL);
    task_register(task, _startup, NULL);
    on_closed(task, _net_close);
}
