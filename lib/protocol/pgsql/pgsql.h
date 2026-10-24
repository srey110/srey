#ifndef PGSQL_H_
#define PGSQL_H_

#include "protocol/pgsql/pgsql_pack.h"

// 初始化握手完成推送回调（在协议注册时调用）
void _pgsql_init(void *hspush);
// 释放 pgpack_ctx 数据包
void _pgsql_pkfree(void *pack);
// 释放 ud_cxt 中绑定的 pgsql 上下文资源
void _pgsql_udfree(ud_cxt *ud);
// 连接建立后处理：配置 evssl 则发送 SSL 协商请求，否则跳过协商直接发送 Startup
int32_t _pgsql_on_connected(ev_ctx *ev, sock_ctx *sk, ud_cxt *ud, int32_t err);
// SSL 握手完成后提取服务端证书摘要（用于 SCRAM-PLUS 通道绑定）并发送 Startup 消息，成功返 PROT_SSL_OWNED
int32_t _pgsql_ssl_exchanged(ev_ctx *ev, ud_cxt *ud, void *ssl);
// 判断当前数据包是否允许 task 恢复（通知包不允许立即恢复）
int32_t _pgsql_may_resume(void *data);
/// <summary>
/// PostgreSQL 协议解包入口：按连接状态分派 SSL / 认证 / 命令响应处理
/// </summary>
/// <param name="ev">事件上下文</param>
/// <param name="buf">接收缓冲区</param>
/// <param name="ud">连接上下文，内部维护解析状态</param>
/// <param name="size">输出：结果包为上一个 ReadyForQuery 之后到本次 ReadyForQuery 的命令阶段消息字节之和，
///   通知（NotificationResponse）与 CopyInResponse 包为该条消息自身的字节数；只用于内存记账，不是 data 处可读的长度。
///   没返回包时也可能写入（如收到 ReadyForQuery 但没有累积包）。必须非 NULL</param>
/// <param name="status">输出：解包状态标志，见 prot_status</param>
/// <returns>命令阶段返回 pgpack_ctx，认证阶段内部消费返回 NULL；数据不足或出错返回 NULL</returns>
void *pgsql_unpack(ev_ctx *ev, sock_ctx *sk, int32_t client,
    buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status);
/// <summary>
/// 初始化 pgsql 连接参数
/// </summary>
/// <param name="pg">pgsql_ctx 指针</param>
/// <param name="ip">服务端 IP 地址</param>
/// <param name="port">服务端端口，0 表示使用默认端口 5432</param>
/// <param name="evssl">SSL 上下文，不使用 SSL 时为 NULL</param>
/// <param name="user">登录用户名</param>
/// <param name="password">登录密码，按原样参与认证（SCRAM 前不做 SASLprep）：含需 NFKC 规范化字符的
/// 非 ASCII 密码须由调用方先规范化，否则与服务端存的验证器对不上、认证失败</param>
/// <param name="database">目标数据库名</param>
/// <returns>ERR_OK 成功，ERR_FAILED 参数过长</returns>
int32_t pgsql_init(pgsql_ctx *pg, const char *ip, uint16_t port, struct evssl_ctx *evssl,
    const char *user, const char *password, const char *database);
/// <summary>
/// 更新 pgsql 连接的用户名和密码
/// </summary>
/// <param name="pg">pgsql_ctx 指针</param>
/// <param name="user">新用户名</param>
/// <param name="password">新密码，约束同 pgsql_init</param>
/// <returns>ERR_OK 成功；任一项超 63 字节时返回 ERR_FAILED，用户名与密码均保持原值</returns>
int32_t pgsql_set_userpwd(pgsql_ctx *pg, const char *user, const char *password);
/// <summary>
/// 更新目标数据库名
/// </summary>
/// <param name="pg">pgsql_ctx 指针</param>
/// <param name="database">新数据库名</param>
/// <returns>ERR_OK 成功；超 63 字节时返回 ERR_FAILED，库名保持原值</returns>
int32_t pgsql_set_db(pgsql_ctx *pg, const char *database);
/// <summary>
/// 获取当前配置的数据库名
/// </summary>
/// <param name="pg">pgsql_ctx 指针</param>
/// <returns>数据库名字符串</returns>
const char *pgsql_get_db(pgsql_ctx *pg);
/// <summary>
/// 结果个数，按 CommandComplete 计，多语句 simple query 每条语句一个；全部结果攒到 ReadyForQuery 才一起交出。
/// 任一条出错整包即 PGPACK_ERR、结果全丢；串里含 COMMIT 等事务控制时出错前已提交的不回滚，
/// 故 PGPACK_ERR 不等于全部回滚。COPY TO STDOUT 别和别的语句拼在一条 query 里，会丢掉前面的结果
/// </summary>
/// <param name="pgpack">pgpack_ctx 指针</param>
/// <returns>结果个数；不是 PGPACK_OK、或响应不带 CommandComplete（prepare / stmt_close / 空 SQL）时为 0</returns>
uint32_t pgsql_result_count(pgpack_ctx *pgpack);
/// <summary>
/// 从最后一条命令完成标签中解析受影响的行数；多语句 simple query 只反映最后一条，
/// 逐条读取用 pgsql_affected_at
/// </summary>
/// <param name="pgpack">pgpack_ctx 指针，complete 字段须已填充</param>
/// <returns>受影响的行数；PGPACK_ERR 或解析失败时为 0。
/// COPY TO STDOUT 的包不入结果数组，它的 "COPY N" 只能从这里取；COPY FROM STDIN 收尾的完成包
/// 是普通 PGPACK_OK、占一个结果，affected_at(0) 与这里相同</returns>
int64_t pgsql_affected_rows(pgpack_ctx *pgpack);
/// <summary>
/// 解析第 idx 个结果的命令完成标签中受影响的行数
/// </summary>
/// <param name="pgpack">pgpack_ctx 指针</param>
/// <param name="idx">结果下标，从 0 开始，总数见 pgsql_result_count</param>
/// <returns>受影响的行数；类型不符、下标越界或解析失败时返回 0</returns>
int64_t pgsql_affected_at(pgpack_ctx *pgpack, uint32_t idx);

#endif//PGSQL_H_
