#include "task_smtp.h"

typedef struct task_smtp_ctx {
    uint16_t smtp_port;
    int32_t prt;
    int32_t *ok;
    mail_ctx mail;
    smtp_ctx smtp;
    char sslname[EVSSL_NAME_LEN];
    char smtp_sv[64];
    char smtp_user[64];
    char smtp_psw[64];
    char mail_from[64];
    char mail_addr1[64];    // TO 收件人
    char mail_addr2[64];    // CC 收件人
    char mail_attach[PATH_LENS];
}task_smtp_ctx;

static void _startup(task_ctx *task) {
    void *ssl = NULL;
    task_smtp_ctx *ctx = (task_smtp_ctx *)coro_get_arg(task);
#if WITH_SSL
    if (!EMPTYSTR(ctx->sslname)) {
        ssl = evssl_qury(ctx->sslname);
    }
#endif
    // 域名需先 DNS 解析，IP 直连
    if (ERR_OK == is_ipaddr(ctx->smtp_sv)) {
        smtp_init(&ctx->smtp, ctx->smtp_sv, ctx->smtp_port, ssl, ctx->smtp_user, ctx->smtp_psw);
    } else {
        size_t n;
        dns_ip *ips = dns_lookup(task, ctx->smtp_sv, 0, 1, &n);
        if (NULL == ips) {
            LOG_ERROR("dns_lookup error.");
            return;
        }
        smtp_init(&ctx->smtp, ips[0].ip, ctx->smtp_port, ssl, ctx->smtp_user, ctx->smtp_psw);
        FREE(ips);
    }
    if (ERR_OK != smtp_connect(task, &ctx->smtp)) {
        LOG_WARN("smtp_connect error.");
        return;
    }
    // 第一封：纯文本正文，TO 收件人，无附件，无 Reply-To
    mail_init(&ctx->mail);
    mail_from(&ctx->mail, "srey", ctx->mail_from);
    if (!EMPTYSTR(ctx->mail_addr1)) {
        mail_addrs_add(&ctx->mail, ctx->mail_addr1, TO);
    }
    if (!EMPTYSTR(ctx->mail_addr2)) {
        mail_addrs_add(&ctx->mail, ctx->mail_addr2, CC);
    }
    mail_subject(&ctx->mail, "srey smtp test");
    mail_msg(&ctx->mail, "this is text message");
    mail_reply(&ctx->mail, 0);
    if (ERR_OK != smtp_send(&ctx->smtp, &ctx->mail)) {
        smtp_quit(&ctx->smtp);
        LOG_WARN("smtp_send error.");
        return;
    }
    if (ctx->prt) {
        LOG_INFO("smtp send 1 ok.");
    }
    // 第二封：纯文本 + HTML 正文，带附件，含 Reply-To 头，覆盖多种邮件组合路径
    const char *html = "<!DOCTYPE html><html><title>HTML Tutorial</title><body><h1>This is a heading</h1><p>This is a paragraph.</p></body></html>";
    mail_html(&ctx->mail, html, strlen(html));
    if (!EMPTYSTR(ctx->mail_attach)) {
        mail_attach_add(&ctx->mail, ctx->mail_attach);
    }
    mail_reply(&ctx->mail, 1);
    if (ERR_OK != smtp_send(&ctx->smtp, &ctx->mail)) {
        smtp_quit(&ctx->smtp);
        LOG_WARN("smtp_send error.");
        return;
    }
    if (ctx->prt) {
        LOG_INFO("smtp send 2 ok.");
    }
    smtp_quit(&ctx->smtp);
    LOG_INFO("smtp tested.");
    *(ctx->ok) = 1;
}
// 连接断开：若两封邮件未全部发送完则说明服务端提前关闭连接
static void _net_close(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client) {
    (void)fd;
    (void)skid;
    (void)pktype;
    (void)client;
    task_smtp_ctx *ctx = (task_smtp_ctx *)coro_get_arg(task);
    if (!(*(ctx->ok))) {
        LOG_WARN("disconnect by remote,befor test complete.");
    }
    mail_free(&ctx->mail);
}
void task_smtp_start(loader_ctx *loader, const char *name, const char *sslname,
                     const char *smtp_sv, uint16_t smtp_port,
                     const char *smtp_user, const char *smtp_psw,
                     const char *mail_from, const char *mail_addr1, const char *mail_addr2,
                     const char *mail_att, int32_t pt, int32_t *ok) {
    if (NULL == ok
        || (NULL == smtp_sv || strlen(smtp_sv) >= 64)
        || (NULL == smtp_user || strlen(smtp_user) >= 64)
        || (NULL == smtp_psw || strlen(smtp_psw) >= 64)
        || (NULL == mail_from || strlen(mail_from) >= 64)
        || (NULL == mail_addr1 && NULL == mail_addr2)
        || (NULL != mail_addr1 && strlen(mail_addr1) >= 64)
        || (NULL != mail_addr2 && strlen(mail_addr2) >= 64)
        || (NULL != mail_att && strlen(mail_att) >= PATH_LENS)) {
        return;
    }
    task_smtp_ctx *ctx;
    CALLOC(ctx, 1, sizeof(task_smtp_ctx));
    ctx->smtp_port = smtp_port;
    ctx->prt = pt;
    if (!EMPTYSTR(sslname)) {
        safe_fill_str(ctx->sslname, sizeof(ctx->sslname), sslname);
    }
    ctx->ok = ok;
    safe_fill_str(ctx->smtp_sv, sizeof(ctx->smtp_sv), smtp_sv);
    safe_fill_str(ctx->smtp_user, sizeof(ctx->smtp_user), smtp_user);
    safe_fill_str(ctx->smtp_psw, sizeof(ctx->smtp_psw), smtp_psw);
    safe_fill_str(ctx->mail_from, sizeof(ctx->mail_from), mail_from);
    if (NULL != mail_addr1) {
        safe_fill_str(ctx->mail_addr1, sizeof(ctx->mail_addr1), mail_addr1);
    }
    if (NULL != mail_addr2) {
        safe_fill_str(ctx->mail_addr2, sizeof(ctx->mail_addr2), mail_addr2);
    }
    if (NULL != mail_att) {
        safe_fill_str(ctx->mail_attach, sizeof(ctx->mail_attach), mail_att);
    }
    task_ctx *task = coro_task_register(loader, name, 0, _startup, NULL, _free, ctx);
    task_closed(task, _net_close);
}
