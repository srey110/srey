#ifndef HTTP_H_
#define HTTP_H_

#include "utils/buffer.h"
#include "utils/binary.h"
#include "utils/utils.h"

// HTTP 头部块最大允许长度（4 KB）。解析侧据此拒收超长头部，打包侧也需要它：
// 发出去的响应若超过本值，对端（含 srey 自己的 http 解析器）会整包解析失败
#define MAX_HEADLENS (ONEK * 4)
typedef struct http_header_ctx {
    buf_ctx key;
    buf_ctx value;
}http_header_ctx;
struct http_pack_ctx;
struct ev_ctx;

// 释放 http_pack_ctx 结构体及其内部资源
void _http_pkfree(struct http_pack_ctx *pack);
// 释放与 ud_cxt 关联的 http 上下文资源
void _http_udfree(ud_cxt *ud);
// 连接关闭时的协议收尾：正按"关闭界定 body"接收(见 http_unpack 的 client 说明)时返回一个空载荷
// 末片包，调用方须把它当 PROT_SLICE_END 投给业务并负责释放；其余情形返 NULL
struct http_pack_ctx *_http_on_close(ud_cxt *ud);
/// <summary>
/// HTTP 解包：从缓冲区解析完整 HTTP 报文（头部 + 内容 / chunked）
/// </summary>
/// <param name="buf">接收缓冲区</param>
/// <param name="ud">连接上下文，内部维护解析状态</param>
/// <param name="client">1 表示本端是客户端（收到的是响应），0 表示收到的是请求。
/// 只对响应成立的两条规则都靠它：1xx/204/304 无报文体；以及既无 Content-Length 又无
/// Transfer-Encoding 时 body 由连接关闭界定(RFC 7230 §3.3.3 规则 7)。本端解析不出方向，故须由调用方给。
/// 规则 7 那档按分片投递：头部包带 PROT_SLICE_START，body 逐段带 PROT_SLICE，
/// 末片由 _http_on_close 在连接关闭时补 PROT_SLICE_END——业务须按分片循环收，同 chunked。
/// 规则 1 的 HEAD 那半靠发起方登记：本接口拿不到请求方法，须由 http_set_method 登记</param>
/// <param name="status">输出：解包状态标志，见 prot_status</param>
/// <returns>解析完成的 http_pack_ctx，数据不足或出错返回 NULL</returns>
struct http_pack_ctx *http_unpack(buffer_ctx *buf, ud_cxt *ud, int32_t client, int32_t *status);
/// <summary>
/// 获取状态码对应描述
/// </summary>
/// <param name="code">状态码</param>
/// <returns>描述</returns>
const char *http_code_status(int32_t code);
/// <summary>
/// 该状态码的响应是否禁止携带报文体与 Content-Length（RFC 7230 §3.3.2/§3.3.3：1xx / 204 / 304）。
/// 组包侧据此改写结束包，解析侧据此判定无 body，两边共用同一条判据
/// </summary>
/// <param name="code">状态码</param>
/// <returns>非 0 表示禁止携带</returns>
int32_t http_code_nobody(int32_t code);
/// <summary>
/// http请求包
/// </summary>
/// <param name="bwriter">binary_ctx</param>
/// <param name="method">方法 GET POST...；客户端侧须先把同一个 method 传给 http_set_method
/// 登记到连接上再组包，否则 HEAD 的响应会被当成有报文体</param>
/// <param name="url">url</param>
void http_pack_req(binary_ctx *bwriter, const char *method, const char *url);
/// <summary>
/// 把本次要发的请求方法登记到连接上：解包侧要靠它才能判定响应有无报文体
/// （HEAD 的响应按 RFC 7230 §3.3.3 规则 1 不带 body 却照样带 Content-Length，而 http_unpack
/// 拿不到请求方法）。哪些方法需要特殊处理由本函数判断，调用方只管把 method 原样传进来。
/// 三条硬约束：紧挨在 http_pack_req 之前调，且必须排在 ev_send 之前；登记了就必须把对应请求
/// 发出去；登记到收下那条响应之间，同一连接上不得再发别的请求。登记只对紧随的那一条响应生效，
/// 违反任一条都会让它落到别人的响应上，把那条的 body 当作不存在。
/// 多协程共享同一连接时，用 coro_serial 把"登记 → 发送 → 收响应"整段圈进临界区（见 coro_serial_new）
/// </summary>
/// <param name="ev">ev_ctx</param>
/// <param name="fd">socket 句柄</param>
/// <param name="skid">链接ID</param>
/// <param name="method">与 http_pack_req 同一个 method；按 RFC 7231 §4.1 区分大小写</param>
/// <returns>ERR_OK 已登记，或该方法无需登记——后者不投命令，因而也不校验 fd。
///   需要登记的方法在 fd 为 INVALID_SOCK 时返 ERR_FAILED；
///   命令执行时连接不是 HTTP、或正在读某条响应的 body，则该次登记被忽略并落 WARN</returns>
int32_t http_set_method(struct ev_ctx *ev, SOCKET fd, uint64_t skid, const char *method);
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
/// http头（值按长度取，不要求 \0 结尾，可含 NUL 等非文本字节）；键仍须 \0 结尾。
/// 键或值含 CR 或 LF 一律断言失败即退进程，故值若来自不可信来源，调用方必须先行过滤
/// 而不能依赖本函数拒绝（router_req_respond 即在其上层先筛后调）
/// </summary>
/// <param name="bwriter">binary_ctx</param>
/// <param name="key">键，\0 结尾</param>
/// <param name="val">值</param>
/// <param name="lens">值长度</param>
void http_pack_head2(binary_ctx *bwriter, const char *key, const char *val, size_t lens);
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
/// http内容包，Chunked 分块流式发送，配合 binary_offset(bwriter, 0) 循环复用同一 bwriter：
/// 首次调用时 bwriter 已含状态行+头部（offset > 0），自动补 Transfer-Encoding: Chunked\r\n\r\n
/// 再写块；此后每轮先 send(copy=1) 再 binary_offset(bwriter, 0)，该次调用就只写块行+数据；
/// 最后以 lens=0 写终止块 0\r\n\r\n
/// </summary>
/// <param name="bwriter">binary_ctx</param>
/// <param name="data">数据</param>
/// <param name="lens">长度，0 表示终止块</param>
void http_pack_chunked(binary_ctx *bwriter, void *data, size_t lens);
// 解析 HTTP 头部，返回解析后的 http_pack_ctx，transfer 输出传输方式（0/CONTENT/CHUNKED）
// ud 提供 prot_offset 缓存：半包到达时记忆扫描位置，避免对未变化前缀重复线性扫描
struct http_pack_ctx *_http_parsehead(buffer_ctx *buf, ud_cxt *ud, int32_t *transfer, int32_t *status);
/* 检查已解析头部字段的键是否大小写不敏感匹配 key（长度 klen），
 * 若 val 非 NULL，还需检查值按 RFC 7230 §3.3.1 token 列表（',' 分隔 + OWS）
 * 严格匹配 val（长度 vlen）。token 严格匹配防 "chunkedfoo" 等子串误命中导致 HTTP smuggling。 */
