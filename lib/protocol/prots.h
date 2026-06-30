#ifndef PROTS_H_
#define PROTS_H_

#include "event/evpub.h"
#include "protocol/message.h"

// 协议包类型枚举
typedef enum pack_type {
    PACK_NONE = 0x00,       // 无协议（透传原始数据）
    PACK_DNS,               // DNS 协议
    PACK_HTTP,              // HTTP 协议
    PACK_WEBSOCK,           // WebSocket 协议
    PACK_MQTT,              // MQTT 协议
    PACK_SMTP,              // SMTP 协议
    PACK_CUSTZ_FIXED,       // 自定义协议 - 固定 4 字节长度头
    PACK_CUSTZ_FLAG,        // 自定义协议 - 标志位变长头
    PACK_CUSTZ_VAR,         // 自定义协议 - MQTT 风格变长头

    PACK_REDIS = 0x50,      // Redis RESP 协议
    PACK_MYSQL,             // MySQL 协议
    PACK_PGSQL,             // PostgreSQL 协议
    PACK_MONGO              // MongoDB Wire 协议
}pack_type;
// 协议解包状态标志（可多个标志同时置位）
typedef enum prot_status {
    PROT_INIT = 0x00,          // 初始/正常状态
    PROT_SLICE_START = 0x01,   // 分片起始包
    PROT_SLICE = 0x02,         // 分片中间包
    PROT_SLICE_END = 0x04,     // 分片结束包
    PROT_ERROR = 0x08,         // 协议错误
    PROT_MOREDATA = 0x10,      // 数据不足，需等待更多数据
    PROT_CLOSE = 0x20          // 连接关闭信号
}prot_status;

// 握手完成后的推送回调函数类型
typedef int32_t(*_handshaked_push)(SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud, int32_t erro, void *data, size_t lens);
// 消息汇：网络事件回调向上推消息的接口，由 task 层注册实现
typedef void*(*prots_emit_begin_cb)(ud_cxt *ud);              // 开窗：grab 目标，返回不透明句柄，NULL=目标不存在
typedef void(*prots_emit_cb)(void *target, message_ctx *msg);// 推一条消息给已开窗的目标
typedef void(*prots_emit_end_cb)(void *target);              // 关窗：释放 begin 取得的句柄
typedef struct prot_emit {
    prots_emit_begin_cb begin;
    prots_emit_cb emit;
    prots_emit_end_cb end;
}prot_emit;

