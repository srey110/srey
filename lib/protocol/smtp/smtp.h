#ifndef SMTP_H_
#define SMTP_H_

#include "srey/spub.h"
#include "protocol/smtp/mail.h"

typedef struct smtp_ctx {
    uint16_t port;           //SMTP 服务器端口
    int32_t authtype;        //认证类型（LOGIN 或 PLAIN），握手后自动设置
    struct evssl_ctx *evssl; //TLS 上下文，NULL 表示不加密
    task_ctx *task;          //所属任务上下文
    SOCKET fd;               //套接字文件描述符
    uint64_t skid;           //套接字唯一 ID
    char user[64];           //SMTP 用户名
    char psw[64];            //SMTP 密码
    char ip[IP_LENS];        //SMTP 服务器 IP 地址
}smtp_ctx;

// 初始化模块：注册握手完成回调并获取本机主机名
void _smtp_init(void *hspush);
// 连接断开时释放 ud_cxt 中的 smtp_ctx 引用并重置 fd
void _smtp_udfree(ud_cxt *ud);
// 连接关闭时的清理回调，等同于 _smtp_udfree
void _smtp_closed(ud_cxt *ud);
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
/// <returns>数据包</returns>
char *smtp_pack_reset(void);
/// <summary>
/// QUIT命令数据包
/// </summary>
/// <returns>数据包</returns>
char *smtp_pack_quit(void);
/// <summary>
/// NOOP命令数据包
/// </summary>
/// <returns>数据包</returns>
char *smtp_pack_ping(void);
/// <summary>
/// MAIL FROM 命令数据包
/// </summary>
/// <param name="from">发件人地址 test@163.com</param>
/// <returns>数据包</returns>
char *smtp_pack_from(const char *from);
/// <summary>
/// RCPT TO 命令数据包
/// </summary>
/// <param name="from">收件人地址 test@163.com</param>
/// <returns>数据包</returns>
char *smtp_pack_rcpt(const char *rcpt);
/// <summary>
/// DATA 命令数据包
/// </summary>
/// <returns>数据包</returns>
char *smtp_pack_data(void);
/// <summary>
/// SMTP 协议解包入口：根据当前握手状态（INIT/EHLO/AUTH/AUTH_CHECK/COMMAND）分发处理，
/// COMMAND 状态下返回响应数据包，其余状态内部驱动握手流程
/// </summary>
/// <param name="ev">事件上下文</param>
/// <param name="fd">套接字文件描述符</param>
/// <param name="skid">套接字唯一 ID</param>
/// <param name="buf">接收缓冲区</param>
/// <param name="ud">连接上下文（含 smtp_ctx 和解析状态）</param>
/// <param name="size">COMMAND 状态下输出数据包长度</param>
/// <param name="status">解析结果标志位（PROT_MOREDATA / PROT_ERROR）</param>
/// <returns>COMMAND 状态下返回响应数据包（需调用者释放），其余状态返回 NULL</returns>
void *smtp_unpack(ev_ctx *ev, SOCKET fd, uint64_t skid, buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status);

#endif//SMTP_H_
