#include "task_smtp.h"

// 假 SMTP 服务端的容量与并发规模（说明见下面的同名段）
#define FAKE_MAXCONN  8
#define FAKE_BUF_LENS 4096
#define FAKE_CONC_N   4
#define FAKE_ROUNDS   4
#define FAKE_TERM     "\r\n.\r\n"

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
    int32_t inited;
    if (ERR_OK == is_ipaddr(ctx->smtp_sv)) {
        inited = smtp_init(&ctx->smtp, ctx->smtp_sv, ctx->smtp_port, ssl, ctx->smtp_user, ctx->smtp_psw);
    } else {
        size_t n;
        dns_ip *ips = dns_lookup(task, ctx->smtp_sv, 0, 1, &n);
        if (NULL == ips) {
            LOG_ERROR("dns_lookup error.");
            return;
        }
        inited = smtp_init(&ctx->smtp, ips[0].ip, ctx->smtp_port, ssl, ctx->smtp_user, ctx->smtp_psw);
        FREE(ips);
    }
    if (ERR_OK != inited) {
        LOG_ERROR("smtp_init error.");
        return;
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
static void _net_close(task_ctx *task, sk_id *sk, subtype_t pktype, uint8_t client, int32_t erro) {
    (void)sk;
    (void)pktype;
    (void)client;
    (void)erro;
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

// ── 假 SMTP 服务端 + 并发投递用例 ────────────────────────────────────────
// 真服务端要外网账号，跑不通，smtp 的命令串行化就一直没有运行时验证。这里只实现
// 刚好够 smtp_connect / smtp_send / smtp_quit 走完的那部分协议，专门用来抓命令交错。
// 交错检测靠事务状态机：MAIL FROM 开事务并记下发件人编号，RCPT 的编号必须与之相同，
// DATA 收到 "\r\n.\r\n" 收尾，RSET 关事务。两个协程的邮件挤到一起时必然撞上其中一条。
// 服务端回调与投递协程同属一个 task，单线程依次执行，故下面这些计数用普通变量即可
typedef struct fake_conn {
    uint64_t skid;      // 0 = 空槽
    int32_t authstep;   // 0 未认证 1 已发 Username 挑战 2 已发 Password 挑战 3 已认证
    int32_t indata;     // 1 = 正在收信体
    int32_t sender;     // 当前事务的发件人编号；-1 = 无事务
    size_t used;
    char buf[FAKE_BUF_LENS];
}fake_conn;

typedef struct fake_smtp_ctx {
    uint16_t port;
    int32_t interleave;     // 检测到的交错次数
    int32_t mails;          // 服务端确认收下的邮件数
    int32_t *ok;
    smtp_ctx smtp;
    fake_conn conns[FAKE_MAXCONN];
}fake_smtp_ctx;

static void _fake_reply(task_ctx *task, sk_id *sk, const char *resp) {
    ev_send(&task->loader->netev, sk->fd, sk->skid, (void *)resp, strlen(resp), 1);
}
static fake_conn *_fake_conn_get(fake_smtp_ctx *ctx, uint64_t skid, int32_t create) {
    int32_t i;
    for (i = 0; i < FAKE_MAXCONN; i++) {
        if (ctx->conns[i].skid == skid) {
            return &ctx->conns[i];
        }
    }
    if (0 == create) {
        return NULL;
    }
    for (i = 0; i < FAKE_MAXCONN; i++) {
        if (0 == ctx->conns[i].skid) {
            ZERO(&ctx->conns[i], sizeof(fake_conn));
            ctx->conns[i].skid = skid;
            ctx->conns[i].sender = -1;
            return &ctx->conns[i];
        }
    }
    return NULL;
}
// 从 "<c3@t>" 这类地址里取出编号；取不到返回 -1
static int32_t _fake_tag(const char *line, char prefix) {
    const char *p = strchr(line, '<');
    if (NULL == p || prefix != p[1]) {
        return -1;
    }
    int32_t n = 0;
    p += 2;
    if (*p < '0' || *p > '9') {
        return -1;
    }
    while (*p >= '0' && *p <= '9') {
        n = n * 10 + (*p - '0');
        p++;
    }
    return n;
}
// 处理一行命令（line 已去掉 CRLF 并以 '\0' 结尾）
static void _fake_cmd(task_ctx *task, sk_id *sk, fake_smtp_ctx *ctx, fake_conn *fc, char *line) {
    int32_t tag;
    if (0 == STRNCMP(line, "EHLO", 4)
        || 0 == STRNCMP(line, "HELO", 4)) {
        // 只广告 LOGIN，客户端 _smtp_get_authtype 优先 PLAIN，不给它选择余地
        _fake_reply(task, sk, "250-fake.smtp.local\r\n250-AUTH LOGIN\r\n250 OK\r\n");
        return;
    }
    if (0 == STRNCMP(line, "AUTH LOGIN", 10)) {
        fc->authstep = 1;
        _fake_reply(task, sk, "334 VXNlcm5hbWU6\r\n");// base64("Username:")
        return;
    }
    if (1 == fc->authstep) {
        fc->authstep = 2;
        _fake_reply(task, sk, "334 UGFzc3dvcmQ6\r\n");// base64("Password:")
        return;
    }
    if (2 == fc->authstep) {
        fc->authstep = 3;
        _fake_reply(task, sk, "235 2.7.0 Authentication successful\r\n");
        return;
    }
    if (0 == STRNCMP(line, "MAIL FROM:", 10)) {
        tag = _fake_tag(line, 'c');
        // 解不出编号也要算一次失败：_fake_tag 的失败值 -1 与"无事务"哨兵 -1 是同一个数，
        // 不在这里拦住的话 sender 会被写成 -1，此后 MAIL FROM 的 -1 != sender 永远为假、
        // RCPT 的 tag != sender 变成 -1 != -1，两条交错检测同时失效；
        // 而 mails 只数信体终止符、与地址解析无关，判据 0 == interleave && 16 == mails 就变成空真
        if (-1 == tag) {
            ctx->interleave++;
            LOG_ERROR("fake smtp: MAIL FROM tag unparsable: %s", line);
        }
        if (-1 != fc->sender) {// 上一笔还没收尾就又来一个发件人：交错
            ctx->interleave++;
            LOG_ERROR("fake smtp: MAIL FROM c%d while c%d still open.", tag, fc->sender);
        }
        fc->sender = tag;
        _fake_reply(task, sk, "250 OK\r\n");
        return;
    }
    if (0 == STRNCMP(line, "RCPT TO:", 8)) {
        tag = _fake_tag(line, 'r');
        if (-1 == tag) {// 理由同上
            ctx->interleave++;
            LOG_ERROR("fake smtp: RCPT tag unparsable: %s", line);
        }
        if (tag != fc->sender) {// 收件人编号与本事务发件人对不上：交错
            ctx->interleave++;
            LOG_ERROR("fake smtp: RCPT r%d under sender c%d.", tag, fc->sender);
        }
        _fake_reply(task, sk, "250 OK\r\n");
        return;
    }
    if (0 == STRNCMP(line, "DATA", 4)) {
        fc->indata = 1;
        _fake_reply(task, sk, "354 End data with <CR><LF>.<CR><LF>\r\n");
        return;
    }
    if (0 == STRNCMP(line, "RSET", 4)) {
        fc->sender = -1;
        _fake_reply(task, sk, "250 OK\r\n");
        return;
    }
    if (0 == STRNCMP(line, "QUIT", 4)) {
        _fake_reply(task, sk, "221 Bye\r\n");
        return;
    }
    _fake_reply(task, sk, "250 OK\r\n");// NOOP 及其余一律 250
}
static void _fake_accept(task_ctx *task, sk_id *sk, subtype_t pktype) {
    (void)pktype;
    fake_smtp_ctx *ctx = (fake_smtp_ctx *)coro_get_arg(task);
    if (NULL == _fake_conn_get(ctx, sk->skid, 1)) {
        LOG_ERROR("fake smtp: too many connections.");
        return;
    }
    _fake_reply(task, sk, "220 fake.smtp.local ESMTP\r\n");
}
static void _fake_recv(task_ctx *task, sk_id *sk, subtype_t pktype, uint8_t client,
                       uint8_t slice, void *data, size_t size) {
    (void)pktype;
    (void)slice;
    if (client) {
        return;
    }
    fake_smtp_ctx *ctx = (fake_smtp_ctx *)coro_get_arg(task);
    fake_conn *fc = _fake_conn_get(ctx, sk->skid, 0);
    if (NULL == fc) {
        return;
    }
    if (fc->used + size >= sizeof(fc->buf)) {
        LOG_ERROR("fake smtp: line buffer overflow.");
        fc->used = 0;
        return;
    }
    memcpy(fc->buf + fc->used, data, size);
    fc->used += size;
    fc->buf[fc->used] = '\0';
    char *term;
    char *crlf;
    size_t rest;
    char line[FAKE_BUF_LENS];
    for (;;) {
        if (0 != fc->indata) {
            // 信体只找结束标记,找不到就只留可能被截断的末尾几字节,不占着整封信
            term = strstr(fc->buf, FAKE_TERM);
            if (NULL == term) {
                rest = strlen(FAKE_TERM) - 1;
                if (fc->used > rest) {
                    memmove(fc->buf, fc->buf + fc->used - rest, rest);
                    fc->used = rest;
                    fc->buf[fc->used] = '\0';
                }
                return;
            }
            rest = fc->used - (size_t)(term - fc->buf) - strlen(FAKE_TERM);
            memmove(fc->buf, term + strlen(FAKE_TERM), rest);
            fc->used = rest;
            fc->buf[fc->used] = '\0';
            fc->indata = 0;
            fc->sender = -1;// 一封收完,事务结束
            ctx->mails++;
            _fake_reply(task, sk, "250 OK\r\n");
            continue;
        }
        crlf = strstr(fc->buf, FLAG_CRLF);
        if (NULL == crlf) {
            return;
        }
        *crlf = '\0';
        rest = fc->used - (size_t)(crlf - fc->buf) - CRLF_SIZE;
        safe_fill_str(line, sizeof(line), fc->buf);
        memmove(fc->buf, crlf + CRLF_SIZE, rest);
        fc->used = rest;
        fc->buf[fc->used] = '\0';
        _fake_cmd(task, sk, ctx, fc, line);
    }
}
static void _fake_close(task_ctx *task, sk_id *sk, subtype_t pktype, uint8_t client, int32_t erro) {
    (void)pktype;
    (void)erro;
    if (client) {
        return;
    }
    fake_smtp_ctx *ctx = (fake_smtp_ctx *)coro_get_arg(task);
    fake_conn *fc = _fake_conn_get(ctx, sk->skid, 0);
    if (NULL != fc) {
        fc->skid = 0;
    }
}
// 并发投递：每个协程发自己编号的邮件，服务端按事务状态机校验没有交错
typedef struct fake_conc_arg {
    int32_t idx;
    int32_t done;   // 1 成功 -1 失败
    smtp_ctx *smtp;
}fake_conc_arg;

static void _fake_conc_worker(task_ctx *task, void *arg) {
    (void)task;
    fake_conc_arg *a = (fake_conc_arg *)arg;
    char from[32];
    char to[32];
    mail_ctx mail;
    int32_t rtn;
    int32_t i;
    SNPRINTF(from, sizeof(from), "c%d@t", a->idx);
    SNPRINTF(to, sizeof(to), "r%d@t", a->idx);
    for (i = 0; i < FAKE_ROUNDS; i++) {
        mail_init(&mail);
        mail_from(&mail, "srey", from);
        mail_addrs_add(&mail, to, TO);
        mail_subject(&mail, "concurrency");
        mail_msg(&mail, "body");
        mail_reply(&mail, 0);
        rtn = smtp_send(a->smtp, &mail);
        mail_free(&mail);
        if (ERR_OK != rtn) {
            a->done = -1;
            return;
        }
    }
    a->done = 1;
}
static void _fake_startup(task_ctx *task) {
    fake_smtp_ctx *ctx = (fake_smtp_ctx *)coro_get_arg(task);
    task_accepted(task, _fake_accept);
    task_recved(task, _fake_recv);
    task_closed(task, _fake_close);
    uint64_t lsnid;
    // NETEV_ACCEPT 必开：220 欢迎行要在 accept 回调里主动发，掩码不带它 _fake_accept 不触发
    if (ERR_OK != task_listen(task, PACK_NONE, NULL, "127.0.0.1", ctx->port, &lsnid, NETEV_ACCEPT)) {
        LOG_ERROR("fake smtp: listen %u failed.", ctx->port);
        return;
    }
    if (ERR_OK != smtp_init(&ctx->smtp, "127.0.0.1", ctx->port, NULL, "user", "psw")) {
        LOG_ERROR("fake smtp: smtp_init failed.");
        return;
    }
    if (ERR_OK != smtp_connect(task, &ctx->smtp)) {
        LOG_ERROR("fake smtp: smtp_connect failed.");
        return;
    }
    fake_conc_arg args[FAKE_CONC_N];
    fork_serial_cb funcs[FAKE_CONC_N];
    void *argp[FAKE_CONC_N];
    int32_t i;
    for (i = 0; i < FAKE_CONC_N; i++) {
        args[i].idx = i;
        args[i].done = 0;
        args[i].smtp = &ctx->smtp;
        funcs[i] = _fake_conc_worker;
        argp[i] = &args[i];
    }
    if (ERR_OK != coro_fork_wait(task, funcs, argp, FAKE_CONC_N)) {
        LOG_ERROR("fake smtp: fork_wait error.");
        smtp_quit(&ctx->smtp);
        return;
    }
    for (i = 0; i < FAKE_CONC_N; i++) {
        if (1 != args[i].done) {
            LOG_ERROR("fake smtp: worker %d failed.", i);
            smtp_quit(&ctx->smtp);
            return;
        }
    }
    smtp_quit(&ctx->smtp);
    if (0 != ctx->interleave) {
        LOG_ERROR("fake smtp: %d interleavings detected.", ctx->interleave);
        return;
    }
    if (FAKE_CONC_N * FAKE_ROUNDS != ctx->mails) {
        LOG_ERROR("fake smtp: server took %d mails, want %d.",
                  ctx->mails, FAKE_CONC_N * FAKE_ROUNDS);
        return;
    }
    LOG_INFO("smtp fake tested: %d mails, no interleaving.", ctx->mails);
    *(ctx->ok) = 1;
}
void task_smtp_fake_start(loader_ctx *loader, const char *name, uint16_t port, int32_t *ok) {
    if (NULL == ok) {
        return;
    }
    fake_smtp_ctx *ctx;
    CALLOC(ctx, 1, sizeof(fake_smtp_ctx));
    ctx->port = port;
    ctx->ok = ok;
    coro_task_register(loader, name, 0, _fake_startup, NULL, _free, ctx);
}
