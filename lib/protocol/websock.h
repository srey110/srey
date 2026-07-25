#ifndef WEBSOCK_H_
#define WEBSOCK_H_

#include "event/event.h"
#include "crypt/base64.h"
#include "crypt/digest.h"
#include "protocol/prots_pub.h"

#define WS_SIGN_KEY_LENS B64EN_SIZE(SHA1_BLOCK_SIZE)
#define WS_MAXCNT_SECPROT 8

typedef enum ws_prot {
    WS_CONTINUE = 0x00,
    WS_TEXT = 0x01,
    WS_BINARY = 0x02,
    WS_CLOSE = 0x08,
    WS_PING = 0x09,
    WS_PONG = 0x0A
}ws_prot;
//客户端握手签名 子协议信息
typedef struct ws_hs_ctx {
    int32_t cnt;//prots总数
    size_t dlens;//data 长度
    char signkey[WS_SIGN_KEY_LENS];//签名
    buf_ctx prots[WS_MAXCNT_SECPROT];//拆分后的子协议
    char data[];//数据载体
}ws_hs_ctx;
//子协议信息，MSG_TYPE_HANDSHAKED 供调用方使用
typedef struct ws_secprots_ctx {
    int32_t cnt; //子协议数
    int32_t index;//匹配到的子协议下标 -1 无
    size_t dlens;//data 长度
    buf_ctx prots[WS_MAXCNT_SECPROT];//全部子协议集合
    char data[];//数据载体
}ws_secprots_ctx;

void _websock_pkfree(void *data);
void *_websock_pack_next(void *pack);
void _websock_udfree(ud_cxt *ud);
void _websock_secextra(ud_cxt *ud, void *val);
/// <summary>
/// 按子协议名匹配内建承载协议类型(当前仅 "mqtt"->PACK_MQTT，区分大小写)
/// </summary>
/// <param name="data">子协议名(可非 NUL 结尾)</param>
/// <param name="lens">子协议名字节长度</param>
/// <param name="sectype">out 匹配成功写入承载协议类型</param>
/// <returns>匹配成功 ERR_OK，否则 ERR_FAILED</returns>
int32_t websock_secprot_match(const char *data, size_t lens, pack_type *sectype);
/// <summary>
/// 设置 WebSocket 承载子协议(如 MQTT over WebSocket)的额外上下文数据(ws->ud->context)
/// </summary>
/// <param name="ev">ev_ctx</param>
/// <param name="fd">socket句柄</param>
/// <param name="skid">链接ID</param>
/// <param name="val">业务自定义数据,所有权转移给 ws->ud->context</param>
/// <returns>ERR_OK 成功投递；stop 非0失败</returns>
int32_t websock_set_secextra(ev_ctx *ev, SOCKET fd, uint64_t skid, void *val);
/// <summary>
/// WebSocket 解包：握手阶段完成 HTTP 升级，数据阶段从缓冲区解析一个完整帧（含分片）
/// </summary>
/// <param name="ev">事件上下文</param>
/// <param name="fd">socket 句柄</param>
/// <param name="skid">链接 ID</param>
/// <param name="client">非0 客户端解析，0 服务端解析</param>
/// <param name="buf">接收缓冲区</param>
/// <param name="ud">连接上下文，内部维护握手/解析状态</param>
/// <param name="status">输出：解包状态标志，见 prot_status</param>
/// <returns>解析完成的 websock_pack_ctx，数据不足或握手未完成返回 NULL</returns>
struct websock_pack_ctx *websock_unpack(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client,
    buffer_ctx *buf, ud_cxt *ud, int32_t *status);
