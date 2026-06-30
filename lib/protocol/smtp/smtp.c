#include "protocol/smtp/smtp.h"
#include "utils/utils.h"
#include "utils/binary.h"
#include "srey/task.h"
#include "protocol/prots.h"
#include "crypt/base64.h"

#define SMTP_OK "250"
#define SMTP_CODE_LENS 3

typedef enum smtp_authtype {
    LOGIN = 1, //AUTH LOGIN 认证方式（逐字段 Base64 编码）
    PLAIN      //AUTH PLAIN 认证方式（整体 Base64 编码）
}smtp_authtype;
typedef enum parse_status {
    INIT = 0,  //初始连接，等待服务端 220 响应
    EHLO,      //已发送 EHLO，等待服务端能力列表响应
    AUTH,      //已发送 AUTH 命令，等待服务端 334 挑战
    AUTH_CHECK,//已发送认证凭据，等待服务端 235 成功响应
    COMMAND,   //握手完成，正常命令交互阶段
}parse_status;
static _handshaked_push _hs_push;

void _smtp_init(void *hspush) {
    _hs_push = (_handshaked_push)hspush;
}
void _smtp_udfree(ud_cxt *ud) {
    if (NULL == ud->context) {
        return;
    }
    smtp_ctx *smtp = ud->context;
    smtp->sk.fd = INVALID_SOCK;
    ud->context = NULL;
}
void _smtp_closed(ud_cxt *ud) {
    _smtp_udfree(ud);
}
void smtp_init(smtp_ctx *smtp, const char *ip, uint16_t port, struct evssl_ctx *evssl, const char *user, const char *psw) {
    ZERO(smtp, sizeof(smtp_ctx));
    safe_fill_str(smtp->ip, sizeof(smtp->ip), ip);
    smtp->port = port;
    smtp->evssl = evssl;
    smtp->sk.fd = INVALID_SOCK;
    safe_fill_str(smtp->user, sizeof(smtp->user), user);
    safe_fill_str(smtp->psw, sizeof(smtp->psw), psw);
}
int32_t smtp_try_connect(task_ctx *task, smtp_ctx *smtp) {
    smtp->task = task;
    return task_connect(task, PACK_SMTP, smtp->evssl, smtp->ip, smtp->port, 0, smtp, &smtp->sk.fd, &smtp->sk.skid);
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
char *smtp_pack_reset(void) {
    return format_va("RSET%s", FLAG_CRLF);
}
char *smtp_pack_quit(void) {
    return format_va("QUIT%s", FLAG_CRLF);
}
char *smtp_pack_ping(void) {
    return format_va("NOOP%s", FLAG_CRLF);
}
char *smtp_pack_from(const char *from) {
    //拒绝 CRLF 注入：邮件地址含 \r 或 \n 时返回 NULL
    if (NULL == from || NULL != strpbrk(from, "\r\n")) {
        return NULL;
    }
    return format_va("MAIL FROM:<%s>%s", from, FLAG_CRLF);
}
char *smtp_pack_rcpt(const char *rcpt) {
    //拒绝 CRLF 注入：邮件地址含 \r 或 \n 时返回 NULL
    if (NULL == rcpt || NULL != strpbrk(rcpt, "\r\n")) {
        return NULL;
    }
    return format_va("RCPT TO:<%s>%s", rcpt, FLAG_CRLF);
}
char *smtp_pack_data(void) {
    return format_va("DATA%s", FLAG_CRLF);
}
// 在 buffer 中扫描完整 SMTP 多行响应（RFC 5321 §4.2.1）
// 多行格式：每行 "<code><sep>[text]\r\n"，sep='-' 表示后续仍有行，sep=' ' 或裸行 "<code>\r\n" 表示结束行
// code 非 NULL 时校验每行 code 一致；code 为 NULL 时以首行 code 为准（COMMAND 命令响应 code 不固定）
// 返回值：>0 表示完整响应总字节数（含末尾 CRLF）；0 表示需要等待更多数据；ERR_FAILED 表示协议错误
int32_t _smtp_full_response(buffer_ctx *buf, const char *code) {
    size_t blens = buffer_size(buf);
    if (PACK_TOO_LONG(blens)) {
        return ERR_FAILED;
    }
    int32_t pos = 0;
    char line[SMTP_CODE_LENS];
    char expect[SMTP_CODE_LENS] = { 0 };
    char sep;
    int32_t crlf;
    int32_t haveexp = (NULL != code);
    if (haveexp) {
        memcpy(expect, code, SMTP_CODE_LENS);
    }
    while ((size_t)pos < blens) {
        //每行至少需要 code(3) + CRLF(2) = 5 字节（裸结束行 "<code>\r\n"）
        if (blens - (size_t)pos < SMTP_CODE_LENS + CRLF_SIZE) {
            return 0;
        }
        if (SMTP_CODE_LENS != buffer_copyout(buf, (size_t)pos, line, SMTP_CODE_LENS)) {
            return 0;
        }
        if (!haveexp) {
            memcpy(expect, line, SMTP_CODE_LENS);
            haveexp = 1;
        }
        if (0 != memcmp(line, expect, SMTP_CODE_LENS)) {
            return ERR_FAILED;
        }
        if (1 != buffer_copyout(buf, (size_t)pos + SMTP_CODE_LENS, &sep, 1)) {
            return 0;
        }
        //code 之后是 '-'(续行) / ' '(结束行) / CR(裸结束行 "<code>\r\n")，其余非法
        if ('-' != sep && ' ' != sep && '\r' != sep) {
            return ERR_FAILED;
        }
        crlf = buffer_search(buf, 0, (size_t)pos + SMTP_CODE_LENS, 0, FLAG_CRLF, CRLF_SIZE);
        if (ERR_FAILED == crlf) {
            return 0;
        }
        if ('-' != sep) {
            return crlf + (int32_t)CRLF_SIZE;//结束行尾部位置
        }
        pos = crlf + (int32_t)CRLF_SIZE;//继续扫描下一行
    }
    return 0;
}
// INIT 阶段：等待服务端 220 欢迎行，收到后发送 EHLO 命令并切换到 EHLO 状态。
// EHLO 参数直接取 220 行中的服务器主机名（"220[ -]hostname ..."的第二个 token），
// 以服务器返回值为准，避免本机 gethostname() 返回无效域名被拒绝。
static void _smtp_connected(ev_ctx *ev, SOCKET fd, uint64_t skid, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    //等待完整的 220 多行响应（RFC 5321 §4.2.1，TCP 分包时不可凭单个 CRLF 判定完整）
    int32_t total = _smtp_full_response(buf, "220");
    if (ERR_FAILED == total) {
        BIT_SET(*status, PROT_ERROR);
        return;
    }
    if (0 == total) {
        BIT_SET(*status, PROT_MOREDATA);
        return;
    }
    // 从 220 首行提取服务器主机名：跳过 "220 " 或 "220-"（4字节），
    // 取到下一个空格或 CRLF 之前的内容作为 EHLO 参数。
    char svhost[HOST_LENS] = { 0 };
    int32_t host_start = SMTP_CODE_LENS + 1; // 跳过 "220 " 或 "220-"
    //首行 CRLF 位置作为主机名搜索上界，避免越界扫描到后续行
    int32_t first_crlf = buffer_search(buf, 0, (size_t)host_start, 0, FLAG_CRLF, CRLF_SIZE);
    int32_t host_end = buffer_search(buf, 1, (size_t)host_start, 0, " ", 1);
    if (ERR_FAILED == host_end
        || (ERR_FAILED != first_crlf && first_crlf < host_end)) {
        host_end = first_crlf;
    }
    if (ERR_FAILED != host_end
        && host_end > host_start
        && (size_t)(host_end - host_start) < HOST_LENS) {
        ASSERTAB((size_t)(host_end - host_start) == buffer_copyout(buf, host_start, svhost, host_end - host_start), "copy buffer failed.");
    }
    buffer_drain(buf, (size_t)total);
    char *cmd = format_va("EHLO %s%s", '\0' != svhost[0] ? svhost : "localhost", FLAG_CRLF);
    ud->status = EHLO;
    if (ERR_OK != ev_send(ev, fd, skid, cmd, strlen(cmd), 0)) {
        BIT_SET(*status, PROT_ERROR);
    }
}
// 从 EHLO 响应中解析服务端支持的认证类型，优先返回 PLAIN，其次 LOGIN
// RFC 5321 §4.2.1：多行响应中间行用 '-' 分隔，末行/单行用空格分隔；
// 带尾随空格的字面量避免 "250-AUTHENTICATION" 等其他扩展误匹配
static int32_t _smtp_get_authtype(buffer_ctx *buf) {
    const char *authmid = "250-AUTH ";
    const char *authend = "250 AUTH ";
    size_t mlen = strlen(authmid);
    size_t elen = strlen(authend);
    int32_t start = buffer_search(buf, 1, 0, 0, (char *)authmid, mlen);
    if (ERR_FAILED != start) {
        start += (int32_t)mlen;
    } else {
        start = buffer_search(buf, 1, 0, 0, (char *)authend, elen);
        if (ERR_FAILED == start) {
            LOG_WARN("can't find auth type.");
            return ERR_FAILED;
        }
        start += (int32_t)elen;
    }
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
// EHLO 阶段：等待服务端 250 响应，解析认证类型并发送 AUTH 命令，切换到 AUTH 状态
static void _smtp_ehlo(smtp_ctx *smtp, ev_ctx *ev, SOCKET fd, uint64_t skid, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    //等待完整的 250 多行 EHLO 响应（典型形如 "250-AUTH LOGIN PLAIN\r\n250 OK\r\n"）
    int32_t total = _smtp_full_response(buf, SMTP_OK);
    if (ERR_FAILED == total) {
        BIT_SET(*status, PROT_ERROR);
        return;
    }
    if (0 == total) {
        BIT_SET(*status, PROT_MOREDATA);
        return;
    }
    smtp->authtype = _smtp_get_authtype(buf);
    if (ERR_FAILED == smtp->authtype) {
        BIT_SET(*status, PROT_ERROR);
        return;
    }
    buffer_drain(buf, (size_t)total);
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
// 对字符串进行 Base64 编码并追加 CRLF，构造 AUTH LOGIN 认证命令行
static char *_smtp_loin_cmd(const char *up) {
    size_t lens = strlen(up);
    char *b64;
    CALLOC(b64, 1, B64EN_SIZE(lens));
    lens = bs64_encode(up, lens, b64);
    char *cmd = format_va("%s%s", b64, FLAG_CRLF);
    secure_zero(b64, lens);
    FREE(b64);
    return cmd;
}
// AUTH LOGIN 认证阶段：解析服务端 334 挑战，按 "Username:"/"Password:" 顺序发送 Base64 凭据
static void _smtp_loin(smtp_ctx *smtp, ev_ctx *ev, SOCKET fd, uint64_t skid, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    char code[SMTP_CODE_LENS + 1] = { 0 };
    ASSERTAB(SMTP_CODE_LENS == buffer_copyout(buf, 0, code, SMTP_CODE_LENS), "copy buffer failed.");
    if (0 != strcmp(code, "334")) {
        BIT_SET(*status, PROT_ERROR);
        return;
    }
    //找首个 CRLF 确定单条响应边界，避免与流水线后续响应混淆
    int32_t crlf = buffer_search(buf, 0, SMTP_CODE_LENS + 1, 0, FLAG_CRLF, CRLF_SIZE);
    if (ERR_FAILED == crlf) {
        BIT_SET(*status, PROT_ERROR);
        return;
    }
    size_t lens = (size_t)crlf - SMTP_CODE_LENS - 1;
    // 空挑战（"334 \r\n"）或挑战体超长均非法，丢弃响应并报错
    if (0 == lens || PACK_TOO_LONG(lens)) {
        buffer_drain(buf, (size_t)crlf + CRLF_SIZE);
        BIT_SET(*status, PROT_ERROR);
        return;
    }
    char *b64flag;
    CALLOC(b64flag, 1, lens + 1);
    ASSERTAB(lens == buffer_copyout(buf, SMTP_CODE_LENS + 1, b64flag, lens), "copy buffer failed.");
    buffer_drain(buf, (size_t)crlf + CRLF_SIZE);
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
// AUTH PLAIN 认证阶段：构造 "\0user\0password" 格式并 Base64 编码后发送，切换到 AUTH_CHECK 状态
static void _smtp_plain(smtp_ctx *smtp, ev_ctx *ev, SOCKET fd, uint64_t skid, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    char code[SMTP_CODE_LENS + 1] = { 0 };
    ASSERTAB(SMTP_CODE_LENS == buffer_copyout(buf, 0, code, SMTP_CODE_LENS), "copy buffer failed.");
    if (0 != strcmp(code, "334")) {
        BIT_SET(*status, PROT_ERROR);
        return;
    }
    //找首个 CRLF 确定单条响应边界，仅消费当前响应
    int32_t crlf = buffer_search(buf, 0, SMTP_CODE_LENS, 0, FLAG_CRLF, CRLF_SIZE);
    if (ERR_FAILED == crlf) {
        BIT_SET(*status, PROT_ERROR);
        return;
    }
    buffer_drain(buf, (size_t)crlf + CRLF_SIZE);
    size_t ulens = strlen(smtp->user);
    size_t plens = strlen(smtp->psw);
    size_t enlens = ulens + plens + 2;
    char *enbuf;
    CALLOC(enbuf, 1, enlens);
    memcpy(enbuf + 1, smtp->user, ulens);
    memcpy(enbuf + 1 + ulens + 1, smtp->psw, plens);
    char *b64;
    CALLOC(b64, 1, B64EN_SIZE(enlens));
    size_t b64lens = bs64_encode(enbuf, enlens, b64);
    secure_zero(enbuf, enlens);
    FREE(enbuf);
    char *cmd = format_va("%s%s", b64, FLAG_CRLF);
    secure_zero(b64, b64lens);
    FREE(b64);
    ud->status = AUTH_CHECK;
    if (ERR_OK != ev_send(ev, fd, skid, cmd, strlen(cmd), 0)) {
        BIT_SET(*status, PROT_ERROR);
    }
}
// AUTH 阶段：等待完整的服务端挑战行，根据认证类型分发到 LOGIN 或 PLAIN 处理函数
static void _smtp_auth(smtp_ctx *smtp, ev_ctx *ev, SOCKET fd, uint64_t skid, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    size_t blens = buffer_size(buf);
    if (blens < SMTP_CODE_LENS + CRLF_SIZE) {
        BIT_SET(*status, PROT_MOREDATA);
        return;
    }
    //找首个 CRLF 而非末尾 CRLF，支持流水线场景下首条已完整即可消费
    if (ERR_FAILED == buffer_search(buf, 0, 0, 0, FLAG_CRLF, CRLF_SIZE)) {
        if (PACK_TOO_LONG(blens)) {
            BIT_SET(*status, PROT_ERROR);
            return;
        }
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
    default:
        BIT_SET(*status, PROT_ERROR);
        break;
    }
}
// AUTH_CHECK 阶段：等待服务端 235 认证成功响应，成功后切换到 COMMAND 状态并触发握手完成回调
static void _smtp_auth_check(SOCKET fd, uint64_t skid, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    size_t blens = buffer_size(buf);
    if (blens < SMTP_CODE_LENS + CRLF_SIZE) {
        BIT_SET(*status, PROT_MOREDATA);
        return;
    }
    //找首个 CRLF 而非末尾 CRLF，支持流水线场景下首条已完整即可消费
    if (ERR_FAILED == buffer_search(buf, 0, 0, 0, FLAG_CRLF, CRLF_SIZE)) {
        if (PACK_TOO_LONG(blens)) {
            BIT_SET(*status, PROT_ERROR);
            return;
        }
        BIT_SET(*status, PROT_MOREDATA);
        return;
    }
    char code[SMTP_CODE_LENS + 1] = { 0 };
    ASSERTAB(SMTP_CODE_LENS == buffer_copyout(buf, 0, code, SMTP_CODE_LENS), "copy buffer failed.");
    //找首个 CRLF 确定单条响应边界（与失败/成功路径共用，避免吞掉后续流水线响应）
    int32_t crlf = buffer_search(buf, 0, SMTP_CODE_LENS, 0, FLAG_CRLF, CRLF_SIZE);
    if (ERR_FAILED == crlf) {
        BIT_SET(*status, PROT_ERROR);
        return;
    }
    if (PACK_TOO_LONG(crlf)) {
        BIT_SET(*status, PROT_ERROR);
        return;
    }
    size_t total = (size_t)crlf + CRLF_SIZE;
    if (0 != strcmp(code, "235")) {
        BIT_SET(*status, PROT_ERROR);
        char *err;
        CALLOC(err, 1, (size_t)crlf + 1);
        ASSERTAB((size_t)crlf == buffer_copyout(buf, 0, err, (size_t)crlf), "copy buffer failed.");
        buffer_drain(buf, total);
        _hs_push(fd, skid, 1, ud, ERR_FAILED, err, (size_t)crlf);
        return;
    }
    buffer_drain(buf, total);
    if (ERR_OK != _hs_push(fd, skid, 1, ud, ERR_OK, NULL, 0)) {
        BIT_SET(*status, PROT_ERROR);
        return;
    }
    ud->status = COMMAND;
}
// COMMAND 阶段：等待完整服务端响应（含多行，RFC 5321 §4.2.1），提取内容（不含末尾 CRLF）返回给调用者
static char *_smtp_command(buffer_ctx *buf, size_t *size, int32_t *status) {
    //命令响应 code 不固定，传 NULL 让 _smtp_full_response 以首行 code 为准合并多行
    int32_t total = _smtp_full_response(buf, NULL);
    if (ERR_FAILED == total) {
        BIT_SET(*status, PROT_ERROR);
        return NULL;
    }
    if (0 == total) {
        BIT_SET(*status, PROT_MOREDATA);
        return NULL;
    }
    char *pack;
    *size = (size_t)total - CRLF_SIZE;
    CALLOC(pack, 1, *size + 1);
    ASSERTAB(*size == buffer_copyout(buf, 0, pack, *size), "copy buffer failed.");
    buffer_drain(buf, (size_t)total);
    return pack;
}
void *smtp_unpack(ev_ctx *ev, SOCKET fd, uint64_t skid, buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status) {
    smtp_ctx *smtp = (smtp_ctx *)ud->context;
    void *pack = NULL;
    switch (ud->status) {
    case INIT:
        _smtp_connected(ev, fd, skid, buf, ud, status);
        break;
    case EHLO:
        _smtp_ehlo(smtp, ev, fd, skid, buf, ud, status);
        break;
    case AUTH:
        _smtp_auth(smtp, ev, fd, skid, buf, ud, status);
        break;
    case AUTH_CHECK:
        _smtp_auth_check(fd, skid, buf, ud, status);
        break;
    case COMMAND:
        pack = _smtp_command(buf, size, status);
        break;
    default:
        BIT_SET(*status, PROT_ERROR);
        break;
    }
    return pack;
}
