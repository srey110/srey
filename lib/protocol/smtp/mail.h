#ifndef MAIL_H_
#define MAIL_H_

#include "containers/sarray.h"

typedef enum mail_addr_type {
    TO = 0x01,
    CC,
    BCC
}mail_addr_type;
typedef struct mail_addr {
    mail_addr_type addr_type;
    char name[64];
    char addr[256];
}mail_addr;
ARRAY_DECL(mail_addr, arr_mail_addr);
typedef struct mail_attach {
    char *content;
    char extension[32];
    char file[PATH_LENS];
}mail_attach;
ARRAY_DECL(mail_attach, arr_mail_attach);
typedef struct mail_ctx {
    char *subject;
    char *msg;
    char *html;
    mail_addr from;
    arr_mail_addr_ctx addrs;
    arr_mail_attach_ctx attach;
}mail_ctx;

void mail_init(mail_ctx *mail);
void mail_free(mail_ctx *mail);
void mail_subject(mail_ctx *mail, const char *subject);
void mail_msg(mail_ctx *mail, const char *msg);
void mail_html(mail_ctx *mail, const char *html, size_t lens);
void mail_from(mail_ctx *mail, const char *name, const char *email);
void mail_addrs_add(mail_ctx *mail, const char *email, mail_addr_type type);
void mail_addrs_clear(mail_ctx *mail);
void mail_attach_add(mail_ctx *content, const char *file);
void mail_attach_clear(mail_ctx *mail);
void mail_clear(mail_ctx *mail);
/// <summary>
/// 邮件内容数据包
/// </summary>
/// <param name="mail">mail_ctx</param>
/// <returns>数据包</returns>
char *mail_pack(mail_ctx *mail);

#endif//MAIL_H_
