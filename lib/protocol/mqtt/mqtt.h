#ifndef MQTT_H_
#define MQTT_H_

#include "protocol/mqtt/mqtt_pack.h"

// 释放 mqtt_pack_ctx 及其子结构的内存
void _mqtt_pkfree(void *data);
// 释放 ud_cxt 中挂载的 mqtt_ctx 上下文
void _mqtt_udfree(ud_cxt *ud);
// 判断当前数据包是否允许 task 恢复（服务端主动推的包不允许立即恢复）
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
/// 原因字符串。小于0x80的原因码指示某次操作成功完成，通常用0来表示。大于等于0x80的原因码用来指示操作失败。
/// </summary>
/// <param name="prot">mqtt_prot</param>
/// <param name="code">原因码</param>
/// <returns>char * 字符串</returns>
const char *mqtt_reason(mqtt_prot prot, int32_t code);

#endif//MQTT_H_
