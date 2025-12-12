#include "protocol/smtp/smtp.h"
#include "utils/utils.h"
#include "utils/binary.h"
#include "srey/task.h"
#include "protocol/prots.h"
#include "crypt/base64.h"

#define SMTP_OK "250"
#define SMTP_CODE_LENS 3

typedef enum smtp_authtype {
    LOGIN = 1,
    PLAIN
}smtp_authtype;
typedef enum parse_status {
    INIT = 0,
    EHLO,
    AUTH,
    AUTH_CHECK,
    COMMAND,
}parse_status;
static char _smtp_host[64] = { 0 };
static _handshaked_push _hs_push;

void _smtp_init(void *hspush) {
    _hs_push = (_handshaked_push)hspush;
    if (ERR_OK != gethostname(_smtp_host, sizeof(_smtp_host))) {
        strcpy(_smtp_host, "srey");
    }
}
void _smtp_udfree(ud_cxt *ud) {
    if (NULL == ud->extra) {
        return;
    }
    smtp_ctx *smtp = ud->extra;
    smtp->fd = INVALID_SOCK;
    ud->extra = NULL;
}
void _smtp_closed(ud_cxt *ud) {
    _smtp_udfree(ud);
}
void smtp_init(smtp_ctx *smtp, const char *ip, uint16_t port, struct evssl_ctx *evssl, const char *user, const char *psw) {
    ZERO(smtp, sizeof(smtp_ctx));
    memcpy(smtp->ip, ip, strlen(ip));
    smtp->port = port;
    smtp->evssl = evssl;
    smtp->fd = INVALID_SOCK;
    memcpy(smtp->user, user, strlen(user));
    memcpy(smtp->psw, psw, strlen(psw));
}
int32_t smtp_try_connect(task_ctx *task, smtp_ctx *smtp) {
    smtp->task = task;
    return task_connect(task, PACK_SMTP, smtp->evssl, smtp->ip, smtp->port, 0, smtp, &smtp->fd, &smtp->skid);
}
int32_t smtp_check_code(char *pack, const char *code) {
    if (0 == memcmp(pack, code, strlen(code))) {
        return ERR_OK;
    }
    LOG_WARN("%s", pack);
    return ERR_FAILED;
}
int32_t smtp_check_ok(char *pack) {
    return smtp_check_code(pack, SMTP_OK);
}
char *smtp_pack_reset(smtp_ctx *smtp) {
    return format_va("RSET%s", FLAG_CRLF);
}
char *smtp_pack_quit(smtp_ctx *smtp) {
    return format_va("QUIT%s", FLAG_CRLF);
}
char *smtp_pack_ping(smtp_ctx *smtp) {
    return format_va("NOOP%s", FLAG_CRLF);
}
char *smtp_pack_from(smtp_ctx *smtp, const char *from) {
    return format_va("MAIL FROM:<%s>%s", from, FLAG_CRLF);
}
char *smtp_pack_rcpt(smtp_ctx *smtp, const char *rcpt) {
    return format_va("RCPT TO:<%s>%s", rcpt, FLAG_CRLF);
}
char *smtp_pack_data(smtp_ctx *smtp) {
    return format_va("DATA%s", FLAG_CRLF);
}
static void _smtp_connected(smtp_ctx *smtp, ev_ctx *ev, SOCKET fd, uint64_t skid, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    size_t blens = buffer_size(buf);
    if (blens < SMTP_CODE_LENS + CRLF_SIZE) {
        BIT_SET(*status, PROT_MOREDATA);
        return;
    }
    if (ERR_FAILED == buffer_search(buf, 0, blens - CRLF_SIZE, 0, FLAG_CRLF, CRLF_SIZE)) {
        BIT_SET(*status, PROT_MOREDATA);
        return;
    }
    char code[SMTP_CODE_LENS + 1] = {0};
    ASSERTAB(SMTP_CODE_LENS == buffer_copyout(buf, 0, code, SMTP_CODE_LENS), "copy buffer failed.");
    if (0 != strcmp(code, "220")) {
        BIT_SET(*status, PROT_ERROR);
        return;
    }
    buffer_drain(buf, blens);
    char *cmd = format_va("EHLO %s%s", _smtp_host, FLAG_CRLF);
    ud->status = EHLO;
    if (ERR_OK != ev_send(ev, fd, skid, cmd, strlen(cmd), 0)) {
        BIT_SET(*status, PROT_ERROR);
    }
}
static int32_t _smtp_get_authtype(buffer_ctx *buf) {
    const char *authtype = "250-AUTH";
    size_t atlens = strlen(authtype);
    int32_t start = buffer_search(buf, 1, 0, 0, (char *)authtype, atlens);
    if (ERR_FAILED == start) {
        LOG_WARN("can't find auth type.");
        return ERR_FAILED;
    }
    start += ((int32_t)atlens + 1);
    int32_t end = buffer_search(buf, 1, start, 0, FLAG_CRLF, CRLF_SIZE);
    if (ERR_FAILED == end) {
        LOG_WARN("format error.");
        return ERR_FAILED;
    }
    if (ERR_FAILED != buffer_search(buf, 1, start, end, "PLAIN", strlen("PLAIN"))) {
        return PLAIN;
    }
    if (ERR_FAILED != buffer_search(buf, 1, start, end, "LOGIN", strlen("LOGIN"))) {
        return LOGIN;
    }
    return ERR_FAILED;
}
static void _smtp_ehlo(smtp_ctx *smtp, ev_ctx *ev, SOCKET fd, uint64_t skid, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    size_t blens = buffer_size(buf);
    if (blens < SMTP_CODE_LENS + CRLF_SIZE) {
        BIT_SET(*status, PROT_MOREDATA);
        return;
    }
    if (ERR_FAILED == buffer_search(buf, 0, blens - CRLF_SIZE, 0, FLAG_CRLF, CRLF_SIZE)) {
        BIT_SET(*status, PROT_MOREDATA);
        return;
    }
    char code[SMTP_CODE_LENS + 1] = { 0 };
    ASSERTAB(SMTP_CODE_LENS == buffer_copyout(buf, 0, code, SMTP_CODE_LENS), "copy buffer failed.");
    if (0 != strcmp(code, SMTP_OK)) {
        BIT_SET(*status, PROT_ERROR);
        return;
    }
    smtp->authtype = _smtp_get_authtype(buf);
    if (ERR_FAILED == smtp->authtype) {
        BIT_SET(*status, PROT_ERROR);
        return;
    }
    buffer_drain(buf, blens);
    char *cmd = NULL;
    switch (smtp->authtype) {
    case LOGIN:
        cmd = format_va("AUTH LOGIN%s", FLAG_CRLF);
        break;
    case PLAIN:
        cmd = format_va("AUTH PLAIN%s", FLAG_CRLF);
        break;
    }
    ud->status = AUTH;
    if (ERR_OK != ev_send(ev, fd, skid, cmd, strlen(cmd), 0)) {
        BIT_SET(*status, PROT_ERROR);
    }
}
static char *_smtp_loin_cmd(const char *up) {
    size_t lens = strlen(up);
    char *b64;
    CALLOC(b64, 1, B64EN_SIZE(lens));
    lens = bs64_encode(up, lens, b64);
    char *cmd = format_va("%s%s", b64, FLAG_CRLF);
    FREE(b64);
    return cmd;
}
static void _smtp_loin(smtp_ctx *smtp, ev_ctx *ev, SOCKET fd, uint64_t skid, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    char code[SMTP_CODE_LENS + 1] = { 0 };
    ASSERTAB(SMTP_CODE_LENS == buffer_copyout(buf, 0, code, SMTP_CODE_LENS), "copy buffer failed.");
    if (0 != strcmp(code, "334")) {
        BIT_SET(*status, PROT_ERROR);
        return;
    }
    size_t blens = buffer_size(buf);
    size_t lens = blens - CRLF_SIZE - SMTP_CODE_LENS - 1;
    char *b64flag;
    CALLOC(b64flag, 1, lens + 1);
    ASSERTAB(lens == buffer_copyout(buf, SMTP_CODE_LENS + 1, b64flag, lens), "copy buffer failed.");
    buffer_drain(buf, blens);
    char *flag;
    CALLOC(flag, 1, B64DE_SIZE(lens));
    bs64_decode(b64flag, lens, flag);
    FREE(b64flag);
    flag = strlower(flag);
    if (0 == strcmp(flag, "username:")) {
        FREE(flag);
        char *cmd = _smtp_loin_cmd(smtp->user);
        if (ERR_OK != ev_send(ev, fd, skid, cmd, strlen(cmd), 0)) {
            BIT_SET(*status, PROT_ERROR);
        }
        return;
    }
    if (0 == strcmp(flag, "password:")) {
        FREE(flag);
        char *cmd = _smtp_loin_cmd(smtp->psw);
        ud->status = AUTH_CHECK;
        if (ERR_OK != ev_send(ev, fd, skid, cmd, strlen(cmd), 0)) {
            BIT_SET(*status, PROT_ERROR);
        }
        return;
    }
    BIT_SET(*status, PROT_ERROR);
    FREE(flag);
}
static void _smtp_plain(smtp_ctx *smtp, ev_ctx *ev, SOCKET fd, uint64_t skid, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    char code[SMTP_CODE_LENS + 1] = { 0 };
    ASSERTAB(SMTP_CODE_LENS == buffer_copyout(buf, 0, code, SMTP_CODE_LENS), "copy buffer failed.");
    if (0 != strcmp(code, "334")) {
        BIT_SET(*status, PROT_ERROR);
        return;
    }
    buffer_drain(buf, buffer_size(buf));
    size_t ulens = strlen(smtp->user);
    size_t plens = strlen(smtp->psw);
    size_t blens = ulens + plens + 2;
    char *enbuf;
    CALLOC(enbuf, 1, blens);
    memcpy(enbuf + 1, smtp->user, ulens);
    memcpy(enbuf + 1 + ulens + 1, smtp->psw, plens);
    char *b64;
    CALLOC(b64, 1, B64EN_SIZE(blens));
    blens = bs64_encode(enbuf, blens, b64);
    FREE(enbuf);
    char *cmd = format_va("%s%s", b64, FLAG_CRLF);
    FREE(b64);
    ud->status = AUTH_CHECK;
    if (ERR_OK != ev_send(ev, fd, skid, cmd, strlen(cmd), 0)) {
        BIT_SET(*status, PROT_ERROR);
    }
}
static void _smtp_auth(smtp_ctx *smtp, ev_ctx *ev, SOCKET fd, uint64_t skid, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    size_t blens = buffer_size(buf);
    if (blens < SMTP_CODE_LENS + CRLF_SIZE) {
        BIT_SET(*status, PROT_MOREDATA);
        return;
    }
    if (ERR_FAILED == buffer_search(buf, 0, blens - CRLF_SIZE, 0, FLAG_CRLF, CRLF_SIZE)) {
        BIT_SET(*status, PROT_MOREDATA);
        return;
    }
    switch (smtp->authtype) {
    case LOGIN:
        _smtp_loin(smtp, ev, fd, skid, buf, ud, status);
        break;
    case PLAIN:
        _smtp_plain(smtp, ev, fd, skid, buf, ud, status);
        break;
    }
}
static void _smtp_auth_check(smtp_ctx *smtp, ev_ctx *ev, SOCKET fd, uint64_t skid, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    size_t blens = buffer_size(buf);
    if (blens < SMTP_CODE_LENS + CRLF_SIZE) {
        BIT_SET(*status, PROT_MOREDATA);
        return;
    }
    if (ERR_FAILED == buffer_search(buf, 0, blens - CRLF_SIZE, 0, FLAG_CRLF, CRLF_SIZE)) {
        BIT_SET(*status, PROT_MOREDATA);
        return;
    }
    char code[SMTP_CODE_LENS + 1] = { 0 };
    ASSERTAB(SMTP_CODE_LENS == buffer_copyout(buf, 0, code, SMTP_CODE_LENS), "copy buffer failed.");
    if (0 != strcmp(code, "235")) {
        BIT_SET(*status, PROT_ERROR);
        char *err;
        CALLOC(err, 1, blens);
        ASSERTAB(blens - CRLF_SIZE == buffer_copyout(buf, 0, err, blens - CRLF_SIZE), "copy buffer failed.");
        _hs_push(fd, skid, 1, ud, ERR_FAILED, err, blens - CRLF_SIZE);
        return;
    }
    buffer_drain(buf, blens);
    _hs_push(fd, skid, 1, ud, ERR_OK, NULL, 0);
    ud->status = COMMAND;
}
static char *_smtp_command(smtp_ctx *smtp, ev_ctx *ev, SOCKET fd, uint64_t skid, buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status) {
    size_t blens = buffer_size(buf);
    if (blens < SMTP_CODE_LENS + CRLF_SIZE) {
        BIT_SET(*status, PROT_MOREDATA);
        return NULL;
    }
    if (ERR_FAILED == buffer_search(buf, 0, blens - CRLF_SIZE, 0, FLAG_CRLF, CRLF_SIZE)) {
        BIT_SET(*status, PROT_MOREDATA);
        return NULL;
    }
    char *pack;
    *size = blens - CRLF_SIZE;
    CALLOC(pack, 1, *size);
    ASSERTAB(*size == buffer_copyout(buf, 0, pack, *size), "copy buffer failed.");
    buffer_drain(buf, blens);
    return pack;
}
void *smtp_unpack(ev_ctx *ev, SOCKET fd, uint64_t skid, buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status) {
    smtp_ctx *smtp = (smtp_ctx *)ud->extra;
    void *pack = NULL;
    switch (ud->status) {
    case INIT:
        _smtp_connected(smtp, ev, fd, skid, buf, ud, status);
        break;
    case EHLO:
        _smtp_ehlo(smtp, ev, fd, skid, buf, ud, status);
        break;
    case AUTH:
        _smtp_auth(smtp, ev, fd, skid, buf, ud, status);
        break;
    case AUTH_CHECK:
        _smtp_auth_check(smtp, ev, fd, skid, buf, ud, status);
        break;
    case COMMAND:
        pack = _smtp_command(smtp, ev, fd, skid, buf, ud, size, status);
        break;
    }
    return pack;
}
