#ifndef URL_PARSE_H_
#define URL_PARSE_H_

#include "base/structs.h"

#define URL_MAX_PARAM       64 // 参数上限
#define URL_MAX_PATH_DEPTH  64 // 路径上限
#define URL_BUF_LENS   ONEK  // url_parse 内部工作缓冲区大小

typedef struct url_param {
    buf_ctx key; // 参数名
    buf_ctx val; // 参数值
}url_param;
typedef struct url_ctx {
    int8_t sep;               // 路径拆分标记
    int32_t decode;           // 是否解码
    int32_t npath;            // 拆分后的路径数量
    size_t pathlens;          // 重组后路径总长（含各段分隔符）；可据此预分配缓冲（>= pathlens+1）
    buf_ctx scheme;           // 协议类型（如 http、https）
    buf_ctx user;             // 用户名
    buf_ctx psw;              // 密码
    buf_ctx host;             // 主机地址
    buf_ctx port;             // 端口号
    buf_ctx anchor;           // 片段标识（# 之后的部分）
    url_param param[URL_MAX_PARAM]; // 查询参数列表（最多 URL_MAX_PARAM 个）
    buf_ctx segs[URL_MAX_PATH_DEPTH]; // 拆分后的各路径段（decode 时已就地解码）
    char buf[URL_BUF_LENS];  // url_parse 内部工作缓冲区，持有输入 url 的可写副本
}url_ctx;

/// <summary>
/// 不支持嵌套 [协议类型]://[访问资源需要的凭证信息]@[服务器地址]:[端口号]/[资源层级UNIX文件路径][文件名]?[查询]#[片段ID]
/// </summary>
/// <param name="ctx">url_ctx</param>
/// <param name="url">待解析 url；decode 时复制进 ctx->buf 处理（原串不改、超长返 ERR_FAILED），不 decode 时就地解析、各字段与 segs 指向 url 本身（调用方须保证其生命周期）</param>
/// <param name="lens">url长度</param>
/// <param name="sep">路径段分隔符（拆分写入 ctx->segs）</param>
/// <param name="decode">非 0 时对 path 段与 query 做 url 解码（host/port/scheme 不解码）</param>
/// <returns>ERR_OK成功</returns>
int32_t url_parse(url_ctx *ctx, const char *url, size_t lens, int8_t sep, int32_t decode);
/// <summary>
/// 把 url_parse 拆分后的 segs 重组为完整路径写入 path（每段前补 sep，形如 /a/b），并写结尾 '\0'。
/// 容量不足时截断到放得下的整段为止
/// </summary>
/// <param name="ctx">url_ctx（须先经 url_parse）</param>
/// <param name="path">输出缓冲</param>
/// <param name="cap">path 缓冲容量（字节）；不截断需 >= ctx->pathlens + 1</param>
/// <returns>写入 path 的字节数（不含结尾 '\0'）</returns>
size_t url_reorg_path(url_ctx *ctx, char *path, size_t cap);
/// <summary>
/// 把 url_parse 解析的 param[] 重组为 key=val&amp;k2=v2 形式的查询字符串写入 param，并写结尾 '\0'。
/// 容量不足时截断到放得下的整对为止
/// </summary>
/// <param name="ctx">url_ctx（须先经 url_parse）</param>
/// <param name="param">输出缓冲</param>
/// <param name="cap">param 缓冲容量（字节）</param>
/// <returns>写入 param 的字节数（不含结尾 '\0'）；无参数时返回 0</returns>
size_t url_reorg_param(url_ctx *ctx, char *param, size_t cap);
/// <summary>
/// 获取参数
/// </summary>
/// <param name="ctx">url_ctx</param>
/// <param name="key">名称</param>
/// <returns>buf_ctx值</returns>
buf_ctx *url_get_param(url_ctx *ctx, const char *key);

#endif//URL_PARSE_H_
