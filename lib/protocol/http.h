#ifndef HTTP_H_
#define HTTP_H_

#include "utils/buffer.h"
#include "utils/binary.h"
#include "protocol/prots_pub.h"

// 100 Continue 中间响应整包(服务端用)
#define HTTP_CONTINUE_RESP "HTTP/1.1 100 Continue\r\n\r\n"
// 附加头名的长度上限(不含)，超了整条丢
#define HTTP_HEAD_KEY_MAX 128
// http_head_key_check 的 autoflags：组包函数自己写的头，调用方再给就丢
#define HTTP_AUTO_CT    0x01 // Content-Type
#define HTTP_AUTO_FRAME 0x02 // Content-Length 与 Transfer-Encoding

// http_head_key_check 的结果
typedef enum http_key_rc {
    HTTP_KEY_BAD = -1,  // 不是 token，或长度达到 HTTP_HEAD_KEY_MAX
    HTTP_KEY_OK = 0,
    HTTP_KEY_AUTO = 1   // 组包函数自己生成的头
}http_key_rc;
typedef struct http_header_ctx {
    buf_ctx key;
    buf_ctx value;
}http_header_ctx;
struct http_pack_ctx;

// 释放 http_pack_ctx 结构体及其内部资源
void _http_pkfree(void *data);
// 释放与 ud_cxt 关联的 http 上下文资源
void _http_udfree(ud_cxt *ud);
// 连接关闭时的协议收尾：正按"关闭界定 body"接收(见 http_unpack 的 client 说明)时返回一个空载荷
// 末片包，调用方须把它当 PROT_SLICE_END 投给业务并负责释放；其余情形返 NULL
void *_http_on_close(ud_cxt *ud);
/// <summary>
/// HTTP 解包：从缓冲区解析完整 HTTP 报文（头部 + 内容 / chunked）。
/// Transfer-Encoding 只解 chunked 分帧：只要 chunked 是最后一层就接受，排在它前面的编码(如 "gzip, chunked" 的 gzip)不解，
/// 报文体原样交上去，原始的 Transfer-Encoding 头留在头部里，业务需要的话自己看头解码。
/// chunked 不是最后一层时：请求判协议错误；响应的 body 由连接关闭界定(见 client)。HTTP/1.0 报文带 Transfer-Encoding 一律判协议错误。
/// 服务端解请求时忽略请求行前的空行(CRLF)，跳过的字节计入头块上限 HTTP_MAX_HEADLENS。
/// 服务端收到 http_expect_continue 成立、Content-Length 大于 0 且不超上限的请求时，等体之前经 ev / sk
/// 自动回一条 HTTP_CONTINUE_RESP；chunked 请求不自动回
/// </summary>
/// <param name="ev">ev_ctx；只在服务端自动回 100 Continue 时用到。sk 是有效连接时不得为 NULL</param>
/// <param name="sk">连接标识，用途同 ev；必须非 NULL，可以是无效连接(此时不回 100 Continue)</param>
/// <param name="buf">接收缓冲区</param>
/// <param name="ud">连接上下文，内部维护解析状态</param>
/// <param name="client">1 表示本端是客户端（收到的是响应），0 表示收到的是请求。
/// 只对响应成立的两条规则都靠它：1xx/204/304 无报文体；以及既无 Content-Length 又无
/// Transfer-Encoding、或 Transfer-Encoding 最后一层不是 chunked 时 body 由连接关闭界定(RFC 7230 §3.3.3 规则 7 / 规则 3)。
/// 本端解析不出方向，故须由调用方给。
/// 连接关闭界定那档按分片投递：头部包带 PROT_SLICE_START，body 逐段带 PROT_SLICE，
/// 末片由 _http_on_close 在连接关闭时补 PROT_SLICE_END——业务须按分片循环收，同 chunked。
/// 规则 1 的 HEAD 那半靠发起方登记：本接口拿不到请求方法，须由 http_set_method 登记</param>
/// <param name="size">输出：返回包时写入记账字节数，口径见 prots_unpack。头包为头 + 体，不是载荷长度；
/// chunk / tillclose 片就是本片载荷长，末片为 0。没返回包时不写。必须非 NULL</param>
/// <param name="status">输出：解包状态标志，见 prot_status</param>
/// <returns>解析完成的 http_pack_ctx，数据不足或出错返回 NULL</returns>
void *http_unpack(struct ev_ctx *ev, sock_ctx *sk, int32_t client,
    buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status);
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
/// <param name="sk">连接标识</param>
/// <param name="method">与 http_pack_req 同一个 method；按 RFC 7231 §4.1 区分大小写</param>
/// <returns>ERR_OK 已登记，或该方法无需登记——后者不投命令，因而也不校验 fd。
///   需要登记的方法在 fd 为 INVALID_SOCK 时返 ERR_FAILED；
///   命令执行时连接不是 HTTP、或正在读某条响应的 body，则该次登记被忽略并落 WARN</returns>
int32_t http_set_method(struct ev_ctx *ev, sock_ctx *sk, const char *method);
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
/// 而不能依赖本函数拒绝
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
// 解析 HTTP 头部，返回解析后的 http_pack_ctx，transfer 输出传输方式（0/CONTENT/CHUNKED/TILLCLOSE）。
// client 同 http_unpack；为 0 时跳过请求行前的空行。ud 提供 prot_offset 缓存：半包到达时记忆扫描位置，避免对未变化前缀重复线性扫描
struct http_pack_ctx *_http_parsehead(buffer_ctx *buf, ud_cxt *ud, int32_t client, int32_t *transfer, int32_t *status);
/* 检查已解析头部字段的键是否大小写不敏感匹配 key（长度 klen），
 * 若 val 非 NULL，还需检查值按 RFC 7230 §3.3.1 token 列表（',' 分隔 + OWS）
 * 严格匹配 val（长度 vlen）。token 严格匹配防 "chunkedfoo" 等子串误命中导致 HTTP smuggling。 */