/// <summary>
/// 初始化协议模块，注册消息汇（网络事件回调经它向上推消息）
/// </summary>
/// <param name="emit">消息汇实现（begin/emit/end），prots 内部按值保存</param>
void prots_init(prot_emit *emit);
/// <summary>
/// 释放协议模块全局资源
/// </summary>
void prots_free(void);
/// <summary>
/// 释放解包数据包内存，根据协议类型调用对应的释放函数
/// </summary>
/// <param name="pktype">协议包类型</param>
/// <param name="data">待释放的包指针</param>
void prots_pkfree(pack_type pktype, void *data);
/// <summary>
/// 释放握手阶段数据包内存
/// </summary>
/// <param name="pktype">协议包类型</param>
/// <param name="data">待释放的包指针</param>
void prots_hsfree(pack_type pktype, void *data);
/// <summary>
/// 释放 ud_cxt 关联的协议上下文资源
/// </summary>
/// <param name="arg">ud_cxt 指针</param>
void prots_udfree(void *arg);
/// <summary>
/// 连接关闭时通知各协议模块做清理（如发送挂断命令）
/// </summary>
/// <param name="ud">ud_cxt 指针</param>
void prots_closed(ud_cxt *ud);
/// <summary>
/// 新连接被接受时的回调，目前各协议均返回 ERR_OK
/// </summary>
/// <param name="ev">事件上下文</param>
/// <param name="fd">套接字</param>
/// <param name="skid">套接字 ID</param>
/// <param name="ud">ud_cxt 指针</param>
/// <returns>ERR_OK</returns>
int32_t prots_accepted(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud);
/// <summary>
/// 主动连接建立后的回调，部分协议需在此发送初始化包
/// </summary>
/// <param name="ev">事件上下文</param>
/// <param name="fd">套接字</param>
/// <param name="skid">套接字 ID</param>
/// <param name="ud">ud_cxt 指针</param>
/// <param name="err">连接错误码</param>
/// <returns>ERR_OK 或错误码</returns>
int32_t prots_connected(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud, int32_t err);
/// <summary>
/// SSL 握手完成后的回调，部分协议需在 SSL 建立后发送认证包
/// pgsql 会从 ssl 中提取服务端证书 SHA-256 摘要用于 SCRAM-SHA-256-PLUS 通道绑定
/// </summary>
/// <param name="ev">事件上下文</param>
/// <param name="fd">套接字</param>
/// <param name="skid">套接字 ID</param>
/// <param name="client">1=客户端 0=服务端</param>
/// <param name="ud">ud_cxt 指针</param>
/// <param name="ssl">SSL 对象指针（WITH_SSL 时有效，否则为 NULL）</param>
/// <returns>ERR_OK 或错误码</returns>
int32_t prots_ssl_exchanged(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud, void *ssl);
/// <summary>
/// 检查协议是否允许继续读取数据（如 pgsql 的流量控制）
/// </summary>
/// <param name="pktype">协议包类型</param>
/// <param name="data">已解析的包数据</param>
/// <returns>ERR_OK=可继续，其他值=暂停读取</returns>
int32_t prots_may_resume(pack_type pktype, void *data);
/// <summary>
/// 统一解包入口，根据 ud->pktype 调用对应协议的解包函数
/// </summary>
/// <param name="ev">事件上下文</param>
/// <param name="fd">套接字</param>
/// <param name="skid">套接字 ID</param>
/// <param name="client">1=客户端 0=服务端</param>
/// <param name="buf">接收缓冲区</param>
/// <param name="ud">ud_cxt 指针</param>
/// <param name="size">输出：数据包长度</param>
/// <param name="status">输出：解包状态标志</param>
/// <returns>解包后的数据指针，NULL 表示数据不足或出错</returns>
void *prots_unpack(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client,
    buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status);
// 以下 7 个为网络事件回调，由 task_listen/connect/udp 装入 cbs_ctx、event 层触发；
// 各自经 prots_init 注册的消息汇 begin→emit→end 把事件转成 message_ctx 推给上层；
// 签名与 cbs_ctx 对应回调（accept_cb/connect_cb/recv_cb/...）一致。
/// <summary>接受新连接：完成协议初始化并推送 MSG_TYPE_ACCEPT</summary>
int32_t prots_net_accept(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud);
/// <summary>主动连接建立：完成协议初始化并推送 MSG_TYPE_CONNECT</summary>
int32_t prots_net_connect(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t err, ud_cxt *ud);
/// <summary>数据接收：循环解包并推送 MSG_TYPE_RECV（按 slice 标记分片）</summary>
void prots_net_recv(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, buffer_ctx *buf, size_t size, ud_cxt *ud);
/// <summary>发送完成：推送 MSG_TYPE_SEND</summary>
void prots_net_send(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, size_t size, ud_cxt *ud);
/// <summary>SSL 握手完成：完成协议 SSL 初始化并推送 MSG_TYPE_SSLEXCHANGED</summary>
int32_t prots_net_ssl_exchanged(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud, void *ssl);
/// <summary>连接关闭：通知协议层并推送 MSG_TYPE_CLOSE</summary>
void prots_net_close(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud);
/// <summary>UDP 接收：打包地址+数据并推送 MSG_TYPE_RECVFROM</summary>
void prots_net_recvfrom(ev_ctx *ev, SOCKET fd, uint64_t skid, char *buf, size_t size, netaddr_ctx *addr, ud_cxt *ud);

#endif//PROTS_H_
