#ifndef URL_PARSE_H_
#define URL_PARSE_H_

#include "base/structs.h"

#define MAX_NPARAM 16

typedef struct url_param {
    buf_ctx key;
    buf_ctx val;
}url_param;
typedef struct url_ctx {
    buf_ctx scheme;
    buf_ctx user;
    buf_ctx psw;
    buf_ctx host;
    buf_ctx port;
    buf_ctx path;
    buf_ctx anchor;
    url_param param[MAX_NPARAM];
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
