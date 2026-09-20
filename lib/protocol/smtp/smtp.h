#ifndef SMTP_H_
#define SMTP_H_

#include "event/evpub.h"
#include "protocol/smtp/mail.h"

struct coro_serial_ctx;

typedef enum smtp_authtype {
    LOGIN = 1, //AUTH LOGIN 认证方式（逐字段 Base64 编码）
    PLAIN      //AUTH PLAIN 认证方式（整体 Base64 编码）
}smtp_authtype;

typedef struct smtp_ctx {
    uint16_t port;           //SMTP 服务器端口
    int32_t authtype;        //认证类型（smtp_authtype），握手后自动设置
    atomic_t ref;            //上层 handle 引用计数：0=C 借用(事件层不 free 块)，>0=持有者数
    int32_t established;     // 当前是否连着（建连失败 / quit / 就地关连接都清零）
    uint32_t generation;     // 连接身份代次，建连成功 / 断开各前进一次；判短路见 _serial_connect
    struct evssl_ctx *evssl; //TLS 上下文，NULL 表示不加密
    struct task_ctx *task;   //所属任务上下文
    struct coro_serial_ctx *serial;// 命令串行化执行器，多协程共用一条连接时按 FIFO 排队
    sock_ctx sk;                //连接标识 fd+skid
    char user[64];           //SMTP 用户名
    char psw[64];            //SMTP 密码
    char ip[IP_LENS];        //SMTP 服务器 IP 地址
}smtp_ctx;

// 初始化模块：注册握手完成回调
void _smtp_init(void *hspush);
// 连接断开时释放 ud_cxt 中的 smtp_ctx 引用并重置 fd
void _smtp_udfree(ud_cxt *ud);
/// <summary>
/// 简单邮件传输协议smtp初始化。字段超长不做截断——
/// 截断后的密码拿去认证只会换回服务端一句 535，调用方看不出是自己传长了
/// </summary>
/// <param name="smtp">smtp_ctx</param>
/// <param name="ip">smtp服务器，最长 IP_LENS-1</param>
/// <param name="port">smtp端口</param>
/// <param name="evssl">evssl_ctx</param>
/// <param name="user">用户名，最长 63 字节</param>
/// <param name="psw">密码，最长 63 字节</param>
/// <returns>ERR_OK 成功；ERR_FAILED 某个字段超长，此时 smtp 已被清零且可能填了前几个字段，
/// 按 init 失败处理（丢弃或 FREE），不得继续用</returns>
int32_t smtp_init(smtp_ctx *smtp, const char *ip, uint16_t port, struct evssl_ctx *evssl, const char *user, const char *psw);
/// <summary>
/// 检查返回码是否命中 codes 中任意一个。用于一条命令有多个合法应答的场合，
/// 如 RCPT TO 的 250(已接受) 与 251(已接受但将转发)，见 RFC 5321 §4.3.2。
/// 逐个按前缀比较，全不中才落一条 WARN（不会每个码刷一条）
/// </summary>
/// <param name="pack">smtp服务器返回的数据包</param>
/// <param name="codes">状态码数组</param>
/// <param name="ncode">codes 元素个数；为 0 时恒失败</param>
/// <returns>ERR_OK 命中其中之一</returns>
int32_t smtp_check_codes(char *pack, const char *const *codes, size_t ncode);
/// <summary>
/// 检查返回码是否匹配（单码，等价于 ncode 为 1 的 smtp_check_codes）
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
/// <param name="from">发件人地址 test@163.com，不可包含 \r 或 \n（防 CRLF 注入）</param>
/// <returns>数据包；from 为 NULL 或含 CR/LF 时返回 NULL</returns>
char *smtp_pack_from(const char *from);
/// <summary>
/// RCPT TO 命令数据包
/// </summary>
/// <param name="rcpt">收件人地址 test@163.com，不可包含 \r 或 \n（防 CRLF 注入）</param>
/// <returns>数据包；rcpt 为 NULL 或含 CR/LF 时返回 NULL</returns>
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
/// <param name="sk">连接标识</param>
/// <param name="buf">接收缓冲区</param>
/// <param name="ud">连接上下文（含 smtp_ctx 和解析状态）</param>
/// <param name="size">COMMAND 状态下输出数据包长度</param>
/// <param name="status">解析结果标志位（PROT_MOREDATA / PROT_ERROR）</param>
/// <returns>COMMAND 状态下返回响应数据包（需调用者释放），其余状态返回 NULL</returns>
void *smtp_unpack(ev_ctx *ev, sock_ctx *sk, int32_t client,
    buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status);

#endif//SMTP_H_
