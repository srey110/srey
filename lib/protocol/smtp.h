#ifndef SMTP_H_
#define SMTP_H_

#include "srey/spub.h"

typedef struct smtp_ctx {
    uint16_t port;
    int32_t authtype;
    int32_t status;
    struct evssl_ctx *evssl;
    task_ctx *task;
    SOCKET fd;
    uint64_t skid;
    char user[64];
    char psw[64];
    char ip[IP_LENS];
}smtp_ctx;

void _smtp_init(void *hspush);
void _smtp_udfree(ud_cxt *ud);
void _smtp_closed(ud_cxt *ud);
int32_t _smtp_on_connected(ud_cxt *ud, int32_t err);
/// <summary>
/// 简单邮件传输协议smtp初始化
/// </summary>
/// <param name="smtp">smtp_ctx</param>
/// <param name="ip">smtp服务器</param>
/// <param name="port">smtp端口</param>
/// <param name="evssl">evssl_ctx</param>
/// <param name="user">用户名</param>
/// <param name="psw">密码</param>
void smtp_init(smtp_ctx *smtp, const char *ip, uint16_t port, struct evssl_ctx *evssl, const char *user, const char *psw);
/// <summary>
/// 链接smtp服务器
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="smtp">smtp_ctx</param>
/// <returns>ERR_OK 成功</returns>
int32_t smtp_try_connect(task_ctx *task, smtp_ctx *smtp);
/// <summary>
/// 检查是否完成身份验证
/// </summary>
/// <param name="smtp">smtp_ctx</param>
/// <returns>ERR_OK 成功</returns>
int32_t smtp_check_auth(smtp_ctx *smtp);
/// <summary>
/// 检查返回码是否匹配
/// </summary>
/// <param name="pack">smtp服务器返回的数据包</param>
/// <param name="code">状态码</param>
/// <returns>ERR_OK 匹配</returns>
int32_t smtp_check_code(char *pack, const char *code);
/// <summary>
/// 检查返回码是否为250
/// </summary>
/// <param name="pack">smtp服务器返回的数据包</param>
/// <returns>ERR_OK 匹配</returns>
int32_t smtp_check_ok(char *pack);
/// <summary>
/// RSET命令数据包
/// </summary>
/// <param name="smtp">smtp_ctx</param>
/// <returns>数据包</returns>
char *smtp_pack_reset(smtp_ctx *smtp);
/// <summary>
/// QUIT命令数据包
/// </summary>
/// <param name="smtp">smtp_ctx</param>
/// <returns>数据包</returns>
char *smtp_pack_quit(smtp_ctx *smtp);
/// <summary>
/// NOOP命令数据包
/// </summary>
/// <param name="smtp">smtp_ctx</param>
/// <returns>数据包</returns>
char *smtp_pack_ping(smtp_ctx *smtp);
/// <summary>
/// MAIL FROM 命令数据包
/// </summary>
/// <param name="smtp">smtp_ctx</param>
/// <param name="from">发件人地址</param>
/// <returns>数据包</returns>
char *smtp_pack_from(smtp_ctx *smtp, const char *from);
/// <summary>
/// RCPT TO 命令数据包
/// </summary>
/// <param name="smtp">smtp_ctx</param>
/// <param name="from">收件人地址</param>
/// <returns>数据包</returns>
char *smtp_pack_rcpt(smtp_ctx *smtp, const char *rcpt);
/// <summary>
/// DATA 命令数据包
/// </summary>
/// <param name="smtp">smtp_ctx</param>
/// <returns>数据包</returns>
char *smtp_pack_data(smtp_ctx *smtp);
/// <summary>
/// DATA 命令数据包 (邮件内容)
/// </summary>
/// <param name="smtp">smtp_ctx</param>
/// <param name="subject">主题</param>
/// <param name="data">内容</param>
/// <returns>数据包</returns>
char *smtp_pack_mail(smtp_ctx *smtp, const char *subject, const char *data);
void *smtp_unpack(ev_ctx *ev, SOCKET fd, uint64_t skid, buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status);

#endif//SMTP_H_
