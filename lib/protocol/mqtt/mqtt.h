#ifndef MQTT_H_
#define MQTT_H_

#include "srey/spub.h"
#include "protocol/mqtt/mqtt_pack.h"

// 释放 mqtt_pack_ctx 及其子结构的内存
void _mqtt_pkfree(void *data);
// 释放 ud_cxt 中挂载的 mqtt_ctx 上下文
void _mqtt_udfree(ud_cxt *ud);
/// <summary>
/// 从缓冲区中解析一个完整的 MQTT 数据包
/// </summary>
/// <param name="client">1 表示当前端为客户端，0 表示服务端</param>
/// <param name="buf">接收缓冲区</param>
/// <param name="ud">连接上下文，内部存储协议版本和解析状态</param>
/// <param name="status">解析结果标志位（PROT_MOREDATA / PROT_ERROR / PROT_CLOSE）</param>
/// <returns>解析成功返回 mqtt_pack_ctx*，数据不足或出错返回 NULL</returns>
mqtt_pack_ctx *mqtt_unpack(int32_t client, buffer_ctx *buf, ud_cxt *ud, int32_t *status);
/// <summary>
/// 客户端发起 MQTT 异步连接。内部 malloc 一份 mqtt_ctx 仅记录协议版本，挂在
/// ud->context 上供后续报文解析使用；连接关闭时由 _mqtt_udfree 自动 FREE，无需调用方管理。
/// 函数本身只是发起 connect，调用方需通过 coro_handshaked / wait_connect 等同步原语
/// 等待 TCP（或 SSL）建立后再发送 CONNECT 控制报文。
/// </summary>
/// <param name="task">所属 task_ctx</param>
/// <param name="evssl">SSL 上下文；NULL 表示明文连接</param>
/// <param name="ip">服务器 IP 地址</param>
/// <param name="port">服务器端口</param>
/// <param name="netev">附加事件标志位（NETEV_SEND / NETEV_AUTHSSL 等，0 表示无）</param>
/// <param name="version">MQTT 协议版本（mqtt_protversion，决定后续解包格式）</param>
/// <param name="fd">输出：socket 句柄</param>
/// <param name="skid">输出：连接 ID</param>
/// <returns>ERR_OK 连接请求发起成功，ERR_FAILED 失败</returns>
int32_t mqtt_try_connect(task_ctx *task, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, int32_t netev,
    mqtt_protversion version, SOCKET *fd, uint64_t *skid);
/// <summary>
/// 原因字符串。小于0x80的原因码指示某次操作成功完成，通常用0来表示。大于等于0x80的原因码用来指示操作失败。
/// </summary>
/// <param name="prot">mqtt_prot</param>
/// <param name="code">原因码</param>
/// <returns>char * 字符串</returns>
const char *mqtt_reason(mqtt_prot prot, int32_t code);

#endif//MQTT_H_
