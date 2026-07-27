#ifndef HARBOR_H_
#define HARBOR_H_

#include "srey/coro.h"

/// <summary>
/// 启动harbor,服务器间通信
/// </summary>
/// <param name="loader">loader_ctx</param>
/// <param name="tname">字符串任务名；NULL 或空串表示不启动 harbor</param>
/// <param name="ssl">evssl_ctx 名称；配双向证书(mTLS)则跨节点鉴权由 TLS 层保证，NULL 或 "" 为明文无鉴权(仅限受信内网)</param>
/// <param name="ip">IP</param>
/// <param name="port">端口</param>
/// <returns>ERR_OK 成功</returns>
int32_t harbor_start(loader_ctx *loader, const char *tname, const char *ssl, const char *ip, uint16_t port);
/// <summary>
/// 服务器间通信请求包
/// </summary>
/// <param name="task">目标任务名</param>
/// <param name="call">1 执行call 0 执行request</param>
/// <param name="reqtype">请求类型</param>
/// <param name="data">数据</param>
/// <param name="size">数据长度</param>
/// <param name="lens">请求包长度</param>
/// <returns>请求包</returns>
/// <remarks>对端响应格式：
///   200 目标已处理(call 为投递成功)；400 目标返回非 ERR_OK；404 目标 task 不存在或 reqtype 落在
///   spub.h 的框架保留区间(REQ_DEBUG / REQ_DC_* / REQ_SC_*，不经 harbor 转发)。
///   body 即目标响应负载原文，目标未回负载时为零长(不会以状态文本充当负载)，Content-Type
///   仅在负载非空时出现。X-Srey-Erro 头(十进制，目标真实错误码)只出现在 request 的 200/400 上：
///   call 是单向投递、不存在"目标错误码"，其 200 不带该头；404 由 harbor 自身产生，也不带。
///   注意 400 + X-Srey-Erro:-1 同时覆盖"请求超时"与"目标返回 ERR_FAILED"两种情形——
///   coro_request 超时路径写的也是 ERR_FAILED，harbor 无从区分。</remarks>
void *harbor_pack(name_t task, int32_t call, subtype_t reqtype, void *data, size_t size, size_t *lens);

#endif//HARBOR_H_
