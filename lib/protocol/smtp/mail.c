#include "protocol/smtp/mail.h"
#include "crypt/base64.h"
#include "utils/utils.h"
#include "utils/contenttype.h"
#include "utils/binary.h"

#define MIME_CHARSET "utf-8"
#define MIME_B64_LINE 76 // RFC 2045 §6.8：base64 每行不超过 76 字符
#define MIME_B64_RAW 57 // 编成 base64 恰好 76 字符的原文字节数（76/4*3），即一行的原文用量
#define MIME_BOUND_RAND 16 // boundary 的随机字节数，转 hex 后即 boundary 主体
#define MIME_BOUND_LENS (HEX_ENSIZE(MIME_BOUND_RAND) + 8) // hex 主体 + "srey_" 前缀 + 余量
// 每个 RFC 2047 encoded-word 最多编码的原文字节数。单个 word 连同 "=?utf-8?B?" 与 "?=" 不得
// 超过 75 字符：45 字节原文 → base64 60 字符 + 12 字符外壳 = 72，是 3 的整数倍里最大的那个
#define MIME_EW_RAW 45
// 头字段裸写 ASCII 的长度上限。RFC 5322 §2.1.1 限一行 998 octet，减去字段名与余量；
// 超过就改走 encoded-word，那条路按 MIME_EW_RAW 切段且自带折行
#define MIME_HDR_RAW_MAX 900

void mail_init(mail_ctx *mail) {
    ZERO(mail, sizeof(mail_ctx));
    mail->reply = 1;
    array_init(&mail->addrs, sizeof(mail_addr), 0);
    array_init(&mail->attach, sizeof(mail_attach), 0);
}
// 释放附件列表中每个附件的 content 缓冲区（不释放数组本身）
static void _mail_attach_free(array_ctx *attach) {
    for (uint32_t i = 0; i < array_size(attach); i++) {
        FREE(((mail_attach *)array_at(attach, i))->content);
    }
}
void mail_free(mail_ctx *mail) {
    FREE(mail->subject);
    FREE(mail->msg);
    FREE(mail->html);
    array_free(&mail->addrs);
    _mail_attach_free(&mail->attach);
    array_free(&mail->attach);
}
void mail_reply(mail_ctx *mail, int32_t reply) {
    mail->reply = reply;
}
/* 将 src 复制到 dst（最多 maxlen-1 字节），跳过所有 \r 和 \n 字符，末尾补 '\0'。
 * 用于邮件头字段的注入过滤：任何包含 CRLF 的用户输入均被净化，
 * 防止攻击者通过 Subject/From/To 等字段注入任意 MIME 头部（如 Bcc 注入）。*/
