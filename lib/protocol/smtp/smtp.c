#include "protocol/smtp/smtp.h"
#include "utils/binary.h"
#include "event/event.h"
#include "crypt/base64.h"
#include "protocol/prots_pub.h"

#define SMTP_OK "250"
#define SMTP_CODE_LENS 3
#define SMTP_CMD_LIT(cmd) dup_zero(cmd FLAG_CRLF, sizeof(cmd FLAG_CRLF) - 1) // 固定命令行的堆上副本

typedef enum parse_status {
    INIT = 0,  //初始连接，等待服务端 220 响应
    EHLO,      //已发送 EHLO，等待服务端能力列表响应
    AUTH,      //已发送 AUTH 命令，等待服务端 334 挑战
    AUTH_CHECK,//已发送认证凭据，等待服务端 235 成功响应
    COMMAND,   //握手完成，正常命令交互阶段
    AUTH_PSW,  //AUTH LOGIN 已回用户名，等第二个 334 挑战（放在末尾：测试里按数值写了前几个）
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
    sock_mark_closed(&smtp->sk);// 网络线程上写，理由见 sock_mark_closed
    ud->context = NULL;
    PROT_REF_RELEASE(smtp);
}
int32_t smtp_init(smtp_ctx *smtp, const char *ip, uint16_t port, struct evssl_ctx *evssl, const char *user, const char *psw) {
    ZERO(smtp, sizeof(smtp_ctx));
    smtp->port = port;
    smtp->evssl = evssl;
    sock_set_invalid(&smtp->sk);
    // safe_fill_str 装不下即拒绝写入并返回 ERR_FAILED：psw 只有 64 字节，OAuth token 之类
    // 轻松超过，截断后拿去认证只换回服务端一句 535，本地一点线索都没有。
    // 失败时 smtp 已被 ZERO 且可能填了前几个字段，调用方按 init 失败处理（丢弃或 FREE），不得继续用
    if (ERR_OK != safe_fill_str(smtp->ip, sizeof(smtp->ip), ip)) {
        LOG_ERROR("smtp ip exceeds %zu bytes: %zu.", sizeof(smtp->ip) - 1, strlen(ip));
        return ERR_FAILED;
    }
    if (ERR_OK != safe_fill_str(smtp->user, sizeof(smtp->user), user)) {
        LOG_ERROR("smtp user name exceeds %zu bytes: %zu.", sizeof(smtp->user) - 1, strlen(user));
        return ERR_FAILED;
    }
    // 只报长度，不打印密码本身
    if (ERR_OK != safe_fill_str(smtp->psw, sizeof(smtp->psw), psw)) {
        LOG_ERROR("smtp password exceeds %zu bytes: %zu.", sizeof(smtp->psw) - 1, strlen(psw));
        return ERR_FAILED;
    }
    return ERR_OK;
}
int32_t smtp_check_codes(char *pack, const char *const *codes, size_t ncode) {
    for (size_t i = 0; i < ncode; i++) {
        if (0 == strncmp(pack, codes[i], strlen(codes[i]))) {
            return ERR_OK;
        }
    }
    // 告警只在这里打：全部码都不中才算失败，逐码打会给"命中第二个码"的正常应答刷假告警
    LOG_WARN("%s", pack);
    return ERR_FAILED;
}
int32_t smtp_check_code(char *pack, const char *code) {
    return smtp_check_codes(pack, &code, 1);
}
int32_t smtp_check_ok(char *pack) {
    return smtp_check_code(pack, SMTP_OK);
}
// 拼一行命令 pre + arg + suf(suf 自带行尾 CRLF)，返回堆上 '\0' 结尾的串
static char *_smtp_cmd_line(const char *pre, const char *arg, const char *suf) {
    size_t plen = strlen(pre);
    size_t alen = strlen(arg);
    size_t slen = strlen(suf);
    char *cmd;
    MALLOC(cmd, plen + alen + slen + 1);
    memcpy(cmd, pre, plen);
    memcpy(cmd + plen, arg, alen);
    memcpy(cmd + plen + alen, suf, slen + 1);
    return cmd;
}
// 凭据编成 Base64 直接写进命令缓冲再补 CRLF，不留中间的编码副本
static char *_smtp_b64_line(const void *data, size_t lens) {
    char *cmd;
    MALLOC(cmd, B64EN_SIZE(lens) + CRLF_SIZE);
    size_t n = bs64_encode(data, lens, cmd);
    memcpy(cmd + n, FLAG_CRLF, CRLF_SIZE + 1);
    return cmd;
}
char *smtp_pack_reset(void) {
    return SMTP_CMD_LIT("RSET");
}
char *smtp_pack_quit(void) {
    return SMTP_CMD_LIT("QUIT");
}
char *smtp_pack_ping(void) {
    return SMTP_CMD_LIT("NOOP");
}
// \0 结尾串里有没有 CR / LF
static inline int32_t _smtp_has_crlf(const char *s) {
    size_t lens = strlen(s);
    return lens != memcspn(s, lens, "\r\n", 2);
}
char *smtp_pack_from(const char *from) {
    //拒绝 CRLF 注入：邮件地址含 \r 或 \n 时返回 NULL
    if (NULL == from || _smtp_has_crlf(from)) {
        return NULL;
    }
    return _smtp_cmd_line("MAIL FROM:<", from, ">" FLAG_CRLF);
}
char *smtp_pack_rcpt(const char *rcpt) {
    //拒绝 CRLF 注入：邮件地址含 \r 或 \n 时返回 NULL
    if (NULL == rcpt || _smtp_has_crlf(rcpt)) {
        return NULL;
    }
    return _smtp_cmd_line("RCPT TO:<", rcpt, ">" FLAG_CRLF);
}
char *smtp_pack_data(void) {
    return SMTP_CMD_LIT("DATA");
}
// 在 buffer 中扫描完整 SMTP 多行响应（RFC 5321 §4.2.1）
// 多行格式：每行 "<code><sep>[text]\r\n"，sep='-' 表示后续仍有行，sep=' ' 或裸行 "<code>\r\n" 表示结束行
// code 非 NULL 时校验每行 code 一致；code 为 NULL 时以首行 code 为准（COMMAND 命令响应 code 不固定）
// 返回值：>0 表示完整响应总字节数（含末尾 CRLF）；0 表示需要等待更多数据；ERR_FAILED 表示协议错误
static int32_t _smtp_full_response(buffer_ctx *buf, const char *code) {
    size_t blens = buffer_size(buf);
    if (blens > SMTP_MAX_PACK_LENS) {
        return ERR_FAILED;
    }
    int32_t pos = 0;
    char line[SMTP_CODE_LENS + 1];
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
        if (sizeof(line) != buffer_copyout(buf, (size_t)pos, line, sizeof(line))) {
            return 0;
        }
        if (!haveexp) {
            memcpy(expect, line, SMTP_CODE_LENS);
            haveexp = 1;
        }
        // 整行到齐再判：拒绝行的 CRLF 还没到就判失败的话，调用方交不出这行原文
        crlf = buffer_search(buf, 0, (size_t)pos + SMTP_CODE_LENS, 0, FLAG_CRLF, CRLF_SIZE);
        if (ERR_FAILED == crlf) {
            return 0;
        }
        sep = line[SMTP_CODE_LENS];
        //code 之后是 '-'(续行) / ' '(结束行) / CR(裸结束行 "<code>\r\n")，其余非法
        if (0 != memcmp(line, expect, SMTP_CODE_LENS)
            || ('-' != sep && ' ' != sep && '\r' != sep)) {
            return ERR_FAILED;
        }
        if ('-' != sep) {
            return crlf + (int32_t)CRLF_SIZE;//结束行尾部位置
        }
        pos = crlf + (int32_t)CRLF_SIZE;//继续扫描下一行
    }
    return 0;
}
// 把缓冲区开头到 crlf 之间的那行原文当失败原因回给等待者（问候被拒与认证被拒共用）。
// 载荷所有权交给 _hs_push，成功失败它都会释放，调用方不必也不能再碰；
// 从哪儿开始找 CRLF、事后要不要 drain，两个调用点各不相同，留在各自那边
// 长度由调用方定:整段多行响应都要交出去的场合用它(见 _smtp_auth 末尾)。
// 只交首行的常见场合走 _smtp_push_firstline
static void _smtp_push_errline(sock_ctx *sk, ud_cxt *ud, buffer_ctx *buf, int32_t crlf) {
    char *line;
    CALLOC(line, 1, (size_t)crlf + 1);
    ASSERTAB((size_t)crlf == buffer_copyout(buf, 0, line, (size_t)crlf), "copy buffer failed.");
    _hs_push(sk, 1, ud, ERR_FAILED, line, (size_t)crlf);
}
// 把缓冲里第一行(到首个 CRLF 为止)作为失败原因交给等待方；没有 CRLF 或该行超长即不交
static inline void _smtp_push_firstline(sock_ctx *sk, ud_cxt *ud, buffer_ctx *buf) {
    int32_t crlf = buffer_search(buf, 0, 0, 0, FLAG_CRLF, CRLF_SIZE);
    if (crlf <= 0
        || crlf > SMTP_MAX_PACK_LENS) {
        return;
    }
    _smtp_push_errline(sk, ud, buf, crlf);
}
// INIT 阶段：等待服务端 220 欢迎行，收到后发送 EHLO 命令并切换到 EHLO 状态。
// EHLO 参数直接取 220 行中的服务器主机名（"220[ -]hostname ..."的第二个 token），
// 以服务器返回值为准，避免本机 gethostname() 返回无效域名被拒绝。
static void _smtp_connected(ev_ctx *ev, sock_ctx *sk, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    //等待完整的 220 多行响应（RFC 5321 §4.2.1，TCP 分包时不可凭单个 CRLF 判定完整）
    int32_t total = _smtp_full_response(buf, "220");
    if (ERR_FAILED == total) {
        BIT_SET(*status, PROT_ERROR);
        // 服务端可以拿 421/554 之类的问候直接拒连(限流 / 黑名单 / TLS-only)，原因就在缓冲里这一行。
        // 丢掉的话业务只看到一次无原因的握手失败
        _smtp_push_firstline(sk, ud, buf);
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
    //首行 CRLF 位置作为主机名搜索上界，避免越界扫描到后续行；须从 0 起找，host_start 可能已越过短行(如裸"220")的 CRLF
    int32_t first_crlf = buffer_search(buf, 0, 0, 0, FLAG_CRLF, CRLF_SIZE);
    int32_t host_end = buffer_search(buf, 1, (size_t)host_start, 0, " ", 1);
    if (ERR_FAILED == host_end
        || (ERR_FAILED != first_crlf && first_crlf < host_end)) {
        host_end = first_crlf;
    }
    if (ERR_FAILED != host_end
        && host_end > host_start
        && (size_t)(host_end - host_start) < HOST_LENS) {
        size_t hlens = (size_t)(host_end - host_start);
        ASSERTAB(hlens == buffer_copyout(buf, host_start, svhost, hlens), "copy buffer failed.");
        // 裸 LF / 裸 CR 不是 _smtp_full_response 认的行尾，能混在首行里活到这儿；
        // 原样拼进 EHLO 就是往自己的命令行里插了第二条命令（smtp_pack_from/rcpt 同样拒这两个字节）
        if (NULL != memchr(svhost, '\r', hlens)
            || NULL != memchr(svhost, '\n', hlens)) {
            LOG_WARN("smtp greeting host contains CR/LF, fall back to localhost.");
            svhost[0] = '\0';
        }
    }
    buffer_drain(buf, (size_t)total);
    char *cmd = _smtp_cmd_line("EHLO ", '\0' != svhost[0] ? svhost : "localhost", FLAG_CRLF);
    ud->status = EHLO;
    if (ERR_OK != ev_send(ev, sk, cmd, strlen(cmd), 0)) {
        BIT_SET(*status, PROT_ERROR);
    }
}
// AUTH 行 [pos, end) 这段机制列表里有没有 mech：按空格切词整词比较，大小写不敏感。
// 子串不算，"PLAIN-CLIENTTOKEN" 不是 PLAIN
static int32_t _smtp_has_mech(buffer_ctx *buf, int32_t pos, int32_t end, const char *mech) {
    size_t mlens = strlen(mech);
    char word[8];// 调用方只传 "PLAIN" / "LOGIN"
    int32_t sp;
    while (pos < end) {
        sp = buffer_search(buf, 0, (size_t)pos, (size_t)end, " ", 1);
        if (ERR_FAILED == sp) {
            sp = end;
        }
        if ((size_t)(sp - pos) == mlens) {
            ASSERTAB(mlens == buffer_copyout(buf, (size_t)pos, word, mlens), "copy buffer failed.");
            if (0 == memcasecmp(word, mech, mlens)) {
                return 1;
            }
        }
        pos = sp + 1;
    }
    return 0;
}
// 从 EHLO 响应里解析认证类型，优先 PLAIN 其次 LOGIN。total 是本条 250 响应的字节数，
// 搜索一律卡在它之内。返回 smtp_authtype，没有可用的 AUTH 通告返回 ERR_FAILED。
// RFC 5321 §4.2.1：多行响应中间行用 '-' 分隔，末行/单行用空格分隔；
// 带尾随空格的字面量避免 "250-AUTHENTICATION" 等其他扩展误匹配
static int32_t _smtp_get_authtype(buffer_ctx *buf, int32_t total) {
    // 挡到 1：total - 1 在 0 处下溢成 SIZE_MAX，在 1 处算出 0，
    // 而 buffer_search 的 end 为 0 表示"搜到缓冲末尾"，边界会整个失效
    if (total <= 1) {
        return ERR_FAILED;
    }
    const char *authmid = "250-AUTH ";
    const char *authend = "250 AUTH ";
    size_t mlen = strlen(authmid);
    size_t elen = strlen(authend);
    size_t last = (size_t)total - 1;// buffer_search 的 end 是含端下标，不是长度
    int32_t start = buffer_search(buf, 1, 0, last, (char *)authmid, mlen);
    if (ERR_FAILED != start) {
        start += (int32_t)mlen;
    } else {
        start = buffer_search(buf, 1, 0, last, (char *)authend, elen);
        if (ERR_FAILED == start) {
            LOG_WARN("can't find auth type.");
            return ERR_FAILED;
        }
        start += (int32_t)elen;
    }
    int32_t end = buffer_search(buf, 1, start, last, FLAG_CRLF, CRLF_SIZE);
    if (ERR_FAILED == end) {
        LOG_WARN("format error.");
        return ERR_FAILED;
    }
    if (_smtp_has_mech(buf, start, end, "PLAIN")) {
        return PLAIN;
    }
    if (_smtp_has_mech(buf, start, end, "LOGIN")) {
        return LOGIN;
    }
    return ERR_FAILED;
}
// EHLO 阶段：等待服务端 250 响应，解析认证类型并发送 AUTH 命令，切换到 AUTH 状态；没设用户名则直接握手完成
static void _smtp_ehlo(smtp_ctx *smtp, ev_ctx *ev, sock_ctx *sk, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    //等待完整的 250 多行 EHLO 响应（典型形如 "250-AUTH LOGIN PLAIN\r\n250 OK\r\n"）
    int32_t total = _smtp_full_response(buf, SMTP_OK);
    if (ERR_FAILED == total) {
        BIT_SET(*status, PROT_ERROR);
        // 同 _smtp_connected: 服务端 502/550 拒 EHLO 的原因就在缓冲里这一行, 丢了业务无从查
        _smtp_push_firstline(sk, ud, buf);
        return;
    }
    if (0 == total) {
        BIT_SET(*status, PROT_MOREDATA);
        return;
    }
    // 没给用户名就是免认证投递：服务端通不通告 AUTH 都不认证，EHLO 成功即握手完成
    if ('\0' == smtp->user[0]) {
        buffer_drain(buf, (size_t)total);
        if (ERR_OK != _hs_push(sk, 1, ud, ERR_OK, NULL, 0)) {
            BIT_SET(*status, PROT_ERROR);
            return;
        }
        ud->status = COMMAND;
        return;
    }
    smtp->authtype = _smtp_get_authtype(buf, total);
    if (ERR_FAILED == smtp->authtype) {
        BIT_SET(*status, PROT_ERROR);
        // 交整份 250 应答而不是首行:失败原因是"能力列表里没有可用的 AUTH",
        // 而首行只是问候行,拿它当原因反而误导。取原文要在 drain 之前
        _smtp_push_errline(sk, ud, buf, total - (int32_t)CRLF_SIZE);
        return;
    }
    buffer_drain(buf, (size_t)total);
    char *cmd = NULL;
    switch (smtp->authtype) {
    case LOGIN:
        cmd = SMTP_CMD_LIT("AUTH LOGIN");
        break;
    case PLAIN:
        cmd = SMTP_CMD_LIT("AUTH PLAIN");
        break;
    default:// authtype 是 int32_t, -Wswitch 盯不住; 漏一档就是 strlen(NULL)。口径同 _smtp_auth
        BIT_SET(*status, PROT_ERROR);
        return;
    }
    ud->status = AUTH;
    if (ERR_OK != ev_send(ev, sk, cmd, strlen(cmd), 0)) {
        BIT_SET(*status, PROT_ERROR);
    }
}
// 334 挑战体（"334 " 与 CRLF 之间）是不是非空的合法 Base64；超长也算不合法。
// 解码在中途撞上非法字符会就地返回 0，"334 dXNlcm5hbWU6!!!" 这种半截合法的也拦得住
static int32_t _smtp_challenge_ok(buffer_ctx *buf, int32_t crlf) {
    if (crlf <= SMTP_CODE_LENS + 1
        || crlf - SMTP_CODE_LENS - 1 > SMTP_MAX_PACK_LENS) {
        return 0;
    }
    size_t lens = (size_t)crlf - SMTP_CODE_LENS - 1;
    char *b64;
    MALLOC(b64, lens + B64DE_SIZE(lens));
    ASSERTAB(lens == buffer_copyout(buf, SMTP_CODE_LENS + 1, b64, lens), "copy buffer failed.");
    size_t declens = bs64_decode(b64, lens, b64 + lens);
    FREE(b64);
    return 0 != declens;
}
// AUTH LOGIN 认证阶段：第一个 334 回用户名、第二个回密码，按顺序配对，不认提示文本——
// 各家提示不一（"Username:" 之外还有 "User Name" 与本地化文本）。挑战不合法即断开，原文交给等待方
static void _smtp_loin(smtp_ctx *smtp, ev_ctx *ev, sock_ctx *sk, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    // _smtp_auth 已确认首个 CRLF 在缓冲里，只取这一行，不和后面的响应混
    int32_t crlf = buffer_search(buf, 0, 0, 0, FLAG_CRLF, CRLF_SIZE);
    if (!_smtp_challenge_ok(buf, crlf)) {
        BIT_SET(*status, PROT_ERROR);
        _smtp_push_firstline(sk, ud, buf);
        return;
    }
    buffer_drain(buf, (size_t)crlf + CRLF_SIZE);
    char *cmd;
    if (AUTH == ud->status) {
        cmd = _smtp_b64_line(smtp->user, strlen(smtp->user));
        ud->status = AUTH_PSW;
    } else {
        cmd = _smtp_b64_line(smtp->psw, strlen(smtp->psw));
        ud->status = AUTH_CHECK;
    }
    if (ERR_OK != ev_send(ev, sk, cmd, strlen(cmd), 0)) {
        BIT_SET(*status, PROT_ERROR);
    }
}
// AUTH PLAIN 认证阶段：构造 "\0user\0password" 格式并 Base64 编码后发送，切换到 AUTH_CHECK 状态
static void _smtp_plain(smtp_ctx *smtp, ev_ctx *ev, sock_ctx *sk, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
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
    char *cmd = _smtp_b64_line(enbuf, enlens);
    SECURE_FREE(enbuf, enlens);
    ud->status = AUTH_CHECK;
    if (ERR_OK != ev_send(ev, sk, cmd, strlen(cmd), 0)) {
        BIT_SET(*status, PROT_ERROR);
    }
}
// AUTH 阶段：等待完整的服务端挑战行，根据认证类型分发到 LOGIN 或 PLAIN 处理函数
static void _smtp_auth(smtp_ctx *smtp, ev_ctx *ev, sock_ctx *sk, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    size_t blens = buffer_size(buf);
    if (blens < SMTP_CODE_LENS + CRLF_SIZE) {
        BIT_SET(*status, PROT_MOREDATA);
        return;
    }
    //找首个 CRLF 而非末尾 CRLF，支持流水线场景下首条已完整即可消费
    if (ERR_FAILED == buffer_search(buf, 0, 0, 0, FLAG_CRLF, CRLF_SIZE)) {
        if (blens > SMTP_MAX_PACK_LENS) {
            BIT_SET(*status, PROT_ERROR);
            return;
        }
        BIT_SET(*status, PROT_MOREDATA);
        return;
    }
    // 两种机制的挑战行同一形状：码必须是 334，且只能是单行——续行形式（"334-"）下面接不住，
    // 残留那行会错开后续配对。上面已保证至少 code + CRLF 五字节，第 4 字节必然可读
    char code[SMTP_CODE_LENS + 1] = { 0 };
    ASSERTAB(SMTP_CODE_LENS == buffer_copyout(buf, 0, code, SMTP_CODE_LENS), "copy buffer failed.");
    char sep;
    ASSERTAB(1 == buffer_copyout(buf, SMTP_CODE_LENS, &sep, 1), "copy buffer failed.");
    if (0 != strcmp(code, "334")
        || '-' == sep) {
        BIT_SET(*status, PROT_ERROR);
        // 同 _smtp_connected: AUTH 被 504/538/530/454 明文拒是生产上最常见的一档, 原因在这一行
        _smtp_push_firstline(sk, ud, buf);
        return;
    }
    switch (smtp->authtype) {
    case LOGIN:
        _smtp_loin(smtp, ev, sk, buf, ud, status);
        break;
    case PLAIN:
        _smtp_plain(smtp, ev, sk, buf, ud, status);
        break;
    default:
        BIT_SET(*status, PROT_ERROR);
        break;
    }
}
// AUTH_CHECK 阶段：等待服务端 235 认证成功响应，成功后切换到 COMMAND 状态并触发握手完成回调
static void _smtp_auth_check(sock_ctx *sk, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    // 认证结果也可能是多行（"235-...\r\n235 ...\r\n" 合法），必须整段消费
    int32_t total = _smtp_full_response(buf, NULL);
    if (ERR_FAILED == total) {
        BIT_SET(*status, PROT_ERROR);
        // 框不出整段时原文还在缓冲里，照 _smtp_connected 把首行交出去，别让业务只看到一次无原因的失败
        _smtp_push_firstline(sk, ud, buf);
        return;
    }
    if (0 == total) {
        BIT_SET(*status, PROT_MOREDATA);
        return;
    }
    char code[SMTP_CODE_LENS + 1] = { 0 };
    ASSERTAB(SMTP_CODE_LENS == buffer_copyout(buf, 0, code, SMTP_CODE_LENS), "copy buffer failed.");
    if (0 != strcmp(code, "235")) {
        BIT_SET(*status, PROT_ERROR);
        // 整段响应都当失败原因交出去：多行诊断（如 Gmail 把说明链接放在第二行）不能只留首行
        _smtp_push_errline(sk, ud, buf, total - (int32_t)CRLF_SIZE);// 取原文要在 drain 之前
        buffer_drain(buf, (size_t)total);
        return;
    }
    buffer_drain(buf, (size_t)total);
    if (ERR_OK != _hs_push(sk, 1, ud, ERR_OK, NULL, 0)) {
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
void *smtp_unpack(ev_ctx *ev, sock_ctx *sk, int32_t client,
    buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status) {
    (void)client;
    smtp_ctx *smtp = (smtp_ctx *)ud->context;
    void *pack = NULL;
    switch (ud->status) {
    case INIT:
        _smtp_connected(ev, sk, buf, ud, status);
        break;
    case EHLO:
        if (NULL == smtp) {
            BIT_SET(*status, PROT_ERROR);
            break;
        }
        _smtp_ehlo(smtp, ev, sk, buf, ud, status);
        break;
    case AUTH:
    case AUTH_PSW:
        if (NULL == smtp) {
            BIT_SET(*status, PROT_ERROR);
            break;
        }
        _smtp_auth(smtp, ev, sk, buf, ud, status);
        break;
    case AUTH_CHECK:
        _smtp_auth_check(sk, buf, ud, status);
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
