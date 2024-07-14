#ifndef HTTP_H_
#define HTTP_H_

#include "base/structs.h"
#include "utils/buffer.h"
#include "utils/binary.h"

typedef struct http_header_ctx {
    buf_ctx key;
    buf_ctx value;
}http_header_ctx;
struct http_pack_ctx;

void _http_pkfree(struct http_pack_ctx *pack);
void _http_udfree(ud_cxt *ud);
//http解包
struct http_pack_ctx *http_unpack(buffer_ctx *buf, ud_cxt *ud, int32_t *status);
/// <summary>
/// 获取状态码对应的描述
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
/// http返回包
/// </summary>
/// <param name="bwriter">binary_ctx</param>
/// <param name="code">状态码</param>
void http_pack_resp(binary_ctx *bwriter, int32_t code);
/// <summary>
/// http头
/// </summary>
/// <param name="bwriter">binary_ctx</param>
/// <param name="key">名称</param>
/// <param name="val">值</param>
void http_pack_head(binary_ctx *bwriter, const char *key, const char *val);
/// <summary>
/// http包结束, 只有头的时候
/// </summary>
/// <param name="bwriter">binary_ctx</param>
void http_pack_end(binary_ctx *bwriter);
/// <summary>
/// http包内容
/// </summary>
/// <param name="bwriter">binary_ctx</param>
/// <param name="data">数据</param>
/// <param name="lens">长度</param>
void http_pack_content(binary_ctx *bwriter, void *data, size_t lens);
/// <summary>
/// http包内容,Chunked 方式. 循环 send(...); binary_offset(bwriter, 0);
/// </summary>
/// <param name="bwriter">binary_ctx</param>
/// <param name="data">数据</param>
/// <param name="lens">长度</param>
void http_pack_chunked(binary_ctx *bwriter, void *data, size_t lens);

struct http_pack_ctx *_http_parsehead(buffer_ctx *buf, int32_t *transfer, int32_t *status);
int32_t _http_check_keyval(http_header_ctx *head, const char *key, const char *val);
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
/// <param name="header">名称</param>
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
