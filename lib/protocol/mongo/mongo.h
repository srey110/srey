#ifndef MONGO_H_
#define MONGO_H_

#include "event/evpub.h"
#include "protocol/mongo/mongo_pack.h"
#include "protocol/mongo/mongo_parse.h"

// 初始化模块：注册握手完成回调函数
void _mongo_init(void *hspush);
// 释放 mgopack_ctx 及其 payload 缓冲区
void _mongo_pkfree(void *pack);
// 释放连接上挂的 scram、把 mongo->sk 标成已关、放手本连接持有的 mongo_ctx 引用
void _mongo_udfree(ud_cxt *ud);
/// <summary>
/// 从缓冲区解析一个完整的 MongoDB OP_MSG 数据包；AUTH 状态下内部处理 SCRAM 认证流程
/// </summary>
/// <param name="ev">事件上下文</param>
/// <param name="buf">接收缓冲区</param>
/// <param name="ud">连接上下文（含 mongo_ctx 和解析状态）</param>
/// <param name="size">输出：COMMAND 状态下为该 OP_MSG 的线上字节数，只用于内存记账，不是 data 处可读的长度。
/// 没返回包时不写。必须非 NULL</param>
/// <param name="status">解析结果标志位（PROT_MOREDATA / PROT_ERROR）</param>
/// <returns>COMMAND 状态下返回 mgopack_ctx*，AUTH 状态下内部消费返回 NULL</returns>
void *mongo_unpack(ev_ctx *ev, sock_ctx *sk, int32_t client,
    buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status);
/// <summary>
/// mongo_ctx 初始化。服务端最低 4.4：SCRAM 认证依赖 saslStart 的 skipEmptyExchange，更早的版本认证必败
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="ip">ip</param>
/// <param name="port">端口</param>
/// <param name="evssl">struct evssl_ctx</param>
/// <param name="db">数据库；空串用 "admin"</param>
/// <returns>ERR_OK 成功；ip 或 db 超 63 字节返 ERR_FAILED 并记 LOG_ERROR，此时 mongo 已被清零、
/// 可能填了部分字段，按 init 失败丢弃，不得继续用</returns>
int32_t mongo_init(mongo_ctx *mongo, const char *ip, uint16_t port, struct evssl_ctx *evssl, const char *db);
/// <summary>
/// 设置当前数据库。成功时顺带清空当前集合名，切库后须重新 mongo_collection，
/// 否则后续命令组出空集合名、服务端按非法命名空间拒收
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
/// <param name="pwd">密码，SCRAM-SHA-256 下按原样参与认证、不做 SASLprep：含需 NFKC 规范化字符的
/// 非 ASCII 密码须由调用方先规范化，否则与服务端存的凭据对不上、认证失败</param>
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
/// <param name="mongo">mongo_ctx</param>
/// <returns>requestid</returns>
int32_t mongo_requestid(mongo_ctx *mongo);
/// <summary>
/// 置上消息标志位（目前仅实现 MORETOCOME，其余位收下即丢弃）。
/// 置上就一直有效直到 mongo_clear_flag——不是只管下一条：此后每条写命令都只发不等，
/// 服务端的失败(重复键、校验不过)没有响应可解析，一律报成功；要等回包的命令组包时恒不带（见 mongo_pack.h 文件头）。
/// 标志挂在连接上，多协程共用时别人的写也跟着变 fire-and-forget，批量写完及时清掉
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="flag">mongo_flags 的按位或；MONGO_FLAGS_ALL 之外的位一律不收（整个调用无效），
///   0 等于什么都不置。还原惯用法 mongo_set_flag(mongo, mongo_clear_flag(mongo)) 直接传旧值整字</param>
void mongo_set_flag(mongo_ctx *mongo, int32_t flag);
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
/// 强制清空当前挂载的事务会话指针（不释放 session 对象本身）。清掉之后的写不再带事务上下文、
/// 按自动提交执行；重连作废事务不靠它（见 mongo_begin），事务中途别调
/// </summary>
/// <param name="mongo">mongo_ctx</param>
void mongo_clear_session(mongo_ctx *mongo);
/// <summary>
/// 把 mongo_pack_scram_client_first 新建的 scram 投到连接所在的网络线程装上，并把连接切到 AUTH 态。
/// 必须在发 saslStart 包之前调：二者走同一条命令队列，保证先装后发。
/// 到达时不是 mongo 连接、上下文已拆或上一轮认证还没收尾就不装，并关掉连接
/// </summary>
/// <param name="ev">事件上下文</param>
/// <param name="sk">mongo 连接标识</param>
/// <param name="scram">所有权交给本函数，没装上（含连接已不在、事件循环拆除时命令还没执行）都由它释放</param>
/// <returns>ERR_OK 命令已入队；sk 已失效返回 ERR_FAILED，scram 已释放</returns>
int32_t mongo_auth_begin(ev_ctx *ev, sock_ctx *sk, struct scram_ctx *scram);

#endif//MONGO_H_
