#ifndef MONGO_PARSE_H_
#define MONGO_PARSE_H_
#include "protocol/mongo/mongo_struct.h"

/// <summary>
/// 解析 SCRAM 认证响应包，提取 conversationId、done 标志和 payload 数据
/// </summary>
/// <param name="mgopack">服务端返回的消息包</param>
/// <param name="convid">输出 conversationId</param>
/// <param name="done">输出认证是否完成标志（非零表示完成）</param>
/// <param name="payload">输出认证 payload 数据指针（指向 mgopack 内部，无需释放）</param>
/// <param name="plens">输出 payload 长度</param>
/// <returns>ok 字段值（非零表示服务端返回成功）</returns>
int32_t mongo_parse_auth_response(mgopack_ctx *mgopack, int32_t *convid, int32_t *done, char **payload, size_t *plens);
/// <summary>
/// 从响应包中提取游标 ID
/// </summary>
/// <param name="mgpack">服务端返回的消息包</param>
/// <returns>游标 ID，0 表示无游标或未找到</returns>
int64_t mongo_cursorid(mgopack_ctx *mgpack);
/// <summary>
/// 检查响应包中是否包含错误（ok 为 0 或含 writeErrors/errmsg 等字段）
/// </summary>
/// <param name="mongo">mongo_ctx（出错时写入 error 字段）</param>
/// <param name="mgpack">服务端返回的消息包</param>
/// <returns>成功返回影响文档数（n 字段值），失败返回 ERR_FAILED</returns>
int32_t mongo_parse_check_error(mongo_ctx *mongo, mgopack_ctx *mgpack);
/// <summary>
/// 设置 mongo_ctx 的错误信息
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="err">错误字符串，NULL 则清空</param>
/// <param name="copy">非零表示复制字符串（需释放），0 表示直接持有指针</param>
void mongo_set_error(mongo_ctx *mongo, const char *err, int32_t copy);
/// <summary>
/// 解析 startSession 响应，提取会话 UUID 和超时时间
/// </summary>
/// <param name="mongo">mongo_ctx（出错时写入 error 字段）</param>
/// <param name="mgpack">服务端返回的消息包</param>
/// <param name="uid">输出会话 UUID（UUID_LENS 字节）</param>
/// <param name="timeout">输出会话超时分钟数</param>
/// <returns>ok 字段值（非零表示成功）</returns>
int32_t mongo_parse_startsession(mongo_ctx *mongo, mgopack_ctx *mgpack, char uid[UUID_LENS], int32_t *timeout);

#endif//MONGO_PARSE_H_
