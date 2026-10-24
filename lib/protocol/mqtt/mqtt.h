#ifndef MQTT_H_
#define MQTT_H_

#include "protocol/mqtt/mqtt_pack.h"

#define MQTT_SPLIT_HEAD 256 // 跨节点的包拷出来解析时先用栈上这块，装不下才 MALLOC

// 释放 mqtt_pack_ctx 及其子结构的内存
void _mqtt_pkfree(void *data);
// 释放 ud_cxt 中挂载的 mqtt_ctx 上下文
void _mqtt_udfree(ud_cxt *ud);
// 判断当前数据包能否唤醒等待者：对端主动发起、不是回复的包不能(名单见实现)
int32_t _mqtt_may_resume(void *data);
/// <summary>
/// 从缓冲区中解析一个完整的 MQTT 数据包
/// </summary>
/// <param name="client">1 表示当前端为客户端，0 表示服务端</param>
/// <param name="buf">接收缓冲区</param>
/// <param name="ud">连接上下文，内部存储协议版本和解析状态</param>
/// <param name="size">输出：返回包时写入该报文总长（固定头 + 剩余长度），用于内存记账，不是 data 处可读的长度；
/// 没返回包时不写。必须非 NULL</param>
/// <param name="status">解析结果标志位（PROT_MOREDATA / PROT_ERROR / PROT_CLOSE）</param>
/// <returns>解析成功返回 mqtt_pack_ctx*，数据不足或出错返回 NULL</returns>
void *mqtt_unpack(ev_ctx *ev, sock_ctx *sk, int32_t client,
    buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status);
// 同 mqtt_unpack，只是数据在一段连续内存上、data 开头就是包头。固定头读不全、超 MQTT_MAX_PACK_LENS 或本包不完整时
// 返回 NULL、*used 为 0 且不动 status，这些情形交给 mqtt_unpack 判；解过包 *used 即非 0，是该从 data 前头排掉的字节数(成功时为报文总长)
void *_mqtt_unpack_mem(int32_t client, char *data, size_t lens, ud_cxt *ud, size_t *used, int32_t *status);
/// <summary>
/// 取 MQTT 连接上下文(只记协议版本)。用在需要调用方注入上下文的承载场景：
/// MQTT over WebSocket 的客户端方向没有别的建立点，注入走 mqtt_ws_bind(它内部就调本函数)；
/// 服务端方向由 mqtt_unpack 解 CONNECT 时自建，不要再注入
/// </summary>
/// <param name="version">mqtt_protversion</param>
/// <returns>mqtt_ctx；version 不是 MQTT_311 / MQTT_50 返回 NULL。返回的是每个版本一份的全局只读实例，不能写；
/// 用法照旧按"所有权归调用方"：交给 websock_set_secextra 时连 mqtt_ctx_free 一起当 fcb 传，调用方不再持有</returns>
mqtt_ctx *mqtt_ctx_new(mqtt_protversion version);
/// <summary>
/// 释放 mqtt_ctx_new 产出的上下文。实例是全局的，本函数为空操作；签名匹配 free_cb，可直接当 websock_set_secextra 的 fcb 传
/// </summary>
/// <param name="ctx">mqtt_ctx；NULL 安全</param>
void mqtt_ctx_free(void *ctx);
/// <summary>
/// 原因码转可读字符串，按协议版本取表：5.0 原因码小于 0x80 表示成功、大于等于 0x80 表示失败；
/// 3.1.1 的 CONNACK 返回码 0 为接受、1~5 各是一种拒绝(没有 0x80 分界)，其余 3.1.1 报文与 5.0 共用一张表
/// </summary>
/// <param name="version">mqtt_protversion，取自报文的 version</param>
/// <param name="prot">mqtt_prot</param>
/// <param name="code">原因码</param>
/// <returns>静态只读字符串；版本、报文类型与原因码对不上时为 "Unknown"</returns>
const char *mqtt_reason(mqtt_protversion version, mqtt_prot prot, int32_t code);

#endif//MQTT_H_
