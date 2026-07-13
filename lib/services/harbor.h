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
void *harbor_pack(name_t task, int32_t call, subtype_t reqtype, void *data, size_t size, size_t *lens);

#endif//HARBOR_H_
