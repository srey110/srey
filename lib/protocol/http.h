#ifndef HTTP_H_
#define HTTP_H_

#include "utils/buffer.h"
#include "utils/binary.h"
#include "utils/utils.h"

typedef struct http_header_ctx {
    buf_ctx key;
    buf_ctx value;
}http_header_ctx;
struct http_pack_ctx;

// 释放 http_pack_ctx 结构体及其内部资源
void _http_pkfree(struct http_pack_ctx *pack);
// 释放与 ud_cxt 关联的 http 上下文资源
void _http_udfree(ud_cxt *ud);
// http 解包：从缓冲区中解析完整 HTTP 报文（头部+内容/chunked）
struct http_pack_ctx *http_unpack(buffer_ctx *buf, ud_cxt *ud, int32_t *status);
/// <summary>
/// 获取状态码对应描述
/// </summary>
/// <param name="code">状态码</param>
/// <returns>描述</returns>
const char *http_code_status(int32_t code);
/// <summary>
/// http请求包
/// </summary>
/// <param name="bwriter">binary_ctx</param>
/// <param name="method">方法 GET POST...</param>
/// <param name="url">url</param>
void http_pack_req(binary_ctx *bwriter, const char *method, const char *url);
/// <summary>
/// http响应包
/// </summary>
/// <param name="bwriter">binary_ctx</param>
/// <param name="code">状态码</param>
void http_pack_resp(binary_ctx *bwriter, int32_t code);
/// <summary>
/// http头
/// </summary>
/// <param name="bwriter">binary_ctx</param>
/// <param name="key">键</param>
/// <param name="val">值</param>
void http_pack_head(binary_ctx *bwriter, const char *key, const char *val);
/// <summary>
/// http结束包, 只有头部时使用
/// </summary>
/// <param name="bwriter">binary_ctx</param>
void http_pack_end(binary_ctx *bwriter);
/// <summary>
/// http内容包
/// </summary>
/// <param name="bwriter">binary_ctx</param>
/// <param name="data">数据</param>
/// <param name="lens">长度</param>
void http_pack_content(binary_ctx *bwriter, void *data, size_t lens);
/// <summary>
/// http内容包，Chunked 分块流式发送。调用方须在写入状态行和其他头部之后按如下循环使用：
///   第一次调用时 bwriter 已含状态行+头部（offset>0），函数自动追加
///   Transfer-Encoding: Chunked\r\n\r\n 完成头部，再写入块行和数据；
///   之后每次调用前须先 send(bwriter) copy=1，再 binary_offset(bwriter, 0) 重置，
///   使 offset 归零，后续调用只写块行+数据，不再重复写头部；
///   最后以 lens=0 调用写入终止块 0\r\n\r\n，send 后结束。
/// 示例：
///   http_pack_resp(bw, 200);
///   http_pack_head(bw, "Content-Type", "text/plain");
///   while (有数据) {
///       http_pack_chunked(bw, data, lens);
///       send(fd, skid, bw->data, bw->offset, 1);
///       binary_offset(bw, 0);
///   }
///   http_pack_chunked(bw, NULL, 0);   // 终止块
///   send(fd, skid, bw->data, bw->offset, 1);
/// </summary>
/// <param name="bwriter">binary_ctx</param>
/// <param name="data">数据</param>
/// <param name="lens">长度，0 表示终止块</param>
void http_pack_chunked(binary_ctx *bwriter, void *data, size_t lens);

// 解析 HTTP 头部，返回解析后的 http_pack_ctx，transfer 输出传输方式（0/CONTENT/CHUNKED）
struct http_pack_ctx *_http_parsehead(buffer_ctx *buf, int32_t *transfer, int32_t *status);
/* 检查已解析头部字段的键是否大小写不敏感匹配 key（长度 klen），
 * 若 val 非 NULL，还需检查值中是否包含 val（长度 vlen）。
 * 传入显式长度以便调用方使用编译期常量（sizeof(literal)-1），函数足够小可内联。 */
static inline int32_t _http_check_keyval(http_header_ctx *head,
                                          const char *key, size_t klen,
                                          const char *val, size_t vlen) {
    if (!buf_icompare(&head->key, key, klen)) {
        return ERR_FAILED;
    }
    if (NULL == val) {
        return ERR_OK;
    }
    if (NULL != memstr(1, head->value.data, head->value.lens, val, vlen)) {
        return ERR_OK;
    }
    return ERR_FAILED;
}
/// <summary>
/// 获取第一行数据
/// </summary>
/// <param name="pack">http_pack_ctx</param>
/// <returns>buf_ctx</returns>
buf_ctx *http_status(struct http_pack_ctx *pack);
/// <summary>
/// 获取头数量
/// </summary>
/// <param name="pack">http_pack_ctx</param>
/// <returns>数量</returns>
uint32_t http_nheader(struct http_pack_ctx *pack);
/// <summary>
/// 获取头
/// </summary>
/// <param name="pack">http_pack_ctx</param>
/// <param name="pos">第几个</param>
/// <returns>http_header_ctx</returns>
http_header_ctx *http_header_at(struct http_pack_ctx *pack, uint32_t pos);
/// <summary>
/// 获取头
/// </summary>
/// <param name="pack">http_pack_ctx</param>
/// <param name="header">键</param>
/// <param name="lens">值长度</param>
/// <returns>值</returns>
char *http_header(struct http_pack_ctx *pack, const char *header, size_t *lens);
/// <summary>
/// chunked
/// </summary>
/// <param name="pack">http_pack_ctx</param>
/// <returns>0 非chunked, 1 chunked开始 2 chunked数据包</returns>
int32_t http_chunked(struct http_pack_ctx *pack);
/// <summary>
/// 获取数据包
/// </summary>
/// <param name="pack">http_pack_ctx</param>
/// <param name="lens">数据包长度</param>
/// <returns>数据包</returns>
void *http_data(struct http_pack_ctx *pack, size_t *lens);

#endif//HTTP_H_
