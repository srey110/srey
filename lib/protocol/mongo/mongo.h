#ifndef MONGO_H_
#define MONGO_H_

#include "srey/spub.h"
#include "protocol/mongo/mongo_pack.h"
#include "protocol/mongo/mongo_parse.h"

void _mongo_init(void *hspush);
void _mongo_pkfree(void *pack);
void _mongo_udfree(ud_cxt *ud);
void _mongo_closed(ud_cxt *ud);
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
/// 设置、检查、清除 接下来命令的消息标识
/// </summary>
/// <param name="mgpack">mgopack_ctx</param>
void mongo_set_flag(mongo_ctx *mongo, mongo_flags flag);
int32_t mongo_check_flag(mongo_ctx *mongo, mongo_flags flag);
//清除标识并返回旧值
int32_t mongo_clear_flag(mongo_ctx *mongo);
int32_t mongo_status_auth(void);


#endif//MONGO_H_