static void _mail_strip_crlf(char *dst, const char *src, size_t maxlen) {
    size_t j = 0;
    for (size_t i = 0; '\0' != src[i] && j < maxlen - 1; i++) {
        if ('\r' != src[i] && '\n' != src[i]) {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}
void mail_subject(mail_ctx *mail, const char *subject) {
    FREE(mail->subject);
    size_t lens = strlen(subject);
    MALLOC(mail->subject, lens + 1);
    _mail_strip_crlf(mail->subject, subject, lens + 1);
}
void mail_msg(mail_ctx *mail, const char *msg) {
    FREE(mail->msg);
    size_t lens = strlen(msg);
    // 规范化 bare CR / bare LF 为 CRLF：MIME text/plain 的行终止符就该是 CRLF(RFC 5321)。
    // 容量上限 2*lens+1：每字节最坏情况(独立 CR 或独立 LF)扩展为 2 字节
    MALLOC(mail->msg, 2 * lens + 1);
    size_t j = 0;
    char c;
    for (size_t i = 0; i < lens; i++) {
        c = msg[i];
        if ('\r' == c) {
            mail->msg[j++] = '\r';
            mail->msg[j++] = '\n';
            // 跳过紧跟的 '\n'（已构成合法 CRLF 对）
            if (i + 1 < lens && '\n' == msg[i + 1]) {
                i++;
            }
        } else if ('\n' == c) {
            // bare LF：补为 CRLF
            mail->msg[j++] = '\r';
            mail->msg[j++] = '\n';
        } else {
            mail->msg[j++] = c;
        }
    }
    mail->msg[j] = '\0';
}
void mail_html(mail_ctx *mail, const char *html, size_t lens) {
    FREE(mail->html);
    size_t b64lens = B64EN_SIZE(lens);
    MALLOC(mail->html, b64lens);
    bs64_encode(html, lens, mail->html);
}
// 填充 mail_addr 结构：设置显示名称和邮箱地址，name 为 NULL 或空时清空 name 字段
static inline void _mail_addr(mail_addr *addr, const char *name, const char *email) {
    if (EMPTYSTR(name)) {
        addr->name[0] = '\0';
    } else {
        _mail_strip_crlf(addr->name, name, sizeof(addr->name));
    }
    _mail_strip_crlf(addr->addr, email, sizeof(addr->addr));
}
void mail_from(mail_ctx *mail, const char *name, const char *email) {
    _mail_addr(&mail->from, name, email);
}
void mail_addrs_add(mail_ctx *mail, const char *email, mail_addr_type type) {
    mail_addr addr;
    addr.addr_type = type;
    _mail_addr(&addr, NULL, email);
    array_push_back(&mail->addrs, &addr);
}
void mail_addrs_clear(mail_ctx *mail) {
    array_clear(&mail->addrs);
}
void mail_attach_add(mail_ctx *mail, const char *file) {
    size_t flens;
    char *info = readall(file, &flens);
    if (NULL == info) {
        return;
    }
    mail_attach att;
    _mail_strip_crlf(att.file, __FILENAME__(file), sizeof(att.file));
    char *ex = strrchr(att.file, '.');
    // 扩展名装不下就当没有：截断会查出一个错误的 Content-Type，留空则退化为通用类型，
    // 附件本身照常发出去（att 是栈上未初始化结构，safe_fill_str 失败时不写，必须自己兜底）
    if (ERR_OK != safe_fill_str(att.extension, sizeof(att.extension), ex)) {
        LOG_WARN("mail attach extension exceeds %zu bytes, treated as none: %s.", sizeof(att.extension) - 1, att.file);
        att.extension[0] = '\0';
    }
    size_t b64lens = B64EN_SIZE(flens);
    MALLOC(att.content, b64lens);
    bs64_encode(info, flens, att.content);
    FREE(info);
    array_push_back(&mail->attach, &att);
}
void mail_attach_clear(mail_ctx *mail) {
    _mail_attach_free(&mail->attach);
    array_clear(&mail->attach);
}
void mail_clear(mail_ctx *mail) {
    if (NULL != mail->subject) {
        mail->subject[0] = '\0';
    }
    if (NULL != mail->msg) {
        mail->msg[0] = '\0';
    }
    if (NULL != mail->html) {
        mail->html[0] = '\0';
    }
    mail->from.name[0] = '\0';
    mail->from.addr[0] = '\0';
    mail->reply = 1;
    mail_addrs_clear(mail);
    mail_attach_clear(mail);
}
// 统计指定类型（TO/CC/BCC）的地址数量
static uint32_t _mail_addr_count(mail_ctx *mail, mail_addr_type type) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < array_size(&mail->addrs); i++) {
        if (type == ((mail_addr *)array_at(&mail->addrs, i))->addr_type) {
            count++;
        }
    }
    return count;
}
// 将指定类型的地址列表写入 MIME 头部（To:/Cc: 行；BCC 只走信封 RCPT TO，不入头部）
static void _mail_pack_addr(mail_ctx *mail, binary_ctx *bwriter, mail_addr_type type) {
    uint32_t count = _mail_addr_count(mail, type);
    if (0 == count) {
        return;
    }
    switch (type) {
    case TO:
        binary_set_binary(bwriter, "To: ", strlen("To: "));
        break;
    case CC:
        binary_set_binary(bwriter, "Cc: ", strlen("Cc: "));
        break;
    default:
        return;
    }
    uint32_t index = 0;
    mail_addr *addr;
    for (uint32_t i = 0; i < array_size(&mail->addrs); i++) {
        addr = array_at(&mail->addrs, i);
        if (type != addr->addr_type) {
            continue;
        }
        index++;
        if (count > 1 && index < count) {
            binary_set_va(bwriter, "%s,\r\n ", addr->addr);// 逗号后需要加空格
        } else {
            binary_set_va(bwriter, "%s\r\n", addr->addr);
        }
        if (index >= count) {
            break;
        }
    }
}
// 生成本封邮件专用的 MIME boundary。RFC 2046 §5.1.1 要求 boundary 不得出现在任何 body part 中，
// 写死的字面量做不到这一点——它就在源码里摆着，正文放一行 "--<boundary>" 就能提前终结 text 段，
// 后面再伪造出一个附件或 text/html 替代段，顶着发件人的地址与签名发出去。
// 各 part 现在全是 base64（正文 / html / 附件），行首只可能是 base64 字符、出不了 '-'，
// 所以撞不上；随机化是为了满足 RFC 的唯一性要求，也给将来真加了非 base64 的 part 留一层保险
static inline int32_t _mail_gen_boundary(char *out, size_t cap) {
    ASSERTAB(cap >= HEX_ENSIZE(MIME_BOUND_RAND) + 5, ERRSTR_INVPARAM);
    char rnd[MIME_BOUND_RAND];
    if (ERR_OK != csprng_rand(rnd, sizeof(rnd))) {
        return ERR_FAILED;
    }
    memcpy(out, "srey_", 5);
    tohex(rnd, sizeof(rnd), out + 5, 1);
    return ERR_OK;
}
// 是否含非 ASCII 字节，决定头字段走原样写还是 encoded-word
static int32_t _mail_has_nonascii(const char *s, size_t lens) {
    for (size_t i = 0; i < lens; i++) {
        if (0 != (0x80 & (unsigned char)s[i])) {
            return 1;
        }
    }
    return 0;
}
// 头字段文本：短的纯 ASCII 原样写，含非 ASCII 或过长时按 RFC 2047 编成 encoded-word。
// RFC 5322 §2.2 规定 header field body 只能是 US-ASCII，而本实现从不协商 SMTPUTF8
// （EHLO 响应只解析 AUTH 类型），裸 UTF-8 主题在严格服务端上会被改写甚至拒收。
// 超过一个 word 时按 RFC 5322 §2.2.3 折行（CRLF + 一个空格）；切段必须落在 UTF-8 字符边界上，
// 从中间劈开会让对端解出半个字
static void _mail_set_header_text(binary_ctx *bw, const char *s) {
    size_t lens = strlen(s);
    if (lens <= MIME_HDR_RAW_MAX
        && !_mail_has_nonascii(s, lens)) {
        binary_set_binary(bw, s, lens);
        return;
    }
    char b64[B64EN_SIZE(MIME_EW_RAW)];
    size_t off = 0;
    size_t n;
    while (off < lens) {
        n = (lens - off > MIME_EW_RAW) ? MIME_EW_RAW : (lens - off);
        // UTF-8 续接字节形如 10xxxxxx，落在这种字节上说明切在了字符中间，往回退
        while (n > 1
            && off + n < lens
            && 0x80 == (0xC0 & (unsigned char)s[off + n])) {
            n--;
        }
        bs64_encode(s + off, n, b64);
        binary_set_va(bw, "=?" MIME_CHARSET "?B?%s?=", b64);
        off += n;
        if (off < lens) {
            binary_set_binary(bw, "\r\n ", 3);
        }
    }
}
// 头字段参数值（Content-Type 的 name= 与 Content-Disposition 的 filename=）。纯 ASCII 且不含
// quoted-string 转义字符时裸写引号形式，否则按 RFC 2231 编成 key*=UTF-8''<百分号编码>。
// 不能改走 _mail_set_header_text：RFC 2047 §5 禁止 encoded-word 出现在 quoted-string 里。
// 编码集取 RFC 3986 unreserved，它是 RFC 2231 attribute-char 的子集，多编几个字符合法
static void _mail_set_header_param(binary_ctx *bw, const char *key, const char *val) {
    size_t lens = strlen(val);
    if (!_mail_has_nonascii(val, lens)
        && NULL == strpbrk(val, "\"\\")) {
        binary_set_va(bw, "%s=\"%s\"", key, val);
        return;
    }
    static const char hexchars[] = "0123456789ABCDEF";
    unsigned char c;
    char pct[3];
    binary_set_va(bw, "%s*=" MIME_CHARSET "''", key);
    for (size_t i = 0; i < lens; i++) {
        c = (unsigned char)val[i];
        if ((c >= 'A' && c <= 'Z')
            || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9')
            || '-' == c
            || '.' == c
            || '_' == c) {
            binary_set_binary(bw, val + i, 1);
            continue;
        }
        pct[0] = '%';
        pct[1] = hexchars[c >> 4];
        pct[2] = hexchars[c & 15];
        binary_set_binary(bw, pct, sizeof(pct));
    }
}
// 发件人显示名。RFC 5322 §3.4 的 display-name 是 phrase：只由 atom 组成时可以裸写，
// 一旦含 specials（RFC 5322 §3.2.3 的 ()<>[]:;@\,." 这几个）就必须整体加引号——
// 否则 "Doe, John <a@b>" 会被解析成 "Doe" 与 "John <a@b>" 两个地址。
// 含非 ASCII 时走 encoded-word，且不能再加引号：RFC 2047 §5 明确禁止 encoded-word
// 出现在 quoted-string 里。两条路互斥，编码过的必然不带引号，带引号的必然是纯 ASCII
static void _mail_set_display_name(binary_ctx *bw, const char *name) {
    size_t lens = strlen(name);
    if (_mail_has_nonascii(name, lens)) {
        _mail_set_header_text(bw, name);
        return;
    }
    size_t i;
    for (i = 0; i < lens; i++) {
        if (NULL != strchr("()<>[]:;@\\,.\"", name[i])) {
            break;
        }
    }
    if (i == lens) {
        binary_set_binary(bw, name, lens);
        return;
    }
    binary_set_binary(bw, "\"", 1);
    for (i = 0; i < lens; i++) {
        // quoted-string 里只有 '\' 和 '"' 需要转义
        if ('"' == name[i]
            || '\\' == name[i]) {
            binary_set_binary(bw, "\\", 1);
        }
        binary_set_binary(bw, name + i, 1);
    }
    binary_set_binary(bw, "\"", 1);
}
// 按 RFC 2045 §6.8 折行写出 base64 正文（每 MIME_B64_LINE 字符插 CRLF，末行不补）。
// bs64_encode 不插换行，直接整段写出会让 DATA 单行远超 RFC 5321 §4.5.3.1.6 的 1000 octet 上限
static void _mail_set_b64(binary_ctx *bw, const char *b64) {
    size_t lens = strlen(b64);
    size_t off = 0;
    size_t n;
    while (off < lens) {
        n = (lens - off > MIME_B64_LINE) ? MIME_B64_LINE : (lens - off);
        binary_set_binary(bw, b64 + off, n);
        off += n;
        if (off < lens) {
            binary_set_binary(bw, FLAG_CRLF, CRLF_SIZE);
        }
    }
}
// 纯文本正文按 base64 写出，一次一行边编边写：57 字节原文正好编成 76 个 base64 字符（RFC 2045
// §6.8 的行宽），故按 57 切块天然落在折行位置，栈上一个小缓冲够用。
// 不能 8bit 原样写有两条硬理由：RFC 5321 §4.5.3.1.6 限单行含 CRLF 不超 1000 octet，长正文会被
// 拒收或强行折行；纯文本单段分支不写 Content-Type，缺省即 us-ascii，UTF-8 正文必乱码。
// 顺带 dot-stuffing（RFC 5321 §4.5.2）也不需要了：base64 行首出不了 '.'
static void _mail_set_text_b64(binary_ctx *bw, const char *msg) {
    size_t lens = strlen(msg);
    char line[B64EN_SIZE(MIME_B64_RAW)];
    size_t off = 0;
    size_t n;
    while (off < lens) {
        n = (lens - off > MIME_B64_RAW) ? MIME_B64_RAW : (lens - off);
        binary_set_binary(bw, line, bs64_encode(msg + off, n, line));
        off += n;
        if (off < lens) {
            binary_set_binary(bw, FLAG_CRLF, CRLF_SIZE);
        }
    }
}
// 把"段头 + 可选正文"写出来。text/plain 段在三条路径上出现（多段无 html、多段有 html 的
// alternative 内层、单段），三份逐字相同
static inline void _mail_set_text_part(binary_ctx *bw, const char *msg) {
    binary_set_va(bw, "%s", "Content-Type: text/plain; charset=" MIME_CHARSET
        "\r\nContent-Transfer-Encoding: base64\r\n\r\n");
    if (!EMPTYSTR(msg)) {
        _mail_set_text_b64(bw, msg);
    }
}
char *mail_pack(mail_ctx *mail) {
    uint32_t nattach = array_size(&mail->attach);
    int32_t multipart = (!EMPTYSTR(mail->html) || nattach > 0) ? 1 : 0;
    // 用不到的分支不生成：innerboundary 只有 multipart/alternative(即有 html)才用得上，
    // 而 csprng_rand 在 Linux 上是裸 getrandom，熵池未就绪时会阻塞。
    // 仍显式清零：两处使用都在 if 里，编译器未必能关联到那一点
    char boundary[MIME_BOUND_LENS] = { 0 };
    char innerboundary[MIME_BOUND_LENS] = { 0 };
    if (multipart) {
        if (ERR_OK != _mail_gen_boundary(boundary, sizeof(boundary))
            || (!EMPTYSTR(mail->html)
                && ERR_OK != _mail_gen_boundary(innerboundary, sizeof(innerboundary)))) {
            LOG_ERROR("%s", "mail_pack: cannot get entropy for MIME boundary.");
            return NULL;
        }
    }
    binary_ctx bwriter;
    binary_init_write(&bwriter, ONEK, ONEK);
    // RFC 5322 §3.4 的 name-addr 形式：display-name <addr-spec>。
    // 原来写的是 "addr (name)"——把名字塞进注释里，虽然合法但没有哪个 MUA 会拿它当发件人名显示
    binary_set_binary(&bwriter, "From: ", 6);
    if (0 != strlen(mail->from.name)) {
        _mail_set_display_name(&bwriter, mail->from.name);
        binary_set_va(&bwriter, " <%s>\r\n", mail->from.addr);
    } else {
        binary_set_va(&bwriter, "%s\r\n", mail->from.addr);
    }
    if (mail->reply) {
        binary_set_va(&bwriter, "Reply-To: %s\r\n", mail->from.addr);
    } else {
        binary_set_va(&bwriter, "No-Reply: %s\r\n", mail->from.addr);
    }
    _mail_pack_addr(mail, &bwriter, TO);
    _mail_pack_addr(mail, &bwriter, CC);
    // BCC 收件人已由 _smtp_send 的 RCPT TO 逐个投递(信封层)；密送要求不出现在正文头部，故此处不写 Bcc 头，否则名单对全体收件人可见
    // MIME 头无论单段多段都要写：只有多段时才写的话，最常见的"纯文本一封信"整封没有
    // Content-Type，按 RFC 2045 缺省成 us-ascii，UTF-8 正文到严格客户端上就是乱码
    if (multipart) {
        binary_set_va(&bwriter, "MIME-Version: 1.0\r\nContent-Type: multipart/mixed;\r\n\tboundary=\"%s\"\r\n", boundary);
    } else {
        binary_set_va(&bwriter, "%s", "MIME-Version: 1.0\r\nContent-Type: text/plain; charset=" MIME_CHARSET
            "\r\nContent-Transfer-Encoding: base64\r\n");
    }
    char date[TIME_LENS] = { 0 };
    // sectostr 失败时跳过 Date header（RFC 5322 §3.6.1 推荐但不强制），不写空行避免协议歧义
    if (ERR_OK == sectostr(nowsec(), "Date: %d %b %y %H:%M:%S %z", date)) {
        binary_set_va(&bwriter, "%s\r\n", date);
    }
    binary_set_binary(&bwriter, "Subject: ", 9);
    if (!EMPTYSTR(mail->subject)) {
        _mail_set_header_text(&bwriter, mail->subject);
    }
    // 空行终止头部
    binary_set_binary(&bwriter, CONCAT2(FLAG_CRLF, FLAG_CRLF), CRLF_SIZE * 2);
    if (multipart) {
        binary_set_va(&bwriter, "This is a MIME encapsulated message\r\n\r\n--%s\r\n", boundary);
        if (EMPTYSTR(mail->html)) {
            _mail_set_text_part(&bwriter, mail->msg);
            binary_set_va(&bwriter, "\r\n\r\n--%s\r\n", boundary);
        } else {
            // 含 html 内容，使用 multipart/alternative
            binary_set_va(&bwriter, "Content-Type: multipart/alternative;\r\n\tboundary=\"%s\"\r\n", innerboundary);
            // 写内层 boundary 起始标记
            binary_set_va(&bwriter, "\r\n\r\n--%s\r\n", innerboundary);
            _mail_set_text_part(&bwriter, mail->msg);
            binary_set_va(&bwriter, "\r\n\r\n--%s\r\n", innerboundary);
            // 写入 html 内容
            binary_set_va(&bwriter, "%s", "Content-Type: text/html; charset=" MIME_CHARSET "\r\nContent-Transfer-Encoding: base64\r\n\r\n");
            _mail_set_b64(&bwriter, mail->html);
            binary_set_va(&bwriter, "\r\n\r\n--%s--\r\n", innerboundary);
            // 无附件时直接结束边界
            if (0 == nattach) {
                binary_set_va(&bwriter, "\r\n--%s--\r\n", boundary);
            } else {
                binary_set_va(&bwriter, "\r\n--%s\r\n", boundary);
            }
        }
        mail_attach *att;
        for (uint32_t i = 0; i < nattach; i++) {
            att = array_at(&mail->attach, i);
            binary_set_va(&bwriter, "Content-Type: %s;\r\n", contenttype(att->extension));
            binary_set_binary(&bwriter, "\t", 1);
            _mail_set_header_param(&bwriter, "name", att->file);
            binary_set_binary(&bwriter, FLAG_CRLF, CRLF_SIZE);
            binary_set_va(&bwriter, "%s", "Content-Transfer-Encoding: base64\r\n");
            binary_set_va(&bwriter, "%s", "Content-Disposition: attachment; ");
            _mail_set_header_param(&bwriter, "filename", att->file);
            binary_set_binary(&bwriter, CONCAT2(FLAG_CRLF, FLAG_CRLF), CRLF_SIZE * 2);
            _mail_set_b64(&bwriter, att->content);
            if (i + 1 == nattach) {
                binary_set_va(&bwriter, "\r\n\r\n--%s--\r\n", boundary);
            } else {
                binary_set_va(&bwriter, "\r\n\r\n--%s\r\n", boundary);
            }
        }
    } else if (!EMPTYSTR(mail->msg)) {
        // 单段：段头已在上面的 MIME 头里写过，这里只写正文
        _mail_set_text_b64(&bwriter, mail->msg);
    }
    binary_set_binary(&bwriter, "\r\n.\r\n", 5);
    // 调用方 coro_utils.c / lprot.c 以 strlen() 计算长度；显式追加 NUL 终结避免读未初始化内存 UB
    binary_set_uint8(&bwriter, 0);
    return bwriter.data;
}
