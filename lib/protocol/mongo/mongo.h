#ifndef MONGO_H_
#define MONGO_H_

#include "event/evpub.h"
#include "protocol/mongo/mongo_pack.h"
#include "protocol/mongo/mongo_parse.h"

// 初始化模块：注册握手完成回调函数
void _mongo_init(void *hspush);
// 释放 mgopack_ctx 及其 payload 缓冲区
void _mongo_pkfree(void *pack);
// 释放 ud_cxt 中挂载的 mongo_ctx 相关资源（scram/error），并重置 fd
void _mongo_udfree(ud_cxt *ud);
/// <summary>
/// 从缓冲区解析一个完整的 MongoDB OP_MSG 数据包；AUTH 状态下内部处理 SCRAM 认证流程
/// </summary>
/// <param name="ev">事件上下文</param>
/// <param name="buf">接收缓冲区</param>
/// <param name="ud">连接上下文（含 mongo_ctx 和解析状态）</param>
/// <param name="status">解析结果标志位（PROT_MOREDATA / PROT_ERROR）</param>
/// <returns>COMMAND 状态下返回 mgopack_ctx*，AUTH 状态下内部消费返回 NULL</returns>
void *mongo_unpack(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client,
    buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status);
/// <summary>
/// mongo_ctx 初始化
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="ip">ip</param>
/// <param name="port">端口</param>
/// <param name="evssl">struct evssl_ctx</param>
/// <param name="db">数据库；空串用 "admin"</param>
/// <returns>ERR_OK 成功；ip 或 db 超 63 字节返 ERR_FAILED 并记 LOG_ERROR，此时不改动 mongo 任何字段</returns>
int32_t mongo_init(mongo_ctx *mongo, const char *ip, uint16_t port, struct evssl_ctx *evssl, const char *db);
/// <summary>
/// 设置当前数据库
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="db">数据库</param>
/// <returns>ERR_OK 成功；超 63 字节返 ERR_FAILED 并记 LOG_ERROR，不改动任何字段</returns>
int32_t mongo_db(mongo_ctx *mongo, const char *db);
/// <summary>
/// 设置当前验证数据库
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="db">数据库</param>
/// <returns>ERR_OK 成功；超 63 字节返 ERR_FAILED 并记 LOG_ERROR，不改动任何字段</returns>
int32_t mongo_authdb(mongo_ctx *mongo, const char *db);
/// <summary>
/// 设置当前集合
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="collection">集合</param>
/// <returns>ERR_OK 成功；超 63 字节返 ERR_FAILED 并记 LOG_ERROR，不改动任何字段。
/// 调用方必须据此原地失败：继续发命令会打到上一个集合上（写入还会自动建集合）</returns>
int32_t mongo_collection(mongo_ctx *mongo, const char *collection);
/// <summary>
/// 设置用户名 密码
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="user">用户名</param>
/// <param name="pwd">密码</param>
/// <returns>ERR_OK 成功；任一超 63 字节返 ERR_FAILED 并记 LOG_ERROR，两个字段都不改动</returns>
int32_t mongo_user_pwd(mongo_ctx *mongo, const char *user, const char *pwd);
/// <summary>
/// 设置 SCRAM 认证算法名；mongo_init 已默认 SCRAM-SHA-256，连接时由 mongo_connect 取用
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="authmod">SCRAM-SHA-1 SCRAM-SHA-256</param>
/// <returns>ERR_OK 成功；超 63 字节返 ERR_FAILED 并记 LOG_ERROR，不改动任何字段</returns>
int32_t mongo_authmod(mongo_ctx *mongo, const char *authmod);
/// <summary>
/// 获取当前命令requestid
/// </summary>
/// <param name="mgpack">mgopack_ctx</param>
/// <returns>requestid</returns>
int32_t mongo_requestid(mongo_ctx *mongo);
/// <summary>
/// 置上消息标志位（目前仅支持 MORETOCOME）。
/// 置上就一直有效直到 mongo_clear_flag——不是只管下一条：此后每条写命令都只发不等，
/// 服务端的失败(重复键、校验不过)没有响应可解析，一律报成功；读命令内部临时清掉再恢复。
/// 标志挂在连接上，多协程共用时别人的写也跟着变 fire-and-forget，批量写完及时清掉
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="flag">mongo_flags 标志位</param>
void mongo_set_flag(mongo_ctx *mongo, mongo_flags flag);
/// <summary>
/// 检查消息标志位是否已设置
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="flag">mongo_flags 标志位</param>
/// <returns>非零表示已设置</returns>
int32_t mongo_check_flag(mongo_ctx *mongo, mongo_flags flag);
/// <summary>
/// 清除所有消息标志位
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <returns>清除前的标志位值</returns>
int32_t mongo_clear_flag(mongo_ctx *mongo);
/// <summary>
/// 把会话的超时时刻续到"此刻 + logicalSessionTimeoutMinutes"。服务端每处理一条带 lsid 的
/// 命令就会延长会话寿命，本地跟着记一次，否则 mongo_session_expires 报的剩余时间会偏小。
/// 服务端没给出超时分钟数（timeoutmin <= 0）时不动，由 mongo_session_expires 按未知处理
/// </summary>
/// <param name="session">会话；必须非 NULL，函数内裸解引用</param>
void mongo_session_renew(mongo_session *session);
/// <summary>
/// 同 mongo_session_renew，但作用于连接当前绑定的会话；未绑定会话时无操作。
/// 供发送路径在收到应答后统一调用，不必各自判空
/// </summary>
/// <param name="mongo">mongo_ctx</param>
void mongo_session_touch(mongo_ctx *mongo);
/// <summary>
/// 会话距超时还剩多少秒，供调用方决定何时发 refreshSessions。
/// 只在服务端确实应答过之后才续期，发送失败不计，故读数不会偏乐观
/// </summary>
/// <param name="session">会话；必须非 NULL，函数内裸解引用</param>
/// <returns>剩余秒数；已过期返回 0 或负数；服务端未给出超时分钟数时恒返回 0（按需刷新处理）</returns>
int64_t mongo_session_expires(mongo_session *session);
/// <summary>
/// 强制清空当前挂载的事务会话指针（不释放 session 对象本身）；重连后调用，避免跨代残留的
/// lsid/txnNumber 被 TRANSACTION_OPTIONS 宏自动附加到后续普通命令
/// </summary>
/// <param name="mongo">mongo_ctx</param>
void mongo_clear_session(mongo_ctx *mongo);
/// <summary>
/// AUTH 状态值，供外部设置 ud_cxt.status 以触发认证流程
/// </summary>
/// <returns>AUTH 状态枚举值</returns>
int32_t mongo_status_auth(void);
/// <summary>
/// COMMAND 状态值，供外部在 AUTH 初始化失败时回滚 ud_cxt.status，避免连接卡在 AUTH 态
/// </summary>
/// <returns>COMMAND 状态枚举值</returns>
int32_t mongo_status_command(void);

#endif//MONGO_H_
