#ifndef TASK_SMTP_H_
#define TASK_SMTP_H_

#include "lib.h"

// 启动 SMTP 邮件发送测试任务，依次发送两封邮件：
//   第一封：纯文本正文，TO 收件人，无附件
//   第二封：纯文本 + HTML 正文，TO + CC 收件人，带附件，含 Reply-To 头
// 两封均发送成功后将 *ok 置 1；host 为域名时自动 DNS 解析。
// mail_addr1/mail_addr2 至少一个非 NULL；各字符串字段长度须小于 64
void task_smtp_start(loader_ctx *loader, const char *name, const char *sslname,
                     const char *smtp_sv, uint16_t smtp_port,
                     const char *smtp_user, const char *smtp_psw,
                     const char *mail_from, const char *mail_addr1, const char *mail_addr2,
                     const char *mail_att, int32_t pt, int32_t *ok);

#endif//TASK_SMTP_H_
