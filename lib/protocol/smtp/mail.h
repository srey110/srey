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

/// <summary>
/// 邮件内容初始化
/// </summary>
/// <param name="mail">mail_ctx</param>
void mail_init(mail_ctx *mail);
/// <summary>
/// 邮件内容释放
/// </summary>
/// <param name="mail">mail_ctx</param>
void mail_free(mail_ctx *mail);
/// <summary>
/// 邮件主题
/// </summary>
/// <param name="mail">mail_ctx</param>
/// <param name="subject">主题</param>
void mail_subject(mail_ctx *mail, const char *subject);
/// <summary>
/// 邮件内容
/// </summary>
/// <param name="mail">mail_ctx</param>
/// <param name="msg">内容</param>
void mail_msg(mail_ctx *mail, const char *msg);
/// <summary>
/// 邮件内容html
/// </summary>
/// <param name="mail">mail_ctx</param>
/// <param name="html">html</param>
/// <param name="lens">html长度</param>
void mail_html(mail_ctx *mail, const char *html, size_t lens);
/// <summary>
/// 发件人邮箱
/// </summary>
/// <param name="mail">mail_ctx</param>
/// <param name="name">发件人</param>
/// <param name="email">邮箱</param>
void mail_from(mail_ctx *mail, const char *name, const char *email);
/// <summary>
/// 收件、抄送、秘送 邮箱设置
/// </summary>
/// <param name="mail">mail_ctx</param>
/// <param name="email">邮箱</param>
/// <param name="type">mail_addr_type</param>
void mail_addrs_add(mail_ctx *mail, const char *email, mail_addr_type type);
/// <summary>
/// 收件、抄送、秘送 清空
/// </summary>
/// <param name="mail">mail_ctx</param>
void mail_addrs_clear(mail_ctx *mail);
/// <summary>
/// 附件
/// </summary>
/// <param name="mail">mail_ctx</param>
/// <param name="file">文件</param>
void mail_attach_add(mail_ctx *mail, const char *file);
/// <summary>
/// 清空
/// </summary>
/// <param name="mail">mail_ctx</param>
void mail_attach_clear(mail_ctx *mail);
/// <summary>
/// 邮件清空
/// </summary>
/// <param name="mail">mail_ctx</param>
void mail_clear(mail_ctx *mail);
/// <summary>
/// 邮件内容数据包
/// </summary>
/// <param name="mail">mail_ctx</param>
/// <returns>数据包</returns>
char *mail_pack(mail_ctx *mail);

#endif//MAIL_H_
