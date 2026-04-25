#ifndef URL_PARSE_H_
#define URL_PARSE_H_

#include "base/structs.h"

#define MAX_NPARAM 16

typedef struct url_param {
    buf_ctx key; // 参数名
    buf_ctx val; // 参数值
}url_param;
typedef struct url_ctx {
    buf_ctx scheme;           // 协议类型（如 http、https）
    buf_ctx user;             // 用户名
    buf_ctx psw;              // 密码
    buf_ctx host;             // 主机地址
    buf_ctx port;             // 端口号
    buf_ctx path;             // 路径（不含前导 /）
    buf_ctx anchor;           // 片段标识（# 之后的部分）
    url_param param[MAX_NPARAM]; // 查询参数列表（最多 MAX_NPARAM 个）
}url_ctx;
/// <summary>
/// 不支持嵌套 [协议类型]://[访问资源需要的凭证信息]@[服务器地址]:[端口号]/[资源层级UNIX文件路径][文件名]?[查询]#[片段ID]
/// </summary>
/// <param name="ctx">url_ctx</param>
/// <param name="url">url</param>
/// <param name="lens">url长度</param>
void url_parse(url_ctx *ctx, char *url, size_t lens);
/// <summary>
/// 获取参数
/// </summary>
/// <param name="ctx">url_ctx</param>
/// <param name="key">名称</param>
/// <returns>buf_ctx值</returns>
buf_ctx *url_get_param(url_ctx *ctx, const char *key);

#endif//URL_PARSE_H_
