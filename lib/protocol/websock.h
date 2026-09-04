#ifndef WEBSOCK_H_
#define WEBSOCK_H_

#include "event/event.h"
#include "crypt/base64.h"
#include "crypt/digest.h"
#include "protocol/prots_pub.h"

struct websock_pack_ctx;

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
//连接上下文，每条连接一个，挂在 ud_cxt.context 上。
//放头文件是因为测试要在栈上造一个直接喂给 websock_unpack：
//两边各写一份等价布局的话，这里加字段那边不报错，解包会写出对象边界
typedef struct websock_ctx {
    int8_t slice;//是否处于分片接收状态(1=是)
    pack_type secprot;//子协议类型
    buffer_ctx *buf;//子协议数据缓冲区
    ud_cxt *ud;//子协议的 ud_cxt(用于子协议解包)
    struct websock_pack_ctx *pack;//当前正在解析的帧(DATA 状态下有效)
}websock_ctx;

void _websock_pkfree(void *data);
void *_websock_pack_next(void *pack);
void _websock_udfree(ud_cxt *ud);
/// <summary>
/// 按子协议名匹配内建承载协议类型(当前仅 "mqtt"->PACK_MQTT，区分大小写)
/// </summary>
/// <param name="data">子协议名(可非 NUL 结尾)</param>
/// <param name="lens">子协议名字节长度</param>
/// <param name="sectype">out 匹配成功写入承载协议类型</param>
/// <returns>匹配成功 ERR_OK，否则 ERR_FAILED</returns>
int32_t websock_secprot_match(const char *data, size_t lens, pack_type *sectype);
/// <summary>
/// 设置 WebSocket 承载子协议(如 MQTT over WebSocket)的额外上下文数据(ws->ud->context)。
/// 只能设一次，解析器会把 val 当该子协议的上下文解引用。按方向分两种情形：
/// 客户端方向 ws->ud->context 恒为空，正是本口子的用途(wbsock_connect 的 MQTT 承载即走它)；
/// 服务端方向 MQTT 由 _mqtt_connect 解 CONNECT 时自建上下文，不要往那个方向注入
/// </summary>
/// <param name="ev">ev_ctx</param>
/// <param name="fd">socket句柄</param>
/// <param name="skid">链接ID</param>
/// <param name="val">业务自定义数据,设置成功后所有权转移给 ws->ud->context</param>
/// <param name="fcb">val 的释放回调，可为 NULL(不释放)。凡没能真的设上一律用它回收 val：
/// fd 非法、命令到达时连接已不在、协议层拒收、事件循环拆除时命令还没执行</param>
/// <returns>ERR_OK 命令已投递，只表示投递成功、不代表真的设上了；fd 为 INVALID_SOCK 时
/// 返回 ERR_FAILED 并已用 fcb 回收 val，调用方不可再释放。
/// 非 WebSocket 连接、握手尚未完成、子协议没有内建解析器、已注入过一次——这几种一律不设置
/// 且就地断开该连接，已注入过的那个旧值留给拆连接时回收</returns>
int32_t websock_set_secextra(ev_ctx *ev, SOCKET fd, uint64_t skid, void *val, free_cb fcb);
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
void *websock_unpack(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client,
    buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status);
/// <summary>
/// 握手包
/// </summary>
/// <param name="host">Host</param>
/// <param name="uri">HTTP request-target（path?query）；NULL 或空字符串时使用 "/"</param>
/// <param name="secprot">Sec-WebSocket-Protocol；NULL 或空表示不带子协议</param>
/// <param name="hsctx">out 握手上下文(含签名与子协议)，作为 task_connect extra 参数传入交协议层管理</param>
/// <returns>握手包；host / uri / secprot 任一含 CRLF、secprot 校验失败、或取不到 CSPRNG 熵生成 nonce 时
/// 返回 NULL(此时 *hsctx 不写入)</returns>
char *websock_pack_handshake(const char *host, const char *uri, const char *secprot, ws_hs_ctx **hsctx);
/// <summary>
/// ping包
/// </summary>
/// <param name="mask">1 掩码, 客户端向服务器发送数据都需要掩码, 0 无掩码</param>
/// <param name="size">包长度</param>
/// <returns>ping包；mask 非 0 且取不到 CSPRNG 熵生成掩码 key 时返回 NULL(*size 置 0)</returns>
void *websock_pack_ping(int32_t mask, size_t *size);
/// <summary>
/// pong包
/// </summary>
/// <param name="mask">1 掩码, 客户端向服务器发送数据都需要掩码, 0 无掩码</param>
/// <param name="size">包长度</param>
/// <returns>pong包；返回 NULL 的情形同 websock_pack_ping</returns>
void *websock_pack_pong(int32_t mask, size_t *size);
/// <summary>
/// close包
/// </summary>
/// <param name="mask">1 掩码, 客户端向服务器发送数据都需要掩码, 0 无掩码</param>
/// <param name="size">包长度</param>
/// <returns>close包；返回 NULL 的情形同 websock_pack_ping</returns>
void *websock_pack_close(int32_t mask, size_t *size);
/// <summary>
/// 文本消息包
/// </summary>
/// <param name="mask">1 掩码, 客户端向服务器发送数据都需要掩码, 0 无掩码</param>
/// <param name="fin">1 完整包 0 分片</param>
/// <param name="data">数据</param>
/// <param name="dlens">数据长度</param>
/// <param name="size">包长度</param>
/// <returns>文本消息包；除 websock_pack_ping 的情形外，dlens 大到使帧长回绕时同样返回 NULL(*size 置 0)</returns>
void *websock_pack_text(int32_t mask, int32_t fin, void *data, size_t dlens, size_t *size);
/// <summary>
/// 二进制消息包
/// </summary>
/// <param name="mask">1 掩码, 客户端向服务器发送数据都需要掩码, 0 无掩码</param>
/// <param name="fin">1 完整包 0 分片</param>
/// <param name="data">数据</param>
/// <param name="dlens">数据长度</param>
/// <param name="size">包长度</param>
/// <returns>二进制消息包；除 websock_pack_ping 的情形外，dlens 大到使帧长回绕时同样返回 NULL(*size 置 0)</returns>
void *websock_pack_binary(int32_t mask, int32_t fin, void *data, size_t dlens, size_t *size);
/// <summary>
/// 分片消息包
/// </summary>
/// <param name="mask">1 掩码, 客户端向服务器发送数据都需要掩码, 0 无掩码</param>
/// <param name="fin">1 结束 0 未结束</param>
/// <param name="data">数据</param>
/// <param name="dlens">数据长度</param>
/// <param name="size">包长度</param>
/// <returns>分片消息包；除 websock_pack_ping 的情形外，dlens 大到使帧长回绕时同样返回 NULL(*size 置 0)</returns>
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
/// <returns>子协议类型;未协商子协议时为 PACK_NONE</returns>
int32_t websock_secprot(struct websock_pack_ctx *pack);
/// <summary>
/// 获取子协议数据包
/// </summary>
/// <param name="pack">websock_pack_ctx</param>
/// <returns>子协议包;控制帧(PING/PONG/CLOSE)与零长数据帧即使 websock_secprot 非 PACK_NONE 也返回 NULL,取用前必判</returns>
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