/// <summary>
/// 握手包
/// </summary>
/// <param name="host">Host</param>
/// <param name="uri">HTTP request-target（path?query）；NULL 或空字符串时使用 "/"</param>
/// <param name="secprot">Sec-WebSocket-Protocol；NULL 或空表示不带子协议</param>
/// <param name="hsctx">out 握手上下文(含签名与子协议)，作为 task_connect extra 参数传入交协议层管理</param>
/// <returns>握手包；secprot校验失败返回 NULL(此时 *hsctx 不写入)</returns>
char *websock_pack_handshake(const char *host, const char *uri, const char *secprot, ws_hs_ctx **hsctx);
/// <summary>
/// ping包
/// </summary>
/// <param name="mask">1 掩码, 客户端向服务器发送数据都需要掩码, 0 无掩码</param>
/// <param name="size">包长度</param>
/// <returns>ping包</returns>
void *websock_pack_ping(int32_t mask, size_t *size);
/// <summary>
/// pong包
/// </summary>
/// <param name="mask">1 掩码, 客户端向服务器发送数据都需要掩码, 0 无掩码</param>
/// <param name="size">包长度</param>
/// <returns>pong包</returns>
void *websock_pack_pong(int32_t mask, size_t *size);
/// <summary>
/// close包
/// </summary>
/// <param name="mask">1 掩码, 客户端向服务器发送数据都需要掩码, 0 无掩码</param>
/// <param name="size">包长度</param>
/// <returns>close包</returns>
void *websock_pack_close(int32_t mask, size_t *size);
/// <summary>
/// 文本消息包
/// </summary>
/// <param name="mask">1 掩码, 客户端向服务器发送数据都需要掩码, 0 无掩码</param>
/// <param name="fin">1 完整包 0 分片</param>
/// <param name="data">数据</param>
/// <param name="dlens">数据长度</param>
/// <param name="size">包长度</param>
/// <returns>文本消息包</returns>
void *websock_pack_text(int32_t mask, int32_t fin, void *data, size_t dlens, size_t *size);
/// <summary>
/// 二进制消息包
/// </summary>
/// <param name="mask">1 掩码, 客户端向服务器发送数据都需要掩码, 0 无掩码</param>
/// <param name="fin">1 完整包 0 分片</param>
/// <param name="data">数据</param>
/// <param name="dlens">数据长度</param>
/// <param name="size">包长度</param>
/// <returns>二进制消息包</returns>
void *websock_pack_binary(int32_t mask, int32_t fin, void *data, size_t dlens, size_t *size);
/// <summary>
/// 分片消息包
/// </summary>
/// <param name="mask">1 掩码, 客户端向服务器发送数据都需要掩码, 0 无掩码</param>
/// <param name="fin">1 结束 0 未结束</param>
/// <param name="data">数据</param>
/// <param name="dlens">数据长度</param>
/// <param name="size">包长度</param>
/// <returns>分片消息包</returns>
void *websock_pack_continua(int32_t mask, int32_t fin, void *data, size_t dlens, size_t *size);
/// <summary>
/// 获取fin值
/// </summary>
/// <param name="pack">websock_pack_ctx</param>
/// <returns>fin</returns>
int32_t websock_fin(struct websock_pack_ctx *pack);
/// <summary>
/// 获取协议号
/// </summary>
/// <param name="pack">websock_pack_ctx</param>
/// <returns>协议号</returns>
int32_t websock_prot(struct websock_pack_ctx *pack);
/// <summary>
/// 获取子协议
/// </summary>
/// <param name="pack">websock_pack_ctx</param>
/// <returns>协议号</returns>
int32_t websock_secprot(struct websock_pack_ctx *pack);
/// <summary>
/// 获取子协议数据包
/// </summary>
/// <param name="pack">websock_pack_ctx</param>
/// <returns>协议包</returns>
void *websock_secpack(struct websock_pack_ctx *pack);
/// <summary>
/// 获取数据
/// </summary>
/// <param name="pack">websock_pack_ctx</param>
/// <param name="pack">数据长度</param>
/// <returns>数据</returns>
char *websock_data(struct websock_pack_ctx *pack, size_t *lens);
void _websock_init(void *hspush);

#endif//WEBSOCK_H_
