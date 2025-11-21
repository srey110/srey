#ifndef WEBSOCK_H_
#define WEBSOCK_H_

#include "event/event.h"

typedef enum ws_proto {
    WS_CONTINUE = 0x00,
    WS_TEXT = 0x01,
    WS_BINARY = 0x02,
    WS_CLOSE = 0x08,
    WS_PING = 0x09,
    WS_PONG = 0x0A
}ws_proto;

void _websock_pkfree(void *data);
void _websock_udfree(ud_cxt *ud);
void _websock_secextra(ud_cxt *ud, void *val);
//解包
struct websock_pack_ctx *websock_unpack(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client,
    buffer_ctx *buf, ud_cxt *ud, int32_t *status);
/// <summary>
/// 握手包
/// </summary>
/// <param name="host">Host</param>
/// <param name="secproto">Sec-WebSocket-Protocol</param>
/// <returns>握手包</returns>
char *websock_pack_handshake(const char *host, const char *secproto);
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
int32_t websock_proto(struct websock_pack_ctx *pack);
/// <summary>
/// 获取子协议
/// </summary>
/// <param name="pack">websock_pack_ctx</param>
/// <returns>协议号</returns>
int32_t websock_secproto(struct websock_pack_ctx *pack);
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