int32_t _http_check_keyval(http_header_ctx *head,
                           const char *key, size_t klen,
                           const char *val, size_t vlen);
/// <summary>
/// 获取第一行数据。数据片(chunked 的数据块与结束块、连接关闭界定的 body 片与末片)只有数据、没有首行——
/// 首行/头部四个访问器共用这条判据，调用方不必各自再判一遍 http_chunked
/// </summary>
/// <param name="pack">http_pack_ctx</param>
/// <returns>buf_ctx 三元组；数据片返回 NULL</returns>
buf_ctx *http_status(struct http_pack_ctx *pack);
/// <summary>
/// 获取头数量
/// </summary>
/// <param name="pack">http_pack_ctx</param>
/// <returns>数量；数据片返回 0（判据同 http_status）</returns>
uint32_t http_nheader(struct http_pack_ctx *pack);
/// <summary>
/// 获取头
/// </summary>
/// <param name="pack">http_pack_ctx</param>
/// <param name="pos">第几个</param>
/// <returns>http_header_ctx；数据片返回 NULL（判据同 http_status）</returns>
http_header_ctx *http_header_at(struct http_pack_ctx *pack, uint32_t pos);
/// <summary>
/// 获取头
/// </summary>
/// <param name="pack">http_pack_ctx</param>
/// <param name="header">键</param>
/// <param name="lens">值长度；只在返回非 NULL 时写入</param>
/// <returns>值；字段不存在或数据片返回 NULL（判据同 http_status）</returns>
char *http_header(struct http_pack_ctx *pack, const char *header, size_t *lens);
/// <summary>
/// 包的分帧类别。流的起止以投递时的 slice 为准：连接关闭界定 body 的头包带 PROT_SLICE_START，这里却返回 0
/// </summary>
/// <param name="pack">http_pack_ctx</param>
/// <returns>0 非 chunked 的整包，或连接关闭界定 body 的头包；1 chunked 的头包；
/// 2 数据片(chunked 的数据块与结束块、连接关闭界定的 body 片与末片，见 http_status)</returns>
int32_t http_chunked(struct http_pack_ctx *pack);
/// <summary>
/// 获取数据包
/// </summary>
/// <param name="pack">http_pack_ctx</param>
/// <param name="lens">数据包长度</param>
/// <returns>数据包</returns>
void *http_data(struct http_pack_ctx *pack, size_t *lens);
/// <summary>
/// 把 Content-Length 请求/响应的数据体整块摘走，省掉一次拷贝；pack 照常由协议层释放
/// </summary>
/// <param name="pack">http_pack_ctx</param>
/// <param name="lens">必须非 NULL；写入数据体长度，返回 NULL 时写 0</param>
/// <returns>数据体，末尾另有一个不计入 lens 的 '\0'，所有权归调用方(用 FREE 释放)；返回非 NULL 后同一 pack 上
/// http_data 返回 NULL、长度 0。无数据体、已摘过、或 chunked / 连接关闭界定的分片(载荷与 pack 同一块分配，
/// 交不出去)返回 NULL</returns>
void *http_take_data(struct http_pack_ctx *pack, size_t *lens);
/// <summary>
/// 请求是否在等 100 Continue：HTTP/1.1 请求且 Expect 含 100-continue(不分大小写)，HTTP/1.0 的一律不认。
/// Content-Length 请求由 http_unpack 自动回；chunked 请求由上层准入通过后自己发 HTTP_CONTINUE_RESP
/// </summary>
/// <param name="pack">服务端收到的请求包</param>
/// <returns>1 在等；否则 0，数据片恒为 0</returns>
int32_t http_expect_continue(struct http_pack_ctx *pack);
/// <summary>
/// 回完这条请求的响应后是否该关连接(RFC 9112 §9.6)：带 Connection: close，或是 HTTP/1.0 且没带 Connection: keep-alive
/// </summary>
/// <param name="pack">服务端收到的请求包</param>
/// <returns>1 该关；否则 0，数据片恒为 0</returns>
int32_t http_want_close(struct http_pack_ctx *pack);
/// <summary>
/// 判一段头值能否进线格式：CR / LF 会把一条报文劈成两条，NUL 会让按 C 字符串取值的下游截断。
/// 收业务数据的组包方(router 等)拿它筛完再组头行。头名的规则是 is_token。
/// 比 http_pack_head2 的断言严：那道只挡结构性的 CR / LF，本函数还挡 NUL
/// </summary>
/// <param name="val">头值；lens 非 0 时不得为 NULL</param>
/// <param name="lens">头值字节数；0 表示空值头，恒合法</param>
/// <returns>可以进线格式非 0；否则 0</returns>
int32_t http_head_val_ok(const char *val, size_t lens);
/// <summary>
/// 判一条业务给的附加头的头名能否进报文：是 token、短于 HTTP_HEAD_KEY_MAX、不是 autoflags 标明由组包函数
/// 自己写的头(大小写无关)。头值另用 http_head_val_ok 判。收附加头的组包方共用这一份判据
/// </summary>
/// <param name="key">头名；可为 NULL，按不合法算</param>
/// <param name="klens">头名字节数</param>
/// <param name="autoflags">HTTP_AUTO_* 的组合</param>
/// <returns>HTTP_KEY_OK 合法；HTTP_KEY_BAD 不是 token 或过长；HTTP_KEY_AUTO 是组包函数自己生成的头</returns>
http_key_rc http_head_key_check(const char *key, size_t klens, int32_t autoflags);

#endif//HTTP_H_
