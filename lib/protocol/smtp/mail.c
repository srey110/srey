#include "protocol/smtp/mail.h"
#include "crypt/base64.h"
#include "utils/utils.h"
#include "utils/binary.h"

void mail_init(mail_ctx *mail) {
    ZERO(mail, sizeof(mail_ctx));
    arr_mail_addr_init(&mail->addrs, 0);
    arr_mail_attach_init(&mail->attach, 0);
}
static void _mail_attach_free(arr_mail_attach_ctx *attach) {
    for (uint32_t i = 0; i < arr_mail_attach_size(attach); i++) {
        FREE(arr_mail_attach_at(attach, i)->content);
    }
}
void mail_free(mail_ctx *mail) {
    FREE(mail->subject);
    FREE(mail->msg);
    FREE(mail->html)
    arr_mail_addr_free(&mail->addrs);
    _mail_attach_free(&mail->attach);
    arr_mail_attach_free(&mail->attach);
}
void mail_subject(mail_ctx *mail, const char *subject) {
    FREE(mail->subject);
    MALLOC(mail->subject, strlen(subject) + 1);
    strcpy(mail->subject, subject);
}
void mail_msg(mail_ctx *mail, const char *msg) {
    FREE(mail->msg);
    MALLOC(mail->msg, strlen(msg) + 1);
    strcpy(mail->msg, msg);
}
void mail_html(mail_ctx *mail, const char *html, size_t lens) {
    FREE(mail->html);
    size_t b64lens = B64EN_SIZE(lens);
    MALLOC(mail->html, b64lens);
    bs64_encode(html, lens, mail->html);
}
static void _mail_addr(mail_addr *addr, const char *name, const char *email) {
    if (EMPTYSTR(name)) {
        addr->name[0] = '\0';
    } else {
        strcpy(addr->name, name);
    }
    strcpy(addr->addr, email);
}
void mail_from(mail_ctx *mail, const char *name, const char *email) {
    _mail_addr(&mail->from, name, email);
}
void mail_addrs_add(mail_ctx *mail, const char *email, mail_addr_type type) {
    mail_addr addr;
    addr.addr_type = type;
    _mail_addr(&addr, NULL, email);
    arr_mail_addr_push_back(&mail->addrs, &addr);
}
void mail_addrs_clear(mail_ctx *mail) {
    arr_mail_addr_clear(&mail->addrs);
}
void mail_attach_add(mail_ctx *content, const char *file) {
    size_t flens;
    char *info = readall(file, &flens);
    if (NULL == info) {
        return;
    }
    mail_attach att;
    strcpy(att.file, __FILENAME__(file));
    char *ex = strrchr(att.file, '.');
    if (NULL == ex) {
        att.extension[0] = '\0';
    } else {
        strcpy(att.extension, ex);
    }
    size_t b64lens = B64EN_SIZE(flens);
    MALLOC(att.content, b64lens);
    bs64_encode(info, flens, att.content);
    FREE(info);
    arr_mail_attach_push_back(&content->attach, &att);
}
void mail_attach_clear(mail_ctx *mail) {
    _mail_attach_free(&mail->attach);
    arr_mail_attach_clear(&mail->attach);
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
static uint32_t _smtp_addr_count(mail_ctx *mail, mail_addr_type type) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < arr_mail_addr_size(&mail->addrs); i++) {
        if (type == arr_mail_addr_at(&mail->addrs, i)->addr_type) {
            count++;
        }
    }
    return count;
}
static void _smtp_pack_addr(mail_ctx *mail, binary_ctx *bwriter, mail_addr_type type) {
    uint32_t count = _smtp_addr_count(mail, type);
    if (0 == count) {
        return;
    }
    switch (type) {
    case TO:
        binary_set_string(bwriter, "To: ", strlen("To: "));
        break;
    case CC:
        binary_set_string(bwriter, "Cc: ", strlen("Cc: "));
        break;
    case BCC:
        binary_set_string(bwriter, "Bcc: ", strlen("Bcc: "));
        break;
    default:
        return;
    }
    uint32_t index = 0;
    mail_addr *addr;
    for (uint32_t i = 0; i < arr_mail_addr_size(&mail->addrs); i++) {
        addr = arr_mail_addr_at(&mail->addrs, i);
        if (type != addr->addr_type) {
            continue;
        }
        index++;
        if (count > 1 
            && index < count) {
            binary_set_va(bwriter, "%s,\r\n ", addr->addr);//must add a space after the comma
        } else {
            binary_set_va(bwriter, "%s\r\n", addr->addr);
        }
        if (index >= count) {
            break;
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
    binary_set_va(&bwriter, "Reply-To: %s\r\n", mail->from.addr);
    _smtp_pack_addr(mail, &bwriter, TO);
    _smtp_pack_addr(mail, &bwriter, CC);
    _smtp_pack_addr(mail, &bwriter, BCC);
    const char *boundary = "bounds=_NextP_0056wi_0_8_ty789432_tp";
    uint32_t nattach = arr_mail_attach_size(&mail->attach);
    if (!EMPTYSTR(mail->html)
        || nattach > 0) {
        binary_set_va(&bwriter, "MIME-Version: 1.0\r\nContent-Type: multipart/mixed;\r\n\tboundary=\"%s\"\r\n", boundary);
    }
    char date[TIME_LENS] = { 0 };
    sectostr(nowsec(), "Date: %d %b %y %H:%M:%S %z", date);
    binary_set_va(&bwriter, "%s\r\n", date);
    binary_set_va(&bwriter, "Subject: %s\r\n\r\n", mail->subject);
    if (!EMPTYSTR(mail->html)
        || nattach > 0) {
        binary_set_va(&bwriter, "This is a MIME encapsulated message\r\n\r\n--%s\r\n", boundary);
        if (EMPTYSTR(mail->html)) {
            //plain text message first.
            binary_set_va(&bwriter, "%s", "Content-type: text/plain; charset=iso-8859-1\r\nContent-transfer-encoding: 7BIT\r\n\r\n");
            if (!EMPTYSTR(mail->msg)) {
                binary_set_string(&bwriter, mail->msg, strlen(mail->msg));
            }
            binary_set_va(&bwriter, "\r\n\r\n--%s\r\n", boundary);
        } else {
            //make it multipart/alternative as we have html
            const char *innerboundary = "inner_jfd_0078hj_0_8_part_tp";
            binary_set_va(&bwriter, "Content-Type: multipart/alternative;\r\n\tboundary=\"%s\"\r\n", innerboundary);
            //need the inner boundary starter.
            binary_set_va(&bwriter, "\r\n\r\n--%s\r\n", innerboundary);
            //plain text message first.
            binary_set_va(&bwriter, "%s", "Content-type: text/plain; charset=iso-8859-1\r\nContent-transfer-encoding: 7BIT\r\n\r\n");
            if (!EMPTYSTR(mail->msg)) {
                binary_set_string(&bwriter, mail->msg, strlen(mail->msg));
            }
            binary_set_va(&bwriter, "\r\n\r\n--%s\r\n", innerboundary);
            //Add html message here
            binary_set_va(&bwriter, "%s", "Content-type: text/html; charset=iso-8859-1\r\nContent-Transfer-Encoding: base64\r\n\r\n");
            binary_set_string(&bwriter, mail->html, strlen(mail->html));
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
            att = arr_mail_attach_at(&mail->attach, i);
            binary_set_va(&bwriter, "Content-Type: %s;\r\n", contenttype(att->extension));
            binary_set_va(&bwriter, "\tname=\"%s\"\r\n", att->file);
            binary_set_va(&bwriter, "%s", "Content-Transfer-Encoding: base64\r\n");
            binary_set_va(&bwriter, "Content-Disposition: attachment; filename=\"%s\"\r\n\r\n", att->file);
            binary_set_string(&bwriter, att->content, strlen(att->content));
            if (i + 1  == nattach) {
                binary_set_va(&bwriter, "\r\n\r\n--%s--\r\n", boundary);
            } else {
                binary_set_va(&bwriter, "\r\n\r\n--%s\r\n", boundary);
            }
        }
    } else {
        binary_set_string(&bwriter, mail->msg, strlen(mail->msg));
    }
    binary_set_string(&bwriter, "\r\n.\r\n", 0);
    return bwriter.data;
}