int32_t _http_check_keyval(http_header_ctx *head,
                           const char *key, size_t klen,
                           const char *val, size_t vlen);
/// <summary>
/// 获取第一行数据。chunked 的中间块与结束块只有数据、没有首行——
/// 首行/头部四个访问器共用这条判据，调用方不必各自再判一遍 http_chunked
/// </summary>
/// <param name="pack">http_pack_ctx</param>
/// <returns>buf_ctx 三元组；chunked 中间/结束块返回 NULL</returns>
buf_ctx *http_status(struct http_pack_ctx *pack);
/// <summary>
/// 获取头数量
/// </summary>
/// <param name="pack">http_pack_ctx</param>
/// <returns>数量；chunked 中间/结束块返回 0（判据同 http_status）</returns>
uint32_t http_nheader(struct http_pack_ctx *pack);
/// <summary>
/// 获取头
/// </summary>
/// <param name="pack">http_pack_ctx</param>
/// <param name="pos">第几个</param>
/// <returns>http_header_ctx；chunked 中间/结束块返回 NULL（判据同 http_status）</returns>
http_header_ctx *http_header_at(struct http_pack_ctx *pack, uint32_t pos);
/// <summary>
/// 获取头
/// </summary>
/// <param name="pack">http_pack_ctx</param>
/// <param name="header">键</param>
/// <param name="lens">值长度；只在返回非 NULL 时写入</param>
/// <returns>值；字段不存在或 chunked 中间/结束块返回 NULL（判据同 http_status）</returns>
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
