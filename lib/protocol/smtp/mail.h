#ifndef MAIL_H_
#define MAIL_H_

#include "containers/sarray.h"

typedef enum mail_addr_type {
    TO = 0x01, //收件人
    CC,        //抄送
    BCC        //密送
}mail_addr_type;
typedef struct mail_addr {
    mail_addr_type addr_type; //地址类型（收件/抄送/密送）
    char name[64];            //显示名称
    char addr[256];           //邮箱地址
}mail_addr;
typedef struct mail_attach {
    char *content;          //附件内容（Base64 编码后的数据）
    char extension[32];     //文件扩展名（含点号，如 ".pdf"）
    char file[PATH_LENS];   //文件名（不含目录）
}mail_attach;
typedef struct mail_ctx {
    int32_t reply;            //是否要求回复：1 回复（默认），0 不回复
    char *subject;            //邮件主题
    char *msg;                //纯文本正文
    char *html;               //HTML 正文（Base64 编码）
    mail_addr from;           //发件人
    array_ctx addrs;          //收件/抄送/密送地址列表（元素 mail_addr）
    array_ctx attach;         //附件列表（元素 mail_attach）
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
/// 是否回复
/// </summary>
/// <param name="mail">mail_ctx</param>
/// <param name="reply">1回复(默认) 0不回复</param>
void mail_reply(mail_ctx *mail, int32_t reply);
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
/// 发件人邮箱。组包时按 RFC 5322 §3.4 的 name-addr 形式写出：display-name &lt;addr-spec&gt;；
/// name 为空则只写地址。name 含 specials 时自动加引号、含非 ASCII 时自动编成 RFC 2047
/// encoded-word，调用方按原文传即可
/// </summary>
/// <param name="mail">mail_ctx</param>
/// <param name="name">发件人显示名，可为 NULL</param>
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
/// 邮件清空：内容字段与 reply 标志一并还原到 mail_init 后的状态，供同一对象复用
/// </summary>
/// <param name="mail">mail_ctx</param>
void mail_clear(mail_ctx *mail);
/// <summary>
/// 邮件内容数据包（含 DATA 终止符，NUL 结尾，调用方以 strlen 取长度并负责释放）。
/// 正文一律按 base64 编码写出：RFC 5321 §4.5.3.1.6 限单行 1000 octet，8bit 原样写出时
/// 一段没有换行的长正文会被服务端拒收；base64 折行后顺带免掉 dot-stuffing。
/// 多段邮件（有 html 或附件）的 MIME boundary 每封随机生成，取不到熵时返回 NULL
/// </summary>
/// <param name="mail">mail_ctx</param>
/// <returns>数据包；生成 boundary 失败返回 NULL</returns>
char *mail_pack(mail_ctx *mail);

#endif//MAIL_H_
