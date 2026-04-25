#ifndef MONGO_H_
#define MONGO_H_

#include "srey/spub.h"
#include "protocol/mongo/mongo_pack.h"
#include "protocol/mongo/mongo_parse.h"

// 初始化模块：注册握手完成回调函数
void _mongo_init(void *hspush);
// 释放 mgopack_ctx 及其 payload 缓冲区
void _mongo_pkfree(void *pack);
// 释放 ud_cxt 中挂载的 mongo_ctx 相关资源（scram/error），并重置 fd
void _mongo_udfree(ud_cxt *ud);
// 连接关闭时的清理回调，等同于 _mongo_udfree
void _mongo_closed(ud_cxt *ud);
/// <summary>
/// 从缓冲区解析一个完整的 MongoDB OP_MSG 数据包；AUTH 状态下内部处理 SCRAM 认证流程
/// </summary>
/// <param name="ev">事件上下文</param>
/// <param name="buf">接收缓冲区</param>
/// <param name="ud">连接上下文（含 mongo_ctx 和解析状态）</param>
/// <param name="status">解析结果标志位（PROT_MOREDATA / PROT_ERROR）</param>
/// <returns>COMMAND 状态下返回 mgopack_ctx*，AUTH 状态下内部消费返回 NULL</returns>
void *mongo_unpack(ev_ctx *ev, buffer_ctx *buf, ud_cxt *ud, int32_t *status);
/// <summary>
/// mongo_ctx 初始化
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="ip">ip</param>
/// <param name="port">端口</param>
/// <param name="evssl">struct evssl_ctx</param>
/// <param name="db">数据库</param>
void mongo_init(mongo_ctx *mongo, const char *ip, uint16_t port, struct evssl_ctx *evssl, const char *db);
/// <summary>
/// 设置当前数据库
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="db">数据库</param>
void mongo_db(mongo_ctx *mongo, const char *db);
/// <summary>
/// 设置当前验证数据库
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="db">数据库</param>
void mongo_authdb(mongo_ctx *mongo, const char *db);
/// <summary>
/// 设置当前集合
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="collection">集合</param>
void mongo_collection(mongo_ctx *mongo, const char *collection);
/// <summary>
/// 设置用户名 密码
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="user">用户名</param>
/// <param name="pwd">密码</param>
void mongo_user_pwd(mongo_ctx *mongo, const char *user, const char *pwd);
/// <summary>
/// 获取错误信息
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <returns>错误</returns>
const char *mongo_error(mongo_ctx *mongo);
/// <summary>
/// 获取当前命令requestid
/// </summary>
/// <param name="mgpack">mgopack_ctx</param>
/// <returns>requestid</returns>
int32_t mongo_requestid(mongo_ctx *mongo);
/// <summary>
/// 设置下一条命令的消息标志位（目前仅支持 MORETOCOME）
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
/// 清除所有消息标志位并返回旧值
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <returns>清除前的标志位值</returns>
int32_t mongo_clear_flag(mongo_ctx *mongo);
/// <summary>
/// 返回 AUTH 状态值，供外部设置 ud_cxt.status 以触发认证流程
/// </summary>
/// <returns>AUTH 状态枚举值</returns>
int32_t mongo_status_auth(void);


#endif//MONGO_H_
