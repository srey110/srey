#include "protocol/smtp/mail.h"
#include "crypt/base64.h"
#include "utils/utils.h"
#include "utils/binary.h"

#define MIME_CHARSET "utf-8"

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
    // 规范化 bare CR / bare LF 为 CRLF：RFC 5321 要求行结尾严格 CRLF，但部分容错 SMTP server
    // (CVE-2023-51764 / CVE-2023-51765 同模式) 把 bare LF 视为行终止 —— 攻击者在用户可控 body 内
    // 注入 \n.\r\n... 即可伪造 DATA 终止符走私后续 SMTP 命令。在入口统一规范化后，下游 _mail_dot_stuff
    // 只需处理 CRLF 即可，杜绝出站邮件含 bare CR/LF 触发下游 smuggling
    // 容量上限 2*lens+1：每字节最坏情况 (独立 CR 或独立 LF) 扩展为 2 字节
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
static void _mail_addr(mail_addr *addr, const char *name, const char *email) {
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
    safe_fill_str(att.file, sizeof(att.file), __FILENAME__(file));
    char *ex = strrchr(att.file, '.');
    safe_fill_str(att.extension, sizeof(att.extension), ex);
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
// 将指定类型的地址列表写入 MIME 头部（To:/Cc:/Bcc: 行）
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
    case BCC:
        binary_set_binary(bwriter, "Bcc: ", strlen("Bcc: "));
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
            binary_set_va(bwriter, "%s,\r\n ", addr->addr);//must add a space after the comma
        } else {
            binary_set_va(bwriter, "%s\r\n", addr->addr);
        }
        if (index >= count) {
            break;
        }
    }
}
// RFC 5321 §4.5.2 Transparency：行首 '.' 在 DATA body 中需复制为 ".."，
// 否则 <CRLF>.<CRLF> 会被服务端误判为 DATA 终止符 → SMTP Smuggling
// (CVE-2023-51764 / CVE-2023-51765 同模式)
static void _mail_dot_stuff(binary_ctx *bw, const char *body, size_t lens) {
    if (0 == lens) {
        return;
    }
    const char *start = body;
    const char *end = body + lens;
    // 输入首字节即第一行行首
    if ('.' == *start) {
        binary_set_binary(bw, ".", 1);
    }
    while (start < end) {
        const char *crlf = memstr(0, (char *)start, (size_t)(end - start), "\r\n", 2);
        if (NULL == crlf) {
            binary_set_binary(bw, start, (size_t)(end - start));
            return;
        }
        // 写到 \r\n 含
        binary_set_binary(bw, start, (size_t)(crlf + 2 - start));
        start = crlf + 2;
        // \r\n 之后若紧跟 '.'，插入额外 '.' 实现 dot-stuffing
        if (start < end && '.' == *start) {
            binary_set_binary(bw, ".", 1);
        }
    }
}
char *mail_pack(mail_ctx *mail) {
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, ONEK, ONEK);
    if (0 != strlen(mail->from.name)) {
        binary_set_va(&bwriter, "From: %s (%s) \r\n", mail->from.addr, mail->from.name);
    } else {
        binary_set_va(&bwriter, "From: %s\r\n", mail->from.addr);
    }
    if (mail->reply) {
        binary_set_va(&bwriter, "Reply-To: %s\r\n", mail->from.addr);
    } else {
        binary_set_va(&bwriter, "No-Reply: %s\r\n", mail->from.addr);
    }
    _mail_pack_addr(mail, &bwriter, TO);
    _mail_pack_addr(mail, &bwriter, CC);
    _mail_pack_addr(mail, &bwriter, BCC);
    const char *boundary = "bounds=_NextP_0056wi_0_8_ty789432_tp";
    uint32_t nattach = array_size(&mail->attach);
    if (!EMPTYSTR(mail->html)
        || nattach > 0) {
        binary_set_va(&bwriter, "MIME-Version: 1.0\r\nContent-Type: multipart/mixed;\r\n\tboundary=\"%s\"\r\n", boundary);
    }
    char date[TIME_LENS] = { 0 };
    // sectostr 失败时跳过 Date header（RFC 5322 §3.6.1 推荐但不强制），不写空行避免协议歧义
    if (ERR_OK == sectostr(nowsec(), "Date: %d %b %y %H:%M:%S %z", date)) {
        binary_set_va(&bwriter, "%s\r\n", date);
    }
    binary_set_va(&bwriter, "Subject: %s\r\n\r\n", EMPTYSTR(mail->subject) ? "" : mail->subject);
    if (!EMPTYSTR(mail->html)
        || nattach > 0) {
        binary_set_va(&bwriter, "This is a MIME encapsulated message\r\n\r\n--%s\r\n", boundary);
        if (EMPTYSTR(mail->html)) {
            //plain text message first.
            binary_set_va(&bwriter, "%s", "Content-type: text/plain; charset=" MIME_CHARSET "\r\nContent-transfer-encoding: 8bit\r\n\r\n");
            if (!EMPTYSTR(mail->msg)) {
                _mail_dot_stuff(&bwriter, mail->msg, strlen(mail->msg));
            }
            binary_set_va(&bwriter, "\r\n\r\n--%s\r\n", boundary);
        } else {
            //make it multipart/alternative as we have html
            const char *innerboundary = "inner_jfd_0078hj_0_8_part_tp";
            binary_set_va(&bwriter, "Content-Type: multipart/alternative;\r\n\tboundary=\"%s\"\r\n", innerboundary);
            //need the inner boundary starter.
            binary_set_va(&bwriter, "\r\n\r\n--%s\r\n", innerboundary);
            //plain text message first.
            binary_set_va(&bwriter, "%s", "Content-type: text/plain; charset=" MIME_CHARSET "\r\nContent-transfer-encoding: 8bit\r\n\r\n");
            if (!EMPTYSTR(mail->msg)) {
                _mail_dot_stuff(&bwriter, mail->msg, strlen(mail->msg));
            }
            binary_set_va(&bwriter, "\r\n\r\n--%s\r\n", innerboundary);
            //Add html message here
            binary_set_va(&bwriter, "%s", "Content-type: text/html; charset=" MIME_CHARSET "\r\nContent-Transfer-Encoding: base64\r\n\r\n");
            binary_set_binary(&bwriter, mail->html, strlen(mail->html));
            binary_set_va(&bwriter, "\r\n\r\n--%s--\r\n", innerboundary);
            //end the boundaries if there are no attachments
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
            binary_set_va(&bwriter, "\tname=\"%s\"\r\n", att->file);
            binary_set_va(&bwriter, "%s", "Content-Transfer-Encoding: base64\r\n");
            binary_set_va(&bwriter, "Content-Disposition: attachment; filename=\"%s\"\r\n\r\n", att->file);
            binary_set_binary(&bwriter, att->content, strlen(att->content));
            if (i + 1 == nattach) {
                binary_set_va(&bwriter, "\r\n\r\n--%s--\r\n", boundary);
            } else {
                binary_set_va(&bwriter, "\r\n\r\n--%s\r\n", boundary);
            }
        }
    } else {
        if (!EMPTYSTR(mail->msg)) {
            _mail_dot_stuff(&bwriter, mail->msg, strlen(mail->msg));
        }
    }
    binary_set_binary(&bwriter, "\r\n.\r\n", 5);
    // 调用方 coro_utils.c / lprot.c 以 strlen() 计算长度；显式追加 NUL 终结避免读未初始化内存 UB
    binary_set_uint8(&bwriter, 0);
    return bwriter.data;
}
